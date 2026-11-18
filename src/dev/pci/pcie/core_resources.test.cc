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

#include <limits>
#include <stdexcept>

#include "dev/pci/pcie/core_resources.hh"

namespace gem5::pcie
{
namespace
{

constexpr VoqKey posted{1, 0, TlpClass::Posted, 0};
constexpr VoqKey read{1, 0, TlpClass::NonPosted, 0};
constexpr VoqKey completion{1, 0, TlpClass::Completion, 0};

TEST(PcieCoreResources, SectorDenialLeavesDescriptorAndVoqAvailable)
{
    CoreResources pool({2, 2, 2}, 64, {});
    const auto data = pool.reserve(posted, 65);
    ASSERT_TRUE(data);
    EXPECT_EQ(pool.occupied(), (CoreCapacity{1, 2, 1}));
    EXPECT_FALSE(pool.reserve(read, 1));
    EXPECT_EQ(pool.occupied(), (CoreCapacity{1, 2, 1}));
    EXPECT_EQ(pool.voqReferences(read), 0);

    const auto header_only = pool.reserve(read, 0);
    ASSERT_TRUE(header_only);
    EXPECT_EQ(pool.occupied(), (CoreCapacity{2, 2, 2}));
    EXPECT_FALSE(pool.reserve(posted, 0));
    EXPECT_TRUE(pool.cancel(header_only.value()));
    EXPECT_TRUE(pool.cancel(data.value()));

    const auto retry = pool.reserve(read, 1);
    ASSERT_TRUE(retry);
    EXPECT_EQ(pool.occupied(), (CoreCapacity{1, 1, 1}));
    EXPECT_TRUE(pool.cancel(retry.value()));
    EXPECT_EQ(pool.occupied(), CoreCapacity{});
}

TEST(PcieCoreResources, ActiveIndexFollowsAllReferences)
{
    CoreResources pool({3, 3, 1}, 64, {});
    const VoqKey another_domain{1, 0, TlpClass::Posted, 1};
    const auto first = pool.reserve(posted, 64);
    ASSERT_TRUE(first);
    const auto second = pool.reserve(posted, 64);
    ASSERT_TRUE(second);
    EXPECT_EQ(pool.voqReferences(posted), 2);
    EXPECT_FALSE(pool.reserve(another_domain, 0));
    EXPECT_EQ(pool.occupied(), (CoreCapacity{2, 2, 1}));

    EXPECT_TRUE(pool.commit(first.value()));
    EXPECT_FALSE(pool.commit(first.value()));
    EXPECT_FALSE(pool.cancel(first.value()));
    EXPECT_FALSE(pool.release(second.value()));
    EXPECT_EQ(pool.occupied(), (CoreCapacity{2, 2, 1}));

    EXPECT_TRUE(pool.cancel(second.value()));
    EXPECT_EQ(pool.voqReferences(posted), 1);
    EXPECT_FALSE(pool.reserve(another_domain, 0));
    EXPECT_TRUE(pool.release(first.value()));
    EXPECT_FALSE(pool.release(first.value()));
    EXPECT_EQ(pool.voqReferences(posted), 0);
    EXPECT_EQ(pool.occupied(), CoreCapacity{});

    const auto retry = pool.reserve(another_domain, 64);
    ASSERT_TRUE(retry);
    EXPECT_NE(retry, first);
    EXPECT_FALSE(pool.commit(first.value()));
    EXPECT_TRUE(pool.cancel(retry.value()));
}

TEST(PcieCoreResources, NonPostedCannotConsumeClassMinimums)
{
    const CoreResources::ClassMinimums minimums{
        CoreCapacity{1, 1, 1}, CoreCapacity{}, CoreCapacity{1, 1, 1}};
    CoreResources pool({3, 2, 3}, 64, minimums);

    EXPECT_FALSE(pool.reserve(read, 1));
    const auto non_posted = pool.reserve(read, 0);
    ASSERT_TRUE(non_posted);
    EXPECT_FALSE(pool.reserve(read, 0));
    EXPECT_EQ(pool.occupied(), (CoreCapacity{1, 0, 1}));

    const auto p = pool.reserve(posted, 64);
    ASSERT_TRUE(p);
    const auto c = pool.reserve(completion, 64);
    ASSERT_TRUE(c);
    EXPECT_EQ(pool.occupied(), (CoreCapacity{3, 2, 3}));
    EXPECT_TRUE(pool.cancel(p.value()));
    EXPECT_FALSE(pool.reserve(read, 0));
    EXPECT_TRUE(pool.cancel(c.value()));
    EXPECT_TRUE(pool.cancel(non_posted.value()));
    EXPECT_EQ(pool.occupied(), CoreCapacity{});
}

TEST(PcieCoreResources, RejectsOvercommittedMinimums)
{
    const CoreResources::ClassMinimums minimums{
        CoreCapacity{1, 1, 1}, CoreCapacity{}, CoreCapacity{1, 1, 1}};
    EXPECT_THROW((CoreResources({2, 1, 2}, 64, minimums)),
                 std::invalid_argument);
    EXPECT_THROW((CoreResources({1, 1, 1}, 0, {})), std::invalid_argument);
}

TEST(PcieCoreResources, RoundsWithoutOverflow)
{
    CoreResources pool({1, 1, 1}, 64, {});
    EXPECT_FALSE(pool.reserve(posted, std::numeric_limits<uint32_t>::max()));
    EXPECT_EQ(pool.occupied(), CoreCapacity{});
}

} // anonymous namespace
} // namespace gem5::pcie
