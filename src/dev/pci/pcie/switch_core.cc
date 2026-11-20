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

#include "dev/pci/pcie/switch_core.hh"

#include <algorithm>
#include <cassert>
#include <numeric>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace gem5::pcie
{
namespace
{

Tick
after(Tick now, Tick delay)
{
    if (delay > MaxTick - now) {
        throw std::overflow_error("PCIe Switch event exceeds Tick range");
    }
    return now + delay;
}

Tick
cycleDelay(Tick period, uint32_t cycles)
{
    if (cycles && period > MaxTick / cycles) {
        throw std::overflow_error("PCIe switch cycles exceed Tick range");
    }
    return period * cycles;
}

Tick
aligned(Tick now, Tick period)
{
    return after(now, (period - now % period) % period);
}

uint32_t
ceilUnits(uint32_t bytes, uint32_t unit)
{
    return bytes / unit + (bytes % unit != 0);
}

size_t
classIndex(TlpClass type)
{
    const auto index = static_cast<size_t>(type);
    if (index >= 3) {
        throw std::invalid_argument("invalid PCIe TLP class");
    }
    return index;
}

} // anonymous namespace

uint32_t
ForwardTlp::dataBytes() const
{
    return payload ? payload->size() : 0;
}

uint32_t
ForwardTlp::wireBytes() const
{
    return headerBytes + dataBytes();
}

SwitchCore::QueueKey
SwitchCore::Transfer::key() const
{
    const uint32_t domain = (uint32_t{tlp.partition} << 3) | tlp.trafficClass;
    return {tlp.ingress, {tlp.egress, tlp.vc, tlp.type, domain}};
}

uint32_t
SwitchCore::Egress::total() const
{
    return std::accumulate(used.begin(), used.end(), uint32_t{0});
}

SwitchCore::SwitchCore(SwitchLimits limits, LinkDirection::Schedule schedule,
                       Transmit transmit,
                       LinkDirection::Schedule service_schedule, Trace trace)
    : limits(limits),
      schedule(std::move(schedule)),
      serviceSchedule(service_schedule ? std::move(service_schedule)
                                       : this->schedule),
      transmit(std::move(transmit)),
      trace(std::move(trace)),
      egress(limits.ports),
      readBusy(limits.bankCount),
      writeBusy(limits.bankCount),
      fabricBusy(limits.fabricChannels),
      parserQueues(limits.ports),
      parserPending(limits.ports)
{
    const auto reserved =
        std::accumulate(limits.stagingMinimums.begin(),
                        limits.stagingMinimums.end(), uint64_t{0});
    if (limits.ports < 2 || !limits.bankCount || !limits.fabricChannels ||
        !limits.portsPerSlice || !limits.cellBytes || !limits.cellsPerCycle ||
        !limits.sectorBytes || !limits.voqCells || !limits.stagingCells ||
        !limits.ingress.descriptors || !limits.ingress.activeVoqs ||
        !limits.cycle || reserved > limits.stagingCells || !this->schedule ||
        !this->transmit || (limits.parserSlots && !limits.parserCycles) ||
        ((limits.parserSlots || limits.egressCdcCycles) &&
         !limits.linkCycle)) {
        throw std::invalid_argument("invalid PCIe switch limits or callbacks");
    }
    for (const auto *clocks :
         {&limits.ingressLinkCycles, &limits.egressLinkCycles}) {
        if (!clocks->empty() &&
            (clocks->size() != limits.ports ||
             std::find(clocks->begin(), clocks->end(), 0) != clocks->end())) {
            throw std::invalid_argument("invalid PCIe switch port clocks");
        }
    }
    for (uint32_t port = 0; port < limits.ports; ++port) {
        resources.push_back(std::make_unique<CoreResources>(
            limits.ingress, limits.sectorBytes, limits.ingressMinimums));
    }
}

void
SwitchCore::receive(Tick now, ForwardTlp tlp, ReleaseRx release_rx)
{
    if (tlp.ingress >= limits.ports || tlp.egress >= limits.ports ||
        tlp.ingress == tlp.egress || tlp.vc >= 8 || !tlp.headerBytes ||
        tlp.headerBytes > 128 || (tlp.payload && tlp.payload->size() > 4096) ||
        !release_rx || active.contains(tlp.id)) {
        throw std::invalid_argument("invalid or duplicate PCIe RX TLP");
    }

    const auto cls = classIndex(tlp.type);
    const uint32_t cells = ceilUnits(tlp.wireBytes(), limits.cellBytes);
    const uint32_t sectors = ceilUnits(tlp.dataBytes(), limits.sectorBytes);
    uint32_t staging = limits.stagingCells;
    for (size_t i = 0; i < limits.stagingMinimums.size(); ++i) {
        if (i != cls) {
            staging -= limits.stagingMinimums[i];
        }
    }
    auto capacity = limits.ingress;
    for (size_t i = 0; i < limits.ingressMinimums.size(); ++i) {
        if (i != cls) {
            capacity.descriptors -= limits.ingressMinimums[i].descriptors;
            capacity.sectors -= limits.ingressMinimums[i].sectors;
            capacity.activeVoqs -= limits.ingressMinimums[i].activeVoqs;
        }
    }
    if (cells > limits.voqCells || cells > staging ||
        sectors > capacity.sectors || !capacity.descriptors ||
        !capacity.activeVoqs) {
        throw std::invalid_argument("PCIe TLP exceeds configured switch room");
    }

    const Id id = tlp.id;
    const auto seq =
        ordering.arrived({tlp.ingress, tlp.partition, tlp.trafficClass},
                         tlp.type, tlp.completion);
    active.emplace(
        id, Transfer{std::move(tlp), seq, std::move(release_rx), cells, now});
    if (limits.pipelined) {
        orderingOwners.emplace(seq, id);
    }
    auto &entry = active.at(id);
    const auto ingress = entry.tlp.ingress;
    traceStage(Stage::Rx, now, id, ingress);
    if (limits.parserSlots) {
        entry.phase = Phase::Parsing;
        parserQueues[ingress].push_back(id);
        queueParser(now, ingress);
    } else {
        coreReady(now, id);
    }
}

void
SwitchCore::traceStage(Stage stage, Tick now, Id id, uint32_t resource) const
{
    if (trace) {
        trace({stage, now, id, resource});
    }
}

void
SwitchCore::queueParser(Tick now, uint32_t ingress)
{
    if (parserPending[ingress]) {
        return;
    }
    const auto cycle = portCycle(ingress, true);
    const auto next = after(now, cycle - now % cycle);
    parserPending[ingress] = true;
    serviceSchedule(next, [this, next, ingress] {
        parserPending[ingress] = false;
        launchParser(next, ingress);
    });
}

void
SwitchCore::launchParser(Tick now, uint32_t ingress)
{
    auto &queue = parserQueues[ingress];
    const auto done =
        after(now, cycleDelay(portCycle(ingress, true), limits.parserCycles));
    const auto core_ready =
        limits.ingressCdcCycles
            ? after(aligned(done, limits.cycle),
                    cycleDelay(limits.cycle, limits.ingressCdcCycles))
            : done;
    for (uint32_t i = 0; i < limits.parserSlots && !queue.empty(); ++i) {
        const Id id = queue.front();
        queue.pop_front();
        traceStage(Stage::ParseStart, now, id, ingress);
        schedule(done, [this, done, core_ready, ingress, id] {
            traceStage(Stage::ParseDone, done, id, ingress);
            if (core_ready == done) {
                coreReady(done, id);
            } else {
                active.at(id).phase = Phase::IngressCdc;
                schedule(core_ready, [this, core_ready, id] {
                    coreReady(core_ready, id);
                });
            }
        });
    }
    if (!queue.empty()) {
        queueParser(now, ingress);
    }
}

void
SwitchCore::coreReady(Tick now, Id id)
{
    auto &entry = active.at(id);
    entry.phase = Phase::Rx;
    traceStage(Stage::CoreReady, now, id, entry.tlp.ingress);
    const auto sort_key = [this](Id id) {
        const auto &entry = active.at(id);
        return std::tuple(entry.receivedAt, entry.tlp.ingress, entry.order);
    };
    rx.insert(std::upper_bound(rx.begin(), rx.end(), id,
                               [&](Id lhs, Id rhs) {
                                   return sort_key(lhs) < sort_key(rhs);
                               }),
              id);
    observations.peakRx = std::max(observations.peakRx, rx.size());
    wake(now);
}

void
SwitchCore::departed(Tick now, Id id)
{
    auto it = active.find(id);
    if (it == active.end() || it->second.phase != Phase::Tx ||
        !ordering.commit(it->second.order)) {
        throw std::logic_error("unexpected PCIe switch departure");
    }
    traceStage(Stage::TxDeparted, now, id, it->second.tlp.egress);
    if (limits.pipelined) {
        orderingOwners.erase(it->second.order);
    }
    active.erase(it);
    wake(now);
}

void
SwitchCore::wake(Tick now)
{
    if (servicePending) {
        return;
    }

    // Collect all work on this edge before examining the next edge.
    const Tick next = after(now, limits.cycle - now % limits.cycle);
    servicePending = true;
    serviceSchedule(next, [this, next] { pump(next); });
}

void
SwitchCore::pump(Tick now)
{
    servicePending = false;
    transmitStaged(now);
    grant(now);
    admit(now);
}

Tick
SwitchCore::service(uint32_t cells) const
{
    const Tick cycles = ceilUnits(cells, limits.cellsPerCycle);
    if (cycles > MaxTick / limits.cycle) {
        throw std::overflow_error("PCIe core service exceeds Tick range");
    }
    return cycles * limits.cycle;
}

void
SwitchCore::serveBeats(Tick now, Id id, uint32_t cells, Stage stage,
                       uint32_t resource, LinkDirection::Event complete)
{
    if (!limits.pipelined) {
        schedule(after(now, service(cells)), std::move(complete));
        return;
    }
    const Tick edge = after(now, limits.cycle);
    schedule(edge, [this, edge, id, cells, stage, resource,
                    complete = std::move(complete)]() mutable {
        if (stage == Stage::WriteContinuation) {
            ++observations.writeBeats;
        } else {
            ++observations.readBeats;
        }
        if (cells <= limits.cellsPerCycle) {
            complete();
            return;
        }
        traceStage(stage, edge, id, resource);
        serveBeats(edge, id, cells - limits.cellsPerCycle, stage, resource,
                   std::move(complete));
    });
}

void
SwitchCore::admit(Tick now)
{
    std::set<QueueKey> blocked_queues;
    std::set<OrderingDomain> blocked_posted;
    for (auto it = rx.begin(); it != rx.end();) {
        const Id id = *it;
        auto &entry = active.at(id);
        const auto &tlp = entry.tlp;
        const auto key = entry.key();
        const OrderingDomain domain{tlp.ingress, tlp.partition,
                                    tlp.trafficClass};
        const uint32_t bank = tlp.ingress % limits.bankCount;
        auto &pool = *resources[tlp.ingress];
        const auto queue = voqs.find(key);
        const uint32_t used =
            queue == voqs.end() ? 0 : queue->second.usedCells;
        const Tick done = after(now, service(entry.cells));
        std::optional<CoreResources::ReservationId> reservation;
        if (!writeBusy[bank] && !blocked_queues.contains(key) &&
            !blocked_posted.contains(domain) &&
            entry.cells <= limits.voqCells - used) {
            reservation = pool.reserve(key.voq, tlp.dataBytes());
        }
        if (!reservation) {
            ++observations.admissionStalls;
            blocked_queues.insert(key);
            if (tlp.type == TlpClass::Posted) {
                blocked_posted.insert(domain);
            }
            ++it;
            continue;
        }
        if (!pool.commit(*reservation)) {
            throw std::logic_error("PCIe core reservation cannot commit");
        }
        voqs[key].usedCells += entry.cells;
        entry.reservation = *reservation;
        entry.phase = Phase::Writing;
        traceStage(Stage::WriteStart, now, id, bank);
        observations.peakCore =
            std::max(observations.peakCore, ++coreReserved);
        writeBusy[bank] = true;
        if (limits.pipelined) {
            wake(now);
        }
        it = rx.erase(it);
        serveBeats(now, id, entry.cells, Stage::WriteContinuation, bank,
                   [this, id, bank, done] {
                       auto &copy = active.at(id);
                       writeBusy[bank] = false;
                       copy.phase = Phase::Ready;
                       voqs.at(copy.key()).ready.push_back(id);
                       auto release = std::move(copy.releaseRx);
                       traceStage(Stage::RxRelease, done, id, bank);
                       release(done, id);
                       wake(done);
                   });
    }
}

bool
SwitchCore::canStage(const Transfer &entry) const
{
    const auto &port = egress[entry.tlp.egress];
    const auto cls = classIndex(entry.tlp.type);
    uint32_t available = limits.stagingCells - port.total();
    for (size_t i = 0; i < port.used.size(); ++i) {
        if (i != cls && port.used[i] < limits.stagingMinimums[i]) {
            available -= limits.stagingMinimums[i] - port.used[i];
        }
    }
    return entry.cells <= available;
}

bool
SwitchCore::canFollow(const Transfer &entry, bool at_tx) const
{
    if (!limits.pipelined) {
        return ordering.eligible(entry.order);
    }
    // Same-egress predecessors must already hold staging backing at grant.
    // At TX they must be in the outbound FIFO. Cross-egress predecessors
    // still block until physical departure.
    return ordering.eligible(entry.order, [&](Ordering::Id order) {
        const auto &prior = active.at(orderingOwners.at(order));
        return prior.tlp.egress == entry.tlp.egress &&
               (at_tx ? prior.phase == Phase::Tx : prior.grantSequence != 0);
    });
}

void
SwitchCore::releaseService(Tick now, Id id, size_t bank, size_t channel)
{
    readBusy[bank] = false;
    fabricBusy[channel] = false;
    if (limits.pipelined) {
        traceStage(Stage::ReadLaneRelease, now, id, bank);
    }
    wake(now);
}

void
SwitchCore::grant(Tick now)
{
    auto next = lastGrant ? voqs.upper_bound(*lastGrant) : voqs.begin();
    for (size_t scanned = 0; scanned < voqs.size(); ++scanned) {
        if (next == voqs.end()) {
            next = voqs.begin();
        }
        auto &queue = (next++)->second;
        if (queue.ready.empty()) {
            continue;
        }
        const auto lane =
            std::find(fabricBusy.begin(), fabricBusy.end(), false);
        if (lane == fabricBusy.end()) {
            ++observations.fabricStalls;
            return;
        }

        const Id id = queue.ready.front();
        auto &entry = active.at(id);
        const auto &tlp = entry.tlp;
        const uint32_t bank = tlp.ingress % limits.bankCount;
        if (!canFollow(entry, false)) {
            ++observations.orderingStalls;
            continue;
        }
        if (readBusy[bank]) {
            ++observations.bankStalls;
            continue;
        }
        if (!canStage(entry)) {
            ++observations.stagingStalls;
            continue;
        }
        const bool cross = tlp.ingress / limits.portsPerSlice !=
                           tlp.egress / limits.portsPerSlice;
        const Tick served = after(now, service(entry.cells));
        const Tick done =
            cross ? after(served, limits.crossSliceDelay) : served;

        const size_t channel = lane - fabricBusy.begin();
        auto &target = egress[tlp.egress];
        target.used[classIndex(tlp.type)] += entry.cells;
        stagingReserved += entry.cells;
        observations.peakStaging =
            std::max(observations.peakStaging, stagingReserved);
        readBusy[bank] = true;
        fabricBusy[channel] = true;
        queue.ready.pop_front();
        lastGrant = entry.key();
        entry.phase = Phase::Fabric;
        entry.grantSequence = ++grants;
        traceStage(Stage::FabricGrant, now, id, channel);
        const bool release_early = limits.pipelined && served != done;
        if (limits.pipelined) {
            wake(now);
        }
        auto finish = [this, id, bank, channel, done, release_early] {
            auto &transfer = active.at(id);
            auto queue = voqs.find(transfer.key());
            if (!resources[transfer.tlp.ingress]->release(
                    transfer.reservation)) {
                throw std::logic_error("PCIe core release without ownership");
            }
            --coreReserved;
            queue->second.usedCells -= transfer.cells;
            if (!queue->second.usedCells) {
                voqs.erase(queue);
            }
            if (!release_early) {
                releaseService(done, id, bank, channel);
            }
            traceStage(Stage::FabricDone, done, id, channel);
            if (limits.egressCdcCycles) {
                transfer.phase = Phase::EgressCdc;
                const auto cycle = portCycle(transfer.tlp.egress, false);
                const auto ready =
                    after(aligned(done, cycle),
                          cycleDelay(cycle, limits.egressCdcCycles));
                schedule(ready, [this, ready, id] { txReady(ready, id); });
            } else {
                txReady(done, id);
            }
            wake(done);
        };
        if (release_early) {
            serveBeats(now, id, entry.cells, Stage::FabricContinuation,
                       channel,
                       [this, served, done, id, bank, channel,
                        finish = std::move(finish)]() mutable {
                           releaseService(served, id, bank, channel);
                           schedule(done, std::move(finish));
                       });
        } else if (limits.pipelined) {
            serveBeats(now, id, entry.cells, Stage::FabricContinuation,
                       channel, std::move(finish));
        } else {
            schedule(done, std::move(finish));
        }
    }
}

Tick
SwitchCore::portCycle(uint32_t port, bool ingress) const
{
    const auto &clocks =
        ingress ? limits.ingressLinkCycles : limits.egressLinkCycles;
    return clocks.empty() ? limits.linkCycle : clocks.at(port);
}

void
SwitchCore::txReady(Tick now, Id id)
{
    auto &transfer = active.at(id);
    transfer.phase = Phase::Staged;
    transfer.stagedAt = now;
    traceStage(Stage::TxReady, now, id, transfer.tlp.egress);
    auto &staged = egress[transfer.tlp.egress].staged;
    const auto sort_key = [this](Id id) {
        const auto &entry = active.at(id);
        return std::pair(entry.stagedAt, entry.grantSequence);
    };
    staged.insert(std::upper_bound(staged.begin(), staged.end(), id,
                                   [&](Id lhs, Id rhs) {
                                       return sort_key(lhs) < sort_key(rhs);
                                   }),
                  id);
    wake(now);
}

void
SwitchCore::transmitStaged(Tick now)
{
    for (auto &output : egress) {
        for (auto it = output.staged.begin(); it != output.staged.end();) {
            auto &entry = active.at(*it);
            if (!canFollow(entry, true)) {
                ++it;
                continue;
            }
            const auto result = transmit(now, entry.tlp);
            if (result == SendResult::Invalid) {
                throw std::logic_error("PCIe egress rejected TLP as invalid");
            }
            if (result != SendResult::Accepted) {
                ++it;
                continue;
            }
            entry.phase = Phase::Tx;
            traceStage(Stage::TxAccepted, now, entry.tlp.id, entry.tlp.egress);
            output.used[classIndex(entry.tlp.type)] -= entry.cells;
            stagingReserved -= entry.cells;
            entry.tlp.payload.reset();
            it = output.staged.erase(it);
            ++transmissions;
        }
    }
}

SwitchCore::Counts
SwitchCore::counts() const
{
    Counts report = observations;
    report.rxWaiting = rx.size();
    report.transfers = active.size();
    report.grants = grants;
    report.transmissions = transmissions;
    for (auto &pool : resources) {
        report.coreDescriptors += pool->occupied().descriptors;
    }
    for (const auto &port : egress) {
        report.stagedCells += port.total();
    }
    assert(report.coreDescriptors == coreReserved);
    assert(report.stagedCells == stagingReserved);
    return report;
}

std::vector<std::string>
SwitchCore::waits() const
{
    constexpr std::array owner{
        "rx/admission",
        "core/write-bank",
        "voq/fabric-arbitration",
        "fabric/staging-reservation",
        "staging/downstream-tx",
        "serializer/ordering-commit",
        "rx/parser",
        "rx/ingress-cdc",
        "staging/egress-cdc",
    };
    std::vector<std::string> result;
    for (const auto &[id, entry] : active) {
        const auto &tlp = entry.tlp;
        result.push_back("tlp:" + std::to_string(id) + " -> " +
                         owner[static_cast<size_t>(entry.phase)] +
                         " ingress:" + std::to_string(tlp.ingress) +
                         " egress:" + std::to_string(tlp.egress));
    }
    return result;
}

bool
SwitchCore::empty() const
{
    const auto report = counts();
    return !report.transfers && !report.coreDescriptors &&
           !report.stagedCells && rx.empty() && voqs.empty() &&
           ordering.empty() && orderingOwners.empty() && !servicePending;
}

} // namespace gem5::pcie
