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

#include <map>
#include <utility>

#include "dev/pci/pcie/flit_link.hh"

namespace gem5::pcie
{
namespace
{

class Queue
{
  public:
    void
    enqueue(Tick at, LinkDirection::Event event)
    {
        events.emplace(at, std::move(event));
    }

    void
    advance(Tick stop)
    {
        while (!events.empty() && events.begin()->first <= stop) {
            // gem5 runs equal-priority events at a tick in reverse order.
            auto peer = std::prev(events.upper_bound(events.begin()->first));
            auto event = events.extract(peer);
            event.mapped()();
        }
    }

  private:
    std::multimap<Tick, LinkDirection::Event> events;
};

FlitLinkConfig
config()
{
    SharedFcConfig fc;
    fc.dedicated[0][0] = {1, 4};
    return {{64, 10, 5, 20}, fc, 2, 10, 15, 80};
}

constexpr CreditChannel Posted{0, TlpClass::Posted};

FlitMemoryRequest
message()
{
    return {Op::Write,
            {0x1234, 0},
            {0x1000, 64},
            0,
            std::vector<uint8_t>(64, 0x5a)};
}

TEST(PcieFlitLink, CorruptionReplaysSamePayloadThroughSerializer)
{
    Queue q;
    auto cfg = config();
    cfg.corruptFirst = true;
    std::vector<Tick> arrivals;
    std::vector<Tick> departures;
    FlitLink link(
        cfg, [&](Tick t, auto event) { q.enqueue(t, std::move(event)); },
        [&](Tick t, auto id, const FlitTlp &tlp) {
            EXPECT_EQ(id, 1);
            EXPECT_EQ(std::get<FlitMemoryRequest>(tlp).payload,
                      message().payload);
            arrivals.push_back(t);
        },
        [](Tick) {},
        [&](Tick t, auto id) {
            EXPECT_EQ(id, 1);
            departures.push_back(t);
        });
    ASSERT_EQ(link.send(0, 1, Posted, message()), SendResult::Accepted);
    q.advance(70);
    EXPECT_EQ(departures, (std::vector<Tick>{30}));
    EXPECT_TRUE(arrivals.empty());
    EXPECT_EQ(link.counts().crcErrors, 1);

    // NAK arrives at 70 and replay starts without waiting for a pack flush.
    q.advance(115);
    EXPECT_EQ(arrivals, (std::vector<Tick>{115}));
    EXPECT_EQ(departures, (std::vector<Tick>{30}));
    EXPECT_TRUE(link.release(115, 1));
    EXPECT_FALSE(link.release(115, 1));
    q.advance(134);
    EXPECT_EQ(link.send(134, 2, Posted, message()), SendResult::NoCredit);
    q.advance(135);
    EXPECT_TRUE(link.empty());
    EXPECT_EQ(link.counts().original, 1);
    EXPECT_EQ(link.counts().replayed, 1);
    EXPECT_EQ(link.counts().serializedTicks, 80);
}

TEST(PcieFlitLink, TimeoutReplaysAnAckLostFrameWithoutDuplicateDelivery)
{
    Queue q;
    auto cfg = config();
    cfg.dropFirstAck = true;
    unsigned delivered = 0;
    FlitLink *link_ptr = nullptr;
    FlitLink link(
        cfg, [&](Tick t, auto event) { q.enqueue(t, std::move(event)); },
        [&](Tick t, auto id, const FlitTlp &) {
            ++delivered;
            EXPECT_TRUE(link_ptr->release(t, id));
        },
        [](Tick) {}, [](Tick, auto) {});
    link_ptr = &link;
    ASSERT_EQ(link.send(0, 1, Posted, message()), SendResult::Accepted);
    q.advance(300);
    EXPECT_EQ(delivered, 1);
    EXPECT_EQ(link.counts().timeouts, 1);
    EXPECT_EQ(link.counts().replayed, 1);
    EXPECT_EQ(link.counts().duplicates, 1);
    EXPECT_TRUE(link.empty());
}

TEST(PcieFlitLink, FullFramesFillTheWireAndSparseTrafficWaitsToFlush)
{
    Queue q;
    auto cfg = config();
    cfg.flushDelay = 30;
    cfg.replayFlits = 4;
    cfg.credits.dedicated[0][0] = {2, 36};
    auto large = message();
    large.range.bytes = 512;
    large.payload.resize(512, 0x5a);
    std::vector<Tick> arrivals;
    FlitLink link(
        cfg, [&](Tick t, auto event) { q.enqueue(t, std::move(event)); },
        [&](Tick t, auto, const FlitTlp &) { arrivals.push_back(t); },
        [](Tick) {}, [](Tick, auto) {});

    // 512B plus the TLP header spans two full frames and one partial
    // frame. Each frame takes four 10-tick cycles to serialize.
    ASSERT_EQ(link.send(0, 1, Posted, large), SendResult::Accepted);
    q.advance(0);
    EXPECT_EQ(link.counts().original, 1);
    q.advance(40);
    EXPECT_EQ(link.counts().original, 2);
    q.advance(80);
    EXPECT_EQ(link.counts().original, 3);
    q.advance(125);
    EXPECT_EQ(arrivals, (std::vector<Tick>{125}));

    ASSERT_EQ(link.send(131, 2, Posted, message()), SendResult::Accepted);
    q.advance(169);
    EXPECT_EQ(link.counts().original, 3);
    q.advance(170);
    EXPECT_EQ(link.counts().original, 4);
    q.advance(215);
    EXPECT_EQ(arrivals, (std::vector<Tick>{125, 215}));
}

TEST(PcieFlitLink, FullFrameTakesTheNextEdgeBeforeScheduledFlush)
{
    Queue q;
    auto cfg = config();
    cfg.flushDelay = 30;
    cfg.credits.dedicated[0][0] = {2, 14};
    auto tail = message();
    tail.range.bytes = 160;
    tail.payload.resize(160, 0x6b);
    std::vector<Tick> arrivals;
    FlitLink link(
        cfg, [&](Tick t, auto event) { q.enqueue(t, std::move(event)); },
        [&](Tick t, auto, const FlitTlp &) { arrivals.push_back(t); },
        [](Tick) {}, [](Tick, auto) {});

    ASSERT_EQ(link.send(0, 1, Posted, message()), SendResult::Accepted);
    q.advance(5);
    ASSERT_EQ(link.send(5, 2, Posted, tail), SendResult::Accepted);
    q.advance(9);
    EXPECT_EQ(link.counts().original, 0);
    q.advance(10);
    EXPECT_EQ(link.counts().original, 1);
    q.advance(49);
    EXPECT_EQ(link.counts().original, 1);
    q.advance(95);
    EXPECT_EQ(arrivals, (std::vector<Tick>{55, 95}));
    EXPECT_EQ(link.counts().original, 2);
}

TEST(PcieFlitLink, SameBeatEntersWireBeforeTheReceiverPublishesEitherTlp)
{
    Queue q;
    auto cfg = config();
    cfg.timing.wireBytesPerCycle = 256;
    cfg.timing.propagation = 0;
    cfg.credits.dedicated[0][0] = {2, 8};
    std::vector<int> events;
    FlitLink link(
        cfg, [&](Tick t, auto event) { q.enqueue(t, std::move(event)); },
        [&](Tick t, auto id, const FlitTlp &) {
            EXPECT_EQ(t, 20);
            events.push_back(id);
        },
        [](Tick) {},
        [&](Tick t, auto id) {
            EXPECT_EQ(t, 20);
            events.push_back(-static_cast<int>(id));
        });
    ASSERT_EQ(link.send(0, 1, Posted, message()), SendResult::Accepted);
    ASSERT_EQ(link.send(0, 2, Posted, message()), SendResult::Accepted);
    q.advance(20);
    EXPECT_EQ(events, (std::vector<int>{-1, -2, 1, 2}));
}

TEST(PcieFlitLink, PairedControlUsesTheReverseWireWithoutJoiningReplay)
{
    Queue q;
    auto cfg = config();
    cfg.replayTimeout = 200;
    auto schedule = [&](Tick t, auto event) {
        q.enqueue(t, std::move(event));
    };
    FlitLink *forward_ptr = nullptr;
    FlitLink *reverse_ptr = nullptr;
    Tick forward_arrival = 0;
    Tick reverse_arrival = 0;
    FlitLink forward(
        cfg, schedule,
        [&](Tick t, auto id, const FlitTlp &) {
            forward_arrival = t;
            EXPECT_TRUE(forward_ptr->release(t, id));
        },
        [](Tick) {}, [](Tick, auto) {});
    FlitLink reverse(
        cfg, schedule,
        [&](Tick t, auto id, const FlitTlp &) {
            reverse_arrival = t;
            EXPECT_TRUE(reverse_ptr->release(t, id));
        },
        [](Tick) {}, [](Tick, auto) {});
    forward_ptr = &forward;
    reverse_ptr = &reverse;
    forward.connectFeedback(reverse);

    ASSERT_EQ(forward.send(0, 1, Posted, message()), SendResult::Accepted);
    q.advance(60);
    ASSERT_EQ(reverse.send(60, 2, Posted, message()), SendResult::Accepted);
    q.advance(115);
    EXPECT_EQ(forward_arrival, 55);
    EXPECT_EQ(reverse_arrival, 0);
    EXPECT_EQ(reverse.counts().controlFlits, 1);
    EXPECT_EQ(reverse.counts().controlTicks, 40);
    q.advance(225);
    EXPECT_EQ(reverse_arrival, 155);
    for (auto *link : {&forward, &reverse}) {
        EXPECT_EQ(link->counts().original, 1);
        EXPECT_EQ(link->counts().controlFlits, 1);
        EXPECT_EQ(link->counts().serializedTicks, 80);
        EXPECT_EQ(link->counts().replayed, 0);
        EXPECT_TRUE(link->empty());
    }
}

TEST(PcieFlitLink, PairedNakRecoversCorruptedPayloadBeforeTimeout)
{
    Queue q;
    auto cfg = config();
    cfg.replayFlits = 1;
    cfg.replayTimeout = 200;
    auto forward_cfg = cfg;
    forward_cfg.corruptFirst = true;
    auto schedule = [&](Tick t, auto event) {
        q.enqueue(t, std::move(event));
    };
    FlitLink *forward_ptr = nullptr;
    std::vector<Tick> arrivals;
    FlitLink forward(
        forward_cfg, schedule,
        [&](Tick t, auto id, const FlitTlp &tlp) {
            EXPECT_EQ(id, 1);
            EXPECT_EQ(std::get<FlitMemoryRequest>(tlp).payload,
                      message().payload);
            arrivals.push_back(t);
            EXPECT_TRUE(forward_ptr->release(t, id));
        },
        [](Tick) {}, [](Tick, auto) {});
    FlitLink reverse(
        cfg, schedule,
        [](Tick, auto, const FlitTlp &) {
            ADD_FAILURE() << "Control was a TLP";
        },
        [](Tick) {}, [](Tick, auto) {});
    forward_ptr = &forward;
    forward.connectFeedback(reverse);

    ASSERT_EQ(forward.send(0, 1, Posted, message()), SendResult::Accepted);
    q.advance(119);
    EXPECT_EQ(forward.counts().crcErrors, 1);
    EXPECT_EQ(forward.counts().replayed, 0);
    EXPECT_EQ(reverse.counts().controlFlits, 1);
    q.advance(165);
    EXPECT_EQ(arrivals, (std::vector<Tick>{165}));
    q.advance(225);
    EXPECT_EQ(forward.counts().replayed, 1);
    EXPECT_EQ(forward.counts().timeouts, 0);
    EXPECT_EQ(forward.counts().duplicates, 0);
    EXPECT_EQ(reverse.counts().controlFlits, 2);
    EXPECT_EQ(reverse.counts().original, 0);
    EXPECT_TRUE(forward.empty());
    EXPECT_TRUE(reverse.empty());
}

TEST(PcieFlitLink, PairedFeedbackCoalescesCumulativeAcks)
{
    Queue q;
    auto cfg = config();
    cfg.feedbackDelay = 100;
    cfg.replayTimeout = 500;
    cfg.replayFlits = 4;
    cfg.credits.dedicated[0][0] = {1, 32};
    auto large = message();
    large.range.bytes = 512;
    large.payload.resize(512, 0x5a);
    auto schedule = [&](Tick t, auto event) {
        q.enqueue(t, std::move(event));
    };
    FlitLink *forward_ptr = nullptr;
    size_t arrivals = 0;
    FlitLink forward(
        cfg, schedule,
        [&](Tick t, auto id, const FlitTlp &) {
            EXPECT_EQ(t, 125);
            ++arrivals;
            EXPECT_TRUE(forward_ptr->release(t, id));
        },
        [](Tick) {}, [](Tick, auto) {});
    FlitLink reverse(
        cfg, schedule,
        [](Tick, auto, const FlitTlp &) {
            ADD_FAILURE() << "Control was a TLP";
        },
        [](Tick) {}, [](Tick, auto) {});
    forward_ptr = &forward;
    forward.connectFeedback(reverse);

    ASSERT_EQ(forward.send(0, 1, Posted, large), SendResult::Accepted);
    q.advance(149);
    EXPECT_EQ(arrivals, 1);
    EXPECT_EQ(forward.counts().original, 3);
    EXPECT_EQ(reverse.counts().controlFlits, 0);
    q.advance(195);
    EXPECT_EQ(reverse.counts().controlFlits, 1);
    EXPECT_EQ(forward.counts().replayed, 0);
    EXPECT_TRUE(forward.empty());
    EXPECT_TRUE(reverse.empty());
}

} // anonymous namespace
} // namespace gem5::pcie
