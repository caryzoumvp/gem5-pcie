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

#ifndef __DEV_PCI_PCIE_WORKLOAD_HH__
#define __DEV_PCI_PCIE_WORKLOAD_HH__

#include <array>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "dev/pci/pcie/tlp.hh"
#include "dev/pci/pcie/workload_client.h"
#include "mem/port.hh"
#include "params/PcieWorkload.hh"
#include "sim/clocked_object.hh"
#include "sim/eventq.hh"

namespace gem5::pcie
{

class PcieTiming;

class Workload : public ClockedObject
{
  public:
    explicit Workload(const PcieWorkloadParams &params);
    ~Workload() override;

    Port &getPort(const std::string &if_name, PortID idx) override;
    void init() override;
    void startup() override;

    bool finished() const;
    using Completion = std::function<void(Tick, bool)>;

    // On rejection ownership stays with the caller. Offsets are within the
    // named destination's address window. Callbacks are delivered on the
    // workload's gem5 queue, after the timing port call has returned.
    bool submit(size_t source, size_t destination, Addr offset, unsigned bytes,
                bool write, uint8_t pattern, Completion source_done,
                Completion destination_done);
    bool submitExternal(size_t source, size_t destination, Addr offset,
                        unsigned bytes, bool write, uint8_t pattern);
    bool scheduleExternal(Tick when, std::function<void()> callback);
    // Rows contain acceptance, local response, and completion ticks, then
    // status (0 pending, 1 passed, 2 failed). MaxTick marks an absent event.
    std::vector<std::vector<Tick>> flowTrace() const;
    // Rows contain flow index, edge, forward flag, TLP class, and count.
    std::vector<std::array<uint64_t, 5>> linkTrace() const;
    // Rows contain source, destination, address, bytes, write, pattern and
    // release tick. These are only available for native/dynamic clients.
    std::vector<std::array<uint64_t, 7>> externalFlows() const;

  private:
    class SourcePort : public RequestPort
    {
      public:
        SourcePort(Workload &workload, PortID index);

        std::deque<size_t> ready;
        std::unique_ptr<Packet> pending;
        bool blocked = false;

      protected:
        bool recvTimingResp(PacketPtr pkt) override;
        void recvReqRetry() override;

      private:
        Workload &owner;
    };

    enum Status
    {
        Pending,
        Passed,
        Failed
    };

    struct Flow
    {
        size_t source;
        Addr address;
        unsigned bytes;
        bool write;
        uint8_t pattern;
        Tick release;
        size_t predecessors;
        std::vector<size_t> successors;

        Tick accepted = MaxTick;
        Tick responded = MaxTick;
        Tick completed = MaxTick;
        Status status = Pending;
        bool failed = false;
        Completion sourceDone = {};
        Completion destinationDone = {};
        size_t destination = 0;
    };

    struct InFlight
    {
        size_t index;
        // PacketId is derived from the Request pointer. Keep it alive after
        // the local Write response until destination visibility is reported.
        RequestPtr request;
    };

    void scheduleService();
    void service();
    bool receive(PortID source, PacketPtr pkt);
    void visible(PacketId id, bool success);
    void recordLink(PacketId id, size_t edge, bool forward, TlpClass type);
    void retire(PacketId id);
    void finish(size_t index);
    void enqueueCallback(Tick when, std::function<void()> callback);
    void processCallback();
    void notify(Completion &callback, bool success);
    void startExternal();

    PcieTiming &dut;
    const RequestorID requestorId;
    const size_t externalFlowLimit;
    EventFunctionWrapper serviceEvent;
    EventFunctionWrapper callbackEvent;
    std::vector<std::unique_ptr<SourcePort>> ports;
    std::vector<Flow> flows;
    std::set<std::pair<Tick, size_t>> releases;
    std::unordered_map<PacketId, InFlight> inFlight;
    std::map<std::tuple<size_t, size_t, bool, TlpClass>, uint64_t> linkCounts;
    std::map<Tick, std::deque<std::function<void()>>> callbacks;
    void *clientLibrary = nullptr;
    Gem5PcieClient client{};
    size_t finishedFlows = 0;
    size_t controlCallbacks = 0;
};

} // namespace gem5::pcie

#endif // __DEV_PCI_PCIE_WORKLOAD_HH__
