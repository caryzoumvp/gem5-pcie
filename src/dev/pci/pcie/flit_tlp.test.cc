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
#include <stdexcept>

#include "dev/pci/pcie/flit_packer.hh"
#include "dev/pci/pcie/flit_tlp.hh"

namespace gem5::pcie
{
namespace
{

TEST(PcieFlitTlp, ReadsFourKibWithZeroLengthAndMem64Address)
{
    const std::vector<uint8_t> wire{0x20, 0x60, 0x00, 0x00, 0x12, 0x34,
                                    0x01, 0x23, 0x11, 0x22, 0x33, 0x44,
                                    0x55, 0x66, 0x70, 0x00};
    const FlitMemoryRequest read{
        Op::Read, {0x1234, 0x123}, {0x1122334455667000, 4096}, 3, {}};
    ASSERT_EQ(encodeFlitTlp(read), wire);
    EXPECT_EQ(flitTlpSize(std::span<const uint8_t, 4>(wire.data(), 4)), 16);
    const auto decoded = std::get<FlitMemoryRequest>(decodeFlitTlp(wire));
    EXPECT_EQ(decoded.key.requester, 0x1234);
    EXPECT_EQ(decoded.key.tag, 0x123);
    EXPECT_EQ(decoded.trafficClass, 3);
    EXPECT_EQ(decoded.range, read.range);
    EXPECT_EQ(decoded.op, Op::Read);
    EXPECT_TRUE(decoded.payload.empty());
}

TEST(PcieFlitTlp, WritesMem32WithFullDwPayload)
{
    std::vector<uint8_t> wire{0x40, 0, 0, 0x10, 0xbe, 0xef,
                              0,    0, 0, 0,    0x10, 0};
    std::vector<uint8_t> payload;
    for (unsigned i = 0; i < 64; ++i) {
        payload.push_back(i);
    }
    wire.insert(wire.end(), payload.begin(), payload.end());
    const FlitMemoryRequest tx{
        Op::Write, {0xbeef, 0}, {0x1000, 64}, 0, payload};
    EXPECT_EQ(encodeFlitTlp(tx), wire);
    const auto decoded = std::get<FlitMemoryRequest>(decodeFlitTlp(wire));
    EXPECT_EQ(decoded.range, tx.range);
    EXPECT_EQ(decoded.op, Op::Write);
    EXPECT_EQ(decoded.payload, payload);
}

TEST(PcieFlitTlp, CompletionCopiesSplitAddressAndRemainingByteCount)
{
    std::vector<uint8_t> wire{0x4a, 0,    0,    0x40, 0,    0x20,
                              0x41, 0x23, 0x12, 0x34, 0x92, 0};
    wire.resize(12 + 256, 0xa5);
    const FlitCompletion tx{0x20,
                            {{0x1234, 0x123},
                             512,
                             0x64,
                             CompletionStatus::Success,
                             std::vector<uint8_t>(256, 0xa5)}};
    EXPECT_EQ(encodeFlitTlp(tx), wire);
    const auto decoded = std::get<FlitCompletion>(decodeFlitTlp(wire));
    EXPECT_EQ(decoded.completer, tx.completer);
    EXPECT_EQ(decoded.completion.lowerAddress, 0x64);
    EXPECT_EQ(decoded.completion.byteCount, 512);
    EXPECT_EQ(decoded.completion.key.tag, 0x123);
    EXPECT_EQ(decoded.completion.payload, tx.completion.payload);
}

TEST(PcieFlitTlp, UnsupportedCompletionCarriesOhcA5)
{
    const std::vector<uint8_t> wire{0x0a, 0x01, 0,    0,    0,    0x20,
                                    0x01, 0x23, 0x12, 0x34, 0x01, 0xa0,
                                    0,    0,    0,    0x01};
    const FlitCompletion tx{
        0x20,
        {{0x1234, 0x123}, 416, 0, CompletionStatus::UnsupportedRequest, {}}};
    EXPECT_EQ(encodeFlitTlp(tx), wire);
    const auto result = std::get<FlitCompletion>(decodeFlitTlp(wire));
    EXPECT_EQ(result.completion.byteCount, 416);
    EXPECT_EQ(result.completion.status, CompletionStatus::UnsupportedRequest);
    EXPECT_TRUE(result.completion.payload.empty());
}

TEST(PcieFlitTlp, RejectsUnsupportedOhcAndTrailingBytes)
{
    const std::array<uint8_t, 4> pasid{0x03, 0x01, 0, 1};
    EXPECT_THROW(flitTlpSize(pasid), std::invalid_argument);
    std::vector<uint8_t> trailing{0x03, 0, 0,    1, 0, 0, 0, 1,
                                  0,    0, 0x10, 0, 0, 0, 0, 0};
    EXPECT_THROW(decodeFlitTlp(trailing), std::invalid_argument);
}

TEST(PcieFlitTlp, AssemblesAcrossFlitBoundaryWithoutPadding)
{
    std::vector<uint8_t> data(240);
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = i;
    }
    const std::vector<uint8_t> read{0x03, 0, 0, 1, 0, 1, 0, 7, 0, 0, 0x20, 0};
    std::vector<uint8_t> stream{0x40, 0, 0, 60, 0, 1, 0, 0, 0, 0, 0x10, 0};
    stream.insert(stream.end(), data.begin(), data.end());
    stream.insert(stream.end(), read.begin(), read.end());
    std::array<uint8_t, FlitTlpAssembler::PayloadBytes> first{}, second{};
    std::copy_n(stream.begin(), first.size(), first.begin());
    std::copy(stream.begin() + first.size(), stream.end(), second.begin());

    FlitTlpAssembler assembler;
    EXPECT_TRUE(assembler.receive(first).empty());
    EXPECT_FALSE(assembler.empty());
    const auto packets = assembler.receive(second);
    ASSERT_EQ(packets.size(), 2);
    EXPECT_EQ(std::get<FlitMemoryRequest>(packets[0]).payload, data);
    EXPECT_EQ(std::get<FlitMemoryRequest>(packets[1]).key.tag, 7);
    EXPECT_TRUE(assembler.empty());
}

TEST(PcieFlitTlp, ChecksHalfLimitAndNopAlignment)
{
    const std::array<uint8_t, 12> read{0x03, 0, 0, 1, 0,    1,
                                       0,    7, 0, 0, 0x20, 0};
    std::array<uint8_t, FlitTlpAssembler::PayloadBytes> area{};
    FlitTlpAssembler assembler;
    for (size_t i = 0; i < 9; ++i) {
        std::copy(read.begin(), read.end(), area.begin() + i * read.size());
    }
    EXPECT_THROW(assembler.receive(area), std::invalid_argument);
    EXPECT_TRUE(assembler.empty());

    area.fill(0);
    std::copy(read.begin(), read.end(), area.begin() + 12);
    EXPECT_THROW(assembler.receive(area), std::invalid_argument);
    area.fill(0);
    std::copy(read.begin(), read.end(), area.begin() + 16);
    EXPECT_EQ(assembler.receive(area).size(), 1);
}

TEST(PcieFlitTlp, PackedWriteAndReadRoundTrip)
{
    const std::vector<uint8_t> data(256, 0xd2);
    const auto write = encodeFlitTlp(
        FlitMemoryRequest{Op::Write, {0x140, 0}, {0x1000, 256}, 0, data});
    const auto read = encodeFlitTlp(
        FlitMemoryRequest{Op::Read, {0x141, 9}, {0x1000, 256}, 0, {}});
    FlitPacker packer;
    FlitTlpAssembler parser;
    std::vector<size_t> ended;
    ASSERT_TRUE(packer.tryPush(write));
    EXPECT_FALSE(packer.tryPush(read));
    const auto first = packer.next(&ended);
    ASSERT_TRUE(first);
    EXPECT_TRUE(ended.empty());
    EXPECT_TRUE(
        parser.receive(std::span(first->bytes).first<FlitRecord::TlpBytes>())
            .empty());
    ASSERT_TRUE(packer.tryPush(read));
    packer.flush();
    const auto second = packer.next(&ended);
    ASSERT_TRUE(second);
    EXPECT_EQ(ended, (std::vector<size_t>{32, 44}));
    const auto packets =
        parser.receive(std::span(second->bytes).first<FlitRecord::TlpBytes>());
    ASSERT_EQ(packets.size(), 2);
    EXPECT_EQ(std::get<FlitMemoryRequest>(packets[0]).payload, data);
    EXPECT_EQ(std::get<FlitMemoryRequest>(packets[1]).key.tag, 9);
    EXPECT_TRUE(parser.empty());
    EXPECT_FALSE(packer.next());
}

} // anonymous namespace
} // namespace gem5::pcie
