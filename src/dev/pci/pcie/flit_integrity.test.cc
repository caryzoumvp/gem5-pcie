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

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <span>

#include "dev/pci/pcie/flit_integrity.hh"

namespace gem5::pcie
{
namespace
{

void
expectCrc(const FlitRecord &flit, const std::array<uint8_t, 8> &expected)
{
    const auto crc = std::span(flit.bytes).subspan<FlitRecord::CrcOffset, 8>();
    EXPECT_TRUE(std::equal(crc.begin(), crc.end(), expected.begin()));
}

TEST(PcieFlitIntegrity, MatchesIndependentBasisVectors)
{
    FlitRecord flit;
    flit.bytes[0] = 1;
    stampFlitCrc(flit);
    // Appendix K.2 basis for bit 0 of byte 0 and of byte 241 (DLP).
    expectCrc(flit, {0x0b, 0x3b, 0xc3, 0x1a, 0xe9, 0xa7, 0xb9, 0x61});
    EXPECT_TRUE(validFlitCrc(flit));

    flit.bytes[0] = 0;
    flit.bytes[241] = 1;
    stampFlitCrc(flit);
    expectCrc(flit, {0x69, 0x4d, 0x41, 0x33, 0xd5, 0xfe, 0x68, 0xd5});
    EXPECT_TRUE(validFlitCrc(flit));
}

TEST(PcieFlitIntegrity, ChecksTlpDlpAndCrcWithoutTouchingFec)
{
    FlitRecord flit;
    flit.bytes[1] = 0x55;
    flit.bytes[236] = 0x40;
    flit.bytes[250] = 0xab;
    stampFlitCrc(flit);
    EXPECT_EQ(flit.bytes[250], 0xab);
    EXPECT_TRUE(validFlitCrc(flit));
    for (size_t offset : {1, 236, 249}) {
        auto corrupt = flit;
        corrupt.bytes[offset] ^= 0x80;
        EXPECT_FALSE(validFlitCrc(corrupt));
    }
}

} // anonymous namespace
} // namespace gem5::pcie
