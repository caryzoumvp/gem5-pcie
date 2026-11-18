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

#include "dev/pci/pcie/request.hh"

#include <algorithm>
#include <bit>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace gem5::pcie
{
namespace
{

constexpr uint32_t PageBytes = 4096;

bool
validSizeLimit(uint32_t size)
{
    return size >= 128 && size <= PageBytes && std::has_single_bit(size);
}

bool
validRange(uint64_t address, uint64_t bytes)
{
    return bytes && address % 4 == 0 && bytes % 4 == 0 &&
           bytes - 1 <= std::numeric_limits<uint64_t>::max() - address;
}

uint32_t
pageRemaining(uint64_t address)
{
    return PageBytes - address % PageBytes;
}

} // anonymous namespace

std::vector<ByteRange>
splitRequests(Op op, uint64_t address, uint64_t bytes, uint32_t mps,
              uint32_t mrrs)
{
    if (!validRange(address, bytes) || !validSizeLimit(mps) ||
        !validSizeLimit(mrrs)) {
        throw std::invalid_argument(
            "invalid PCIe transfer range or size limit");
    }

    const uint32_t limit = op == Op::Read ? mrrs : mps;
    std::vector<ByteRange> ranges;
    while (bytes) {
        const uint32_t length =
            std::min<uint64_t>(std::min(limit, pageRemaining(address)), bytes);
        ranges.push_back({address, length});
        bytes -= length;
        if (bytes) {
            address += length;
        }
    }
    return ranges;
}

bool
validRcb(uint16_t rcb, bool root_complex)
{
    return rcb == 128 || (rcb == 64 && root_complex);
}

std::vector<ByteRange>
splitCompletions(ByteRange read, uint32_t mps, uint16_t rcb)
{
    auto [address, remaining] = read;
    if (!validRange(address, remaining) ||
        remaining > pageRemaining(address) || !validSizeLimit(mps) ||
        !validRcb(rcb, true)) {
        throw std::invalid_argument("invalid PCIe Completion range or limits");
    }
    std::vector<ByteRange> ranges;
    while (remaining > mps) {
        // Every non-final Completion must end at a Requester RCB.
        const uint32_t length = mps - (address + mps) % rcb;
        ranges.push_back({address, length});
        address += length;
        remaining -= length;
    }
    ranges.push_back({address, remaining});
    return ranges;
}

bool
ReadKey::operator<(const ReadKey &other) const
{
    return std::tie(requester, tag) < std::tie(other.requester, other.tag);
}

ReadTracker::ReadTracker(uint32_t mps) : mps(mps)
{
    if (!validSizeLimit(mps)) {
        throw std::invalid_argument("invalid PCIe MPS");
    }
}

void
ReadTracker::configure(uint16_t requester, FunctionLimits limits)
{
    if (!limits.outstandingTags || !limits.completionBytes ||
        !validRcb(limits.rcb, limits.rootComplex)) {
        throw std::invalid_argument("invalid PCIe Requester limits");
    }
    if (!functions.try_emplace(requester, Function{limits}).second) {
        throw std::invalid_argument("PCIe Requester already configured");
    }
}

bool
ReadTracker::begin(ReadKey key, ByteRange range, uint8_t traffic_class)
{
    auto it = functions.find(key.requester);
    if (it == functions.end() || !validRange(range.address, range.bytes) ||
        range.bytes > pageRemaining(range.address) || key.tag >= (1 << 14) ||
        traffic_class >= 8 || outstanding(key)) {
        return false;
    }
    auto &function = it->second;
    if (function.activeTags == function.limits.outstandingTags ||
        range.bytes > function.limits.completionBytes - function.reserved) {
        return false;
    }

    Pending read{range, traffic_class, {}};
    read.payload.reserve(range.bytes);
    pending.emplace(key, std::move(read));
    ++function.activeTags;
    function.reserved += range.bytes;
    return true;
}

ReadResult
ReadTracker::receive(const Completion &completion)
{
    auto it = pending.find(completion.key);
    if (it == pending.end()) {
        return {Receipt::Unexpected, {}};
    }

    auto &read = it->second;
    auto &function = functions.at(completion.key.requester);
    const size_t received = read.payload.size();
    const size_t remaining = read.range.bytes - received;
    const auto address = read.range.address + received;
    if (completion.trafficClass != read.trafficClass ||
        completion.byteCount != remaining ||
        completion.lowerAddress != (address & 0x7f)) {
        return {Receipt::Malformed, {}};
    }

    ReadResult result{Receipt::Failed, {}};
    const size_t length = completion.payload.size();
    switch (completion.status) {
        case CompletionStatus::Success:
            if (!length || length % 4 != 0 || length > remaining ||
                length > mps ||
                (length < remaining &&
                 (address + length) % function.limits.rcb != 0)) {
                return {Receipt::Malformed, {}};
            }
            read.payload.insert(read.payload.end(), completion.payload.begin(),
                                completion.payload.end());
            if (length < remaining) {
                return {Receipt::Partial, {}};
            }
            result = {Receipt::Complete, std::move(read.payload)};
            break;
        case CompletionStatus::UnsupportedRequest:
        case CompletionStatus::CompleterAbort:
            if (length) {
                return {Receipt::Malformed, {}};
            }
            break;
        default:
            return {Receipt::Malformed, {}};
    }

    --function.activeTags;
    function.reserved -= read.range.bytes;
    pending.erase(it);
    return result;
}

bool
ReadTracker::outstanding(ReadKey key) const
{
    return pending.contains(key);
}

uint64_t
ReadTracker::bytesReserved(uint16_t requester) const
{
    const auto it = functions.find(requester);
    return it == functions.end() ? 0 : it->second.reserved;
}

} // namespace gem5::pcie
