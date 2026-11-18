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

#ifndef __DEV_PCI_PCIE_LINK_HH__
#define __DEV_PCI_PCIE_LINK_HH__

#include <compare>
#include <cstdint>
#include <functional>
#include <map>

#include "base/types.hh"
#include "dev/pci/pcie/tlp.hh"

namespace gem5::pcie
{

struct CreditChannel
{
    uint8_t vc;
    TlpClass type;

    auto operator<=>(const CreditChannel &) const = default;
};

struct Credits
{
    uint32_t headers = 0;
    uint32_t data = 0;

    bool operator==(const Credits &) const = default;
};

struct CreditState
{
    Credits available;
    Credits inFlight;
    Credits occupied;
    Credits returning;
};

struct LinkTiming
{
    uint32_t wireBytesPerCycle;
    Tick cycle;
    Tick propagation;
    Tick creditDelay;
};

enum class SendResult
{
    Accepted,
    Busy,
    NoCredit,
    Invalid
};

/**
 * A unidirectional TLP link. All channels share its serializer.
 * Callback scheduling must be asynchronous on the model's event queue.
 */
class LinkDirection
{
  public:
    using Id = uint64_t;
    using Event = std::function<void()>;
    using Schedule = std::function<void(Tick, Event)>;
    using TransferEvent = std::function<void(Tick, Id)>;
    using Ready = std::function<void(Tick)>;

    LinkDirection(LinkTiming timing, Schedule schedule, TransferEvent arrival,
                  Ready ready, TransferEvent departure = {});
    void configure(CreditChannel channel, Credits backing);
    SendResult send(Tick now, Id id, CreditChannel channel,
                    uint32_t wire_bytes, uint32_t data_bytes);
    bool release(Tick now, Id id);
    CreditState state(CreditChannel channel) const;
    bool empty() const;

  private:
    enum class Phase
    {
        InFlight,
        Occupied,
        Returning
    };

    struct Transfer
    {
        CreditChannel channel;
        Credits cost;
        Phase phase;
    };

    struct Window
    {
        Credits capacity;
        CreditState state;
    };

    void arrive(Tick now, Id id);
    void restore(Tick now, Id id);

    const LinkTiming timing;
    const Schedule schedule;
    const TransferEvent arrival;
    const Ready ready;
    const TransferEvent departure;
    Tick wireFree = 0;
    std::map<CreditChannel, Window> channels;
    std::map<Id, Transfer> transfers;
};

} // namespace gem5::pcie

#endif // __DEV_PCI_PCIE_LINK_HH__
