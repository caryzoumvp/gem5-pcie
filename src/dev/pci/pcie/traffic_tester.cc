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

#include "dev/pci/pcie/traffic_tester.hh"

#include <algorithm>
#include <limits>
#include <memory>

#include "base/logging.hh"
#include "mem/request.hh"
#include "sim/sim_exit.hh"
#include "sim/system.hh"

namespace gem5::pcie
{

namespace
{

constexpr Addr DataAddress = 0x1ffc;
constexpr Addr PageSize = 4096;
constexpr Addr RetryAddress = 0x3000;
constexpr unsigned RetrySize = 16;
constexpr Addr CrossPageMin = PageSize - DataAddress % PageSize + 1;
constexpr Addr MaxDataSize = RetryAddress - DataAddress;
constexpr Addr MaxAddressOffset =
    std::numeric_limits<Addr>::max() - (RetryAddress + RetrySize - 1);
constexpr Cycles FinishDelay{500};
constexpr Cycles ResponseRetryDelay{10};

uint8_t
dataByte(unsigned offset, Addr address_offset)
{
    return (37 * offset + 13 * (offset / 256) + 11 +
            address_offset / PageSize) &
           0xff;
}

} // anonymous namespace

TrafficTester::TestPort::TestPort(TrafficTester &owner)
    : RequestPort(owner.name() + ".port"), tester(owner)
{}

bool
TrafficTester::TestPort::recvTimingResp(PacketPtr pkt)
{
    return tester.receiveResponse(pkt);
}

void
TrafficTester::TestPort::recvReqRetry()
{
    tester.retryRequest();
}

TrafficTester::TrafficTester(const PcieTrafficTesterParams &params)
    : ClockedObject(params),
      port(*this),
      requestorId(params.system->getRequestorId(this)),
      addressOffset(params.address_offset),
      dataSize(params.data_size),
      visibilityDelay(params.visibility_delay),
      exitOnFinish(params.exit_on_finish),
      writeEvent([this] { sendWrites(); }, name() + ".sendWrites"),
      readEvent([this] { sendRead(); }, name() + ".sendRead"),
      responseRetryEvent([this] { port.sendRetryResp(); },
                         name() + ".retryResponse"),
      finishEvent(
          [this] {
              testFinished = true;
              if (exitOnFinish) {
                  exitSimulationLoopClassic("PCIe timing packet test passed");
              }
          },
          name() + ".finish")
{
    fatal_if(addressOffset % PageSize || dataSize < CrossPageMin ||
                 dataSize > MaxDataSize || addressOffset > MaxAddressOffset,
             "PCIe tester needs a page-aligned offset and non-overlapping "
             "cross-page and retry writes");
}

Port &
TrafficTester::getPort(const std::string &if_name, PortID idx)
{
    if (if_name == "port") {
        return port;
    }
    return ClockedObject::getPort(if_name, idx);
}

void
TrafficTester::startup()
{
    ClockedObject::startup();
    schedule(writeEvent, clockEdge(Cycles(1)));
}

void
TrafficTester::sendWrites()
{
    auto first = std::make_shared<Request>(
        DataAddress + addressOffset, dataSize, Request::Flags{}, requestorId);
    writes[0] = Packet::createWrite(first);
    writes[0]->allocate();
    auto *data = writes[0]->getPtr<uint8_t>();
    for (unsigned i = 0; i < dataSize; ++i) {
        data[i] = dataByte(i, addressOffset);
    }
    sendRequest(writes[0]);
    panic_if(pendingRequest, "PCIe tester's first write was rejected");

    auto second =
        std::make_shared<Request>(RetryAddress + addressOffset, RetrySize,
                                  Request::Flags{}, requestorId);
    writes[1] = Packet::createWrite(second);
    writes[1]->allocate();
    std::fill_n(writes[1]->getPtr<uint8_t>(), RetrySize, 0x9b);
    sendRequest(writes[1]);
    panic_if(!pendingRequest, "PCIe tester's second write was not stalled");
}

void
TrafficTester::sendRead()
{
    auto request = std::make_shared<Request>(
        DataAddress + addressOffset, dataSize, Request::Flags{}, requestorId);
    read = Packet::createRead(request);
    read->allocate();
    sendRequest(read);
}

bool
TrafficTester::verifyLocalRead()
{
    auto request = std::make_shared<Request>(
        DataAddress + addressOffset, dataSize, Request::Flags{}, requestorId);
    std::unique_ptr<Packet> check(Packet::createRead(request));
    check->allocate();
    port.sendFunctional(check.get());
    if (check->cmd != MemCmd::ReadResp) {
        return false;
    }
    for (unsigned i = 0; i < dataSize; ++i) {
        if (check->getConstPtr<uint8_t>()[i] != dataByte(i, addressOffset)) {
            return false;
        }
    }
    // This address is outside every address window in the fixture.
    auto invalid = std::make_shared<Request>(0x1'0000'0000, 4,
                                             Request::Flags{}, requestorId);
    std::unique_ptr<Packet> unmapped(Packet::createRead(invalid));
    unmapped->allocate();
    return port.sendAtomic(unmapped.get()) == 0 &&
           unmapped->cmd == MemCmd::BadAddressError;
}

void
TrafficTester::sendRequest(PacketPtr pkt)
{
    panic_if(pendingRequest, "PCIe tester sent a request before retry");
    if (!port.sendTimingReq(pkt)) {
        pendingRequest = pkt;
    }
}

void
TrafficTester::retryRequest()
{
    panic_if(!pendingRequest, "PCIe tester received an unexpected retry");
    PacketPtr pkt = pendingRequest;
    pendingRequest = nullptr;
    sawRequestRetry = true;
    sendRequest(pkt);
}

bool
TrafficTester::receiveResponse(PacketPtr pkt)
{
    const auto write = std::find(writes.begin(), writes.end(), pkt);
    panic_if(write == writes.end() && pkt != read,
             "PCIe tester received an unknown or duplicate response");
    panic_if(!pkt->isResponse() || pkt->isError(),
             "PCIe tester received an invalid response command");

    if (responses == 0 && !blockedResponse) {
        blockedResponse = pkt;
        schedule(responseRetryEvent, clockEdge(ResponseRetryDelay));
        return false;
    }
    if (blockedResponse) {
        panic_if(responseRetryEvent.scheduled() || pkt != blockedResponse,
                 "PCIe tester received a response before it allowed retry");
        blockedResponse = nullptr;
    }

    ++responses;
    if (write != writes.end()) {
        panic_if(pkt->cmd != MemCmd::WriteResp,
                 "PCIe tester expected a WriteResp");
        *write = nullptr;
        if (responses == writes.size()) {
            // A posted write response only acknowledges source acceptance.
            schedule(readEvent, clockEdge(visibilityDelay));
        }
    } else {
        panic_if(pkt->cmd != MemCmd::ReadResp || pkt->getSize() != dataSize,
                 "PCIe tester expected a full ReadResp");
        const auto *data = pkt->getConstPtr<uint8_t>();
        for (unsigned i = 0; i < dataSize; ++i) {
            panic_if(data[i] != dataByte(i, addressOffset),
                     "PCIe tester read data mismatch at byte %u", i);
        }
        panic_if(responses != writes.size() + 1 || !sawRequestRetry,
                 "PCIe tester missed an expected response or request retry");
        read = nullptr;
        schedule(finishEvent, clockEdge(FinishDelay));
    }
    delete pkt;
    return true;
}

} // namespace gem5::pcie
