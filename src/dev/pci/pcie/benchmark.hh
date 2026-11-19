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

#ifndef __DEV_PCI_PCIE_BENCHMARK_HH__
#define __DEV_PCI_PCIE_BENCHMARK_HH__

#include <memory>
#include <unordered_map>

#include "mem/port.hh"
#include "params/PcieBenchmark.hh"
#include "sim/clocked_object.hh"
#include "sim/eventq.hh"

namespace gem5::pcie
{

class Benchmark : public ClockedObject
{
  public:
    explicit Benchmark(const PcieBenchmarkParams &params);
    Port &getPort(const std::string &if_name, PortID idx) override;
    void startup() override;

    bool
    finished() const
    {
        return retiredCount == requestCount;
    }
    unsigned
    accepted() const
    {
        return acceptedCount;
    }
    unsigned
    retired() const
    {
        return retiredCount;
    }
    Tick
    firstAcceptedTick() const
    {
        return firstAccepted;
    }
    Tick
    lastRetiredTick() const
    {
        return lastRetired;
    }
    bool verifyWrites();

  private:
    class BenchmarkPort : public RequestPort
    {
      public:
        explicit BenchmarkPort(Benchmark &owner);

      protected:
        bool recvTimingResp(PacketPtr pkt) override;
        void recvReqRetry() override;

      private:
        Benchmark &owner;
    };

    struct Outstanding
    {
        PacketId id;
        Addr address;
    };

    bool canIssue() const;
    Addr requestAddress(unsigned index) const;
    void scheduleIssue();
    void issue();
    void retry();
    bool receive(PacketPtr pkt);

    BenchmarkPort port;
    const RequestorID requestorId;
    const Addr address;
    const unsigned transferBytes;
    const unsigned requestCount;
    const unsigned window;
    const Tick period;
    const bool write;
    EventFunctionWrapper issueEvent;
    std::unordered_map<PacketPtr, Outstanding> outstanding;
    std::unique_ptr<Packet> pendingRequest;
    bool waitingForRetry = false;
    unsigned acceptedCount = 0;
    unsigned retiredCount = 0;
    Tick nextIssue = 0;
    Tick firstAccepted = 0;
    Tick lastRetired = 0;
};

} // namespace gem5::pcie

#endif // __DEV_PCI_PCIE_BENCHMARK_HH__
