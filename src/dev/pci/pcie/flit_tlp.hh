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

#ifndef __DEV_PCI_PCIE_FLIT_TLP_HH__
#define __DEV_PCI_PCIE_FLIT_TLP_HH__

#include <span>
#include <variant>

#include "dev/pci/pcie/request.hh"

namespace gem5::pcie
{

/** Ordinary full-DW memory traffic in Flit Mode. */
struct FlitMemoryRequest
{
    Op op;
    ReadKey key;
    ByteRange range;
    uint8_t trafficClass;
    std::vector<uint8_t> payload;
};

struct FlitCompletion
{
    uint16_t completer;
    Completion completion;
};

using FlitTlp = std::variant<FlitMemoryRequest, FlitCompletion>;

std::vector<uint8_t> encodeFlitTlp(const FlitTlp &tlp);
FlitTlp decodeFlitTlp(std::span<const uint8_t> bytes);
size_t flitTlpSize(std::span<const uint8_t, 4> first_dw);

/** Reassemble the TLP area of each accepted, in-order FLIT. */
class FlitTlpAssembler
{
  public:
    static constexpr size_t PayloadBytes = 236;

    std::vector<FlitTlp> receive(std::span<const uint8_t, PayloadBytes> area);
    bool
    empty() const
    {
        return partial.empty();
    }

  private:
    std::vector<uint8_t> partial;
    size_t pendingSize = 0;
};

} // namespace gem5::pcie

#endif // __DEV_PCI_PCIE_FLIT_TLP_HH__
