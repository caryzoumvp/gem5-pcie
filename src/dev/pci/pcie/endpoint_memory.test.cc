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

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

#include "dev/pci/pcie/endpoint_memory.hh"

namespace gem5::pcie
{
namespace
{

TEST(PcieEndpointMemory, WriteCommitAndReadReassembly)
{
    EndpointMemory endpoint(0x1000, 4096, 256);
    const ByteRange read{0x1020, 512};
    const ReadKey key{0x1234, 0x315};
    const auto before = endpoint.completeRead(key, read, 3, 128);
    ASSERT_EQ(before.size(), 3);
    EXPECT_EQ(before.front().payload, std::vector<uint8_t>(224, 0));

    std::vector<uint8_t> data(512);
    for (std::size_t i = 0; i < data.size(); ++i) {
        data[i] = (i * 3 + 1) % 251;
    }
    const std::span<const uint8_t> payload(data);
    ASSERT_TRUE(endpoint.commitWrite(0x1020, payload.first(256)));
    ASSERT_TRUE(endpoint.commitWrite(0x1120, payload.last(256)));

    const auto response = endpoint.completeRead(key, read, 3, 128);
    ASSERT_EQ(response.size(), 3);
    EXPECT_EQ(response[0].payload.size(), 224);
    EXPECT_EQ(response[1].payload.size(), 256);
    EXPECT_EQ(response[2].payload.size(), 32);
    EXPECT_EQ(response[0].byteCount, 512);
    EXPECT_EQ(response[1].byteCount, 288);
    EXPECT_EQ(response[2].byteCount, 32);
    EXPECT_EQ(response[0].lowerAddress, 0x20);
    EXPECT_EQ(response[1].lowerAddress, 0);
    EXPECT_EQ(response[2].lowerAddress, 0);

    ReadTracker requester(256);
    requester.configure(key.requester, {1, 512, 128, false});
    ASSERT_TRUE(requester.begin(key, read, 3));
    for (const auto &completion : response) {
        EXPECT_EQ(completion.key.requester, key.requester);
        EXPECT_EQ(completion.key.tag, key.tag);
        EXPECT_EQ(completion.trafficClass, 3);
        EXPECT_EQ(completion.status, CompletionStatus::Success);
    }
    EXPECT_EQ(requester.receive(response[0]).disposition, Receipt::Partial);
    EXPECT_EQ(requester.receive(response[1]).disposition, Receipt::Partial);
    const auto result = requester.receive(response[2]);
    EXPECT_EQ(result.disposition, Receipt::Complete);
    EXPECT_EQ(result.payload, data);
    EXPECT_EQ(before.front().payload, std::vector<uint8_t>(224, 0));

    const ByteRange rc_read{0x1060, 512};
    const auto rc_response = endpoint.completeRead(key, rc_read, 3, 64);
    ASSERT_EQ(rc_response.size(), 3);
    EXPECT_EQ(rc_response.front().payload.size(), 224);
    EXPECT_EQ(rc_response[1].lowerAddress, 0x40);
    EXPECT_EQ(rc_response[2].lowerAddress, 0x40);
    const auto ep_response = endpoint.completeRead(key, rc_read, 3, 128);
    ASSERT_EQ(ep_response.size(), 3);
    EXPECT_EQ(ep_response.front().payload.size(), 160);
}

TEST(PcieEndpointMemory, OutsideWindow)
{
    EndpointMemory endpoint(0x1010, 16, 128);
    const std::vector<uint8_t> payload{1, 2, 3, 4, 5, 6, 7, 8};
    EXPECT_FALSE(endpoint.commitWrite(0x100c, payload));
    EXPECT_FALSE(endpoint.commitWrite(0x101c, payload));
    const ReadKey key{0xabcd, 2};
    const auto response = endpoint.completeRead(key, {0x101c, 8}, 7, 128);
    ASSERT_EQ(response.size(), 1);
    const auto &completion = response.front();
    EXPECT_EQ(completion.key.requester, key.requester);
    EXPECT_EQ(completion.key.tag, key.tag);
    EXPECT_EQ(completion.trafficClass, 7);
    EXPECT_EQ(completion.status, CompletionStatus::UnsupportedRequest);
    EXPECT_TRUE(completion.payload.empty());
    ReadTracker requester(128);
    requester.configure(key.requester, {1, 8, 128, false});
    ASSERT_TRUE(requester.begin(key, {0x101c, 8}, 7));
    EXPECT_EQ(requester.receive(completion).disposition, Receipt::Failed);
    EXPECT_FALSE(requester.outstanding(key));

    const auto unchanged = endpoint.completeRead(key, {0x1010, 16}, 7, 128);
    ASSERT_EQ(unchanged.size(), 1);
    EXPECT_EQ(unchanged.front().payload, std::vector<uint8_t>(16, 0));
}

TEST(PcieEndpointMemory, FinalAddress)
{
    constexpr auto last = std::numeric_limits<uint64_t>::max();
    EndpointMemory endpoint(last - 4095, 4096, 4096);
    const std::vector<uint8_t> payload(4096, 0x5a);
    EXPECT_TRUE(endpoint.commitWrite(last - 4095, payload));
    const auto response =
        endpoint.completeRead({1, 0}, {last - 4095, 4096}, 0, 128);
    ASSERT_EQ(response.size(), 1);
    EXPECT_EQ(response.front().byteCount, 4096);
    EXPECT_EQ(response.front().payload, payload);
}

TEST(PcieEndpointMemory, MalformedCalls)
{
    constexpr auto last = std::numeric_limits<uint64_t>::max();
    EXPECT_THROW(EndpointMemory(0, 0, 128), std::invalid_argument);
    EXPECT_THROW(EndpointMemory(last, 2, 128), std::invalid_argument);
    EXPECT_THROW(EndpointMemory(0, 4096, 192), std::invalid_argument);

    EndpointMemory endpoint(0, 8192, 128);
    const std::vector<uint8_t> payload(8, 0x5a);
    EXPECT_THROW(endpoint.commitWrite(0, {}), std::invalid_argument);
    EXPECT_THROW(endpoint.commitWrite(1, payload), std::invalid_argument);
    EXPECT_THROW(endpoint.commitWrite(4092, payload), std::invalid_argument);
    EXPECT_THROW(endpoint.commitWrite(last - 3, payload),
                 std::invalid_argument);
    const std::vector<uint8_t> oversized(132, 0x5a);
    EXPECT_THROW(endpoint.commitWrite(0, oversized), std::invalid_argument);

    const ReadKey key{1, 0};
    EXPECT_THROW(endpoint.completeRead(key, {0, 0}, 0, 128),
                 std::invalid_argument);
    EXPECT_THROW(endpoint.completeRead(key, {0, 3}, 0, 128),
                 std::invalid_argument);
    EXPECT_THROW(endpoint.completeRead(key, {4092, 8}, 0, 128),
                 std::invalid_argument);
    EXPECT_THROW(endpoint.completeRead(key, {0, 4}, 8, 128),
                 std::invalid_argument);
    EXPECT_THROW(endpoint.completeRead(key, {0, 4}, 0, 32),
                 std::invalid_argument);
    EXPECT_THROW(endpoint.completeRead({1, 16384}, {0, 4}, 0, 128),
                 std::invalid_argument);
}

} // anonymous namespace
} // namespace gem5::pcie
