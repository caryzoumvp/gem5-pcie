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

#include "dev/pci/pcie/link.hh"

#include <stdexcept>
#include <utility>

namespace gem5::pcie
{
namespace
{

bool
covers(Credits available, Credits cost)
{
    return available.headers >= cost.headers && available.data >= cost.data;
}

void
moveCredits(Credits &from, Credits &to, Credits cost)
{
    if (!covers(from, cost)) {
        throw std::logic_error("PCIe credit ledger underflow");
    }
    from.headers -= cost.headers;
    from.data -= cost.data;
    to.headers += cost.headers;
    to.data += cost.data;
}

Tick
tickAfter(Tick now, Tick offset)
{
    if (offset > MaxTick - now) {
        throw std::overflow_error("PCIe link event exceeds Tick range");
    }
    return now + offset;
}

} // anonymous namespace

LinkDirection::LinkDirection(LinkTiming timing, Schedule schedule,
                             TransferEvent arrival, Ready ready,
                             TransferEvent departure)
    : timing(timing),
      schedule(std::move(schedule)),
      arrival(std::move(arrival)),
      ready(std::move(ready)),
      departure(std::move(departure))
{
    if (!timing.wireBytesPerCycle || !timing.cycle || !this->schedule ||
        !this->arrival || !this->ready) {
        throw std::invalid_argument("invalid PCIe Link timing or callbacks");
    }
}

void
LinkDirection::configure(CreditChannel channel, Credits backing)
{
    if (channel.vc >= 8 ||
        !channels.try_emplace(channel, Window{backing, {backing, {}, {}, {}}})
             .second) {
        throw std::invalid_argument(
            "invalid or duplicate PCIe credit channel");
    }
}

SendResult
LinkDirection::send(Tick now, Id id, CreditChannel channel,
                    uint32_t wire_bytes, uint32_t data_bytes)
{
    const Credits cost{1, data_bytes / 16 + (data_bytes % 16 != 0)};
    auto window = channels.find(channel);
    if (window == channels.end() || !wire_bytes || wire_bytes < data_bytes ||
        transfers.contains(id) || !covers(window->second.capacity, cost)) {
        return SendResult::Invalid;
    }
    if (now < wireFree) {
        return SendResult::Busy;
    }

    auto &credits = window->second.state;
    if (!covers(credits.available, cost)) {
        return SendResult::NoCredit;
    }

    const auto width = timing.wireBytesPerCycle;
    const uint64_t cycles = wire_bytes / width + (wire_bytes % width != 0);
    if (cycles > MaxTick / timing.cycle) {
        throw std::overflow_error("PCIe link service exceeds Tick range");
    }
    const Tick wire_done = tickAfter(now, cycles * timing.cycle);
    const Tick rx_tick = tickAfter(wire_done, timing.propagation);
    transfers.emplace(id, Transfer{channel, cost, Phase::InFlight});
    moveCredits(credits.available, credits.inFlight, cost);
    wireFree = wire_done;
    schedule(wire_done, [this, wire_done, rx_tick, id] {
        if (departure) {
            departure(wire_done, id);
        }
        if (rx_tick == wire_done) {
            arrive(rx_tick, id);
        }
        ready(wire_done);
    });
    if (rx_tick > wire_done) {
        schedule(rx_tick, [this, rx_tick, id] { arrive(rx_tick, id); });
    }
    return SendResult::Accepted;
}

bool
LinkDirection::release(Tick now, Id id)
{
    auto it = transfers.find(id);
    if (it == transfers.end() || it->second.phase != Phase::Occupied) {
        return false;
    }

    const Tick update = tickAfter(now, timing.creditDelay);
    auto &transfer = it->second;
    auto &credits = channels.at(transfer.channel).state;
    moveCredits(credits.occupied, credits.returning, transfer.cost);
    transfer.phase = Phase::Returning;
    schedule(update, [this, update, id] { restore(update, id); });
    return true;
}

void
LinkDirection::arrive(Tick now, Id id)
{
    auto &transfer = transfers.at(id);
    if (transfer.phase != Phase::InFlight) {
        throw std::logic_error("duplicate PCIe receive event");
    }

    auto &credits = channels.at(transfer.channel).state;
    moveCredits(credits.inFlight, credits.occupied, transfer.cost);
    transfer.phase = Phase::Occupied;
    arrival(now, id);
}

void
LinkDirection::restore(Tick now, Id id)
{
    auto it = transfers.find(id);
    if (it == transfers.end() || it->second.phase != Phase::Returning) {
        throw std::logic_error("unexpected PCIe credit return");
    }

    const auto &transfer = it->second;
    auto &credits = channels.at(transfer.channel).state;
    moveCredits(credits.returning, credits.available, transfer.cost);
    transfers.erase(it);
    ready(now);
}

CreditState
LinkDirection::state(CreditChannel channel) const
{
    return channels.at(channel).state;
}

bool
LinkDirection::empty() const
{
    return transfers.empty();
}

} // namespace gem5::pcie
