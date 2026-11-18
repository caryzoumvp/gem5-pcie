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

#include "dev/pci/pcie/ordering.hh"

namespace gem5::pcie
{
namespace
{

constexpr OrderingDomain Base{0, 0, 0};

TEST(PcieOrdering, PAndCompletionMayBypassBlockedNonPosted)
{
    Ordering order;
    const auto blocked = order.arrived(Base, TlpClass::NonPosted);
    const auto cpl = order.arrived(Base, TlpClass::Completion,
                                   CompletionIdentity{0x100, 1});
    const auto posted = order.arrived(Base, TlpClass::Posted);
    EXPECT_TRUE(order.eligible(blocked));
    EXPECT_TRUE(order.commit(cpl));
    EXPECT_TRUE(order.commit(posted));
    EXPECT_FALSE(order.empty());
    EXPECT_TRUE(order.commit(blocked));
    EXPECT_TRUE(order.empty());
}

TEST(PcieOrdering, OlderPostedBlocksLaterTlpUntilActualCommit)
{
    Ordering order;
    const auto p = order.arrived(Base, TlpClass::Posted);
    const auto other_p = order.arrived(Base, TlpClass::Posted);
    const auto np = order.arrived(Base, TlpClass::NonPosted);
    const auto c = order.arrived(Base, TlpClass::Completion,
                                 CompletionIdentity{0x100, 1});
    const auto other_tc = order.arrived({0, 0, 1}, TlpClass::NonPosted);
    EXPECT_FALSE(order.eligible(np));
    EXPECT_FALSE(order.commit(c));
    EXPECT_TRUE(order.commit(other_tc));
    EXPECT_TRUE(order.commit(p));
    EXPECT_FALSE(order.eligible(np));
    EXPECT_TRUE(order.commit(other_p));
    EXPECT_FALSE(order.commit(other_p));
    EXPECT_TRUE(order.commit(c));
    EXPECT_TRUE(order.commit(np));
    EXPECT_TRUE(order.empty());
}

TEST(PcieOrdering, CompletionDependencyIsPerIdentity)
{
    Ordering order;
    const CompletionIdentity req{0x100, 3};
    const auto first = order.arrived(Base, TlpClass::Completion, req);
    const auto second = order.arrived(Base, TlpClass::Completion, req);
    const auto other = order.arrived(Base, TlpClass::Completion,
                                     CompletionIdentity{0x101, 3});
    EXPECT_TRUE(order.commit(other));
    EXPECT_FALSE(order.eligible(second));
    EXPECT_TRUE(order.commit(first));
    EXPECT_TRUE(order.commit(second));
    EXPECT_TRUE(order.empty());
}

TEST(PcieOrdering, ExplicitAbortRemovesDependency)
{
    Ordering order;
    const auto first = order.arrived(Base, TlpClass::NonPosted);
    const auto second = order.arrived(Base, TlpClass::NonPosted);
    EXPECT_FALSE(order.eligible(second));
    EXPECT_TRUE(order.abort(first));
    EXPECT_FALSE(order.abort(first));
    EXPECT_TRUE(order.commit(second));
    EXPECT_TRUE(order.empty());
}

} // anonymous namespace
} // namespace gem5::pcie
