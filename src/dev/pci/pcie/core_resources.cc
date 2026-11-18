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

#include "dev/pci/pcie/core_resources.hh"

#include <cassert>
#include <cstddef>
#include <stdexcept>

namespace gem5::pcie
{
namespace
{

constexpr std::array dimensions{&CoreCapacity::descriptors,
                                &CoreCapacity::sectors,
                                &CoreCapacity::activeVoqs};

} // anonymous namespace

CoreResources::CoreResources(CoreCapacity capacity, uint32_t sector_bytes,
                             ClassMinimums minimums)
    : capacity(capacity), sectorBytes(sector_bytes), minimums(minimums)
{
    if (sectorBytes == 0) {
        throw std::invalid_argument("PCIe core sector size must be positive");
    }

    for (auto dimension : dimensions) {
        uint64_t total = 0;
        for (const auto &minimum : minimums) {
            total += minimum.*dimension;
        }
        if (total > capacity.*dimension) {
            throw std::invalid_argument("PCIe core minimums exceed capacity");
        }
    }
}

std::optional<CoreResources::ReservationId>
CoreResources::reserve(VoqKey key, uint32_t payload_bytes)
{
    const auto tlp_class = static_cast<uint8_t>(key.tlpClass);
    if (tlp_class >= minimums.size()) {
        throw std::invalid_argument("Invalid PCIe TLP class");
    }

    const uint32_t sectors =
        payload_bytes / sectorBytes + (payload_bytes % sectorBytes != 0);
    const CoreCapacity demand{1, sectors, !voqRefs.contains(key)};
    for (auto dimension : dimensions) {
        uint32_t protected_free = 0;
        // Preserve the unoccupied part of each other class's minimum.
        for (std::size_t i = 0; i < minimums.size(); ++i) {
            if (i == tlp_class) {
                continue;
            }
            const uint32_t minimum = minimums[i].*dimension;
            const uint32_t usage = classUsage[i].*dimension;
            if (usage < minimum) {
                protected_free += minimum - usage;
            }
        }
        const uint32_t free = capacity.*dimension - used.*dimension;
        assert(free >= protected_free);
        if (demand.*dimension > free - protected_free) {
            return std::nullopt;
        }
    }

    if (nextId == 0) {
        throw std::overflow_error("PCIe core reservation IDs exhausted");
    }
    const auto allocation =
        allocations.emplace(nextId, Allocation{key, sectors}).first;
    try {
        ++voqRefs.try_emplace(key, 0).first->second;
    } catch (...) {
        allocations.erase(allocation);
        throw;
    }

    for (auto dimension : dimensions) {
        used.*dimension += demand.*dimension;
        classUsage[tlp_class].*dimension += demand.*dimension;
    }
    return nextId++;
}

bool
CoreResources::commit(ReservationId id)
{
    auto allocation = allocations.find(id);
    if (allocation == allocations.end() || allocation->second.committed) {
        return false;
    }

    allocation->second.committed = true;
    return true;
}

bool
CoreResources::cancel(ReservationId id)
{
    return retire(id, false);
}

bool
CoreResources::release(ReservationId id)
{
    return retire(id, true);
}

bool
CoreResources::retire(ReservationId id, bool committed)
{
    auto allocation = allocations.find(id);
    if (allocation == allocations.end() ||
        allocation->second.committed != committed) {
        return false;
    }

    const Allocation &entry = allocation->second;
    auto voq = voqRefs.find(entry.key);
    const bool last_reference = --voq->second == 0;
    const CoreCapacity returned{1, entry.sectors, last_reference};
    const auto tlp_class = static_cast<uint8_t>(entry.key.tlpClass);

    for (auto dimension : dimensions) {
        used.*dimension -= returned.*dimension;
        classUsage[tlp_class].*dimension -= returned.*dimension;
    }
    if (last_reference) {
        voqRefs.erase(voq);
    }
    allocations.erase(allocation);
    return true;
}

uint32_t
CoreResources::voqReferences(VoqKey key) const
{
    auto voq = voqRefs.find(key);
    return voq == voqRefs.end() ? 0 : voq->second;
}

} // namespace gem5::pcie
