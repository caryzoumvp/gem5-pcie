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
#include <vector>

#include "dev/pci/pcie/link.hh"

namespace gem5::pcie
{
namespace
{

class TestQueue
{
  public:
    Tick now = 0;
    std::multimap<Tick, LinkDirection::Event> pending;

    void
    add(Tick tick, LinkDirection::Event event)
    {
        EXPECT_GE(tick, now);
        pending.emplace(tick, std::move(event));
    }

    void
    advance(Tick tick)
    {
        ASSERT_GE(tick, now);
        while (!pending.empty() && pending.begin()->first <= tick) {
            auto e = pending.extract(pending.begin());
            now = e.key();
            e.mapped()();
        }
        now = tick;
    }
};

constexpr CreditChannel Posted{0, TlpClass::Posted};
constexpr CreditChannel NonPosted{0, TlpClass::NonPosted};

void
check(LinkDirection &link, Credits available, Credits flight, Credits occupied,
      Credits returning)
{
    auto actual = link.state(Posted);
    EXPECT_EQ(actual.available, available);
    EXPECT_EQ(actual.inFlight, flight);
    EXPECT_EQ(actual.occupied, occupied);
    EXPECT_EQ(actual.returning, returning);
    EXPECT_EQ(actual.available.headers + actual.inFlight.headers +
                  actual.occupied.headers + actual.returning.headers,
              1);
    EXPECT_EQ(actual.available.data + actual.inFlight.data +
                  actual.occupied.data + actual.returning.data,
              2);
}

TEST(PcieLink, ReleaseAndCreditUpdateAreSeparateEvents)
{
    TestQueue q;
    std::vector<std::pair<Tick, LinkDirection::Id>> arrivals;
    std::vector<Tick> ready;
    LinkDirection link(
        {16, 10, 7, 11},
        [&](Tick t, auto event) { q.add(t, std::move(event)); },
        [&](Tick t, auto id) { arrivals.emplace_back(t, id); },
        [&](Tick t) { ready.push_back(t); });
    link.configure(Posted, {1, 2});
    EXPECT_EQ(link.send(0, 101, Posted, 32, 17), SendResult::Accepted);
    check(link, {}, {1, 2}, {}, {});
    EXPECT_FALSE(link.release(0, 101));
    q.advance(20);
    EXPECT_EQ(ready, (std::vector<Tick>{20}));
    EXPECT_EQ(link.send(20, 102, Posted, 32, 17), SendResult::NoCredit);
    EXPECT_TRUE(arrivals.empty());
    q.advance(27);
    EXPECT_EQ(arrivals, (decltype(arrivals){{27, 101}}));
    check(link, {}, {}, {1, 2}, {});

    q.advance(40);
    EXPECT_EQ(link.send(40, 102, Posted, 32, 17), SendResult::NoCredit);
    EXPECT_TRUE(link.release(40, 101));
    EXPECT_FALSE(link.release(40, 101));
    check(link, {}, {}, {}, {1, 2});
    q.advance(50);
    EXPECT_EQ(link.send(50, 102, Posted, 32, 17), SendResult::NoCredit);
    q.advance(51);
    EXPECT_EQ(ready, (std::vector<Tick>{20, 51}));
    check(link, {1, 2}, {}, {}, {});
    EXPECT_TRUE(link.empty());
    EXPECT_EQ(link.send(51, 102, Posted, 32, 17), SendResult::Accepted);
}

TEST(PcieLink, PhysicalDirectionsAreIndependentAndClassesShareSerializer)
{
    TestQueue q;
    auto schedule = [&](Tick t, auto event) { q.add(t, std::move(event)); };
    auto arrival = [](Tick, LinkDirection::Id) {};
    auto ready = [](Tick) {};
    LinkDirection forward({16, 10, 7, 11}, schedule, arrival, ready);
    LinkDirection reverse({16, 10, 7, 11}, schedule, arrival, ready);
    for (auto *dir : {&forward, &reverse}) {
        dir->configure(Posted, {1, 2});
        dir->configure(NonPosted, {1, 0});
    }
    EXPECT_EQ(forward.send(0, 3, Posted, 48, 33), SendResult::Invalid);
    EXPECT_FALSE(forward.release(0, 3));
    EXPECT_EQ(forward.send(0, 3, Posted, 32, 17), SendResult::Accepted);
    EXPECT_EQ(forward.send(0, 4, NonPosted, 16, 0), SendResult::Busy);
    EXPECT_EQ(reverse.send(0, 4, NonPosted, 16, 0), SendResult::Accepted);
    q.advance(20);
    EXPECT_EQ(forward.send(20, 4, NonPosted, 16, 0), SendResult::Accepted);
}

TEST(PcieLink, DeparturePrecedesReceiveWithoutPropagation)
{
    TestQueue q;
    std::vector<char> fired;
    LinkDirection link(
        {16, 10, 0, 0},
        [&](Tick t, auto event) { q.add(t, std::move(event)); },
        [&](Tick t, auto id) {
            EXPECT_EQ(t, 10);
            EXPECT_EQ(id, 15);
            fired.push_back('a');
        },
        [&](Tick t) {
            EXPECT_EQ(t, 10);
            fired.push_back('r');
        },
        [&](Tick t, auto id) {
            EXPECT_EQ(t, 10);
            EXPECT_EQ(id, 15);
            fired.push_back('d');
        });
    link.configure(NonPosted, {1, 0});
    EXPECT_EQ(link.send(0, 15, NonPosted, 16, 0), SendResult::Accepted);
    q.advance(10);
    EXPECT_EQ(fired, (std::vector<char>{'d', 'a', 'r'}));
}

} // anonymous namespace
} // namespace gem5::pcie
