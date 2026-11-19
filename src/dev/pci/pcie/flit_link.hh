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

#ifndef __DEV_PCI_PCIE_FLIT_LINK_HH__
#define __DEV_PCI_PCIE_FLIT_LINK_HH__

#include <deque>
#include <functional>
#include <map>
#include <optional>

#include "dev/pci/pcie/flit_replay.hh"
#include "dev/pci/pcie/flit_tlp.hh"
#include "dev/pci/pcie/shared_fc.hh"

namespace gem5::pcie
{

struct FlitLinkConfig
{
    LinkTiming timing;
    SharedFcConfig credits;
    size_t replayFlits;
    Tick flushDelay;
    Tick feedbackDelay;
    Tick replayTimeout;
    bool corruptFirst = false;
    bool dropFirstAck = false;
};

class FlitLink
{
  public:
    using Id = LinkDirection::Id;
    using Arrival = std::function<void(Tick, Id, const FlitTlp &)>;

    struct Counts
    {
        uint64_t original = 0;
        uint64_t replayed = 0;
        uint64_t crcErrors = 0;
        uint64_t duplicates = 0;
        uint64_t timeouts = 0;
        uint64_t sharedAdmissions = 0;
        uint64_t dedicatedAdmissions = 0;
        uint64_t serializedTicks = 0;
        uint64_t controlFlits = 0;
        uint64_t controlTicks = 0;
    };

    FlitLink(FlitLinkConfig cfg, LinkDirection::Schedule schedule,
             Arrival arrival, LinkDirection::Ready ready,
             LinkDirection::TransferEvent departure);
    FlitLink(const FlitLink &) = delete;
    FlitLink &operator=(const FlitLink &) = delete;
    void connectFeedback(FlitLink &reverse);

    SendResult send(Tick now, Id id, CreditChannel channel,
                    const FlitTlp &tlp);
    bool release(Tick now, Id id);
    bool empty() const;
    const Counts &
    counts() const
    {
        return stats;
    }

  private:
    enum class Owner
    {
        Admitted,
        Received,
        Returning
    };

    void wake(Tick now);
    void pump(Tick now);
    void finishWire(Tick now);
    void transmitFrame(Tick now, ReplayFrame frame, bool replayed,
                       const std::vector<size_t> &ends);
    void receiveFrame(Tick now, FlitRecord frame);
    void feedback(Tick now, ReplayDisposition disposition);
    void queueFeedback(Tick now, Tick eligible, ReplayDisposition disposition);
    void transmitControl(Tick now);
    void receiveFeedback(Tick now, const FlitRecord &control);
    void armTimeout(Tick now);
    Tick after(Tick now, Tick offset) const;
    Tick wireTime(size_t bytes) const;

    const FlitLinkConfig cfg;
    LinkDirection::Schedule schedule;
    Arrival arrival;
    LinkDirection::Ready ready;
    LinkDirection::TransferEvent departure;
    SharedFc credits;
    FlitPacker packer;
    FlitTlpAssembler assembler;
    ReplayTx tx;
    ReplayRx rx;
    std::deque<Id> transmitOrder;
    std::deque<Id> receiveOrder;
    std::map<Id, Owner> owners;
    bool wireBusy = false;
    std::optional<Tick> flushAt;
    std::optional<Tick> pumpAt;
    bool timeoutArmed = false;
    uint64_t timeoutEpoch = 0;
    bool priorPayload = false;
    bool corrupted = false;
    bool droppedAck = false;
    std::optional<uint16_t> nakPending;
    struct PendingFeedback
    {
        Tick eligible;
        ReplayDisposition disposition;
    };
    FlitLink *peer = nullptr;
    std::optional<PendingFeedback> pendingFeedback;
    size_t controlInFlight = 0;
    Counts stats;
};

} // namespace gem5::pcie

#endif // __DEV_PCI_PCIE_FLIT_LINK_HH__
