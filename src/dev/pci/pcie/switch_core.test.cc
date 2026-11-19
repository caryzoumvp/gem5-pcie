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

#include "dev/pci/pcie/endpoint_memory.hh"
#include "dev/pci/pcie/switch_core.hh"

namespace gem5::pcie
{
namespace
{

class TestQueue
{
  public:
    Tick now = 0;
    std::multimap<std::pair<Tick, bool>, LinkDirection::Event> pending;

    void
    add(Tick tick, LinkDirection::Event event, bool service = false)
    {
        EXPECT_GE(tick, now);
        pending.emplace(std::pair{tick, service}, std::move(event));
    }

    void
    advance(Tick tick)
    {
        while (!pending.empty() && pending.begin()->first.first <= tick) {
            auto e = pending.extract(pending.begin());
            now = e.key().first;
            e.mapped()();
        }
        now = tick;
    }
};

SwitchLimits
limits()
{
    return {4, 2, 2, 2, 64, 1, 64, 1, 1, {}, {1, 1, 1}, {}, 10, 0};
}

ForwardTlp
write(uint64_t id, uint32_t ingress, uint32_t egress)
{
    return {id,
            ingress,
            egress,
            0,
            TlpClass::Posted,
            0,
            0,
            std::nullopt,
            16,
            std::make_shared<const std::vector<uint8_t>>(32, id)};
}

TEST(PcieSwitchCore, FullOutputRetainsCoreAndRxUntilResume)
{
    TestQueue q;
    std::vector<uint64_t> rx_released;
    std::vector<uint64_t> sent;
    bool blocked = true;
    SwitchCore *self = nullptr;
    SwitchCore core(
        limits(), [&](Tick t, auto event) { q.add(t, std::move(event)); },
        [&](Tick t, const ForwardTlp &tlp) {
            if (blocked) {
                return SendResult::NoCredit;
            }
            EXPECT_EQ(tlp.payload->front(), tlp.id);
            sent.push_back(tlp.id);
            q.add(t + 10, [&, t, id = tlp.id] { self->departed(t + 10, id); });
            return SendResult::Accepted;
        });
    self = &core;
    for (uint64_t id : {1, 2, 3}) {
        core.receive(0, write(id, 0, 3),
                     [&](Tick, auto id) { rx_released.push_back(id); });
    }
    q.advance(100);
    const auto stalled = core.counts();
    EXPECT_EQ(stalled.rxWaiting, 1);
    EXPECT_EQ(stalled.coreDescriptors, 1);
    EXPECT_EQ(stalled.stagedCells, 1);
    EXPECT_EQ(stalled.grants, 1);
    EXPECT_EQ(rx_released, (std::vector<uint64_t>{1, 2}));
    EXPECT_TRUE(sent.empty());
    EXPECT_TRUE(q.pending.empty());

    blocked = false;
    core.wake(100);
    core.wake(100);
    EXPECT_EQ(q.pending.size(), 1);
    q.advance(300);
    EXPECT_EQ(sent, (std::vector<uint64_t>{1, 2, 3}));
    EXPECT_EQ(rx_released, sent);
    EXPECT_EQ(core.counts().grants, 3);
    EXPECT_EQ(core.counts().transmissions, 3);
    EXPECT_TRUE(core.empty());
}

TEST(PcieSwitchCore, BankAndFabricAreGrantedTogether)
{
    for (uint32_t channels : {1, 2}) {
        TestQueue q;
        auto config = limits();
        config.fabricChannels = channels;
        SwitchCore core(
            config, [&](Tick t, auto event) { q.add(t, std::move(event)); },
            [](Tick, const ForwardTlp &) { return SendResult::NoCredit; });
        auto release = [](Tick, uint64_t) {};
        core.receive(0, write(1, 0, 2), release);
        core.receive(0, write(2, 1, 3), release);
        q.advance(30);
        EXPECT_EQ(core.counts().grants, channels);
        EXPECT_EQ(core.counts().stagedCells, channels);
        EXPECT_EQ(core.counts().coreDescriptors, 2);
        q.advance(40);
        EXPECT_EQ(core.counts().coreDescriptors, 2 - channels);
        q.advance(60);
        EXPECT_EQ(core.counts().grants, 2);
        EXPECT_EQ(core.counts().stagedCells, 2);
        EXPECT_EQ(core.counts().coreDescriptors, 0);
    }
}

TEST(PcieSwitchCore, SameEgressOrdersOnWireAndLeavesCrossEgressBlocked)
{
    TestQueue q;
    auto config = limits();
    config.pipelined = true;
    config.ingress = {4, 4, 2};
    config.voqCells = 4;
    config.stagingCells = 4;
    config.fabricChannels = 1;
    config.crossSliceDelay = 25;
    std::vector<Tick> grants;
    std::vector<std::pair<uint64_t, Tick>> accepted;
    SwitchCore *self = nullptr;
    SwitchCore core(
        config, [&](Tick t, auto event) { q.add(t, std::move(event)); },
        [&](Tick t, const ForwardTlp &tlp) {
            accepted.emplace_back(tlp.id, t);
            q.add(t + 50, [&, t, id = tlp.id] { self->departed(t + 50, id); });
            return SendResult::Accepted;
        },
        [&](Tick t, auto event) { q.add(t, std::move(event), true); },
        [&](SwitchCore::TraceEvent event) {
            if (event.stage == SwitchCore::Stage::FabricGrant) {
                grants.push_back(event.tick);
            }
        });
    self = &core;
    const auto release = [](Tick, uint64_t) {};
    core.receive(0, write(1, 0, 2), release);
    core.receive(0, write(2, 0, 2), release);
    core.receive(0, write(3, 0, 1), release);
    q.advance(115);
    EXPECT_EQ(grants, (std::vector<Tick>{20, 30}));
    EXPECT_EQ(accepted,
              (std::vector<std::pair<uint64_t, Tick>>{{1, 60}, {2, 70}}));
    q.advance(300);
    ASSERT_EQ(grants.size(), 3);
    EXPECT_GE(grants.back(), 120);
    ASSERT_EQ(accepted.size(), 3);
    EXPECT_EQ(accepted.back().first, 3);
    EXPECT_TRUE(core.empty());
}

TEST(PcieSwitchCore, MultiBeatTransferKeepsOneReservationUntilFinalBeat)
{
    using Stage = SwitchCore::Stage;
    TestQueue q;
    auto config = limits();
    config.pipelined = true;
    config.voqCells = 4;
    config.stagingCells = 4;
    config.ingress = {1, 4, 1};
    std::vector<std::pair<Stage, Tick>> trace;
    SwitchCore core(
        config, [&](Tick t, auto event) { q.add(t, std::move(event)); },
        [](Tick, const ForwardTlp &) { return SendResult::NoCredit; },
        [&](Tick t, auto event) { q.add(t, std::move(event), true); },
        [&](SwitchCore::TraceEvent event) {
            trace.emplace_back(event.stage, event.tick);
        });
    auto packet = write(1, 0, 2);
    // 144 B uses three 64 B cells, with one cell served per cycle.
    packet.payload = std::make_shared<const std::vector<uint8_t>>(128, 1);
    std::vector<Tick> returns;
    core.receive(0, std::move(packet),
                 [&](Tick t, auto) { returns.push_back(t); });
    q.advance(60);
    EXPECT_EQ(core.counts().grants, 1);
    EXPECT_EQ(core.counts().coreDescriptors, 1);
    EXPECT_EQ(core.counts().stagedCells, 3);
    EXPECT_EQ(core.counts().writeBeats, 3);
    EXPECT_EQ(core.counts().readBeats, 1);
    EXPECT_EQ(returns, (std::vector<Tick>{40}));
    q.advance(80);
    const std::vector<std::pair<Stage, Tick>> expected{
        {Stage::Rx, 0},
        {Stage::CoreReady, 0},
        {Stage::WriteStart, 10},
        {Stage::WriteContinuation, 20},
        {Stage::WriteContinuation, 30},
        {Stage::RxRelease, 40},
        {Stage::FabricGrant, 50},
        {Stage::FabricContinuation, 60},
        {Stage::FabricContinuation, 70},
        {Stage::ReadLaneRelease, 80},
        {Stage::FabricDone, 80},
        {Stage::TxReady, 80},
    };
    EXPECT_EQ(trace, expected);
    EXPECT_EQ(core.counts().coreDescriptors, 0);
    EXPECT_EQ(core.counts().stagedCells, 3);
    EXPECT_EQ(core.counts().readBeats, 3);
}

TEST(PcieSwitchCore, DownstreamCreditBacksUpTheUpstreamLink)
{
    constexpr CreditChannel posted{0, TlpClass::Posted};
    constexpr uint64_t writes = 6;
    TestQueue q;
    EndpointMemory sink(0x2000, 4096, 256);
    std::vector<uint64_t> visible;
    std::map<uint64_t, ForwardTlp> output;
    uint64_t next = 1;
    uint64_t upstream_stalls = 0;
    SwitchCore *core_ptr = nullptr;
    LinkDirection *incoming_ptr = nullptr;
    auto schedule = [&](Tick t, auto event) { q.add(t, std::move(event)); };

    const auto inject = [&](Tick t) {
        auto &link = *incoming_ptr;
        if (next > writes) {
            return;
        }
        const auto result = link.send(t, next, posted, 48, 32);
        if (result == SendResult::Accepted) {
            ++next;
        } else if (result == SendResult::NoCredit) {
            ++upstream_stalls;
        } else {
            EXPECT_EQ(result, SendResult::Busy);
        }
    };
    LinkDirection incoming(
        {64, 10, 0, 10}, schedule,
        [&](Tick t, auto id) {
            core_ptr->receive(t, write(id, 0, 3), [&](Tick release, auto id) {
                EXPECT_TRUE(incoming_ptr->release(release, id));
            });
        },
        inject);
    incoming_ptr = &incoming;
    incoming.configure(posted, {2, 4});

    LinkDirection *outgoing_ptr = nullptr;
    LinkDirection outgoing(
        {64, 10, 5, 10}, schedule,
        [&](Tick t, auto id) {
            const auto commit = [&, id](Tick instant) {
                auto data = output.extract(id);
                ASSERT_FALSE(data.empty());
                EXPECT_TRUE(sink.commitWrite(0x2000 + (id - 1) * 32,
                                             *data.mapped().payload));
                visible.push_back(id);
                EXPECT_TRUE(outgoing_ptr->release(instant, id));
            };
            if (id == 1) {
                q.add(200, [commit] { commit(200); });
            } else {
                commit(t);
            }
        },
        [&](Tick t) { core_ptr->wake(t); },
        [&](Tick t, auto id) { core_ptr->departed(t, id); });
    outgoing_ptr = &outgoing;
    outgoing.configure(posted, {1, 2});

    SwitchCore core(limits(), schedule, [&](Tick t, const ForwardTlp &tlp) {
        const auto sent =
            outgoing.send(t, tlp.id, posted, tlp.wireBytes(), tlp.dataBytes());
        if (sent == SendResult::Accepted) {
            EXPECT_TRUE(output.emplace(tlp.id, tlp).second);
        }
        return sent;
    });
    core_ptr = &core;
    inject(0);
    q.advance(199);
    EXPECT_TRUE(visible.empty());
    EXPECT_LT(next, writes + 1);
    EXPECT_GT(upstream_stalls, 0);
    EXPECT_EQ(incoming.state(posted).available.headers, 0);
    EXPECT_EQ(outgoing.state(posted).occupied.headers, 1);
    EXPECT_EQ(core.counts().stagedCells, 1);
    EXPECT_GT(core.counts().rxWaiting, 0);

    q.advance(700);
    EXPECT_EQ(visible, (std::vector<uint64_t>{1, 2, 3, 4, 5, 6}));
    std::vector<uint8_t> expected;
    for (auto id : visible) {
        expected.insert(expected.end(), 32, id);
    }
    const auto data = sink.completeRead({0, 0}, {0x2000, 192}, 0, 128);
    ASSERT_EQ(data.size(), 1);
    EXPECT_EQ(data.front().payload, expected);
    EXPECT_TRUE(q.pending.empty());
    EXPECT_TRUE(core.empty());
    EXPECT_TRUE(incoming.empty());
    EXPECT_TRUE(outgoing.empty());
    EXPECT_TRUE(output.empty());
}

TEST(PcieSwitchCore, EqualTickIngressDoesNotDependOnCallbackOrder)
{
    for (bool reversed : {false, true}) {
        TestQueue q;
        auto config = limits();
        config.bankCount = 1;
        config.voqCells = 2;
        config.stagingCells = 2;
        std::vector<uint32_t> copied;
        SwitchCore core(
            config, [&](Tick t, auto event) { q.add(t, std::move(event)); },
            [](Tick, const ForwardTlp &) { return SendResult::NoCredit; });
        const auto receive = [&](uint32_t port) {
            core.receive(0, write(1 + port, port, 3),
                         [&, port](Tick, auto) { copied.push_back(port); });
        };
        receive(reversed ? 1 : 0);
        receive(reversed ? 0 : 1);
        q.advance(70);
        EXPECT_EQ(copied, (std::vector<uint32_t>{0, 1}));
    }
}

TEST(PcieSwitchCore, IndependentParseAndCrossingEdges)
{
    TestQueue q;
    auto config = limits();
    config.linkCycle = 6;
    config.parserSlots = 1;
    config.parserCycles = 1;
    config.ingressCdcCycles = 2;
    config.egressCdcCycles = 2;
    config.crossSliceDelay = 7;
    using Stage = SwitchCore::Stage;
    using Trace = SwitchCore::TraceEvent;
    std::vector<Trace> trace;
    SwitchCore *self = nullptr;
    SwitchCore core(
        config, [&](Tick t, auto event) { q.add(t, std::move(event)); },
        [&](Tick tick, const ForwardTlp &tlp) {
            q.add(tick + 10,
                  [&, tick, id = tlp.id] { self->departed(tick + 10, id); });
            return SendResult::Accepted;
        },
        {}, [&](Trace event) { trace.push_back(event); });
    self = &core;
    core.receive(3, write(7, 0, 3), [](Tick, auto) {});
    q.advance(150);
    const std::vector<Trace> expected{
        {Stage::Rx, 3, 7, 0},           {Stage::ParseStart, 6, 7, 0},
        {Stage::ParseDone, 12, 7, 0},   {Stage::CoreReady, 40, 7, 0},
        {Stage::WriteStart, 50, 7, 0},  {Stage::RxRelease, 60, 7, 0},
        {Stage::FabricGrant, 70, 7, 0}, {Stage::FabricDone, 87, 7, 0},
        {Stage::TxReady, 102, 7, 3},    {Stage::TxAccepted, 110, 7, 3},
        {Stage::TxDeparted, 120, 7, 3},
    };
    EXPECT_EQ(trace, expected);
    EXPECT_TRUE(core.empty());
}

TEST(PcieSwitchCore, ParserSlotsLimitIssueRatePerInputPort)
{
    for (unsigned slots : {1, 2}) {
        TestQueue q;
        auto config = limits();
        config.linkCycle = 6;
        config.parserSlots = slots;
        config.parserCycles = 2;
        std::vector<Tick> starts;
        SwitchCore core(
            config, [&](Tick t, auto event) { q.add(t, std::move(event)); },
            [](Tick, const ForwardTlp &) { return SendResult::NoCredit; }, {},
            [&](SwitchCore::TraceEvent event) {
                if (event.stage == SwitchCore::Stage::ParseStart) {
                    starts.push_back(event.tick);
                }
            });
        core.receive(3, write(7, 0, 3), [](Tick, auto) {});
        core.receive(3, write(8, 0, 3), [](Tick, auto) {});
        q.advance(25);
        EXPECT_EQ(starts, (std::vector<Tick>{6, slots == 1 ? 12u : 6u}));
    }
}

} // anonymous namespace
} // namespace gem5::pcie
