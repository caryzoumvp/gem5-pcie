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

#include "dev/pci/pcie/flit_replay.hh"

#include <algorithm>
#include <stdexcept>

namespace gem5::pcie
{
namespace
{

constexpr uint16_t LastSequence = 1023;
constexpr size_t MaxOutstanding = 511;

bool
validSequence(uint16_t sequence)
{
    return sequence > 0 && sequence <= LastSequence;
}

uint16_t
nextSequenceAfter(uint16_t sequence)
{
    return sequence == LastSequence ? 1 : sequence + 1;
}

uint16_t
priorSequence(uint16_t sequence)
{
    return sequence == 1 ? LastSequence : sequence - 1;
}

size_t
distanceFrom(uint16_t start, uint16_t end)
{
    return (end + LastSequence - start) % LastSequence;
}

} // anonymous namespace

void
writeReplayDlp(FlitRecord &record, ReplayDlp dlp)
{
    const auto command = static_cast<uint8_t>(dlp.command);
    if (command > 3 || dlp.sequence > LastSequence ||
        ((dlp.payload || command) && !dlp.sequence)) {
        throw std::invalid_argument("Invalid FLIT DLP replay fields");
    }
    auto &upper = record.bytes[FlitRecord::DlpOffset];
    const uint8_t dllp_type = upper & 0x10;
    upper = dllp_type | (dlp.payload ? 0x40 : 0) |
            (dlp.priorPayload ? 0x20 : 0) | (command << 2) |
            (dlp.sequence >> 8);
    record.bytes[FlitRecord::DlpOffset + 1] = dlp.sequence & 0xff;
}

ReplayDlp
readReplayDlp(const FlitRecord &record)
{
    const uint8_t upper = record.bytes[FlitRecord::DlpOffset];
    if (upper & 0x80) {
        throw std::invalid_argument("Reserved FLIT Usage");
    }
    const auto command = static_cast<ReplayCommand>((upper >> 2) & 3);
    const uint16_t sequence =
        ((upper & 3) << 8) | record.bytes[FlitRecord::DlpOffset + 1];
    return {bool(upper & 0x40), bool(upper & 0x20), command, sequence};
}

ReplayTx::ReplayTx(size_t capacity, uint16_t first_sequence)
    : capacity(capacity),
      nextSequence(first_sequence),
      lastAck(priorSequence(first_sequence))
{
    if (!capacity || capacity > MaxOutstanding ||
        !validSequence(first_sequence)) {
        throw std::invalid_argument("Invalid FLIT replay window");
    }
}

bool
ReplayTx::canSend() const
{
    return !cursor && retained.size() < capacity;
}

std::optional<ReplayFrame>
ReplayTx::protect(FlitRecord flit)
{
    if (!canSend()) {
        return std::nullopt;
    }
    retained.push_back({nextSequence, std::move(flit)});
    nextSequence = nextSequenceAfter(nextSequence);
    return retained.back();
}

bool
ReplayTx::feedback(ReplayCommand command, uint16_t last_received)
{
    if ((command != ReplayCommand::Ack &&
         command != ReplayCommand::StandardNak) ||
        !validSequence(last_received)) {
        return false;
    }
    const auto count = distanceFrom(lastAck, last_received);
    if (count > retained.size()) {
        return false;
    }

    for (size_t i = 0; i < count; ++i) {
        retained.pop_front();
    }
    if (cursor && distanceFrom(lastAck, *cursor) <= count) {
        if (retained.empty()) {
            cursor.reset();
        } else {
            cursor = retained.front().sequence;
        }
    }
    lastAck = last_received;
    if (command == ReplayCommand::StandardNak) {
        timeout();
    }
    return true;
}

bool
ReplayTx::timeout()
{
    if (retained.empty() || cursor) {
        return false;
    }
    cursor = retained.front().sequence;
    return true;
}

std::optional<ReplayFrame>
ReplayTx::nextReplay()
{
    if (!cursor) {
        return std::nullopt;
    }
    const auto offset = distanceFrom(lastAck, *cursor);
    auto result = retained.at(offset - 1);
    if (offset == retained.size()) {
        cursor.reset();
    } else {
        cursor = nextSequenceAfter(*cursor);
    }
    return result;
}

ReplayRx::ReplayRx(uint16_t first_sequence) : expected(first_sequence)
{
    if (!validSequence(first_sequence)) {
        throw std::invalid_argument("Invalid initial FLIT receive sequence");
    }
}

ReplayDisposition
ReplayRx::receive(uint16_t sequence, bool valid)
{
    if (valid && sequence == expected) {
        expected = nextSequenceAfter(expected);
        return {ReceiveFlit::Deliver, ReplayCommand::Ack, sequence};
    }
    const auto last = priorSequence(expected);
    if (valid && validSequence(sequence) &&
        distanceFrom(sequence, expected) <= MaxOutstanding) {
        return {ReceiveFlit::Duplicate, ReplayCommand::Ack, last};
    }
    return {ReceiveFlit::Discard, ReplayCommand::StandardNak, last};
}

} // namespace gem5::pcie
