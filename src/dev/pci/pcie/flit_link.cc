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

#include "dev/pci/pcie/flit_link.hh"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "dev/pci/pcie/flit_integrity.hh"

namespace gem5::pcie
{
namespace
{

TlpClass
getType(const FlitMemoryRequest &tlp)
{
    return tlp.op == Op::Read ? TlpClass::NonPosted : TlpClass::Posted;
}

TlpClass
getType(const FlitCompletion &)
{
    return TlpClass::Completion;
}

size_t
payloadSize(const FlitMemoryRequest &tlp)
{
    return tlp.payload.size();
}

size_t
payloadSize(const FlitCompletion &tlp)
{
    return tlp.completion.payload.size();
}

} // anonymous namespace

FlitLink::FlitLink(FlitLinkConfig cfg, LinkDirection::Schedule schedule,
                   Arrival arrival, LinkDirection::Ready ready,
                   LinkDirection::TransferEvent departure)
    : cfg(cfg),
      schedule(std::move(schedule)),
      arrival(std::move(arrival)),
      ready(std::move(ready)),
      departure(std::move(departure)),
      credits(cfg.credits),
      tx(cfg.replayFlits)
{
    if (!cfg.timing.wireBytesPerCycle || !cfg.timing.cycle ||
        !cfg.flushDelay || !cfg.feedbackDelay ||
        cfg.replayTimeout <=
            after(cfg.timing.propagation, cfg.feedbackDelay) ||
        !this->schedule || !this->arrival || !this->ready ||
        !this->departure) {
        throw std::invalid_argument("Invalid PCIe FLIT timing or callbacks");
    }
}

Tick
FlitLink::after(Tick now, Tick offset) const
{
    if (offset > MaxTick - now) {
        throw std::overflow_error("PCIe FLIT event exceeds Tick range");
    }
    return now + offset;
}

void
FlitLink::connectFeedback(FlitLink &reverse)
{
    if (peer == &reverse) {
        return;
    }
    if (&reverse == this || peer || reverse.peer || !empty() ||
        !reverse.empty()) {
        throw std::invalid_argument("PCIe feedback needs two unpaired links");
    }
    peer = &reverse;
    reverse.peer = this;
}

Tick
FlitLink::wireTime(size_t bytes) const
{
    const auto width = cfg.timing.wireBytesPerCycle;
    const Tick cycles = bytes / width + (bytes % width != 0);
    if (cycles > MaxTick / cfg.timing.cycle) {
        throw std::overflow_error("PCIe FLIT service exceeds Tick range");
    }
    return cycles * cfg.timing.cycle;
}

SendResult
FlitLink::send(Tick now, Id id, CreditChannel channel, const FlitTlp &tlp)
{
    if (owners.contains(id) ||
        std::visit([](const auto &packet) { return getType(packet); }, tlp) !=
            channel.type) {
        return SendResult::Invalid;
    }
    if (!tx.canSend() || !packer.accepting()) {
        return SendResult::Busy;
    }

    const auto bytes = encodeFlitTlp(tlp);
    const size_t payload_size = std::visit(
        [](const auto &packet) { return payloadSize(packet); }, tlp);
    const auto admitted = credits.admit(id, channel, payload_size);
    if (admitted == FcAdmission::Invalid) {
        return SendResult::Invalid;
    }
    if (admitted == FcAdmission::NoCredit) {
        return SendResult::NoCredit;
    }
    stats.sharedAdmissions += admitted == FcAdmission::Shared;
    stats.dedicatedAdmissions += admitted == FcAdmission::Dedicated;

    if (!packer.tryPush(bytes)) {
        throw std::logic_error("PCIe FLIT admission without packer room");
    }
    // A sparse record's first bytes start its collection interval.
    if (packer.accepting() && !flushAt) {
        flushAt = after(now, cfg.flushDelay);
    }
    owners.emplace(id, Owner::Admitted);
    transmitOrder.push_back(id);
    receiveOrder.push_back(id);
    wake(now);
    return SendResult::Accepted;
}

bool
FlitLink::release(Tick now, Id id)
{
    auto it = owners.find(id);
    if (it == owners.end() || it->second != Owner::Received) {
        return false;
    }
    const auto update = after(now, cfg.timing.creditDelay);
    if (!credits.release(id)) {
        throw std::logic_error("PCIe FLIT release without credit owner");
    }
    it->second = Owner::Returning;
    schedule(update, [this, id, update] {
        if (!credits.update(id)) {
            throw std::logic_error("PCIe FLIT credit update without release");
        }
        owners.erase(id);
        ready(update);
    });
    return true;
}

void
FlitLink::wake(Tick now)
{
    if (wireBusy) {
        return;
    }
    std::optional<Tick> requested;
    if (pendingFeedback) {
        requested = std::max(now, pendingFeedback->eligible);
    }
    if (tx.replayPending() || (tx.canSend() && !packer.empty())) {
        const Tick payload = tx.replayPending() || !packer.accepting()
                                 ? now
                                 : std::max(now, flushAt.value());
        requested = requested ? std::min(*requested, payload) : payload;
    }
    if (!requested) {
        return;
    }
    const auto cycle = cfg.timing.cycle;
    const auto service =
        after(*requested, (cycle - *requested % cycle) % cycle);
    if (pumpAt && *pumpAt <= service) {
        return;
    }
    pumpAt = service;
    schedule(service, [this, service] {
        // A full record or replay can supersede a later flush callback.
        if (pumpAt != service) {
            return;
        }
        pumpAt.reset();
        pump(service);
    });
}

void
FlitLink::pump(Tick now)
{
    if (wireBusy) {
        return;
    }
    if (pendingFeedback && now >= pendingFeedback->eligible) {
        transmitControl(now);
        return;
    }
    if (auto replayed = tx.nextReplay()) {
        transmitFrame(now, *replayed, true, {});
        return;
    }
    if (!tx.canSend() || packer.empty()) {
        wake(now);
        return;
    }

    std::vector<size_t> ends;
    auto record = packer.next(&ends);
    if (!record) {
        if (now < flushAt.value()) {
            wake(now);
            return;
        }
        packer.flush();
        record = packer.next(&ends);
    }
    if (!record) {
        throw std::logic_error("PCIe FLIT flush returned no record");
    }
    flushAt.reset();
    if (!packer.empty() && packer.accepting()) {
        flushAt = after(now, cfg.flushDelay);
    }
    auto original = tx.protect(*record);
    if (!original) {
        throw std::logic_error("PCIe FLIT allocated without replay room");
    }
    transmitFrame(now, *original, false, ends);
    ready(now);
}

void
FlitLink::transmitFrame(Tick now, ReplayFrame frame, bool replayed,
                        const std::vector<size_t> &ends)
{
    const auto duration = wireTime(frame.protectedFlit.bytes.size());
    const Tick wire_end = after(now, duration);
    wireBusy = true;
    const Tick delivered = after(wire_end, cfg.timing.propagation);
    stats.serializedTicks += duration;
    stats.replayed += replayed;
    stats.original += !replayed;
    if (replayed) {
        timeoutArmed = false;
        ++timeoutEpoch;
    }
    armTimeout(wire_end);

    auto &flit = frame.protectedFlit;
    // DLP2..5 are the zero-valued NOP2 DLLP in this profile.
    writeReplayDlp(
        flit, {true, priorPayload, ReplayCommand::Sequence, frame.sequence});
    priorPayload = true;
    stampFlitCrc(flit);
    if (!replayed && cfg.corruptFirst && !corrupted) {
        corrupted = true;
        flit.bytes[0] ^= 1;
    }

    std::map<Tick, std::vector<Id>> departures;
    for (size_t end : ends) {
        if (transmitOrder.empty()) {
            throw std::logic_error("PCIe FLIT end without TLP owner");
        }
        const auto id = transmitOrder.front();
        transmitOrder.pop_front();
        departures[after(now, wireTime(end))].push_back(id);
    }
    auto at_wire_end = std::move(departures[wire_end]);
    departures.erase(wire_end);
    for (auto &[sent, ids] : departures) {
        schedule(sent, [this, sent, ids = std::move(ids)] {
            for (auto id : ids) {
                departure(sent, id);
            }
        });
    }
    if (delivered != wire_end) {
        schedule(delivered,
                 [this, delivered, flit] { receiveFrame(delivered, flit); });
    }
    schedule(wire_end, [this, time = wire_end, delivered,
                        ids = std::move(at_wire_end), flit] {
        for (auto id : ids) {
            departure(time, id);
        }
        if (delivered == time) {
            receiveFrame(delivered, flit);
        }
        finishWire(time);
    });
}

void
FlitLink::finishWire(Tick now)
{
    wireBusy = false;
    wake(now);
    ready(now);
}

void
FlitLink::receiveFrame(Tick now, FlitRecord frame)
{
    const bool valid = validFlitCrc(frame);
    uint16_t sequence = 0;
    if (valid) {
        const auto header = readReplayDlp(frame);
        if (!header.payload || header.command != ReplayCommand::Sequence) {
            throw std::logic_error("Unexpected PCIe FLIT payload DLP");
        }
        sequence = header.sequence;
    }
    stats.crcErrors += !valid;
    const auto disposition = rx.receive(sequence, valid);
    stats.duplicates += disposition.result == ReceiveFlit::Duplicate;
    if (disposition.result == ReceiveFlit::Deliver) {
        static_assert(FlitRecord::TlpBytes == FlitTlpAssembler::PayloadBytes);
        for (const auto &packet : assembler.receive(
                 std::span(frame.bytes).first<FlitRecord::TlpBytes>())) {
            if (receiveOrder.empty()) {
                throw std::logic_error(
                    "PCIe FLIT delivered without TLP owner");
            }
            const auto id = receiveOrder.front();
            receiveOrder.pop_front();
            auto &owner = owners.at(id);
            if (owner != Owner::Admitted) {
                throw std::logic_error("Duplicate PCIe FLIT TLP delivery");
            }
            owner = Owner::Received;
            arrival(now, id, packet);
        }
    }
    feedback(now, disposition);
}

void
FlitLink::feedback(Tick now, ReplayDisposition disposition)
{
    if (cfg.dropFirstAck && !droppedAck &&
        disposition.feedback == ReplayCommand::Ack) {
        droppedAck = true;
        return;
    }

    const auto update = after(now, cfg.feedbackDelay);
    if (peer) {
        peer->queueFeedback(now, update, disposition);
        return;
    }
    FlitRecord control;
    writeReplayDlp(
        control, {false, false, disposition.feedback, disposition.cumulative});
    stampFlitCrc(control);
    schedule(update,
             [this, update, control] { receiveFeedback(update, control); });
}

void
FlitLink::queueFeedback(Tick now, Tick eligible, ReplayDisposition disposition)
{
    // Feedback is generated in receive order. A same-sequence duplicate
    // ACK cannot withdraw a pending NAK for the next expected payload.
    // The first disposition opens the collection interval for its slot.
    if (!pendingFeedback) {
        pendingFeedback = PendingFeedback{eligible, disposition};
    } else if (pendingFeedback->disposition.feedback !=
                   ReplayCommand::StandardNak ||
               disposition.feedback == ReplayCommand::StandardNak ||
               pendingFeedback->disposition.cumulative !=
                   disposition.cumulative) {
        pendingFeedback->disposition = disposition;
    }
    wake(now);
}

void
FlitLink::transmitControl(Tick now)
{
    // PCIe 6.0 Tables 4-16/17, p. 360: NOP Flits can carry feedback
    // without consuming a sequence number. This profile coalesces one
    // pending disposition and reserves payload DLPs for explicit sequence.
    const auto disposition = pendingFeedback->disposition;
    pendingFeedback.reset();
    FlitRecord control;
    writeReplayDlp(control, {false, priorPayload, disposition.feedback,
                             disposition.cumulative});
    priorPayload = false;
    stampFlitCrc(control);
    const Tick duration = wireTime(control.bytes.size());
    const Tick wire_end = after(now, duration);
    const Tick delivered = after(wire_end, cfg.timing.propagation);
    wireBusy = true;
    ++controlInFlight;
    ++stats.controlFlits;
    stats.controlTicks += duration;
    stats.serializedTicks += duration;
    auto receive = [this, delivered, control] {
        --controlInFlight;
        peer->receiveFeedback(delivered, control);
    };
    if (delivered == wire_end) {
        schedule(wire_end, [this, wire_end, receive] {
            receive();
            finishWire(wire_end);
        });
    } else {
        schedule(delivered, std::move(receive));
        schedule(wire_end, [this, wire_end] { finishWire(wire_end); });
    }
}

void
FlitLink::receiveFeedback(Tick now, const FlitRecord &control)
{
    if (!validFlitCrc(control)) {
        return;
    }
    const auto dlp = readReplayDlp(control);
    if (dlp.payload || (dlp.command == ReplayCommand::StandardNak &&
                        nakPending == dlp.sequence)) {
        return;
    }
    const auto before = tx.occupied();
    if (!tx.feedback(dlp.command, dlp.sequence)) {
        return;
    }
    if (tx.occupied() < before) {
        nakPending.reset();
        timeoutArmed = false;
        ++timeoutEpoch;
        armTimeout(now);
    }
    if (dlp.command == ReplayCommand::StandardNak) {
        nakPending = dlp.sequence;
    }
    wake(now);
    ready(now);
}

void
FlitLink::armTimeout(Tick now)
{
    if (timeoutArmed || !tx.occupied()) {
        return;
    }
    timeoutArmed = true;
    const uint64_t epoch = ++timeoutEpoch;
    const auto expired = after(now, cfg.replayTimeout);
    schedule(expired, [this, expired, epoch] {
        if (epoch != timeoutEpoch) {
            return;
        }
        timeoutArmed = false;
        if (tx.timeout()) {
            ++stats.timeouts;
            wake(expired);
        }
    });
}

bool
FlitLink::empty() const
{
    return owners.empty() && credits.empty() && transmitOrder.empty() &&
           receiveOrder.empty() && packer.empty() && assembler.empty() &&
           !tx.occupied() && !wireBusy && !pumpAt && !timeoutArmed &&
           !pendingFeedback && !controlInFlight;
}

} // namespace gem5::pcie
