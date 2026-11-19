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

#include "dev/pci/pcie/flit_replay.hh"

namespace gem5::pcie
{
namespace
{

TEST(PcieFlitReplay, StandardNakDefersAndResendsBothProtectedFrames)
{
    ReplayTx tx(2);
    ReplayRx rx;
    FlitRecord a, b;
    a.bytes[5] = 0x12;
    b.bytes[230] = 0xbc;
    ASSERT_TRUE(tx.protect(a));
    ASSERT_TRUE(tx.protect(b));
    EXPECT_FALSE(tx.protect({}));

    const auto bad = rx.receive(1, false);
    EXPECT_EQ(bad.result, ReceiveFlit::Discard);
    EXPECT_EQ(bad.feedback, ReplayCommand::StandardNak);
    EXPECT_EQ(bad.cumulative, 1023);
    EXPECT_EQ(rx.receive(2, true).result, ReceiveFlit::Discard);
    ASSERT_TRUE(tx.feedback(bad.feedback, bad.cumulative));
    EXPECT_EQ(tx.occupied(), 2);
    EXPECT_FALSE(tx.canSend());

    const auto one = tx.nextReplay();
    ASSERT_TRUE(one);
    EXPECT_EQ(one->sequence, 1);
    EXPECT_EQ(one->protectedFlit.bytes, a.bytes);
    const auto second = tx.nextReplay();
    ASSERT_TRUE(second);
    EXPECT_EQ(second->sequence, 2);
    EXPECT_EQ(second->protectedFlit.bytes, b.bytes);
    EXPECT_FALSE(tx.nextReplay());

    const auto ack_one = rx.receive(1, true);
    EXPECT_EQ(ack_one.result, ReceiveFlit::Deliver);
    EXPECT_TRUE(tx.feedback(ack_one.feedback, ack_one.cumulative));
    EXPECT_EQ(tx.occupied(), 1);
    const auto ack_two = rx.receive(2, true);
    EXPECT_EQ(ack_two.result, ReceiveFlit::Deliver);
    EXPECT_TRUE(tx.feedback(ack_two.feedback, ack_two.cumulative));
    EXPECT_EQ(tx.occupied(), 0);
}

TEST(PcieFlitReplay, LostAckTimeoutDoesNotDeliverDuplicate)
{
    ReplayTx tx(1);
    ReplayRx rx;
    const auto frame = tx.protect({});
    ASSERT_TRUE(frame);
    EXPECT_EQ(rx.receive(frame->sequence, true).result, ReceiveFlit::Deliver);
    EXPECT_TRUE(tx.timeout());
    EXPECT_FALSE(tx.canSend());
    const auto replay = tx.nextReplay();
    ASSERT_TRUE(replay);
    const auto duplicate = rx.receive(replay->sequence, true);
    EXPECT_EQ(duplicate.feedback, ReplayCommand::Ack);
    EXPECT_EQ(duplicate.result, ReceiveFlit::Duplicate);
    EXPECT_EQ(duplicate.cumulative, 1);
    EXPECT_TRUE(tx.feedback(duplicate.feedback, duplicate.cumulative));
    EXPECT_TRUE(tx.canSend());
    EXPECT_FALSE(tx.timeout());
}

TEST(PcieFlitReplay, SequenceWrapSkipsZeroAndRejectsFutureAck)
{
    ReplayTx tx(3, 1023);
    ReplayRx rx(1023);
    const auto last = tx.protect({});
    ASSERT_TRUE(last);
    EXPECT_EQ(last->sequence, 1023);
    const auto first = tx.protect({});
    ASSERT_TRUE(first);
    EXPECT_EQ(first->sequence, 1);
    EXPECT_FALSE(tx.feedback(ReplayCommand::Ack, 0));
    EXPECT_FALSE(tx.feedback(ReplayCommand::Ack, 2));
    EXPECT_EQ(tx.occupied(), 2);
    EXPECT_EQ(rx.receive(1023, true).result, ReceiveFlit::Deliver);
    const auto ack = rx.receive(1, true);
    EXPECT_EQ(ack.result, ReceiveFlit::Deliver);
    EXPECT_TRUE(tx.feedback(ack.feedback, ack.cumulative));
    EXPECT_EQ(tx.occupied(), 0);
    EXPECT_FALSE(tx.feedback(ReplayCommand::Ack, 1023));
}

TEST(PcieFlitReplay, DlpBytesKeepReplayCommandAndSequenceSeparate)
{
    FlitRecord flit;
    flit.bytes[238] = 0x3a;
    const ReplayDlp control{true, true, ReplayCommand::StandardNak, 0x2a5};
    writeReplayDlp(flit, control);
    EXPECT_EQ(flit.bytes[236], 0x6a);
    EXPECT_EQ(flit.bytes[237], 0xa5);
    EXPECT_EQ(flit.bytes[238], 0x3a);
    EXPECT_EQ(readReplayDlp(flit), control);
}

} // anonymous namespace
} // namespace gem5::pcie
