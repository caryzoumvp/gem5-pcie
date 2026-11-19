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

#ifndef __DEV_PCI_PCIE_FLIT_REPLAY_HH__
#define __DEV_PCI_PCIE_FLIT_REPLAY_HH__

#include <cstdint>
#include <deque>
#include <optional>

#include "dev/pci/pcie/flit_packer.hh"

namespace gem5::pcie
{

enum class ReplayCommand : uint8_t
{
    Sequence,
    Ack,
    StandardNak,
    SelectiveNak
};

struct ReplayDlp
{
    bool payload;
    bool priorPayload;
    ReplayCommand command;
    uint16_t sequence;

    bool operator==(const ReplayDlp &) const = default;
};

// These helpers own DLP0/1. DLP2..5 and integrity bytes are separate.
void writeReplayDlp(FlitRecord &record, ReplayDlp dlp);
ReplayDlp readReplayDlp(const FlitRecord &record);

struct ReplayFrame
{
    uint16_t sequence;
    FlitRecord protectedFlit;
};

/** TX retry buffer in an initialized link's Standard Replay subset. */
class ReplayTx
{
  public:
    explicit ReplayTx(size_t capacity, uint16_t first_sequence = 1);

    bool canSend() const;
    bool
    replayPending() const
    {
        return cursor.has_value();
    }
    std::optional<ReplayFrame> protect(FlitRecord flit);
    bool feedback(ReplayCommand command, uint16_t last_received);
    bool timeout();
    std::optional<ReplayFrame> nextReplay();
    size_t
    occupied() const
    {
        return retained.size();
    }

  private:
    const size_t capacity;
    uint16_t nextSequence;
    uint16_t lastAck;
    std::deque<ReplayFrame> retained;
    std::optional<uint16_t> cursor;
};

enum class ReceiveFlit
{
    Deliver,
    Duplicate,
    Discard
};

struct ReplayDisposition
{
    ReceiveFlit result;
    ReplayCommand feedback;
    uint16_t cumulative;
};

class ReplayRx
{
  public:
    explicit ReplayRx(uint16_t first_sequence = 1);
    ReplayDisposition receive(uint16_t sequence, bool valid);

  private:
    uint16_t expected;
};

} // namespace gem5::pcie

#endif // __DEV_PCI_PCIE_FLIT_REPLAY_HH__
