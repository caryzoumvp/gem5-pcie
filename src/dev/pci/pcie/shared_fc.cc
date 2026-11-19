/*
 * Copyright (c) 2026 cary
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "dev/pci/pcie/shared_fc.hh"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace gem5::pcie
{
namespace
{

constexpr size_t
classIndex(TlpClass type)
{
    return static_cast<size_t>(type);
}

uint32_t &
part(Credits &credits, unsigned index)
{
    return index == 0 ? credits.headers : credits.data;
}

bool
fits(Credits capacity, Credits cost)
{
    return cost.headers <= capacity.headers && cost.data <= capacity.data;
}

Credits
subtract(Credits lhs, Credits rhs)
{
    return {lhs.headers - rhs.headers, lhs.data - rhs.data};
}

void
add(Credits &lhs, Credits rhs)
{
    lhs.headers += rhs.headers;
    lhs.data += rhs.data;
}

} // anonymous namespace

SharedFc::SharedFc(const SharedFcConfig &config)
    : config(config), shared(config.shared)
{
    if (config.vcCount == 0 || config.vcCount > channels.size() ||
        (!config.receiverShared && config.mergePostedCompletion)) {
        throw std::invalid_argument("invalid Shared FC profile");
    }
    for (size_t type = 0; type < shared.size(); ++type) {
        const auto cls = static_cast<TlpClass>(type);
        const auto &capacity = shared[type];
        const auto group = quantum(cls);
        if (((!config.receiverShared || pool(cls) != type) &&
             capacity != Credits{}) ||
            capacity.headers % group || capacity.data % group) {
            throw std::invalid_argument("invalid Shared FC backing");
        }
    }
    unsigned total_limit = 0;
    for (size_t vc = 0; vc < channels.size(); ++vc) {
        const bool enabled = vc < config.vcCount;
        const auto limit = config.usageLimit[vc];
        if (limit && (!enabled || !config.receiverShared || *limit > 7)) {
            throw std::invalid_argument("invalid Shared FC usage limit");
        }
        if (enabled) {
            total_limit += limit.value_or(8);
        }
        for (size_t type = 0; type < shared.size(); ++type) {
            const auto capacity = config.dedicated[vc][type];
            if (!enabled && capacity != Credits{}) {
                throw std::invalid_argument("disabled VC has FC backing");
            }
            channels[vc][type].dedicated = capacity;
        }
    }
    if (total_limit < 8) {
        throw std::invalid_argument("Shared FC usage limits below 100%");
    }
}

bool
SharedFc::valid(CreditChannel channel) const
{
    return channel.vc < config.vcCount &&
           classIndex(channel.type) < shared.size();
}

size_t
SharedFc::pool(TlpClass type) const
{
    if (config.mergePostedCompletion && type == TlpClass::Completion) {
        return classIndex(TlpClass::Posted);
    }
    return classIndex(type);
}

uint32_t
SharedFc::quantum(TlpClass type) const
{
    return config.mergePostedCompletion && type != TlpClass::NonPosted ? 8 : 4;
}

FcAdmission
SharedFc::admit(uint64_t id, CreditChannel channel, uint32_t data_bytes)
{
    if (!valid(channel) || data_bytes > 4096 || transfers.count(id) ||
        sequence == std::numeric_limits<uint64_t>::max()) {
        return FcAdmission::Invalid;
    }
    const Credits cost{1, (data_bytes + 15) / 16};
    auto &state = channels[channel.vc][classIndex(channel.type)];
    const auto backing = pool(channel.type);
    const auto group = quantum(channel.type);
    Credits increment;
    for (unsigned index = 0; index < state.open.size(); ++index) {
        const auto &open = state.open[index];
        const uint32_t remaining = open ? open->remaining : 0;
        const uint32_t units = index == 0 ? cost.headers : cost.data;
        if (units > remaining) {
            part(increment, index) =
                (units - remaining + group - 1) / group * group;
        }
    }

    bool shared_ok = config.receiverShared && fits(shared[backing], increment);
    if (const auto limit = config.usageLimit[channel.vc]; shared_ok && limit) {
        const auto total = config.shared[backing];
        // P and C use the same capacity and usage budget when merged.
        Credits vc_used = state.sharedUsed;
        if (config.mergePostedCompletion &&
            channel.type != TlpClass::NonPosted) {
            const auto other = channel.type == TlpClass::Posted
                                   ? TlpClass::Completion
                                   : TlpClass::Posted;
            add(vc_used, channels[channel.vc][classIndex(other)].sharedUsed);
        }
        shared_ok = (uint64_t{vc_used.headers} + increment.headers) * 8 <=
                        uint64_t{total.headers} * *limit &&
                    (uint64_t{vc_used.data} + increment.data) * 8 <=
                        uint64_t{total.data} * *limit;
    }

    if (shared_ok) {
        Transfer tx{channel, cost, ++sequence, {}};
        consume(tx, 0, cost.headers);
        consume(tx, 1, cost.data);
        transfers.emplace(id, std::move(tx));
        return FcAdmission::Shared;
    }
    if (fits(state.dedicated, cost)) {
        const auto seq = ++sequence;
        state.dedicated = subtract(state.dedicated, cost);
        dedicatedPending.insert(seq);
        transfers.emplace(id, Transfer{channel, cost, seq, {}});
        return FcAdmission::Dedicated;
    }
    return FcAdmission::NoCredit;
}

void
SharedFc::consume(Transfer &tx, unsigned index, uint32_t units)
{
    auto &channel = channels[tx.channel.vc][classIndex(tx.channel.type)];
    auto &open = channel.open[index];
    const auto group = quantum(tx.channel.type);
    while (units > 0) {
        if (!open) {
            open =
                std::make_shared<Group>(Group{tx.channel, index != 0, group});
            part(shared[pool(tx.channel.type)], index) -= group;
            part(channel.sharedUsed, index) += group;
        }
        const auto consumed = std::min(units, open->remaining);
        open->remaining -= consumed;
        units -= consumed;
        ++open->owners;
        ++open->updating;
        open->lastSequence = tx.sequence;
        tx.groups.push_back(open);
        if (open->remaining == 0) {
            open->closed = true;
            open.reset();
        }
    }
}

bool
SharedFc::release(uint64_t id)
{
    const auto iter = transfers.find(id);
    if (iter == transfers.end() || iter->second.released) {
        return false;
    }
    auto &tx = iter->second;
    tx.released = true;
    auto &state = channels[tx.channel.vc][classIndex(tx.channel.type)];
    for (const auto &group : tx.groups) {
        if (--group->owners == 0 && !group->closed) {
            group->closed = true;
            state.open[group->data].reset();
        }
    }
    return true;
}

bool
SharedFc::update(uint64_t id)
{
    const auto iter = transfers.find(id);
    if (iter == transfers.end() || !iter->second.released) {
        return false;
    }
    const auto &tx = iter->second;
    if (tx.groups.empty()) {
        add(channels[tx.channel.vc][classIndex(tx.channel.type)].dedicated,
            tx.cost);
        dedicatedPending.erase(tx.sequence);
    }
    for (const auto &group : tx.groups) {
        if (--group->updating == 0) {
            sharedReady.emplace(group->lastSequence, group);
        }
    }
    transfers.erase(iter);
    drain();
    return true;
}

void
SharedFc::drain()
{
    while (!sharedReady.empty()) {
        const auto ready = sharedReady.begin();
        if (!dedicatedPending.empty() &&
            *dedicatedPending.begin() < ready->first) {
            break;
        }
        const auto &group = *ready->second;
        const auto [vc, type] = group.channel;
        const auto units = quantum(type);
        part(shared[pool(type)], group.data) += units;
        part(channels[vc][classIndex(type)].sharedUsed, group.data) -= units;
        sharedReady.erase(ready);
    }
}

Credits
SharedFc::sharedAvailable(TlpClass type) const
{
    return classIndex(type) < shared.size() ? shared[pool(type)] : Credits{};
}

Credits
SharedFc::sharedInUse(CreditChannel channel) const
{
    return valid(channel)
               ? channels[channel.vc][classIndex(channel.type)].sharedUsed
               : Credits{};
}

Credits
SharedFc::dedicatedAvailable(CreditChannel channel) const
{
    return valid(channel)
               ? channels[channel.vc][classIndex(channel.type)].dedicated
               : Credits{};
}

bool
SharedFc::empty() const
{
    return transfers.empty() && sharedReady.empty();
}

} // namespace gem5::pcie
