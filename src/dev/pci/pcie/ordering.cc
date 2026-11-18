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

#include "dev/pci/pcie/ordering.hh"

#include <stdexcept>

namespace gem5::pcie
{

Ordering::Id
Ordering::arrived(OrderingDomain domain, TlpClass type,
                  std::optional<CompletionIdentity> completion)
{
    if (domain.trafficClass >= 8 ||
        (type == TlpClass::Completion) != completion.has_value()) {
        throw std::invalid_argument("invalid PCIe ordering identity");
    }
    if (!nextId) {
        throw std::overflow_error("PCIe ordering identifiers exhausted");
    }

    const Id id = nextId++;
    auto &state = domains[domain];
    switch (type) {
        case TlpClass::Posted:
            state.posted.insert(id);
            break;
        case TlpClass::NonPosted:
            state.nonPosted.insert(id);
            break;
        case TlpClass::Completion:
            state.completions[*completion].insert(id);
            break;
    }
    ++state.active;
    pending.emplace(id, Pending{domain, type, completion});
    return id;
}

bool
Ordering::eligible(Id id) const
{
    auto it = pending.find(id);
    if (it == pending.end()) {
        return false;
    }
    const auto &entry = it->second;
    const auto &state = domains.at(entry.domain);
    if (!state.posted.empty() && *state.posted.begin() < id) {
        return false;
    }
    if (entry.type == TlpClass::NonPosted) {
        return id == *state.nonPosted.begin();
    }
    if (entry.type == TlpClass::Completion) {
        return id == *state.completions.at(*entry.completion).begin();
    }
    return true;
}

bool
Ordering::commit(Id id)
{
    return eligible(id) && abort(id);
}

bool
Ordering::abort(Id id)
{
    auto it = pending.find(id);
    if (it == pending.end()) {
        return false;
    }
    const auto &entry = it->second;
    auto domain = domains.find(entry.domain);
    auto &state = domain->second;
    switch (entry.type) {
        case TlpClass::Posted:
            state.posted.erase(id);
            break;
        case TlpClass::NonPosted:
            state.nonPosted.erase(id);
            break;
        case TlpClass::Completion: {
            auto group = state.completions.find(*entry.completion);
            group->second.erase(id);
            if (group->second.empty()) {
                state.completions.erase(group);
            }
            break;
        }
    }
    if (--state.active == 0) {
        domains.erase(domain);
    }
    pending.erase(it);
    return true;
}

bool
Ordering::empty() const
{
    return pending.empty();
}

} // namespace gem5::pcie
