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

#include "dev/pci/pcie/flit_integrity.hh"

#include <algorithm>
#include <array>

namespace gem5::pcie
{
namespace
{

using Checksum = std::array<uint8_t, FlitRecord::CrcBytes>;

constexpr uint8_t
multiply(uint8_t x, uint8_t y)
{
    uint8_t result = 0;
    while (y) {
        if (y & 1) {
            result ^= x;
        }
        const bool high = x & 0x80;
        x <<= 1;
        if (high) {
            x ^= 0x2b; // x^8 + x^5 + x^3 + x + 1.
        }
        y >>= 1;
    }
    return result;
}

constexpr auto
divisionTable()
{
    // g(x) coefficient order is low to high (the x^8 coefficient is 1).
    constexpr Checksum generator{0x69, 0x4d, 0x41, 0x33,
                                 0xd5, 0xfe, 0x68, 0xd5};
    std::array<Checksum, 256> table{};
    for (size_t i = 0; i < table.size(); ++i) {
        for (size_t j = 0; j < generator.size(); ++j) {
            table[i][j] = multiply(i, generator[j]);
        }
    }
    return table;
}

Checksum
compute(const FlitRecord &record)
{
    static constexpr auto table = divisionTable();
    Checksum remainder{};
    for (size_t i = 0; i < FlitRecord::CrcOffset; ++i) {
        const auto &row = table[record.bytes[i] ^ remainder.back()];
        for (size_t j = remainder.size() - 1; j > 0; --j) {
            remainder[j] = remainder[j - 1] ^ row[j];
        }
        remainder[0] = row[0];
    }
    return remainder;
}

} // anonymous namespace

void
stampFlitCrc(FlitRecord &record)
{
    const auto remainder = compute(record);
    std::copy(remainder.begin(), remainder.end(),
              record.bytes.begin() + FlitRecord::CrcOffset);
}

bool
validFlitCrc(const FlitRecord &record)
{
    const auto remainder = compute(record);
    return std::equal(remainder.begin(), remainder.end(),
                      record.bytes.begin() + FlitRecord::CrcOffset);
}

} // namespace gem5::pcie
