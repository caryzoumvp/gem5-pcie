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

#include <stdexcept>

#include "dev/pci/pcie/shared_fc.hh"

namespace gem5::pcie
{

namespace
{

constexpr CreditChannel P0{0, TlpClass::Posted};
constexpr CreditChannel P1{1, TlpClass::Posted};
constexpr CreditChannel C0{0, TlpClass::Completion};
constexpr CreditChannel N0{0, TlpClass::NonPosted};

TEST(SharedFc, UnmergedRemaindersAndOutOfOrderReturns)
{
    SharedFcConfig cfg;
    cfg.receiverShared = true;
    cfg.shared = {{{4, 4}, {4, 0}, {4, 4}}};
    SharedFc fc(cfg);
    EXPECT_EQ(fc.admit(1, P0, 17), FcAdmission::Shared);
    EXPECT_EQ(fc.admit(2, P0, 32), FcAdmission::Shared);
    EXPECT_EQ(fc.sharedInUse(P0), (Credits{4, 4}));
    EXPECT_EQ(fc.admit(3, P0, 16), FcAdmission::NoCredit);
    EXPECT_EQ(fc.admit(4, N0, 0), FcAdmission::Shared);
    EXPECT_FALSE(fc.update(2));
    EXPECT_TRUE(fc.release(2));
    EXPECT_FALSE(fc.release(2));
    EXPECT_TRUE(fc.update(2));
    EXPECT_EQ(fc.sharedAvailable(P0.type), (Credits{}));
    EXPECT_TRUE(fc.release(1));
    EXPECT_EQ(fc.admit(3, P0, 0), FcAdmission::NoCredit);
    EXPECT_TRUE(fc.update(1));
    EXPECT_EQ(fc.sharedAvailable(P0.type), (Credits{4, 4}));
    EXPECT_FALSE(fc.update(1));
    EXPECT_TRUE(fc.release(4));
    EXPECT_TRUE(fc.update(4));
    EXPECT_TRUE(fc.empty());
}

TEST(SharedFc, MergedCapacityKeepsSeparateClassRemainders)
{
    SharedFcConfig cfg;
    cfg.receiverShared = true;
    cfg.mergePostedCompletion = true;
    cfg.shared = {{{8, 8}, {4, 0}, {0, 0}}};
    SharedFc fc(cfg);
    EXPECT_EQ(fc.admit(1, P0, 0), FcAdmission::Shared);
    EXPECT_EQ(fc.sharedAvailable(C0.type), (Credits{0, 8}));
    EXPECT_EQ(fc.admit(2, C0, 16), FcAdmission::NoCredit);
    EXPECT_EQ(fc.admit(3, P0, 16), FcAdmission::Shared);
    EXPECT_EQ(fc.sharedInUse(P0), (Credits{8, 8}));
    EXPECT_TRUE(fc.release(3));
    EXPECT_TRUE(fc.update(3));
    EXPECT_EQ(fc.sharedAvailable(P0.type), (Credits{0, 8}));
    EXPECT_TRUE(fc.release(1));
    EXPECT_TRUE(fc.update(1));
    EXPECT_EQ(fc.admit(2, C0, 16), FcAdmission::Shared);
    EXPECT_EQ(fc.sharedAvailable(P0.type), (Credits{}));
    EXPECT_EQ(fc.sharedInUse(C0), (Credits{8, 8}));
    EXPECT_EQ(fc.admit(4, N0, 0), FcAdmission::Shared);
    EXPECT_EQ(fc.sharedInUse(N0), (Credits{4, 0}));
}

TEST(SharedFc, UsageLimitDedicatedFallbackAndReleaseOrder)
{
    SharedFcConfig cfg;
    cfg.receiverShared = true;
    cfg.vcCount = 2;
    cfg.shared = {{{8, 8}, {4, 0}, {4, 4}}};
    cfg.dedicated[0][0] = {1, 5};
    cfg.usageLimit[0] = 4;
    SharedFc fc(cfg);
    EXPECT_EQ(fc.admit(1, P0, 65), FcAdmission::Dedicated);
    EXPECT_EQ(fc.sharedInUse(P0), (Credits{}));
    EXPECT_EQ(fc.dedicatedAvailable(P0), (Credits{}));
    EXPECT_EQ(fc.admit(2, P0, 64), FcAdmission::Shared);
    EXPECT_EQ(fc.admit(3, P0, 16), FcAdmission::NoCredit);
    EXPECT_EQ(fc.admit(4, P1, 64), FcAdmission::Shared);
    EXPECT_TRUE(fc.release(2));
    EXPECT_TRUE(fc.update(2));
    EXPECT_EQ(fc.sharedAvailable(P0.type), (Credits{}));
    EXPECT_EQ(fc.sharedInUse(P0), (Credits{4, 4}));
    EXPECT_TRUE(fc.release(1));
    EXPECT_TRUE(fc.update(1));
    EXPECT_EQ(fc.dedicatedAvailable(P0), (Credits{1, 5}));
    EXPECT_EQ(fc.sharedAvailable(P0.type), (Credits{4, 4}));
    EXPECT_EQ(fc.sharedInUse(P0), (Credits{}));
    EXPECT_EQ(fc.sharedInUse(P1), (Credits{4, 4}));
    EXPECT_EQ(fc.admit(3, P0, 16), FcAdmission::Shared);
}

TEST(SharedFc, DedicatedOnlyAndInvalidAdmission)
{
    SharedFcConfig cfg;
    cfg.dedicated[0][0] = {1, 1};
    SharedFc fc(cfg);
    EXPECT_EQ(fc.admit(1, P1, 16), FcAdmission::Invalid);
    EXPECT_EQ(fc.admit(1, P0, 4097), FcAdmission::Invalid);
    EXPECT_EQ(fc.admit(1, P0, 17), FcAdmission::NoCredit);
    EXPECT_EQ(fc.admit(1, P0, 16), FcAdmission::Dedicated);
    EXPECT_EQ(fc.admit(1, P0, 16), FcAdmission::Invalid);
    EXPECT_TRUE(fc.release(1));
    EXPECT_EQ(fc.admit(2, P0, 16), FcAdmission::NoCredit);
    EXPECT_TRUE(fc.update(1));
    EXPECT_EQ(fc.admit(2, P0, 16), FcAdmission::Dedicated);
}

TEST(SharedFc, DataSpansGroupsAndReusesRemainder)
{
    SharedFcConfig cfg;
    cfg.receiverShared = true;
    cfg.shared[0] = {4, 8};
    SharedFc fc(cfg);
    EXPECT_EQ(fc.admit(1, P0, 65), FcAdmission::Shared);
    EXPECT_EQ(fc.sharedInUse(P0), (Credits{4, 8}));
    EXPECT_EQ(fc.admit(2, P0, 48), FcAdmission::Shared);
    EXPECT_EQ(fc.sharedInUse(P0), (Credits{4, 8}));
    EXPECT_EQ(fc.admit(3, P0, 16), FcAdmission::NoCredit);
    EXPECT_EQ(fc.admit(4, P0, 0), FcAdmission::Shared);
    EXPECT_TRUE(fc.release(1));
    EXPECT_TRUE(fc.update(1));
    EXPECT_EQ(fc.sharedAvailable(P0.type), (Credits{0, 4}));
    EXPECT_EQ(fc.admit(3, P0, 64), FcAdmission::Shared);
    EXPECT_TRUE(fc.release(3));
    EXPECT_TRUE(fc.update(3));
    EXPECT_EQ(fc.sharedAvailable(P0.type), (Credits{0, 4}));
    EXPECT_TRUE(fc.release(2));
    EXPECT_TRUE(fc.update(2));
    EXPECT_EQ(fc.sharedAvailable(P0.type), (Credits{0, 8}));
    EXPECT_TRUE(fc.release(4));
    EXPECT_TRUE(fc.update(4));
    EXPECT_TRUE(fc.empty());
}

TEST(SharedFc, RejectsUnsupportedBacking)
{
    SharedFcConfig cfg;
    cfg.shared[0] = {4, 4};
    EXPECT_THROW(SharedFc{cfg}, std::invalid_argument);
    cfg.receiverShared = true;
    cfg.shared[0] = {3, 4};
    EXPECT_THROW(SharedFc{cfg}, std::invalid_argument);
    cfg.shared[0] = {4, 4};
    cfg.usageLimit[0] = 9;
    EXPECT_THROW(SharedFc{cfg}, std::invalid_argument);
    cfg.usageLimit[0] = 7;
    EXPECT_THROW(SharedFc{cfg}, std::invalid_argument);
}

} // anonymous namespace

} // namespace gem5::pcie
