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
#include <stdexcept>

#include "dev/pci/pcie/flit_packer.hh"

namespace gem5::pcie
{

namespace
{

void
expectBytes(const FlitRecord &record, std::size_t start,
            std::span<const uint8_t> expected)
{
    EXPECT_TRUE(std::equal(expected.begin(), expected.end(),
                           record.bytes.begin() + start));
}

void
expectZeros(const FlitRecord &record, std::size_t start, std::size_t end)
{
    EXPECT_TRUE(std::all_of(record.bytes.begin() + start,
                            record.bytes.begin() + end,
                            [](uint8_t byte) { return byte == 0; }));
}

TEST(FlitPacker, ContiguousTlpsAndIdleAlignment)
{
    FlitPacker packer;
    const std::vector<uint8_t> read(12, 0x03);
    const std::vector<uint8_t> completion(16, 0x4A);
    ASSERT_TRUE(packer.tryPush(read));
    ASSERT_TRUE(packer.tryPush(completion));
    packer.padToAlignment();
    ASSERT_TRUE(packer.tryPush(read));
    EXPECT_FALSE(packer.next());
    packer.flush();

    auto record = packer.next();
    ASSERT_TRUE(record);
    EXPECT_EQ(record->bytes.size(), 256);
    expectBytes(*record, 0, read);
    expectBytes(*record, 12, completion);
    expectZeros(*record, 28, 32);
    expectBytes(*record, 32, read);
    expectZeros(*record, 44, 236);
    expectZeros(*record, 236, 242);
    expectZeros(*record, 242, 250);
    expectZeros(*record, 250, 256);
    packer.flush();
    EXPECT_FALSE(packer.next());
}

TEST(FlitPacker, EnforcesEightPerHalfIncludingContinuation)
{
    FlitPacker packer;
    const std::vector<uint8_t> read(12, 0x03);
    const std::vector<uint8_t> write(56, 0x40);
    for (int i = 0; i < 7; ++i) {
        ASSERT_TRUE(packer.tryPush(read));
    }
    ASSERT_TRUE(packer.tryPush(write));
    for (int i = 0; i < 7; ++i) {
        ASSERT_TRUE(packer.tryPush(read));
    }
    ASSERT_TRUE(packer.tryPush(read));
    EXPECT_FALSE(packer.tryPush(read));

    auto record = packer.next();
    ASSERT_TRUE(record);
    expectBytes(*record, 84, write);
    for (std::size_t offset = 140; offset < 224; offset += 12) {
        expectBytes(*record, offset, read);
    }
    expectZeros(*record, 224, 236);

    packer.flush();
    record = packer.next();
    ASSERT_TRUE(record);
    expectBytes(*record, 0, read);
    expectZeros(*record, 12, 236);
}

TEST(FlitPacker, FirstHalfLimitPreservesNextTlp)
{
    FlitPacker packer;
    const std::vector<uint8_t> read(12, 0x03);
    for (int i = 0; i < 9; ++i) {
        ASSERT_TRUE(packer.tryPush(read));
    }
    packer.flush();

    auto record = packer.next();
    ASSERT_TRUE(record);
    for (std::size_t offset = 0; offset < 96; offset += 12) {
        expectBytes(*record, offset, read);
    }
    expectZeros(*record, 96, 128);
    expectBytes(*record, 128, read);
    expectZeros(*record, 140, 236);
}

TEST(FlitPacker, SplitsAnEncodedTlpAcrossMultipleRecords)
{
    FlitPacker packer;
    const std::vector<uint8_t> read(12, 0x03);
    std::vector<uint8_t> write(488, 0x40);
    for (std::size_t i = 1; i < write.size(); ++i) {
        write[i] = i % 251;
    }

    ASSERT_TRUE(packer.tryPush(read));
    ASSERT_TRUE(packer.tryPush(write));
    packer.flush();
    EXPECT_FALSE(packer.tryPush(read));

    const auto write_bytes = std::span{write};
    auto first = packer.next();
    auto second = packer.next();
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    expectBytes(*first, 0, read);
    expectBytes(*first, 12, write_bytes.first(224));
    expectBytes(*second, 0, write_bytes.subspan(224, 236));
    expectZeros(*first, 236, 256);
    expectZeros(*second, 236, 256);

    ASSERT_TRUE(packer.tryPush(read));
    packer.flush();
    auto last = packer.next();
    ASSERT_TRUE(last);
    expectBytes(*last, 0, write_bytes.subspan(460));
    expectBytes(*last, 28, read);
    expectZeros(*last, 40, 256);
}

TEST(FlitPacker, CountsThePreviousRecordsContinuation)
{
    FlitPacker packer;
    const std::vector<uint8_t> read(12, 0x03);
    const std::vector<uint8_t> write(240, 0x40);
    ASSERT_TRUE(packer.tryPush(write));
    ASSERT_TRUE(packer.next());
    for (int i = 0; i < 8; ++i) {
        ASSERT_TRUE(packer.tryPush(read));
    }
    packer.flush();

    auto record = packer.next();
    ASSERT_TRUE(record);
    expectBytes(*record, 0, std::span{write}.last(4));
    for (std::size_t offset = 4; offset < 88; offset += 12) {
        expectBytes(*record, offset, read);
    }
    expectZeros(*record, 88, 128);
    expectBytes(*record, 128, read);
}

TEST(FlitPacker, StopsIdlePaddingAtTheEndOfTheTlpRegion)
{
    FlitPacker packer;
    const std::vector<uint8_t> write(232, 0x40);
    const std::vector<uint8_t> read(12, 0x03);
    ASSERT_TRUE(packer.tryPush(write));
    packer.padToAlignment();

    auto record = packer.next();
    ASSERT_TRUE(record);
    expectBytes(*record, 0, write);
    expectZeros(*record, 232, 256);

    ASSERT_TRUE(packer.tryPush(read));
    packer.flush();
    record = packer.next();
    ASSERT_TRUE(record);
    expectBytes(*record, 0, read);
}

TEST(FlitPacker, RejectsInvalidInputShape)
{
    FlitPacker packer;
    EXPECT_THROW(packer.tryPush({}), std::invalid_argument);
    EXPECT_THROW(packer.tryPush(std::vector<uint8_t>(12)),
                 std::invalid_argument);
    EXPECT_THROW(packer.tryPush(std::vector<uint8_t>(13, 1)),
                 std::invalid_argument);
    EXPECT_THROW(
        packer.tryPush(std::vector<uint8_t>(FlitPacker::MaxTlpBytes + 4, 1)),
        std::invalid_argument);
    packer.flush();
    EXPECT_FALSE(packer.next());
}

} // anonymous namespace
} // namespace gem5::pcie
