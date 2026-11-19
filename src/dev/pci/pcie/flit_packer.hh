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

#ifndef __DEV_PCI_PCIE_FLIT_PACKER_HH__
#define __DEV_PCI_PCIE_FLIT_PACKER_HH__

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace gem5::pcie
{

struct FlitRecord
{
    static constexpr std::size_t TlpBytes = 236;
    static constexpr std::size_t DlpOffset = TlpBytes;
    static constexpr std::size_t DlpBytes = 6;
    static constexpr std::size_t CrcOffset = DlpOffset + DlpBytes;
    static constexpr std::size_t CrcBytes = 8;
    static constexpr std::size_t FecOffset = CrcOffset + CrcBytes;
    static constexpr std::size_t FecBytes = 6;

    std::array<uint8_t, FecOffset + FecBytes> bytes{};
};

class FlitPacker
{
  public:
    // Bound for a 4DW base header, up to 7DW OHC and 1024DW data.
    static constexpr std::size_t MaxTlpBytes = (4 + 7 + 1024) * 4;

    // Accept one prevalidated non-NOP TLP. A full output stalls input.
    // The caller owns validation of the header, length, and supported types.
    bool tryPush(std::span<const uint8_t> tlp);
    bool
    accepting() const
    {
        return cursor < FlitRecord::TlpBytes;
    }
    bool
    empty() const
    {
        return cursor == 0 && pending.empty();
    }

    // An idle scheduling step emits NOPs through the next 4DW boundary.
    void padToAlignment();

    // Complete a partially populated TLP region using NOPs.
    void flush();

    // Optionally report each completed TLP's exclusive end offset in this
    // record. These are simulator metadata, outside the 256B wire record.
    std::optional<FlitRecord>
    next(std::vector<std::size_t> *end_offsets = nullptr);

  private:
    void packPending();

    FlitRecord record;
    std::size_t cursor = 0;
    std::array<unsigned, 2> halfTlps{};
    std::vector<uint8_t> pending;
    std::vector<std::size_t> ends;
    std::size_t pendingOffset = 0;
};

} // namespace gem5::pcie

#endif // __DEV_PCI_PCIE_FLIT_PACKER_HH__
