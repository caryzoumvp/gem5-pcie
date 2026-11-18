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

#include "dev/pci/pcie/endpoint_memory.hh"

#include <algorithm>
#include <bit>
#include <limits>
#include <stdexcept>

namespace gem5::pcie
{

EndpointMemory::EndpointMemory(uint64_t base, std::size_t bytes, uint32_t mps)
    : base(base), mps(mps)
{
    if (!bytes || bytes - 1 > std::numeric_limits<uint64_t>::max() - base ||
        mps < 128 || mps > 4096 || !std::has_single_bit(mps)) {
        throw std::invalid_argument("invalid PCIe memory window or MPS");
    }
    storage.resize(bytes);
}

bool
EndpointMemory::contains(uint64_t address, std::size_t bytes) const
{
    return address >= base && address - base < storage.size() &&
           bytes <= storage.size() - (address - base);
}

bool
EndpointMemory::commitWrite(uint64_t address, std::span<const uint8_t> payload)
{
    if (payload.empty() || payload.size() > mps || address % 4 != 0 ||
        payload.size() % 4 != 0 || payload.size() > 4096 - address % 4096) {
        throw std::invalid_argument("invalid PCIe Memory Write range");
    }
    if (!contains(address, payload.size())) {
        return false;
    }

    auto *destination = storage.data() + (address - base);
    std::copy(payload.begin(), payload.end(), destination);
    return true;
}

std::vector<Completion>
EndpointMemory::completeRead(ReadKey key, ByteRange range,
                             uint8_t traffic_class,
                             uint16_t requester_rcb) const
{
    if (key.tag >= (1 << 14) || traffic_class >= 8) {
        throw std::invalid_argument("invalid PCIe Memory Read identity or TC");
    }

    const auto fragments = splitCompletions(range, mps, requester_rcb);
    uint16_t remaining = range.bytes;
    if (!contains(range.address, range.bytes)) {
        return {{key,
                 remaining,
                 static_cast<uint8_t>(range.address & 0x7f),
                 CompletionStatus::UnsupportedRequest,
                 {},
                 traffic_class}};
    }

    std::vector<Completion> response;
    response.reserve(fragments.size());
    for (const auto &fragment : fragments) {
        const auto *begin = storage.data() + (fragment.address - base);
        response.push_back(
            {key, remaining, static_cast<uint8_t>(fragment.address & 0x7f),
             CompletionStatus::Success,
             std::vector<uint8_t>(begin, begin + fragment.bytes),
             traffic_class});
        remaining -= fragment.bytes;
    }
    return response;
}

} // namespace gem5::pcie
