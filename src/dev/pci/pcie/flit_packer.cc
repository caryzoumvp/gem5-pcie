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

#include "dev/pci/pcie/flit_packer.hh"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace gem5::pcie
{

namespace
{

constexpr std::size_t DwBytes = 4;
constexpr std::size_t NopAlignment = 4 * DwBytes;
constexpr std::size_t HalfBoundary = 32 * DwBytes;
constexpr unsigned MaxTlpsPerHalf = 8;

} // anonymous namespace

bool
FlitPacker::tryPush(std::span<const uint8_t> tlp)
{
    // Type 0 encodes the 1DW NOP inserted by the packer (Table 2-5).
    if (tlp.size() < 3 * DwBytes || tlp.size() > MaxTlpBytes ||
        tlp.size() % DwBytes != 0 || tlp.front() == 0) {
        throw std::invalid_argument("Invalid TLP byte span for FLIT packing");
    }

    if (cursor == FlitRecord::TlpBytes) {
        return false;
    }

    pending.assign(tlp.begin(), tlp.end());
    packPending();
    return true;
}

void
FlitPacker::packPending()
{
    // Count a continuation once in each half it occupies, including
    // continuations from the preceding FLIT (Section 4.2.3.4.1).
    while (pendingOffset < pending.size() && cursor < FlitRecord::TlpBytes) {
        const bool second_half = cursor >= HalfBoundary;
        const auto half_end =
            second_half ? FlitRecord::TlpBytes : HalfBoundary;
        auto &count = halfTlps[second_half];
        if (count == MaxTlpsPerHalf) {
            cursor = half_end;
            continue;
        }

        ++count;
        const auto count_bytes =
            std::min(half_end - cursor, pending.size() - pendingOffset);
        std::copy_n(pending.begin() + pendingOffset, count_bytes,
                    record.bytes.begin() + cursor);
        pendingOffset += count_bytes;
        cursor += count_bytes;
    }

    if (pendingOffset == pending.size()) {
        if (!pending.empty()) {
            ends.push_back(cursor);
        }
        pending.clear();
        pendingOffset = 0;
    }
}

void
FlitPacker::padToAlignment()
{
    // Unwritten bytes are zero, the complete reserved-field NOP encoding.
    cursor = std::min(FlitRecord::TlpBytes,
                      cursor + NopAlignment - cursor % NopAlignment);
}

void
FlitPacker::flush()
{
    if (cursor != 0) {
        cursor = FlitRecord::TlpBytes;
    }
}

std::optional<FlitRecord>
FlitPacker::next(std::vector<std::size_t> *end_offsets)
{
    if (cursor != FlitRecord::TlpBytes) {
        return std::nullopt;
    }

    auto ready = record;
    if (end_offsets) {
        *end_offsets = std::move(ends);
    }
    ends.clear();
    record = {};
    cursor = 0;
    halfTlps = {};
    packPending();
    return ready;
}

} // namespace gem5::pcie
