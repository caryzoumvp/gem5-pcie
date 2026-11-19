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

#ifndef __DEV_PCI_PCIE_SWITCH_CORE_HH__
#define __DEV_PCI_PCIE_SWITCH_CORE_HH__

#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "dev/pci/pcie/core_resources.hh"
#include "dev/pci/pcie/link.hh"
#include "dev/pci/pcie/ordering.hh"

namespace gem5::pcie
{

struct ForwardTlp
{
    using Payload = std::shared_ptr<const std::vector<uint8_t>>;

    LinkDirection::Id id;
    uint32_t ingress;
    uint32_t egress;
    uint8_t vc;
    TlpClass type;
    uint16_t partition;
    uint8_t trafficClass;
    std::optional<CompletionIdentity> completion;
    uint32_t headerBytes;
    Payload payload;

    uint32_t dataBytes() const;
    uint32_t wireBytes() const;
};

struct SwitchLimits
{
    uint32_t ports;
    uint32_t bankCount;
    uint32_t fabricChannels;
    uint32_t portsPerSlice;
    uint32_t cellBytes;
    uint32_t cellsPerCycle;
    uint32_t sectorBytes;
    uint32_t voqCells;
    uint32_t stagingCells;
    std::array<uint32_t, 3> stagingMinimums{};
    CoreCapacity ingress;
    CoreResources::ClassMinimums ingressMinimums{};
    Tick cycle;
    Tick crossSliceDelay;
    Tick linkCycle = 0;
    uint32_t parserSlots = 0;
    uint32_t parserCycles = 0;
    uint32_t ingressCdcCycles = 0;
    uint32_t egressCdcCycles = 0;
    bool pipelined = false;
};

/** Bounded store-and-forward switch. The caller supplies an RX-backed TLP
 * with its resolved egress. Link arrival and departure use stable IDs.
 */
class SwitchCore
{
  public:
    using Id = LinkDirection::Id;
    using ReleaseRx = LinkDirection::TransferEvent;
    using Transmit = std::function<SendResult(Tick, const ForwardTlp &)>;
    enum class Stage : uint8_t
    {
        Rx,
        ParseStart,
        ParseDone,
        CoreReady,
        WriteStart,
        RxRelease,
        FabricGrant,
        FabricDone,
        TxReady,
        TxAccepted,
        TxDeparted,
        ReadLaneRelease,
        WriteContinuation,
        FabricContinuation
    };
    struct TraceEvent
    {
        Stage stage;
        Tick tick;
        Id id;
        uint32_t resource;
        bool operator==(const TraceEvent &) const = default;
    };
    using Trace = std::function<void(TraceEvent)>;

    struct Counts
    {
        size_t rxWaiting = 0;
        size_t coreDescriptors = 0;
        size_t stagedCells = 0;
        size_t transfers = 0;
        uint64_t grants = 0;
        uint64_t transmissions = 0;
        size_t peakRx = 0;
        size_t peakCore = 0;
        size_t peakStaging = 0;
        uint64_t admissionStalls = 0;
        uint64_t bankStalls = 0;
        uint64_t fabricStalls = 0;
        uint64_t stagingStalls = 0;
        uint64_t orderingStalls = 0;
        uint64_t writeBeats = 0;
        uint64_t readBeats = 0;
    };

    SwitchCore(SwitchLimits limits, LinkDirection::Schedule schedule,
               Transmit transmit,
               LinkDirection::Schedule service_schedule = {},
               Trace trace = {});
    SwitchCore(const SwitchCore &) = delete;
    SwitchCore &operator=(const SwitchCore &) = delete;

    /** Call after RX backing is occupied. ReleaseRx returns the backing. */
    void receive(Tick now, ForwardTlp tlp, ReleaseRx release_rx);
    /** Call once when the last byte entered the outbound link. */
    void departed(Tick now, Id id);
    void wake(Tick now);

    Counts counts() const;
    std::vector<std::string> waits() const;
    bool empty() const;

  private:
    enum class Phase
    {
        Rx,
        Writing,
        Ready,
        Fabric,
        Staged,
        Tx,
        Parsing,
        IngressCdc,
        EgressCdc
    };

    struct QueueKey
    {
        uint32_t ingress;
        VoqKey voq;

        auto operator<=>(const QueueKey &) const = default;
    };

    struct Transfer
    {
        ForwardTlp tlp;
        Ordering::Id order;
        ReleaseRx releaseRx;
        uint32_t cells;
        Tick receivedAt;
        Tick stagedAt = 0;
        uint64_t grantSequence = 0;
        Phase phase = Phase::Rx;
        CoreResources::ReservationId reservation = 0;

        QueueKey key() const;
    };

    struct Voq
    {
        std::deque<Id> ready;
        uint32_t usedCells = 0;
    };

    struct Egress
    {
        std::array<uint32_t, 3> used{};
        std::deque<Id> staged;

        uint32_t total() const;
    };

    void pump(Tick now);
    void queueParser(Tick now, uint32_t ingress);
    void launchParser(Tick now, uint32_t ingress);
    void coreReady(Tick now, Id id);
    void txReady(Tick now, Id id);
    void traceStage(Stage stage, Tick now, Id id, uint32_t resource) const;
    void admit(Tick now);
    void grant(Tick now);
    void transmitStaged(Tick now);
    bool canFollow(const Transfer &entry, bool at_tx) const;
    void releaseService(Tick now, Id id, size_t bank, size_t channel);
    void serveBeats(Tick now, Id id, uint32_t cells, Stage stage,
                    uint32_t resource, LinkDirection::Event complete);
    bool canStage(const Transfer &transfer) const;
    Tick service(uint32_t cells) const;

    SwitchLimits limits;
    LinkDirection::Schedule schedule;
    LinkDirection::Schedule serviceSchedule;
    Transmit transmit;
    Trace trace;
    Ordering ordering;
    std::vector<std::unique_ptr<CoreResources>> resources;
    std::vector<Egress> egress;
    std::vector<bool> readBusy;
    std::vector<bool> writeBusy;
    std::vector<bool> fabricBusy;
    std::vector<std::deque<Id>> parserQueues;
    std::vector<bool> parserPending;
    std::map<Id, Transfer> active;
    std::map<Ordering::Id, Id> orderingOwners;
    std::deque<Id> rx;
    std::map<QueueKey, Voq> voqs;
    std::optional<QueueKey> lastGrant;
    bool servicePending = false;
    uint64_t grants = 0;
    uint64_t transmissions = 0;
    size_t coreReserved = 0;
    size_t stagingReserved = 0;
    Counts observations;
};

} // namespace gem5::pcie

#endif // __DEV_PCI_PCIE_SWITCH_CORE_HH__
