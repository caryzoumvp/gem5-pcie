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

#ifndef __DEV_PCI_PCIE_CORE_RESOURCES_HH__
#define __DEV_PCI_PCIE_CORE_RESOURCES_HH__

#include <array>
#include <compare>
#include <cstdint>
#include <map>
#include <optional>

#include "dev/pci/pcie/tlp.hh"

namespace gem5::pcie
{

struct VoqKey
{
    uint32_t egress;
    uint8_t vc;
    TlpClass tlpClass;
    uint32_t orderingDomain;

    auto operator<=>(const VoqKey &) const = default;
};

struct CoreCapacity
{
    uint32_t descriptors = 0;
    uint32_t sectors = 0;
    uint32_t activeVoqs = 0;

    bool operator==(const CoreCapacity &) const = default;
};

/** Atomic core admission ledger for one ingress port. */
class CoreResources
{
  public:
    using ReservationId = uint64_t;
    // Indexed in TlpClass declaration order.
    using ClassMinimums = std::array<CoreCapacity, 3>;

    CoreResources(CoreCapacity capacity, uint32_t sector_bytes,
                  ClassMinimums minimums);
    CoreResources(const CoreResources &) = delete;
    CoreResources &operator=(const CoreResources &) = delete;

    /**
     * A null result indicates temporary exhaustion and leaves the ledger
     * unchanged. Identifiers are scoped to their originating ledger.
     */
    std::optional<ReservationId> reserve(VoqKey key, uint32_t payload_bytes);

    /** Commit core storage at the start of the RX-to-core copy. */
    bool commit(ReservationId id);

    /** Return resources for a reservation that never committed. */
    bool cancel(ReservationId id);

    /**
     * Return committed resources after the caller retires the descriptor and
     * all its references, including any in-flight grant.
     */
    bool release(ReservationId id);

    /** Combined reserved and committed usage. */
    CoreCapacity
    occupied() const
    {
        return used;
    }
    uint32_t voqReferences(VoqKey key) const;

  private:
    struct Allocation
    {
        VoqKey key;
        uint32_t sectors;
        bool committed = false;
    };

    bool retire(ReservationId id, bool committed);

    const CoreCapacity capacity;
    const uint32_t sectorBytes;
    const ClassMinimums minimums;
    std::array<CoreCapacity, 3> classUsage{};
    CoreCapacity used;
    ReservationId nextId = 1;
    std::map<ReservationId, Allocation> allocations;
    std::map<VoqKey, uint32_t> voqRefs;
};

} // namespace gem5::pcie

#endif // __DEV_PCI_PCIE_CORE_RESOURCES_HH__
