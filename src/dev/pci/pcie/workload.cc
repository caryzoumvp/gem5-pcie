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

#include "dev/pci/pcie/workload.hh"

#include <dlfcn.h>

#include <algorithm>
#include <memory>

#include "base/logging.hh"
#include "dev/pci/pcie/timing.hh"
#include "mem/request.hh"
#include "sim/core.hh"
#include "sim/system.hh"

namespace gem5::pcie
{

Workload::SourcePort::SourcePort(Workload &workload, PortID index)
    : RequestPort(workload.name() + ".ports", index), owner(workload)
{}

bool
Workload::SourcePort::recvTimingResp(PacketPtr pkt)
{
    return owner.receive(id, pkt);
}

void
Workload::SourcePort::recvReqRetry()
{
    panic_if(!blocked || !pending, "Unexpected PCIe workload request retry");
    blocked = false;
    owner.scheduleService();
}

Workload::Workload(const PcieWorkloadParams &p)
    : ClockedObject(p),
      dut(*p.dut),
      requestorId(p.system->getRequestorId(this)),
      externalFlowLimit(p.external_flow_limit),
      // Process workload releases after the timing fabric services this tick.
      serviceEvent([this] { service(); }, name() + ".service", false,
                   Event::Default_Pri + 2),
      callbackEvent([this] { processCallback(); }, name() + ".callback", false,
                    Event::Default_Pri + 1)
{
    const size_t count = p.flow_ids.size();
    fatal_if(externalFlowLimit && count,
             "%s: static and external flow drivers are mutually exclusive",
             name());
    fatal_if(!p.external_library.empty() && !externalFlowLimit,
             "%s: a native workload requires a positive flow limit", name());
    fatal_if(
        p.source_indices.size() != count || p.addresses.size() != count ||
            p.transfer_bytes.size() != count || p.writes.size() != count ||
            p.patterns.size() != count || p.releases.size() != count ||
            p.dependency_offsets.size() != count + 1 ||
            p.dependency_offsets.front() != 0 ||
            p.dependency_offsets.back() != p.dependency_indices.size() ||
            !std::is_sorted(p.dependency_offsets.begin(),
                            p.dependency_offsets.end()),
        "%s: invalid workload vector lengths or dependency offsets", name());

    for (unsigned i = 0; i < p.port_ports_connection_count; ++i) {
        ports.push_back(std::make_unique<SourcePort>(*this, i));
    }
    flows.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const auto start = p.dependency_offsets[i];
        const auto end = p.dependency_offsets[i + 1];
        fatal_if(p.source_indices[i] >= ports.size() ||
                     p.releases[i] == MaxTick,
                 "%s: invalid requester or release tick for flow %s", name(),
                 p.flow_ids[i]);
        flows.push_back({p.source_indices[i],
                         p.addresses[i],
                         p.transfer_bytes[i],
                         p.writes[i],
                         p.patterns[i],
                         p.releases[i],
                         end - start,
                         {}});
        if (start == end) {
            releases.emplace(p.releases[i], i);
        }
    }

    for (size_t i = 0; i < count; ++i) {
        const auto start = p.dependency_offsets[i];
        const auto end = p.dependency_offsets[i + 1];
        for (size_t edge = start; edge < end; ++edge) {
            const size_t predecessor = p.dependency_indices[edge];
            fatal_if(predecessor >= count || predecessor == i,
                     "%s: invalid predecessor for flow %s", name(),
                     p.flow_ids[i]);
            flows[predecessor].successors.push_back(i);
        }
    }
}

Workload::~Workload()
{
    if (callbackEvent.scheduled()) {
        deschedule(callbackEvent);
    }
    if (serviceEvent.scheduled()) {
        deschedule(serviceEvent);
    }
    callbacks.clear();
    if (client.destroy) {
        client.destroy(client.context);
    }
    if (clientLibrary) {
        dlclose(clientLibrary);
    }
}

bool
Workload::submit(size_t source, size_t destination, Addr offset,
                 unsigned bytes, bool write, uint8_t pattern,
                 Completion source_done, Completion destination_done)
{
    if (flows.size() >= externalFlowLimit || source >= ports.size() ||
        source == destination) {
        return false;
    }
    const auto address = dut.treeTransferAddress(destination, offset, bytes);
    if (!address) {
        return false;
    }
    const size_t index = flows.size();
    flows.push_back(
        {source, *address, bytes, write, pattern, curTick(), 0, {}});
    flows.back().sourceDone = std::move(source_done);
    flows.back().destinationDone = std::move(destination_done);
    flows.back().destination = destination;
    releases.emplace(curTick(), index);
    scheduleService();
    return true;
}

bool
Workload::submitExternal(size_t source, size_t destination, Addr offset,
                         unsigned bytes, bool write, uint8_t pattern)
{
    return submit(source, destination, offset, bytes, write, pattern, {}, {});
}

bool
Workload::scheduleExternal(Tick when, std::function<void()> callback)
{
    if (controlCallbacks >= externalFlowLimit || !callback ||
        when < curTick() || when == MaxTick) {
        return false;
    }
    ++controlCallbacks;
    enqueueCallback(when, [this, callback = std::move(callback)] {
        --controlCallbacks;
        callback();
    });
    return true;
}

void
Workload::enqueueCallback(Tick when, std::function<void()> callback)
{
    callbacks[when].push_back(std::move(callback));
    if (!callbackEvent.scheduled()) {
        schedule(callbackEvent, when);
    } else if (callbackEvent.when() > when) {
        reschedule(callbackEvent, when);
    }
}

void
Workload::processCallback()
{
    auto it = callbacks.begin();
    auto callback = std::move(it->second.front());
    it->second.pop_front();
    if (it->second.empty()) {
        callbacks.erase(it);
    }
    if (!callbacks.empty()) {
        schedule(callbackEvent, callbacks.begin()->first);
    }
    callback();
}

void
Workload::notify(Completion &callback, bool success)
{
    if (callback) {
        const auto tick = curTick();
        enqueueCallback(tick, [handler = std::move(callback), tick, success] {
            handler(tick, success);
        });
    }
}

Port &
Workload::getPort(const std::string &if_name, PortID idx)
{
    if (if_name == "ports") {
        return *ports.at(idx);
    }
    return ClockedObject::getPort(if_name, idx);
}

void
Workload::init()
{
    ClockedObject::init();
    dut.setVisibilityObserver(
        [this](PacketId id, bool success) { visible(id, success); });
    dut.setRouteObserver(
        [this](PacketId id, size_t edge, bool forward, TlpClass type) {
            recordLink(id, edge, forward, type);
        });
}

void
Workload::startup()
{
    ClockedObject::startup();
    if (!static_cast<const PcieWorkloadParams &>(params())
             .external_library.empty()) {
        startExternal();
    }
    scheduleService();
}

bool
Workload::finished() const
{
    const int status = client.status ? client.status(client.context) : 1;
    fatal_if(status < 0, "%s: native workload failed", name());
    return status == 1 && finishedFlows == flows.size() && callbacks.empty();
}

void
Workload::startExternal()
{
    const auto &p = static_cast<const PcieWorkloadParams &>(params());
    clientLibrary = dlopen(p.external_library.c_str(), RTLD_NOW | RTLD_LOCAL);
    fatal_if(!clientLibrary, "%s: loading PCIe workload client: %s", name(),
             dlerror());
    auto open_client =
        reinterpret_cast<Gem5PcieOpen>(dlsym(clientLibrary, "gem5_pcie_open"));
    fatal_if(!open_client, "%s: missing gem5_pcie_open: %s", name(),
             dlerror());
    const Gem5PcieHost host{
        GEM5_PCIE_WORKLOAD_ABI,
        this,
        static_cast<uint32_t>(ports.size()),
        sim_clock::as_int::s,
        [](void *) -> uint64_t { return curTick(); },
        [](void *ctx, uint64_t when, Gem5PcieEvent fun, void *arg) -> int {
            return fun && static_cast<Workload *>(ctx)->scheduleExternal(
                              when, [fun, arg] { fun(arg); });
        },
        [](void *ctx, uint32_t src, uint32_t dst, uint64_t offset,
           uint32_t bytes, int write, uint8_t pattern, Gem5PcieCompletion sent,
           Gem5PcieCompletion received, void *arg) -> int {
            const auto bind = [arg](Gem5PcieCompletion fun) -> Completion {
                if (!fun) {
                    return {};
                }
                return [fun, arg](Tick tick, bool ok) { fun(arg, tick, ok); };
            };
            return static_cast<Workload *>(ctx)->submit(
                src, dst, offset, bytes, write, pattern, bind(sent),
                bind(received));
        },
        [](void *ctx, const char *reason) {
            fatal("%s: native PCIe workload: %s",
                  static_cast<Workload *>(ctx)->name(), reason);
        },
    };
    fatal_if(open_client(&host, p.external_config.c_str(), &client) != 1 ||
                 !client.status || !client.destroy,
             "%s: native PCIe workload client setup failed", name());
}

void
Workload::scheduleService()
{
    Tick when = releases.empty() ? MaxTick : releases.begin()->first;
    if (std::any_of(ports.begin(), ports.end(), [](const auto &port) {
            return !port->ready.empty() && !port->blocked;
        })) {
        when = curTick();
    }
    if (when == MaxTick) {
        return;
    }
    when = std::max(when, curTick());
    if (!serviceEvent.scheduled()) {
        schedule(serviceEvent, when);
    } else if (serviceEvent.when() > when) {
        reschedule(serviceEvent, when);
    }
}

void
Workload::service()
{
    while (!releases.empty() && releases.begin()->first <= curTick()) {
        const auto index = releases.begin()->second;
        ports[flows[index].source]->ready.push_back(index);
        releases.erase(releases.begin());
    }

    for (auto &port : ports) {
        while (!port->blocked && !port->ready.empty()) {
            const auto index = port->ready.front();
            auto &flow = flows[index];
            if (!port->pending) {
                auto req = std::make_shared<Request>(
                    flow.address, flow.bytes, Request::Flags{}, requestorId);
                port->pending.reset(flow.write ? Packet::createWrite(req)
                                               : Packet::createRead(req));
                port->pending->allocate();
                // Seed Reads with the opposite byte to catch missing copies.
                const auto fill =
                    flow.write ? flow.pattern : flow.pattern ^ 0xff;
                std::fill_n(port->pending->getPtr<uint8_t>(), flow.bytes,
                            fill);
            }

            auto *pkt = port->pending.get();
            // Record identity before the call across the port. In case of
            // rejection, the source retains the same Packet for the retry.
            const auto id = pkt->id;
            const auto [entry, inserted] =
                inFlight.emplace(id, InFlight{index, pkt->req});
            panic_if(!inserted, "Duplicate active PCIe workload Packet ID");
            flow.accepted = curTick();
            if (!port->sendTimingReq(pkt)) {
                inFlight.erase(entry);
                flow.accepted = MaxTick;
                port->blocked = true;
                break;
            }
            port->pending.release();
            port->ready.pop_front();
        }
    }
    scheduleService();
}

bool
Workload::receive(PortID source, PacketPtr pkt)
{
    const auto it = inFlight.find(pkt->id);
    panic_if(it == inFlight.end(), "Unknown PCIe workload response");
    auto &flow = flows[it->second.index];
    panic_if(flow.source != static_cast<size_t>(source) ||
                 flow.responded != MaxTick || pkt->getAddr() != flow.address ||
                 pkt->getSize() != flow.bytes,
             "Unexpected PCIe workload response");
    flow.responded = curTick();
    const auto expected = flow.write ? MemCmd::WriteResp : MemCmd::ReadResp;
    flow.failed |= pkt->cmd != expected;
    if (!flow.write) {
        if (!flow.failed) {
            const auto *data = pkt->getConstPtr<uint8_t>();
            flow.failed =
                std::any_of(data, data + flow.bytes, [&flow](uint8_t byte) {
                    return byte != flow.pattern;
                });
        }
        flow.completed = curTick();
    }
    notify(flow.sourceDone, !flow.failed);
    if (!flow.write) {
        notify(flow.destinationDone, !flow.failed);
    }
    const auto id = pkt->id;
    delete pkt;
    retire(id);
    return true;
}

void
Workload::visible(PacketId id, bool success)
{
    const auto it = inFlight.find(id);
    panic_if(it == inFlight.end(), "Unknown PCIe workload Write visibility");
    auto &flow = flows[it->second.index];
    panic_if(!flow.write || flow.completed != MaxTick,
             "Unexpected PCIe workload Write visibility");
    flow.completed = curTick();
    flow.failed |= !success;
    notify(flow.destinationDone, !flow.failed);
    retire(id);
}

void
Workload::recordLink(PacketId id, size_t edge, bool forward, TlpClass type)
{
    const auto it = inFlight.find(id);
    panic_if(it == inFlight.end(), "Unknown PCIe workload link traversal");
    ++linkCounts[{it->second.index, edge, forward, type}];
}

void
Workload::retire(PacketId id)
{
    const auto it = inFlight.find(id);
    const auto index = it->second.index;
    const auto &flow = flows[index];
    if (flow.responded != MaxTick && flow.completed != MaxTick) {
        inFlight.erase(it);
        finish(index);
    }
}

void
Workload::finish(size_t index)
{
    std::vector<size_t> terminal{index};
    while (!terminal.empty()) {
        const auto current = terminal.back();
        terminal.pop_back();
        auto &flow = flows[current];
        flow.status = flow.failed ? Failed : Passed;
        ++finishedFlows;
        for (const auto next : flow.successors) {
            auto &successor = flows[next];
            successor.failed |= flow.failed;
            if (--successor.predecessors == 0) {
                if (successor.failed) {
                    terminal.push_back(next);
                } else {
                    releases.emplace(successor.release, next);
                }
            }
        }
    }
    scheduleService();
}

std::vector<std::vector<Tick>>
Workload::flowTrace() const
{
    std::vector<std::vector<Tick>> trace;
    trace.reserve(flows.size());
    for (const auto &flow : flows) {
        trace.push_back(
            {flow.accepted, flow.responded, flow.completed, flow.status});
    }
    return trace;
}

std::vector<std::array<uint64_t, 5>>
Workload::linkTrace() const
{
    std::vector<std::array<uint64_t, 5>> trace;
    trace.reserve(linkCounts.size());
    for (const auto &[key, count] : linkCounts) {
        const auto &[index, edge, forward, type] = key;
        trace.push_back({index, edge, forward, uint64_t(type), count});
    }
    return trace;
}

std::vector<std::array<uint64_t, 7>>
Workload::externalFlows() const
{
    std::vector<std::array<uint64_t, 7>> specs;
    if (externalFlowLimit) {
        specs.reserve(flows.size());
        for (const auto &flow : flows) {
            specs.push_back({flow.source, flow.destination, flow.address,
                             flow.bytes, flow.write, flow.pattern,
                             flow.release});
        }
    }
    return specs;
}

} // namespace gem5::pcie
