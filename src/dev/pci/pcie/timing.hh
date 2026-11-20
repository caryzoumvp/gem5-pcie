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

#ifndef __DEV_PCI_PCIE_TIMING_HH__
#define __DEV_PCI_PCIE_TIMING_HH__

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <vector>

#include "dev/pci/pcie/endpoint_memory.hh"
#include "dev/pci/pcie/flit_link.hh"
#include "dev/pci/pcie/switch_core.hh"
#include "dev/pci/pcie/tree_topology.hh"
#include "mem/port.hh"
#include "params/PcieTiming.hh"
#include "sim/clocked_object.hh"

namespace gem5::pcie
{

/** Native event integration for transparent synthetic PCIe topologies. */
class PcieTiming : public ClockedObject
{
  public:
    explicit PcieTiming(const PcieTimingParams &params);
    void init() override;
    Port &getPort(const std::string &name, PortID idx) override;
    DrainState drain() override;
    void drainResume() override;

    bool idle() const;
    uint64_t
    visibleWrites() const
    {
        return writes;
    }
    uint64_t
    completedReads() const
    {
        return reads;
    }
    std::map<std::string, uint64_t> report() const;
    std::vector<std::string> waitGraph() const;
    const std::vector<std::array<uint64_t, 5>> &coreTrace() const;
    using VisibilityObserver = std::function<void(PacketId, bool)>;
    using RouteObserver =
        std::function<void(PacketId, size_t, bool, TlpClass)>;
    void setVisibilityObserver(VisibilityObserver observer);
    void setRouteObserver(RouteObserver observer);
    std::optional<Addr> treeTransferAddress(size_t destination, Addr offset,
                                            unsigned bytes) const;

  private:
    class CpuPort : public ResponsePort
    {
      public:
        CpuPort(PcieTiming &owner, PortID index);
        AddrRangeList getAddrRanges() const override;

      protected:
        bool recvTimingReq(PacketPtr packet) override;
        void recvRespRetry() override;
        Tick recvAtomic(PacketPtr packet) override;
        void recvFunctional(PacketPtr packet) override;

      private:
        PcieTiming &owner;
        const PortID index;
    };

    using Id = LinkDirection::Id;

    struct Source
    {
        std::unique_ptr<CpuPort> port;
        std::deque<Id> injection;
        std::deque<Id> responses;
        std::optional<Id> candidate;
        size_t outstanding = 0;
        bool retryRequest = false;
        bool responseBlocked = false;
        uint64_t accepted = 0;
        uint64_t writes = 0;
        uint64_t reads = 0;
        uint64_t completedWritePackets = 0;
        uint64_t completedPayloadBytes = 0;
    };

    struct Request
    {
        std::unique_ptr<Packet> packet;
        Op op;
        size_t source;
        PacketId packetId;
        Addr base;
        uint32_t bytes;
        std::vector<ByteRange> segments;
        ForwardTlp::Payload data;
        size_t next = 0;
        size_t remaining = 0;
        bool failed = false;
    };

    struct Message
    {
        Id parent;
        TlpClass type;
        ByteRange range;
        ForwardTlp::Payload payload;
        std::optional<ReadKey> key;
        CompletionStatus status = CompletionStatus::Success;
        uint16_t byteCount = 0;
        uint8_t lowerAddress = 0;

        uint32_t dataBytes() const;
        uint32_t headerBytes(bool detailed = false) const;
    };

    struct Direction
    {
        std::unique_ptr<LinkDirection> tlp;
        std::unique_ptr<FlitLink> flit;
        std::array<uint64_t, 3> accepted{};
        uint64_t tlpBytes = 0;
        uint64_t dataBytes = 0;
        uint64_t creditStalls = 0;
        uint64_t busyStalls = 0;

        bool empty() const;
        bool release(Tick now, Id id);
    };

    struct DirectionPair
    {
        Direction forward;
        Direction reverse;
    };

    struct Target
    {
        std::deque<Id> rx;
        std::deque<Id> completions;
        bool busy = false;
        bool pauseStarted = false;
        bool paused = false;
        uint64_t received = 0;
        uint64_t served = 0;
    };

    void defer(Tick when, LinkDirection::Event action,
               Event::Priority priority = Event::Default_Pri);
    void wake();
    void pump();
    bool accept(size_t source, PacketPtr packet);
    void accessDrainedMemory(PacketPtr packet);
    void queueResponse(Id parent);
    void sendResponses(Source &source);
    void inject(size_t index);
    void receiveTarget(size_t index, Id id);
    void serveTarget(size_t index);
    void sendCompletions(size_t index);
    void targetDone(size_t index, Id id);
    void sourceCompletion(size_t index, Id id);
    void arrive(size_t edge, bool forward, Id id);
    void arriveTree(TreeTopology::Direction wire, Id id);
    void enterCore(size_t core, uint32_t ingress, uint32_t egress,
                   TreeTopology::Direction wire, Id id);
    void arriveFlit(size_t edge, bool forward, Id id, const FlitTlp &packet);
    void departure(size_t edge, bool forward, Id id);
    void progressDrain();
    void finishPiece(Id parent, bool failed);
    void retire(Id parent);
    Id makeMessage(Message message);
    Direction &link(size_t edge, bool forward);
    FlitTlp makeFlitTlp(const Message &msg) const;
    SendResult transmit(size_t edge, bool forward, Id id);
    std::optional<ReadKey> allocateTag(size_t index, ByteRange range);
    size_t targetEdge(size_t index = 0) const;
    size_t targetForAddress(Addr address) const;
    size_t targetForRange(Addr address, uint32_t bytes) const;
    uint16_t requesterId(size_t index) const;
    uint16_t completerId(Addr address) const;
    EndpointMemory &targetMemory(size_t index);
    TreeTopology::Direction sourceWire(size_t index) const;
    TreeTopology::Direction targetWire(size_t index) const;
    LinkTiming treeLinkTiming(TreeTopology::Direction wire) const;
    void buildTree();
    void configureTreeSwitch(size_t index, SwitchLimits &limits) const;
    std::optional<size_t> sendingCore(size_t edge, bool forward) const;

    const PcieTimingParams &settings;
    const AddrRange window;
    EndpointMemory memory;
    std::unique_ptr<TreeTopology> tree;
    std::vector<AddrRange> treeWindows;
    std::vector<std::unique_ptr<EndpointMemory>> treeMemories;
    std::map<uint16_t, size_t> requesterLeaves;
    ReadTracker tracker;
    std::vector<Source> sources;
    std::vector<Target> targets;
    std::vector<DirectionPair> links;
    std::vector<std::unique_ptr<SwitchCore>> cores;
    std::map<Id, Request> requests;
    std::map<Id, Message> messages;
    Id nextId = 1;
    bool pumpPending = false;
    size_t nativeEvents = 0;
    uint64_t writes = 0;
    uint64_t reads = 0;
    std::vector<std::array<uint64_t, 5>> coreEvents;
    uint64_t traceDropped = 0;
    uint64_t accepted = 0;
    uint64_t requestStalls = 0;
    uint64_t responseStalls = 0;
    uint64_t creditStalls = 0;
    uint64_t targetCreditStalls = 0;
    uint64_t wireStalls = 0;
    Tick firstWriteResponse = 0;
    Tick firstWriteVisible = 0;
    Tick lastReadComplete = 0;
    VisibilityObserver visibilityObserver;
    RouteObserver routeObserver;
};

} // namespace gem5::pcie

#endif // __DEV_PCI_PCIE_TIMING_HH__
