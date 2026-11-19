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

#include "dev/pci/pcie/benchmark.hh"

#include <algorithm>
#include <limits>
#include <memory>

#include "base/logging.hh"
#include "mem/request.hh"
#include "sim/system.hh"

namespace gem5::pcie
{

namespace
{

constexpr uint8_t WriteByte = 0xa5;

} // anonymous namespace

Benchmark::BenchmarkPort::BenchmarkPort(Benchmark &benchmark)
    : RequestPort(benchmark.name() + ".port"), owner(benchmark)
{}

bool
Benchmark::BenchmarkPort::recvTimingResp(PacketPtr pkt)
{
    return owner.receive(pkt);
}

void
Benchmark::BenchmarkPort::recvReqRetry()
{
    owner.retry();
}

Benchmark::Benchmark(const PcieBenchmarkParams &params)
    : ClockedObject(params),
      port(*this),
      requestorId(params.system->getRequestorId(this)),
      address(params.address),
      transferBytes(params.transfer_bytes),
      requestCount(params.request_count),
      window(params.window),
      period(params.period),
      write(params.write),
      issueEvent([this] { issue(); }, name() + ".issue")
{
    fatal_if(address % 4 || !transferBytes || transferBytes % 4 ||
                 !requestCount || !window,
             "PCIe benchmark needs nonzero request count and window and "
             "DWORD-aligned address and transfer size");
    const Addr extent = static_cast<Addr>(transferBytes) * requestCount - 1;
    fatal_if(address > std::numeric_limits<Addr>::max() - extent,
             "PCIe benchmark address range overflows");
}

Port &
Benchmark::getPort(const std::string &if_name, PortID idx)
{
    if (if_name == "port") {
        return port;
    }
    return ClockedObject::getPort(if_name, idx);
}

void
Benchmark::startup()
{
    ClockedObject::startup();
    nextIssue = clockEdge(Cycles(1));
    scheduleIssue();
}

bool
Benchmark::canIssue() const
{
    return !waitingForRetry && acceptedCount < requestCount &&
           outstanding.size() < window;
}

Addr
Benchmark::requestAddress(unsigned index) const
{
    return address + static_cast<Addr>(index) * transferBytes;
}

void
Benchmark::scheduleIssue()
{
    if (canIssue() && !issueEvent.scheduled()) {
        schedule(issueEvent, std::max(nextIssue, curTick()));
    }
}

void
Benchmark::issue()
{
    while (canIssue() && curTick() >= nextIssue) {
        const Addr request_address = requestAddress(acceptedCount);
        if (!pendingRequest) {
            auto request = std::make_shared<Request>(
                request_address, transferBytes, Request::Flags{}, requestorId);
            pendingRequest.reset(write ? Packet::createWrite(request)
                                       : Packet::createRead(request));
            pendingRequest->allocate();
            // Seed Read buffers with nonzero data to catch missing copies.
            std::fill_n(pendingRequest->getPtr<uint8_t>(), transferBytes,
                        WriteByte);
        }

        const Outstanding info{pendingRequest->id, request_address};
        if (!port.sendTimingReq(pendingRequest.get())) {
            waitingForRetry = true;
            return;
        }

        outstanding.emplace(pendingRequest.release(), info);
        if (acceptedCount++ == 0) {
            firstAccepted = curTick();
        }
        if (acceptedCount < requestCount) {
            panic_if(period > MaxTick - curTick(),
                     "PCIe benchmark issue tick overflows");
            nextIssue = curTick() + period;
        }
    }
    scheduleIssue();
}

void
Benchmark::retry()
{
    panic_if(!waitingForRetry || !pendingRequest,
             "PCIe benchmark received an unexpected request retry");
    waitingForRetry = false;
    scheduleIssue();
}

bool
Benchmark::receive(PacketPtr pkt)
{
    const auto it = outstanding.find(pkt);
    panic_if(it == outstanding.end() || it->second.id != pkt->id ||
                 it->second.address != pkt->getAddr(),
             "PCIe benchmark received an unknown or duplicate response");
    const auto expected = write ? MemCmd::WriteResp : MemCmd::ReadResp;
    panic_if(pkt->cmd != expected || pkt->getSize() != transferBytes,
             "PCIe benchmark received an invalid response");
    if (!write) {
        const auto *data = pkt->getConstPtr<uint8_t>();
        panic_if(std::any_of(data, data + transferBytes,
                             [](uint8_t byte) { return byte != 0; }),
                 "PCIe benchmark Read returned nonzero data at address %#x",
                 pkt->getAddr());
    }

    outstanding.erase(it);
    delete pkt;
    ++retiredCount;
    lastRetired = curTick();
    scheduleIssue();
    return true;
}

bool
Benchmark::verifyWrites()
{
    if (!write || !finished()) {
        return false;
    }

    // Call only after the fabric has drained its posted writes.
    for (unsigned index = 0; index < requestCount; ++index) {
        auto request =
            std::make_shared<Request>(requestAddress(index), transferBytes,
                                      Request::Flags{}, requestorId);
        std::unique_ptr<Packet> pkt(Packet::createRead(request));
        pkt->allocate();
        std::fill_n(pkt->getPtr<uint8_t>(), transferBytes, 0);
        port.sendFunctional(pkt.get());
        if (pkt->cmd != MemCmd::ReadResp || pkt->getSize() != transferBytes) {
            return false;
        }
        const auto *data = pkt->getConstPtr<uint8_t>();
        if (std::any_of(data, data + transferBytes,
                        [](uint8_t byte) { return byte != WriteByte; })) {
            return false;
        }
    }
    return true;
}

} // namespace gem5::pcie
