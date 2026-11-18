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

#ifndef __DEV_PCI_PCIE_ENDPOINT_MEMORY_HH__
#define __DEV_PCI_PCIE_ENDPOINT_MEMORY_HH__

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "dev/pci/pcie/request.hh"

namespace gem5::pcie
{

/** Zero-initialized backing for one synthetic endpoint address window. */
class EndpointMemory
{
  public:
    EndpointMemory(uint64_t base, std::size_t bytes, uint32_t mps);

    /**
     * Make one posted write visible. Return false for an unmapped range.
     * The caller owns the service timing and any posted-write error report.
     */
    bool commitWrite(uint64_t address, std::span<const uint8_t> payload);

    /**
     * Snapshot one Read at the caller's service point. An unmapped range
     * yields one UR Completion. The requester supplies its configured RCB.
     * Malformed requests passed to either operation raise invalid_argument.
     */
    std::vector<Completion> completeRead(ReadKey key, ByteRange range,
                                         uint8_t traffic_class,
                                         uint16_t requester_rcb) const;

  private:
    bool contains(uint64_t address, std::size_t bytes) const;

    const uint64_t base;
    const uint32_t mps;
    std::vector<uint8_t> storage;
};

} // namespace gem5::pcie

#endif // __DEV_PCI_PCIE_ENDPOINT_MEMORY_HH__
