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
#include <vector>

#include "dev/pci/pcie/request.hh"

namespace gem5::pcie
{
namespace
{

TEST(PcieRequest, SplitsReadByPageAndMrrs)
{
    EXPECT_EQ(splitRequests(Op::Read, 0x1ffc, 4096, 256, 4096),
              (std::vector<ByteRange>{{0x1ffc, 4}, {0x2000, 4092}}));
    EXPECT_EQ(splitRequests(Op::Read, 0x1000, 1024, 256, 512),
              (std::vector<ByteRange>{{0x1000, 512}, {0x1200, 512}}));
}

TEST(PcieRequest, SplitsPostedWriteByPageAndMps)
{
    EXPECT_EQ(
        splitRequests(Op::Write, 0x1ffc, 516, 256, 4096),
        (std::vector<ByteRange>{{0x1ffc, 4}, {0x2000, 256}, {0x2100, 256}}));
}

TEST(PcieRequest, CompleterFillsMpsWithoutBreakingRcb)
{
    EXPECT_EQ(splitCompletions({0x1000, 512}, 256, 128),
              (std::vector<ByteRange>{{0x1000, 256}, {0x1100, 256}}));
    EXPECT_EQ(
        splitCompletions({0x1020, 512}, 256, 128),
        (std::vector<ByteRange>{{0x1020, 224}, {0x1100, 256}, {0x1200, 32}}));
    EXPECT_EQ(splitCompletions({0x107c, 4}, 256, 128),
              (std::vector<ByteRange>{{0x107c, 4}}));
}

std::vector<uint8_t>
pattern(size_t offset, size_t count)
{
    std::vector<uint8_t> result;
    for (size_t i = offset; i < offset + count; ++i) {
        result.push_back(static_cast<uint8_t>(i));
    }
    return result;
}

TEST(PcieRequest, AlignedReadCompletesAtSecondFragment)
{
    ReadTracker tracker(256);
    tracker.configure(0x100, {1, 512, 128, false});
    const ReadKey key{0x100, 7};

    ASSERT_TRUE(tracker.begin(key, {0x1000, 512}, 0));
    EXPECT_EQ(
        tracker
            .receive({key, 512, 0, CompletionStatus::Success, pattern(0, 256)})
            .disposition,
        Receipt::Partial);
    EXPECT_TRUE(tracker.outstanding(key));
    const auto last = tracker.receive(
        {key, 256, 0, CompletionStatus::Success, pattern(256, 256)});
    EXPECT_EQ(last.disposition, Receipt::Complete);
    EXPECT_EQ(last.payload, pattern(0, 512));
    EXPECT_FALSE(tracker.outstanding(key));
}

TEST(PcieRequest, ThreeCompletionsHoldTagAndReservationUntilLast)
{
    ReadTracker tracker(256);
    tracker.configure(0x100, {1, 512, 128, false});
    const ReadKey key{0x100, 7};

    ASSERT_TRUE(tracker.begin(key, {0x1020, 512}, 0));
    ASSERT_FALSE(tracker.begin({0x100, 8}, {0x1800, 4}, 0));

    auto result = tracker.receive(
        {key, 512, 0x20, CompletionStatus::Success, pattern(0, 96)});
    EXPECT_EQ(result.disposition, Receipt::Partial);
    EXPECT_EQ(tracker.bytesReserved(0x100), 512);
    EXPECT_TRUE(tracker.outstanding(key));

    result = tracker.receive(
        {key, 416, 0, CompletionStatus::Success, pattern(96, 256)});
    EXPECT_EQ(result.disposition, Receipt::Partial);
    EXPECT_TRUE(tracker.outstanding(key));

    result = tracker.receive(
        {key, 160, 0, CompletionStatus::Success, pattern(352, 160)});
    EXPECT_EQ(result.disposition, Receipt::Complete);
    EXPECT_EQ(result.payload, pattern(0, 512));
    EXPECT_FALSE(tracker.outstanding(key));
    EXPECT_EQ(tracker.bytesReserved(0x100), 0);
}

TEST(PcieRequest, MalformedCompletionLeavesProgressAndTerminalErrorRetires)
{
    ReadTracker tracker(256);
    tracker.configure(0x100, {2, 512, 128, false});
    const ReadKey key{0x100, 7};
    ASSERT_TRUE(tracker.begin(key, {0x1020, 512}, 0));
    EXPECT_FALSE(tracker.begin({0x100, 8}, {0x1800, 4}, 0));

    const Completion first{key, 512, 0x20, CompletionStatus::Success,
                           pattern(0, 96)};
    EXPECT_EQ(tracker.receive(first).disposition, Receipt::Partial);
    EXPECT_EQ(tracker.receive(first).disposition, Receipt::Malformed);
    EXPECT_EQ(tracker
                  .receive({{0x101, 7},
                            416,
                            0,
                            CompletionStatus::Success,
                            pattern(96, 256)})
                  .disposition,
              Receipt::Unexpected);
    EXPECT_EQ(tracker
                  .receive({key, 416, 4, CompletionStatus::Success,
                            pattern(96, 256)})
                  .disposition,
              Receipt::Malformed);
    EXPECT_EQ(tracker
                  .receive({key, 416, 0, CompletionStatus::Success,
                            pattern(96, 100)})
                  .disposition,
              Receipt::Malformed);
    EXPECT_EQ(tracker.bytesReserved(0x100), 512);
    EXPECT_EQ(
        tracker.receive({key, 416, 0, CompletionStatus::CompleterAbort, {}})
            .disposition,
        Receipt::Failed);
    EXPECT_EQ(tracker.bytesReserved(0x100), 0);
    EXPECT_EQ(tracker.receive(first).disposition, Receipt::Unexpected);
}

TEST(PcieRequest, DistinctFunctionsCanReuseNumericTags)
{
    ReadTracker tracker(256);
    const ReadKey first{0x100, 7};
    const ReadKey second{0x101, 7};
    tracker.configure(first.requester, {2, 16, 128, false});
    tracker.configure(second.requester, {1, 8, 128, false});

    ASSERT_TRUE(tracker.begin(first, {0x1000, 4}, 0));
    ASSERT_FALSE(tracker.begin(first, {0x1200, 4}, 1));
    ASSERT_TRUE(tracker.begin(second, {0x2004, 4}, 0));
    const std::vector<uint8_t> payload{11, 22, 33, 44};
    EXPECT_EQ(
        tracker.receive({second, 4, 4, CompletionStatus::Success, payload})
            .payload,
        payload);
    EXPECT_TRUE(tracker.outstanding(first));
    EXPECT_FALSE(tracker.outstanding(second));
    EXPECT_EQ(tracker.bytesReserved(first.requester), 4);
    EXPECT_EQ(tracker.bytesReserved(second.requester), 0);
}

TEST(PcieRequest, RejectsCompletionWithWrongTrafficClass)
{
    ReadTracker tracker(256);
    const ReadKey key{0x100, 7};
    tracker.configure(key.requester, {1, 16, 128, false});
    ASSERT_TRUE(tracker.begin(key, {0x1000, 4}, 1));

    Completion cpl{key, 4, 0, CompletionStatus::Success, {1, 2, 3, 4}};
    EXPECT_EQ(tracker.receive(cpl).disposition, Receipt::Malformed);
    EXPECT_TRUE(tracker.outstanding(key));
    cpl.trafficClass = 1;
    EXPECT_EQ(tracker.receive(cpl).disposition, Receipt::Complete);
}

TEST(PcieRequest, RcbDependsOnRequesterRole)
{
    EXPECT_TRUE(validRcb(64, true));
    EXPECT_TRUE(validRcb(128, true));
    EXPECT_FALSE(validRcb(64, false));
    EXPECT_TRUE(validRcb(128, false));
    EXPECT_FALSE(validRcb(256, true));
}

} // anonymous namespace
} // namespace gem5::pcie
