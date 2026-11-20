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

#include "dev/pci/pcie/timing.hh"

#include <algorithm>
#include <utility>

#include "base/logging.hh"
#include "dev/pci/pcie/tree_profiles.hh"
#include "sim/cur_tick.hh"

namespace gem5::pcie
{
namespace
{

constexpr uint16_t Requester = 0x100;
// Native arrivals, departures and credits are collected before a service edge.
constexpr Event::Priority ServicePriority = Event::Default_Pri + 1;

AddrRange
addressWindow(const PcieTimingParams &p)
{
    fatal_if(!p.window || p.window > MaxAddr - p.base,
             "Invalid PCIe timing address window");
    return AddrRange(p.base, p.base + p.window);
}

bool
contains(const AddrRange &window, Addr address, uint64_t bytes)
{
    return window.contains(address) && bytes <= window.end() - address;
}

} // anonymous namespace

PcieTiming::CpuPort::CpuPort(PcieTiming &owner, PortID index)
    : ResponsePort(owner.name() + ".cpu_side[" + std::to_string(index) + "]",
                   index),
      owner(owner),
      index(index)
{}

AddrRangeList
PcieTiming::CpuPort::getAddrRanges() const
{
    return owner.tree ? AddrRangeList(owner.treeWindows.begin(),
                                      owner.treeWindows.end())
                      : AddrRangeList{owner.window};
}

bool
PcieTiming::CpuPort::recvTimingReq(PacketPtr packet)
{
    return owner.accept(index, packet);
}

void
PcieTiming::CpuPort::recvRespRetry()
{
    auto &source = owner.sources.at(index);
    panic_if(!source.responseBlocked, "Unexpected PCIe response retry");
    source.responseBlocked = false;
    owner.wake();
}

Tick
PcieTiming::CpuPort::recvAtomic(PacketPtr packet)
{
    owner.accessDrainedMemory(packet);
    return 0;
}

void
PcieTiming::CpuPort::recvFunctional(PacketPtr packet)
{
    owner.accessDrainedMemory(packet);
}

void
PcieTiming::accessDrainedMemory(PacketPtr packet)
{
    panic_if(!idle(), "PCIe local inspection requires an idle timing fabric");
    const bool read = packet->cmd == MemCmd::ReadReq;
    const bool write = packet->cmd == MemCmd::WriteReq;
    panic_if((!read && !write) || packet->isMaskedWrite() ||
                 packet->getAddr() % 4 || packet->getSize() % 4 ||
                 !packet->getSize(),
             "Unsupported PCIe local inspection Packet");

    const auto base = packet->getAddr();
    const auto bytes = packet->getSize();
    const auto target = targetForRange(base, bytes);
    if (target == targets.size()) {
        if (packet->needsResponse()) {
            packet->makeResponse();
            packet->setBadAddress();
        }
        return;
    }

    auto &store = targetMemory(target);
    const auto op = read ? Op::Read : Op::Write;
    for (const auto &range :
         splitRequests(op, base, bytes, settings.mps, settings.mrrs)) {
        const auto offset = range.address - base;
        if (write) {
            const auto *begin = packet->getConstPtr<uint8_t>() + offset;
            panic_if(!store.commitWrite(range.address, {begin, range.bytes}),
                     "PCIe local write failed address check");
        } else {
            auto *destination = packet->getPtr<uint8_t>() + offset;
            for (const auto &cpl :
                 store.completeRead({Requester, 0}, range, 0, settings.rcb)) {
                destination = std::copy(cpl.payload.begin(), cpl.payload.end(),
                                        destination);
            }
        }
    }
    if (packet->needsResponse()) {
        packet->makeResponse();
    }
}

uint32_t
PcieTiming::Message::dataBytes() const
{
    return payload ? payload->size() : 0;
}

uint32_t
PcieTiming::Message::headerBytes(bool detailed) const
{
    if (type == TlpClass::Completion) {
        return detailed && status != CompletionStatus::Success ? 16 : 12;
    }
    return detailed && range.address <= UINT32_MAX ? 12 : 16;
}

bool
PcieTiming::Direction::empty() const
{
    return flit ? flit->empty() : tlp->empty();
}

bool
PcieTiming::Direction::release(Tick now, Id id)
{
    return flit ? flit->release(now, id) : tlp->release(now, id);
}

PcieTiming::PcieTiming(const PcieTimingParams &p)
    : ClockedObject(p),
      settings(p),
      window(addressWindow(p)),
      memory(p.base, p.window, p.mps),
      tracker(p.mps),
      sources(p.port_cpu_side_connection_count),
      targets(p.bidirectional_endpoints || !p.tree_links.empty()
                  ? sources.size()
                  : 1)
{
    const uint32_t max_tlp_data = std::min(p.mps, p.max_transfer);
    fatal_if(sources.empty() ||
                 (p.tree_links.empty() && sources.size() > 32) || !p.hops ||
                 p.hops > 8 || !p.outstanding || !p.max_transfer ||
                 p.max_transfer % 4 || p.endpoint_cycles == Cycles(0) ||
                 !p.tags || p.tags > 768 || !p.header_credits ||
                 !p.link_width || !p.core_descriptors || !p.core_sectors ||
                 p.mrrs < 128 || p.mrrs > 4096 || (p.mrrs & (p.mrrs - 1)) ||
                 !validRcb(p.rcb, true) ||
                 uint64_t{p.data_credits} * 16 < max_tlp_data,
             "%s: invalid PCIe timing geometry", name());
    fatal_if(p.bidirectional_endpoints &&
                 (p.hops != 1 || sources.size() < 2 ||
                  p.window % sources.size() ||
                  (p.window / sources.size()) % 4096 || p.base % 4096),
             "%s: bidirectional ports require one switch and one equal "
             "page-aligned window per endpoint",
             name());

    fatal_if(
        (p.flit_detail && (!p.shared_fc_tx_capable ||
                           !p.parser_tlps_per_cycle || !p.parser_cycles)) ||
            (!p.flit_detail &&
             (p.shared_fc || p.shared_merge_posted_completion ||
              p.corrupt_first_flit || p.drop_first_ack ||
              p.flit_wire_feedback)) ||
            (p.shared_merge_posted_completion && !p.shared_fc) ||
            (p.flit_detail &&
             (p.header_credits > 65536 || p.data_credits > 1048576)) ||
            p.flit_fec_delay > MaxTick - p.link_delay,
        "%s: invalid FLIT capability or flow-control profile", name());

    // Test every possible first RCB position before accepting configuration.
    size_t max_completions = 0;
    for (uint32_t offset = 0; offset < p.rcb; offset += 4) {
        max_completions = std::max(
            max_completions,
            splitCompletions(
                {offset, std::min({p.mrrs, p.max_transfer, 4096 - offset})},
                p.mps, p.rcb)
                .size());
    }
    fatal_if(max_completions > p.completion_slots,
             "%s: Completion queue cannot hold one maximum Read", name());

    buildTree();
    for (size_t i = 0; i < sources.size(); ++i) {
        sources[i].port = std::make_unique<CpuPort>(*this, i);
        tracker.configure(requesterId(i),
                          {static_cast<uint16_t>(p.tags),
                           uint64_t{p.tags} * p.mrrs,
                           static_cast<uint16_t>(p.rcb), true});
    }

    const auto schedule = [this](Tick when, auto action) {
        defer(when, std::move(action));
    };
    const auto service = [this](Tick when, auto action) {
        defer(when, std::move(action), ServicePriority);
    };
    const LinkTiming timing{p.link_width, clockPeriod(), p.link_delay,
                            p.credit_delay};
    const Tick flit_cycle =
        p.flit_link_period ? p.flit_link_period : clockPeriod();
    SharedFcConfig fc;
    fc.receiverShared = p.shared_fc;
    fc.mergePostedCompletion = p.shared_merge_posted_completion;
    for (auto type :
         {TlpClass::Posted, TlpClass::NonPosted, TlpClass::Completion}) {
        const auto index = static_cast<size_t>(type);
        const uint32_t data = type == TlpClass::NonPosted ? 0 : p.data_credits;
        fc.dedicated[0][index] = {p.header_credits, data};
        if (p.shared_fc &&
            !(fc.mergePostedCompletion && type == TlpClass::Completion)) {
            const uint32_t unit =
                fc.mergePostedCompletion && type == TlpClass::Posted ? 8 : 4;
            const auto round = [unit](uint32_t units) {
                return (units + unit - 1) / unit * unit;
            };
            fc.shared[index] = {round(p.header_credits), round(data)};
        }
    }

    const auto edges = tree                        ? p.tree_links.size()
                       : p.bidirectional_endpoints ? sources.size()
                                                   : targetEdge() + 1;
    for (size_t edge = 0; edge < edges; ++edge) {
        const auto direction = [&, edge](bool forward) -> Direction {
            const auto ready = [this, edge, forward](Tick when) {
                if (const auto core = sendingCore(edge, forward)) {
                    cores[*core]->wake(when);
                } else {
                    wake();
                }
            };
            const auto departed = [this, edge, forward](Tick, Id id) {
                departure(edge, forward, id);
            };
            if (p.flit_detail) {
                auto detailed =
                    tree ? treeLinkTiming({edge, forward}) : timing;
                if (!tree) {
                    detailed.cycle = flit_cycle;
                }
                fatal_if(detailed.propagation > MaxTick - p.flit_fec_delay,
                         "%s: invalid PCIe link propagation", name());
                detailed.propagation += p.flit_fec_delay;
                FlitLinkConfig config{
                    detailed,
                    fc,
                    p.replay_flits,
                    p.flit_flush,
                    p.flit_feedback,
                    p.flit_replay_timeout,
                    forward && edge == 0 && p.corrupt_first_flit,
                    forward && edge == 0 && p.drop_first_ack};
                auto result = std::make_unique<FlitLink>(
                    config, schedule,
                    [this, edge, forward](Tick, Id id, const FlitTlp &packet) {
                        arriveFlit(edge, forward, id, packet);
                    },
                    ready, departed);
                return {nullptr, std::move(result)};
            }
            auto result = std::make_unique<LinkDirection>(
                timing, schedule,
                [this, edge, forward](Tick, Id id) {
                    arrive(edge, forward, id);
                },
                ready, departed);
            for (auto type : {TlpClass::Posted, TlpClass::NonPosted,
                              TlpClass::Completion}) {
                result->configure({0, type},
                                  fc.dedicated[0][static_cast<size_t>(type)]);
            }
            return {std::move(result), nullptr};
        };
        links.push_back({direction(true), direction(false)});
        if (p.flit_wire_feedback) {
            auto &pair = links.back();
            pair.forward.flit->connectFeedback(*pair.reverse.flit);
        }
    }

    SwitchLimits limits{2,
                        2,
                        p.fabric_channels,
                        p.ports_per_slice,
                        64,
                        p.fabric_cells,
                        64,
                        p.voq_cells,
                        p.staging_cells,
                        {},
                        {p.core_descriptors, p.core_sectors, p.active_voqs},
                        {},
                        clockPeriod(),
                        p.cross_slice_delay};
    if (p.flit_detail) {
        limits.pipelined = true;
        limits.linkCycle = flit_cycle;
        limits.parserSlots = p.parser_tlps_per_cycle;
        limits.parserCycles = p.parser_cycles;
        limits.ingressCdcCycles = p.ingress_cdc_cycles;
        limits.egressCdcCycles = p.egress_cdc_cycles;
    }
    const uint32_t maximum_cells = (max_tlp_data + 16 + 63) / 64;
    const size_t switches = tree ? p.tree_switches.size() : p.hops;
    for (size_t hop = 0; hop < switches; ++hop) {
        if (tree) {
            configureTreeSwitch(hop, limits);
        } else if (!hop) {
            limits.ports = edges - p.hops + 1;
            limits.bankCount = limits.ports;
        } else {
            limits.ports = 2;
            limits.bankCount = 2;
        }
        fatal_if(maximum_cells >
                         std::min(limits.voqCells, limits.stagingCells) ||
                     (max_tlp_data + 63) / 64 > limits.ingress.sectors,
                 "%s: switch %zu cannot hold one maximum TLP", name(), hop);
        SwitchCore::Trace trace;
        if (p.core_trace_limit) {
            trace = [this, hop](SwitchCore::TraceEvent event) {
                if (coreEvents.size() == settings.core_trace_limit) {
                    ++traceDropped;
                    return;
                }
                coreEvents.push_back({hop, static_cast<uint64_t>(event.stage),
                                      event.tick, event.id, event.resource});
            };
        }
        cores.push_back(std::make_unique<SwitchCore>(
            limits, schedule,
            [this, hop](Tick, const ForwardTlp &tlp) {
                if (tree) {
                    const auto wire =
                        tree->outgoing(sources.size() + hop, tlp.egress);
                    return transmit(wire.edge, wire.forward, tlp.id);
                }
                if (settings.bidirectional_endpoints) {
                    return transmit(tlp.egress, false, tlp.id);
                }
                if (!hop && tlp.egress < sources.size()) {
                    return transmit(tlp.egress, false, tlp.id);
                }
                if (hop && tlp.egress == 0) {
                    return transmit(sources.size() + hop - 1, false, tlp.id);
                }
                return transmit(sources.size() + hop, true, tlp.id);
            },
            service, std::move(trace)));
    }
}

void
PcieTiming::buildTree()
{
    const auto &p = settings;
    if (p.tree_links.empty()) {
        fatal_if(!p.tree_endpoints.empty() || !p.tree_switches.empty(),
                 "%s: PCIe tree profiles require physical links", name());
        return;
    }
    fatal_if(!p.flit_detail || p.bidirectional_endpoints || p.hops != 1 ||
                 p.tree_endpoints.size() != sources.size(),
             "%s: invalid explicit PCIe tree mode or leaf count", name());

    std::vector<TreeTopology::Edge> edges;
    for (const auto *profile : p.tree_links) {
        const auto &link = profile->params();
        edges.push_back(
            {{link.node_a, link.port_a}, {link.node_b, link.port_b}});
    }
    tree = std::make_unique<TreeTopology>(
        sources.size(), p.tree_switches.size(), std::move(edges));
    for (size_t i = 0; i < sources.size(); ++i) {
        const auto &leaf = p.tree_endpoints[i]->params();
        fatal_if(leaf.requester_id > UINT16_MAX || !leaf.size ||
                     leaf.size > MaxAddr - leaf.base || leaf.base % 4096 ||
                     leaf.size % 4096 ||
                     !requesterLeaves.emplace(leaf.requester_id, i).second,
                 "%s: invalid PCIe endpoint ID or address window", name());
        const AddrRange range(leaf.base, leaf.base + leaf.size);
        fatal_if(std::any_of(treeWindows.begin(), treeWindows.end(),
                             [&](const auto &prior) {
                                 return prior.intersects(range);
                             }),
                 "%s: overlapping PCIe endpoint windows", name());
        treeWindows.push_back(range);
        treeMemories.push_back(
            std::make_unique<EndpointMemory>(leaf.base, leaf.size, p.mps));
    }
}

LinkTiming
PcieTiming::treeLinkTiming(TreeTopology::Direction wire) const
{
    const auto &link = settings.tree_links.at(wire.edge)->params();
    return wire.forward ? LinkTiming{link.forward_width, link.forward_cycle,
                                     link.forward_propagation,
                                     link.forward_credit_delay}
                        : LinkTiming{link.reverse_width, link.reverse_cycle,
                                     link.reverse_propagation,
                                     link.reverse_credit_delay};
}

void
PcieTiming::configureTreeSwitch(size_t index, SwitchLimits &limits) const
{
    const auto &p = settings.tree_switches.at(index)->params();
    const auto node = sources.size() + index;
    limits.ports = tree->degree(node);
    limits.bankCount = limits.ports;
    limits.fabricChannels = p.fabric_channels;
    limits.portsPerSlice = p.ports_per_slice;
    limits.cellsPerCycle = p.fabric_cells;
    limits.voqCells = p.voq_cells;
    limits.stagingCells = p.staging_cells;
    limits.ingress = {p.core_descriptors, p.core_sectors, p.active_voqs};
    limits.crossSliceDelay = p.cross_slice_delay;
    limits.parserSlots = p.parser_tlps_per_cycle;
    limits.parserCycles = p.parser_cycles;
    limits.ingressCdcCycles = p.ingress_cdc_cycles;
    limits.egressCdcCycles = p.egress_cdc_cycles;
    limits.ingressLinkCycles.clear();
    limits.egressLinkCycles.clear();
    size_t lanes = 0;
    for (size_t port = 0; port < limits.ports; ++port) {
        const auto wire = tree->outgoing(node, port);
        lanes += settings.tree_links.at(wire.edge)->params().lanes;
        limits.egressLinkCycles.push_back(treeLinkTiming(wire).cycle);
        limits.ingressLinkCycles.push_back(
            treeLinkTiming({wire.edge, !wire.forward}).cycle);
    }
    fatal_if(limits.ports > p.port_budget || lanes > p.lane_budget,
             "%s: explicit PCIe switch exceeds physical budget", name());
}

void
PcieTiming::init()
{
    ClockedObject::init();
    for (auto &source : sources) {
        source.port->sendRangeChange();
    }
}

Port &
PcieTiming::getPort(const std::string &name, PortID idx)
{
    if (name == "cpu_side") {
        return *sources.at(idx).port;
    }
    return ClockedObject::getPort(name, idx);
}

void
PcieTiming::defer(Tick when, LinkDirection::Event action,
                  Event::Priority priority)
{
    auto *event = new EventFunctionWrapper(
        [this, action = std::move(action)] {
            --nativeEvents;
            action();
            progressDrain();
        },
        name() + ".pcie_event", true, priority);
    ++nativeEvents;
    schedule(*event, when);
}

void
PcieTiming::wake()
{
    if (pumpPending) {
        return;
    }
    pumpPending = true;
    const auto next = curTick() + clockPeriod() - curTick() % clockPeriod();
    defer(
        next,
        [this] {
            pumpPending = false;
            pump();
        },
        ServicePriority);
}

bool
PcieTiming::accept(size_t index, PacketPtr packet)
{
    auto &source = sources[index];
    if (source.outstanding >= settings.outstanding ||
        drainState() == DrainState::Draining) {
        source.retryRequest = true;
        ++requestStalls;
        return false;
    }

    const bool write = packet->cmd == MemCmd::WriteReq;
    const bool read = packet->cmd == MemCmd::ReadReq;
    fatal_if(!write && !read, "%s: unsupported timing Packet command", name());
    fatal_if(packet->isMaskedWrite() || !packet->getSize() ||
                 packet->getSize() > settings.max_transfer ||
                 packet->getAddr() % 4 || packet->getSize() % 4 ||
                 packet->getSize() - 1 > MaxAddr - packet->getAddr(),
             "%s: unsupported timing Packet range or byte enables", name());

    const Id id = nextId++;
    const Op op = write ? Op::Write : Op::Read;
    Request request{std::unique_ptr<Packet>(packet),
                    op,
                    index,
                    packet->id,
                    packet->getAddr(),
                    packet->getSize(),
                    {},
                    nullptr};
    const auto destination = targetForRange(request.base, request.bytes);
    if (destination == targets.size() ||
        ((tree || settings.bidirectional_endpoints) && destination == index)) {
        request.failed = true;
    } else {
        request.segments = splitRequests(op, request.base, packet->getSize(),
                                         settings.mps, settings.mrrs);
        request.remaining = request.segments.size();
        if (write) {
            const auto data = packet->getConstPtr<uint8_t>();
            request.data = std::make_shared<const std::vector<uint8_t>>(
                data, data + packet->getSize());
        }
    }

    requests.emplace(id, std::move(request));
    ++accepted;
    ++source.accepted;
    ++source.outstanding;
    if (requests.at(id).remaining) {
        source.injection.push_back(id);
    } else if (write && visibilityObserver) {
        const auto packet_id = packet->id;
        defer(curTick(),
              [this, packet_id] { visibilityObserver(packet_id, false); });
    }
    if (write || requests.at(id).failed) {
        queueResponse(id);
    }
    wake();
    return true;
}

void
PcieTiming::setVisibilityObserver(VisibilityObserver observer)
{
    fatal_if(visibilityObserver || !observer,
             "%s: PCIe visibility observer can only be registered once",
             name());
    visibilityObserver = std::move(observer);
}

void
PcieTiming::setRouteObserver(RouteObserver observer)
{
    fatal_if(routeObserver || !observer,
             "%s: PCIe route observer can only be registered once", name());
    routeObserver = std::move(observer);
}

void
PcieTiming::queueResponse(Id id)
{
    auto &request = requests.at(id);
    if (request.packet->needsResponse()) {
        request.packet->makeResponse();
        if (request.failed) {
            request.packet->setBadAddress();
        }
        sources[request.source].responses.push_back(id);
    } else {
        request.packet.reset();
        retire(id);
    }
}

void
PcieTiming::retire(Id id)
{
    const auto &request = requests.at(id);
    if (!request.remaining && !request.packet) {
        --sources[request.source].outstanding;
        requests.erase(id);
    }
}

void
PcieTiming::sendResponses(Source &source)
{
    while (!source.responseBlocked && !source.responses.empty()) {
        const Id id = source.responses.front();
        auto &parent = requests.at(id);
        if (!source.port->sendTimingResp(parent.packet.get())) {
            source.responseBlocked = true;
            ++responseStalls;
            return;
        }
        if (parent.op == Op::Write && !parent.failed && !firstWriteResponse) {
            firstWriteResponse = curTick();
        }
        parent.packet.release();
        source.responses.pop_front();
        retire(id);
    }
}

void
PcieTiming::pump()
{
    for (auto &source : sources) {
        sendResponses(source);
        if (source.retryRequest && source.outstanding < settings.outstanding &&
            drainState() != DrainState::Draining) {
            source.retryRequest = false;
            source.port->sendRetryReq();
        }
    }
    for (size_t i = 0; i < targets.size(); ++i) {
        sendCompletions(i);
        serveTarget(i);
    }
    for (size_t i = 0; i < sources.size(); ++i) {
        inject(i);
    }
}

std::optional<ReadKey>
PcieTiming::allocateTag(size_t index, ByteRange range)
{
    for (uint16_t tag = 0; tag < settings.tags; ++tag) {
        ReadKey key{requesterId(index), tag};
        if (tracker.begin(key, range, 0)) {
            return key;
        }
    }
    return std::nullopt;
}

PcieTiming::Id
PcieTiming::makeMessage(Message message)
{
    const auto id = nextId++;
    messages.emplace(id, std::move(message));
    return id;
}

void
PcieTiming::inject(size_t index)
{
    auto &source = sources[index];
    if (source.injection.empty()) {
        return;
    }

    const Id id = source.injection.front();
    auto &request = requests.at(id);
    const auto range = request.segments[request.next];
    if (!source.candidate) {
        if (request.op == Op::Write) {
            const auto begin =
                request.data->begin() + (range.address - request.base);
            auto payload = std::make_shared<const std::vector<uint8_t>>(
                begin, begin + range.bytes);
            source.candidate = makeMessage({id, TlpClass::Posted, range,
                                            std::move(payload), std::nullopt});
        } else {
            const auto tag = allocateTag(index, range);
            if (!tag) {
                return;
            }
            source.candidate =
                makeMessage({id, TlpClass::NonPosted, range, nullptr, tag});
        }
    }
    const auto wire = sourceWire(index);
    auto result = transmit(wire.edge, wire.forward, *source.candidate);
    panic_if(result == SendResult::Invalid, "Invalid PCIe source TLP");
    if (result == SendResult::Accepted) {
        source.candidate.reset();
        if (++request.next == request.segments.size()) {
            request.data.reset();
            source.injection.pop_front();
        }
    }
}

void
PcieTiming::receiveTarget(size_t index, Id id)
{
    auto &target = targets.at(index);
    target.rx.push_back(id);
    ++target.received;
    if (!target.pauseStarted && settings.endpoint_hold_cycles != Cycles(0)) {
        target.pauseStarted = true;
        target.paused = true;
        defer(clockEdge(settings.endpoint_hold_cycles), [this, index] {
            targets[index].paused = false;
            wake();
        });
    }
    wake();
}

void
PcieTiming::serveTarget(size_t index)
{
    auto &target = targets[index];
    if (target.busy || target.paused || target.rx.empty()) {
        return;
    }

    auto next =
        std::find_if(target.rx.begin(), target.rx.end(), [this](Id id) {
            return messages.at(id).type == TlpClass::Posted;
        });
    if (next == target.rx.end()) {
        next = target.rx.begin();
        const auto &msg = messages.at(*next);
        const auto count =
            splitCompletions(msg.range, settings.mps, settings.rcb).size();
        if (count > settings.completion_slots - target.completions.size()) {
            return;
        }
    }

    const Id id = *next;
    target.rx.erase(next);
    target.busy = true;
    defer(clockEdge(settings.endpoint_cycles),
          [this, index, id] { targetDone(index, id); });
}

void
PcieTiming::targetDone(size_t index, Id id)
{
    auto &target = targets[index];
    auto &msg = messages.at(id);
    auto &store = targetMemory(index);
    if (msg.type == TlpClass::Posted) {
        const bool visible =
            store.commitWrite(msg.range.address, *msg.payload);
        if (visible) {
            if (!writes) {
                firstWriteVisible = curTick();
            }
            ++writes;
            ++sources[requests.at(msg.parent).source].writes;
        }
        finishPiece(msg.parent, !visible);
    } else {
        for (auto &cpl :
             store.completeRead(*msg.key, msg.range, 0, settings.rcb)) {
            auto payload = std::make_shared<const std::vector<uint8_t>>(
                std::move(cpl.payload));
            target.completions.push_back(
                makeMessage({msg.parent, TlpClass::Completion, msg.range,
                             std::move(payload), cpl.key, cpl.status,
                             cpl.byteCount, cpl.lowerAddress}));
        }
    }
    target.busy = false;
    ++target.served;
    const auto wire = targetWire(index);
    panic_if(!link(wire.edge, !wire.forward).release(curTick(), id),
             "Missing PCIe target RX owner");
    messages.erase(id);
    wake();
}

void
PcieTiming::sendCompletions(size_t index)
{
    auto &queue = targets[index].completions;
    if (queue.empty()) {
        return;
    }

    const auto wire = targetWire(index);
    const auto sent = transmit(wire.edge, wire.forward, queue.front());
    panic_if(sent == SendResult::Invalid, "Invalid PCIe Completion at source");
    if (sent == SendResult::Accepted) {
        queue.pop_front();
    }
}

void
PcieTiming::sourceCompletion(size_t index, Id id)
{
    const auto &msg = messages.at(id);
    panic_if(msg.key->requester != requesterId(index) ||
                 requests.at(msg.parent).source != index,
             "Misrouted PCIe Completion");
    Completion cpl{*msg.key, msg.byteCount, msg.lowerAddress, msg.status,
                   *msg.payload};
    auto result = tracker.receive(cpl);
    panic_if(result.disposition == Receipt::Unexpected ||
                 result.disposition == Receipt::Malformed,
             "Invalid PCIe Completion at Requester");
    if (result.disposition == Receipt::Complete) {
        auto &request = requests.at(msg.parent);
        auto *buffer = request.packet->getPtr<uint8_t>();
        std::copy(result.payload.begin(), result.payload.end(),
                  buffer + (msg.range.address - request.base));
        finishPiece(msg.parent, false);
    } else if (result.disposition == Receipt::Failed) {
        finishPiece(msg.parent, true);
    }
    const auto wire = sourceWire(index);
    panic_if(!link(wire.edge, !wire.forward).release(curTick(), id),
             "Missing PCIe Requester RX owner");
    messages.erase(id);
    wake();
}

void
PcieTiming::finishPiece(Id id, bool failed)
{
    auto &request = requests.at(id);
    assert(request.remaining);
    request.failed |= failed;
    if (--request.remaining) {
        return;
    }
    auto &source = sources[request.source];
    if (!request.failed) {
        source.completedPayloadBytes += request.bytes;
    }
    if (request.op == Op::Read) {
        if (!request.failed) {
            ++reads;
            ++source.reads;
            lastReadComplete = curTick();
        }
        queueResponse(id);
    } else {
        source.completedWritePackets += !request.failed;
        if (visibilityObserver) {
            visibilityObserver(request.packetId, !request.failed);
        }
        retire(id);
    }
}

PcieTiming::Direction &
PcieTiming::link(size_t edge, bool forward)
{
    auto &pair = links.at(edge);
    return forward ? pair.forward : pair.reverse;
}

SendResult
PcieTiming::transmit(size_t edge, bool forward, Id id)
{
    const auto &msg = messages.at(id);
    const auto channel = CreditChannel{0, msg.type};
    auto &wire = link(edge, forward);
    const auto result =
        wire.flit ? wire.flit->send(curTick(), id, channel, makeFlitTlp(msg))
                  : wire.tlp->send(curTick(), id, channel,
                                   msg.headerBytes() + msg.dataBytes(),
                                   msg.dataBytes());
    if (result == SendResult::Accepted) {
        ++wire.accepted[static_cast<size_t>(msg.type)];
        wire.dataBytes += msg.dataBytes();
        wire.tlpBytes += msg.headerBytes(bool(wire.flit)) + msg.dataBytes();
        if (routeObserver) {
            routeObserver(requests.at(msg.parent).packetId, edge, forward,
                          msg.type);
        }
    }
    wire.creditStalls += result == SendResult::NoCredit;
    wire.busyStalls += result == SendResult::Busy;
    creditStalls += result == SendResult::NoCredit;
    const bool to_target =
        tree ? tree->receiver({edge, forward}).node < sources.size() &&
                   msg.type != TlpClass::Completion
        : settings.bidirectional_endpoints
            ? !forward && msg.type != TlpClass::Completion
            : forward && edge == targetEdge();
    targetCreditStalls += to_target && result == SendResult::NoCredit;
    wireStalls += result == SendResult::Busy;
    return result;
}

FlitTlp
PcieTiming::makeFlitTlp(const Message &msg) const
{
    auto data = msg.payload ? *msg.payload : std::vector<uint8_t>{};
    const auto &request = requests.at(msg.parent);
    const auto requester = requesterId(request.source);
    if (msg.type == TlpClass::Completion) {
        return FlitCompletion{completerId(msg.range.address),
                              {*msg.key, msg.byteCount, msg.lowerAddress,
                               msg.status, std::move(data), 0}};
    }
    return FlitMemoryRequest{msg.type == TlpClass::Posted ? Op::Write
                                                          : Op::Read,
                             msg.key.value_or(ReadKey{requester, 0}),
                             msg.range, 0, std::move(data)};
}

void
PcieTiming::arriveFlit(size_t edge, bool forward, Id id, const FlitTlp &packet)
{
    auto &msg = messages.at(id);
    // The internal ID accompanies the ownership event only. Header and
    // payload are obtained from the accepted wire FLITs.
    if (const auto *request = std::get_if<FlitMemoryRequest>(&packet)) {
        const auto source = requests.at(msg.parent).source;
        const auto cls =
            request->op == Op::Read ? TlpClass::NonPosted : TlpClass::Posted;
        panic_if(
            msg.type != cls || request->key.requester != requesterId(source) ||
                request->trafficClass != 0 || msg.range != request->range ||
                (msg.key && msg.key->tag != request->key.tag),
            "PCIe FLIT Request identity mismatch");
        msg.range = request->range;
        msg.payload =
            std::make_shared<const std::vector<uint8_t>>(request->payload);
    } else {
        const auto &reply = std::get<FlitCompletion>(packet);
        const auto &decoded = reply.completion;
        panic_if(msg.type != TlpClass::Completion ||
                     reply.completer != completerId(msg.range.address) ||
                     decoded.trafficClass != 0 ||
                     decoded.key.requester != msg.key->requester ||
                     decoded.key.tag != msg.key->tag,
                 "PCIe FLIT Completion identity mismatch");
        msg.byteCount = decoded.byteCount;
        msg.lowerAddress = decoded.lowerAddress;
        msg.status = decoded.status;
        msg.payload =
            std::make_shared<const std::vector<uint8_t>>(decoded.payload);
    }
    arrive(edge, forward, id);
}

size_t
PcieTiming::targetEdge(size_t index) const
{
    return settings.bidirectional_endpoints
               ? index
               : sources.size() + settings.hops - 1;
}

size_t
PcieTiming::targetForAddress(Addr address) const
{
    if (tree) {
        return std::find_if(treeWindows.begin(), treeWindows.end(),
                            [address](const auto &range) {
                                return range.contains(address);
                            }) -
               treeWindows.begin();
    }
    return settings.bidirectional_endpoints
               ? (address - settings.base) / (settings.window / targets.size())
               : 0;
}

size_t
PcieTiming::targetForRange(Addr address, uint32_t bytes) const
{
    if (!tree && !contains(window, address, bytes)) {
        return targets.size();
    }
    const auto target = targetForAddress(address);
    if (target == targets.size() ||
        (tree && !contains(treeWindows[target], address, bytes)) ||
        (!tree && targetForAddress(address + bytes - 1) != target)) {
        return targets.size();
    }
    return target;
}

uint16_t
PcieTiming::requesterId(size_t index) const
{
    return tree ? settings.tree_endpoints.at(index)->params().requester_id
                : Requester + index;
}

uint16_t
PcieTiming::completerId(Addr address) const
{
    const auto target = targetForAddress(address);
    return tree ? requesterId(target) : 0x200 + target;
}

EndpointMemory &
PcieTiming::targetMemory(size_t index)
{
    return tree ? *treeMemories.at(index) : memory;
}

TreeTopology::Direction
PcieTiming::sourceWire(size_t index) const
{
    return tree ? tree->outgoing(index, 0)
                : TreeTopology::Direction{index, true};
}

TreeTopology::Direction
PcieTiming::targetWire(size_t index) const
{
    return tree ? tree->outgoing(index, 0)
                : TreeTopology::Direction{targetEdge(index),
                                          settings.bidirectional_endpoints};
}

std::optional<size_t>
PcieTiming::sendingCore(size_t edge, bool forward) const
{
    if (tree) {
        const auto &tx = tree->sender({edge, forward});
        return tx.node < sources.size()
                   ? std::nullopt
                   : std::optional<size_t>(tx.node - sources.size());
    }
    if (settings.bidirectional_endpoints) {
        return forward ? std::nullopt : std::optional<size_t>(0);
    }
    if (forward) {
        return edge >= sources.size()
                   ? std::optional<size_t>(edge - sources.size())
                   : std::nullopt;
    }
    if (edge == targetEdge()) {
        return std::nullopt;
    }
    return edge < sources.size() ? 0 : edge - sources.size() + 1;
}

void
PcieTiming::arrive(size_t edge, bool forward, Id id)
{
    if (tree) {
        arriveTree({edge, forward}, id);
        return;
    }
    const auto &msg = messages.at(id);
    const bool permutation = settings.bidirectional_endpoints;
    const bool is_completion = msg.type == TlpClass::Completion;
    if (permutation ? !forward && !is_completion
                    : forward && edge == targetEdge()) {
        const auto target = targetForAddress(msg.range.address);
        panic_if(edge != targetEdge(target), "Misrouted PCIe memory request");
        receiveTarget(target, id);
        return;
    }
    if (!forward && edge < sources.size()) {
        if (permutation) {
            panic_if(!is_completion, "Unexpected PCIe endpoint TLP");
        }
        sourceCompletion(edge, id);
        return;
    }

    const size_t index =
        edge < sources.size() ? 0 : edge - sources.size() + forward;
    const uint32_t downstream = index ? 1 : sources.size();
    const uint32_t ingress = forward ? (index ? 0 : edge) : downstream;
    uint32_t out =
        permutation ? targetForAddress(msg.range.address) : downstream;
    if (permutation ? is_completion : !forward) {
        panic_if(msg.type != TlpClass::Completion ||
                     msg.key->requester < Requester ||
                     size_t(msg.key->requester - Requester) >= sources.size(),
                 "Invalid PCIe Completion route");
        out = index ? 0 : msg.key->requester - Requester;
    }
    enterCore(index, ingress, out, {edge, forward}, id);
}

void
PcieTiming::arriveTree(TreeTopology::Direction wire, Id id)
{
    const auto &msg = messages.at(id);
    const auto &rx = tree->receiver(wire);
    const bool reply = msg.type == TlpClass::Completion;
    const auto destination = reply ? requesterLeaves.at(msg.key->requester)
                                   : targetForAddress(msg.range.address);
    if (rx.node < sources.size()) {
        panic_if(rx.node != destination, "Misrouted PCIe tree TLP");
        if (reply) {
            sourceCompletion(rx.node, id);
        } else {
            receiveTarget(rx.node, id);
        }
        return;
    }
    const auto index = rx.node - sources.size();
    const auto out = tree->route(index, destination);
    enterCore(index, rx.number, tree->sender(out).number, wire, id);
}

void
PcieTiming::enterCore(size_t index, uint32_t ingress, uint32_t out,
                      TreeTopology::Direction wire, Id id)
{
    const auto &msg = messages.at(id);
    std::optional<CompletionIdentity> completion;
    if (msg.type == TlpClass::Completion) {
        completion = {msg.key->requester, msg.key->tag};
    }
    cores[index]->receive(
        curTick(),
        {id, ingress, out, 0, msg.type, 0, 0, completion,
         msg.headerBytes(settings.flit_detail), msg.payload},
        [this, wire](Tick t, auto id) {
            panic_if(!link(wire.edge, wire.forward).release(t, id),
                     "Missing PCIe switch RX owner");
        });
}

void
PcieTiming::departure(size_t edge, bool forward, Id id)
{
    if (const auto core = sendingCore(edge, forward)) {
        cores[*core]->departed(curTick(), id);
    }
}

bool
PcieTiming::idle() const
{
    if (!requests.empty() || !messages.empty() || nativeEvents ||
        pumpPending ||
        std::any_of(targets.begin(), targets.end(), [](const auto &target) {
            return target.busy || target.paused || !target.rx.empty() ||
                   !target.completions.empty();
        })) {
        return false;
    }
    for (size_t i = 0; i < sources.size(); ++i) {
        const auto &source = sources[i];
        if (!source.injection.empty() || !source.responses.empty() ||
            source.candidate || source.outstanding ||
            tracker.bytesReserved(requesterId(i))) {
            return false;
        }
    }
    const auto empty_link = [](const DirectionPair &pair) {
        return pair.forward.empty() && pair.reverse.empty();
    };
    return std::all_of(links.begin(), links.end(), empty_link) &&
           std::all_of(cores.begin(), cores.end(),
                       [](const auto &core) { return core->empty(); });
}

DrainState
PcieTiming::drain()
{
    return idle() ? DrainState::Drained : DrainState::Draining;
}

std::map<std::string, uint64_t>
PcieTiming::report() const
{
    std::map<std::string, uint64_t> result{
        {"tick", curTick()},
        {"accepted_packets", accepted},
        {"visible_write_tlps", writes},
        {"completed_read_packets", reads},
        {"request_stalls", requestStalls},
        {"response_stalls", responseStalls},
        {"link_credit_stalls", creditStalls},
        {"target_link_credit_stalls", targetCreditStalls},
        {"link_wire_stalls", wireStalls},
        {"first_write_response_tick", firstWriteResponse},
        {"first_write_visible_tick", firstWriteVisible},
        {"last_read_complete_tick", lastReadComplete},
        {"pending_packets", requests.size()},
        {"pending_tlps", messages.size()},
        {"pending_events", nativeEvents},
        {"core_trace_dropped", traceDropped},
        {"target_rx_waiting", 0},
        {"target_pending_completions", 0},
        {"target_pause_active", 0},
        {"source_completion_reservation_bytes", 0},
    };
    for (size_t i = 0; i < targets.size(); ++i) {
        const auto &target = targets[i];
        const auto key = "target_" + std::to_string(i) + "_";
        result[key + "received_tlps"] = target.received;
        result[key + "served_tlps"] = target.served;
        result["target_rx_waiting"] += target.rx.size();
        result["target_pending_completions"] += target.completions.size();
        result["target_pause_active"] |= target.paused;
    }
    if (settings.flit_detail) {
        constexpr std::array fields{
            std::pair{"flit_original", &FlitLink::Counts::original},
            std::pair{"flit_replayed", &FlitLink::Counts::replayed},
            std::pair{"flit_crc_errors", &FlitLink::Counts::crcErrors},
            std::pair{"flit_duplicates", &FlitLink::Counts::duplicates},
            std::pair{"flit_timeouts", &FlitLink::Counts::timeouts},
            std::pair{"flit_shared_admissions",
                      &FlitLink::Counts::sharedAdmissions},
            std::pair{"flit_dedicated_admissions",
                      &FlitLink::Counts::dedicatedAdmissions},
            std::pair{"flit_serialized_ticks",
                      &FlitLink::Counts::serializedTicks},
            std::pair{"flit_control", &FlitLink::Counts::controlFlits},
            std::pair{"flit_control_ticks", &FlitLink::Counts::controlTicks},
        };
        for (size_t edge = 0; edge < links.size(); ++edge) {
            for (const bool forward : {true, false}) {
                const auto &wire =
                    forward ? links[edge].forward : links[edge].reverse;
                const auto prefix = "edge_" + std::to_string(edge) +
                                    (forward ? "_forward_" : "_reverse_");
                result[prefix + "posted_tlps"] =
                    wire.accepted[static_cast<size_t>(TlpClass::Posted)];
                result[prefix + "nonposted_tlps"] =
                    wire.accepted[static_cast<size_t>(TlpClass::NonPosted)];
                result[prefix + "completion_tlps"] =
                    wire.accepted[static_cast<size_t>(TlpClass::Completion)];
                result[prefix + "tlp_bytes"] = wire.tlpBytes;
                result[prefix + "data_bytes"] = wire.dataBytes;
                result[prefix + "credit_stalls"] = wire.creditStalls;
                result[prefix + "busy_stalls"] = wire.busyStalls;
                for (const auto &[name, member] : fields) {
                    const auto value = wire.flit->counts().*member;
                    result[prefix + name] = value;
                    result[name] += value;
                }
            }
        }
    }
    for (size_t i = 0; i < sources.size(); ++i) {
        const auto key = "source_" + std::to_string(i) + "_";
        const auto &source = sources[i];
        result[key + "accepted_packets"] = source.accepted;
        result[key + "visible_write_tlps"] = source.writes;
        result[key + "completed_reads"] = source.reads;
        result[key + "completed_write_packets"] = source.completedWritePackets;
        result[key + "completed_payload_bytes"] = source.completedPayloadBytes;
        result[key + "pending_packets"] = source.outstanding;
        result["source_completion_reservation_bytes"] +=
            tracker.bytesReserved(requesterId(i));
    }
    for (size_t i = 0; i < cores.size(); ++i) {
        const auto counters = cores[i]->counts();
        const auto key = "switch_" + std::to_string(i) + "_";
        result[key + "rx_waiting"] = counters.rxWaiting;
        result[key + "core_descriptors"] = counters.coreDescriptors;
        result[key + "staged_cells"] = counters.stagedCells;
        result[key + "grants"] = counters.grants;
        result[key + "transmissions"] = counters.transmissions;
        result[key + "peak_rx"] = counters.peakRx;
        result[key + "peak_core_descriptors"] = counters.peakCore;
        result[key + "peak_staged_cells"] = counters.peakStaging;
        result[key + "admission_stalls"] = counters.admissionStalls;
        result[key + "bank_stalls"] = counters.bankStalls;
        result[key + "fabric_stalls"] = counters.fabricStalls;
        result[key + "staging_stalls"] = counters.stagingStalls;
        result[key + "ordering_stalls"] = counters.orderingStalls;
        result[key + "write_beats"] = counters.writeBeats;
        result[key + "read_beats"] = counters.readBeats;
    }
    return result;
}

const std::vector<std::array<uint64_t, 5>> &
PcieTiming::coreTrace() const
{
    return coreEvents;
}

std::vector<std::string>
PcieTiming::waitGraph() const
{
    std::vector<std::string> graph;
    for (size_t i = 0; i < cores.size(); ++i) {
        for (const auto &edge : cores[i]->waits()) {
            graph.push_back("switch:" + std::to_string(i) + " " + edge);
        }
    }
    for (size_t i = 0; i < sources.size(); ++i) {
        const auto &source = sources[i];
        const auto root = "requester:" + std::to_string(i) + " -> ";
        if (source.responseBlocked) {
            graph.push_back(root + "Packet response retry");
        }
        if (source.candidate) {
            graph.push_back(root + "source link for tlp:" +
                            std::to_string(*source.candidate));
        } else if (!source.injection.empty()) {
            graph.push_back(root + "request/Tag admission");
        }
    }
    for (const auto &target : targets) {
        for (const auto id : target.rx) {
            graph.push_back(
                "tlp:" + std::to_string(id) + " -> " +
                (target.paused ? "target pause" : "target memory service"));
        }
        for (const auto id : target.completions) {
            graph.push_back("tlp:" + std::to_string(id) +
                            " -> target link TX");
        }
    }
    return graph;
}

void
PcieTiming::progressDrain()
{
    if (drainState() == DrainState::Draining && idle()) {
        signalDrainDone();
    }
}

void
PcieTiming::drainResume()
{
    ClockedObject::drainResume();
    if (std::any_of(sources.begin(), sources.end(),
                    [](const auto &source) { return source.retryRequest; })) {
        wake();
    }
}

} // namespace gem5::pcie
