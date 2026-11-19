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

#ifndef __DEV_PCI_PCIE_SHARED_FC_HH__
#define __DEV_PCI_PCIE_SHARED_FC_HH__

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>

#include "dev/pci/pcie/link.hh"

namespace gem5::pcie
{

struct SharedFcConfig
{
    bool receiverShared = false;
    bool mergePostedCompletion = false;
    uint8_t vcCount = 1;
    // Direction-wide backing. Merged Completion uses the Posted entry.
    std::array<Credits, 3> shared{};
    std::array<std::array<Credits, 3>, 8> dedicated{};
    // Limit in eighths of the initial shared capacity, by VC.
    std::array<std::optional<uint8_t>, 8> usageLimit{};
};

enum class FcAdmission
{
    Shared,
    Dedicated,
    NoCredit,
    Invalid
};

/**
 * Finite post-initialization accounting for one link direction.
 *
 * Admissions and RX delivery are ordered. release() marks freed RX backing.
 * update() models arrival of the corresponding FC update; schedule it on the
 * link event queue. Credits never return at the replay acknowledgement.
 * A drained partial group closes at release. This is an abstract coordinated
 * boundary; wire-level InitFC, UpdateFC counters, scales, and timers are
 * outside this interface. Header and data groups drain independently once
 * their own backing is released.
 */
class SharedFc
{
  public:
    explicit SharedFc(const SharedFcConfig &config);
    SharedFc(const SharedFc &) = delete;
    SharedFc &operator=(const SharedFc &) = delete;
    SharedFc(SharedFc &&) = default;

    FcAdmission admit(uint64_t id, CreditChannel channel, uint32_t data_bytes);
    bool release(uint64_t id);
    bool update(uint64_t id);
    Credits sharedAvailable(TlpClass type) const;
    Credits sharedInUse(CreditChannel channel) const;
    Credits dedicatedAvailable(CreditChannel channel) const;
    bool empty() const;

  private:
    struct Group
    {
        CreditChannel channel;
        bool data;
        uint32_t remaining;
        uint32_t owners = 0;
        uint32_t updating = 0;
        uint64_t lastSequence = 0;
        bool closed = false;
    };

    using GroupPtr = std::shared_ptr<Group>;

    struct Transfer
    {
        CreditChannel channel;
        Credits cost;
        uint64_t sequence;
        std::vector<GroupPtr> groups;
        bool released = false;
    };

    struct Channel
    {
        Credits dedicated;
        Credits sharedUsed;
        std::array<GroupPtr, 2> open;
    };

    bool valid(CreditChannel channel) const;
    size_t pool(TlpClass type) const;
    uint32_t quantum(TlpClass type) const;
    void consume(Transfer &tx, unsigned part, uint32_t units);
    void drain();

    const SharedFcConfig config;
    std::array<Credits, 3> shared;
    std::array<std::array<Channel, 3>, 8> channels{};
    std::map<uint64_t, Transfer> transfers;
    std::set<uint64_t> dedicatedPending;
    std::multimap<uint64_t, GroupPtr> sharedReady;
    uint64_t sequence = 0;
};

} // namespace gem5::pcie

#endif // __DEV_PCI_PCIE_SHARED_FC_HH__
