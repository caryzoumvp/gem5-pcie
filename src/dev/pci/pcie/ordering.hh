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

#ifndef __DEV_PCI_PCIE_ORDERING_HH__
#define __DEV_PCI_PCIE_ORDERING_HH__

#include <compare>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>

#include "dev/pci/pcie/tlp.hh"

namespace gem5::pcie
{

struct OrderingDomain
{
    uint32_t ingress;
    uint16_t partition;
    uint8_t trafficClass;

    auto operator<=>(const OrderingDomain &) const = default;
};

struct CompletionIdentity
{
    uint16_t requester;
    uint16_t tag;

    auto operator<=>(const CompletionIdentity &) const = default;
};

/** Conservative RO=0, IDO=0 ordering for ordinary Memory TLPs. */
class Ordering
{
  public:
    using Id = uint64_t;

    Id arrived(OrderingDomain domain, TlpClass type,
               std::optional<CompletionIdentity> completion = std::nullopt);
    bool eligible(Id id, const std::function<bool(Id)> &can_follow = {}) const;
    /** Retire a predecessor after its last byte leaves the serializer. */
    bool commit(Id id);
    /** Remove a TLP that terminated explicitly without transmission. */
    bool abort(Id id);
    bool empty() const;

  private:
    struct Pending
    {
        OrderingDomain domain;
        TlpClass type;
        std::optional<CompletionIdentity> completion;
    };

    struct DomainState
    {
        uint64_t active = 0;
        std::set<Id> posted;
        std::set<Id> nonPosted;
        std::map<CompletionIdentity, std::set<Id>> completions;
    };

    Id nextId = 1;
    std::map<Id, Pending> pending;
    std::map<OrderingDomain, DomainState> domains;
};

} // namespace gem5::pcie

#endif // __DEV_PCI_PCIE_ORDERING_HH__
