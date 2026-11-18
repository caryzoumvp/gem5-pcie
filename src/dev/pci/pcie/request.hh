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

#ifndef __DEV_PCI_PCIE_REQUEST_HH__
#define __DEV_PCI_PCIE_REQUEST_HH__

#include <cstdint>
#include <map>
#include <vector>

namespace gem5::pcie
{

enum class Op
{
    Read,
    Write
};

struct ByteRange
{
    uint64_t address;
    uint32_t bytes;

    bool operator==(const ByteRange &) const = default;
};

std::vector<ByteRange> splitRequests(Op op, uint64_t address, uint64_t bytes,
                                     uint32_t mps, uint32_t mrrs);
std::vector<ByteRange> splitCompletions(ByteRange read, uint32_t mps,
                                        uint16_t rcb);
bool validRcb(uint16_t rcb, bool root_complex);

struct ReadKey
{
    uint16_t requester;
    uint16_t tag;

    bool operator<(const ReadKey &other) const;
};

enum class CompletionStatus
{
    Success,
    UnsupportedRequest,
    CompleterAbort
};

struct Completion
{
    ReadKey key;
    // Decoded values. In particular, a Byte Count of 4096 is legal.
    uint16_t byteCount;
    uint8_t lowerAddress;
    CompletionStatus status;
    std::vector<uint8_t> payload;
    uint8_t trafficClass = 0;
};

enum class Receipt
{
    Partial,
    Complete,
    Failed,
    Malformed,
    Unexpected
};

struct ReadResult
{
    Receipt disposition;
    std::vector<uint8_t> payload;
};

struct FunctionLimits
{
    uint16_t outstandingTags;
    uint64_t completionBytes;
    uint16_t rcb;
    bool rootComplex;
};

/** Outstanding DW-aligned Memory Reads for each Requester Function. */
class ReadTracker
{
  public:
    explicit ReadTracker(uint32_t mps);
    void configure(uint16_t requester, FunctionLimits limits);

    /** Acquire a Tag and reserve the full response buffer atomically. */
    bool begin(ReadKey key, ByteRange range, uint8_t traffic_class);
    ReadResult receive(const Completion &completion);
    bool outstanding(ReadKey key) const;
    uint64_t bytesReserved(uint16_t requester) const;

  private:
    struct Function
    {
        FunctionLimits limits;
        uint16_t activeTags = 0;
        uint64_t reserved = 0;
    };

    struct Pending
    {
        ByteRange range;
        uint8_t trafficClass;
        std::vector<uint8_t> payload;
    };

    const uint32_t mps;
    std::map<uint16_t, Function> functions;
    std::map<ReadKey, Pending> pending;
};

} // namespace gem5::pcie

#endif // __DEV_PCI_PCIE_REQUEST_HH__
