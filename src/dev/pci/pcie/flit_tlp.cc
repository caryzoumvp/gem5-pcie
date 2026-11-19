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

#include "dev/pci/pcie/flit_tlp.hh"

#include <algorithm>
#include <stdexcept>

namespace gem5::pcie
{
namespace
{

constexpr uint8_t Nop = 0x00;
constexpr uint8_t Read32 = 0x03;
constexpr uint8_t Read64 = 0x20;
constexpr uint8_t Write32 = 0x40;
constexpr uint8_t Write64 = 0x60;
constexpr uint8_t Cpl = 0x0a;
constexpr uint8_t CplD = 0x4a;
constexpr uint8_t OhcA = 1;
constexpr uint32_t MaxDwords = 1024;
constexpr uint32_t MaxBytes = 4 * MaxDwords;

void
require(bool condition, const char *message)
{
    if (!condition) {
        throw std::invalid_argument(message);
    }
}

void
write16(std::vector<uint8_t> &out, uint16_t word)
{
    out.push_back(word >> 8);
    out.push_back(word & 0xff);
}

void
write32(std::vector<uint8_t> &out, uint32_t word)
{
    write16(out, word >> 16);
    write16(out, word & 0xffff);
}

uint16_t
read16(std::span<const uint8_t> bytes, size_t offset)
{
    return (uint16_t(bytes[offset]) << 8) | bytes[offset + 1];
}

uint32_t
read32(std::span<const uint8_t> bytes, size_t offset)
{
    return (uint32_t(read16(bytes, offset)) << 16) | read16(bytes, offset + 2);
}

uint32_t
lengthBytes(std::span<const uint8_t, 4> first_dw)
{
    const auto length = read16(first_dw, 2) & 0x03ff;
    return (length ? length : MaxDwords) * 4;
}

void
common(std::vector<uint8_t> &out, uint8_t type, uint8_t tc, bool ohc_a,
       uint32_t bytes)
{
    require(tc < 8 && bytes % 4 == 0 && bytes <= MaxBytes,
            "Invalid FLIT Length or traffic class");
    out.push_back(type);
    out.push_back((tc << 5) | (ohc_a ? OhcA : 0));
    write16(out, (bytes / 4) & 0x03ff);
}

void
validateRange(ByteRange range)
{
    require(range.bytes && range.bytes % 4 == 0 && range.address % 4 == 0 &&
                range.bytes <= MaxBytes - range.address % MaxBytes,
            "Invalid full-DW FLIT memory request range");
}

uint8_t
encodeStatus(CompletionStatus status)
{
    switch (status) {
        case CompletionStatus::Success:
            return 0;
        case CompletionStatus::UnsupportedRequest:
            return 1;
        case CompletionStatus::CompleterAbort:
            return 4;
    }
    throw std::invalid_argument("Unsupported FLIT Completion status");
}

CompletionStatus
decodeStatus(uint8_t status)
{
    switch (status) {
        case 1:
            return CompletionStatus::UnsupportedRequest;
        case 4:
            return CompletionStatus::CompleterAbort;
        default:
            throw std::invalid_argument("Unsupported FLIT Completion status");
    }
}

std::vector<uint8_t>
encode(const FlitMemoryRequest &request)
{
    const bool read = request.op == Op::Read;
    const bool wide = request.range.address > UINT32_MAX;
    validateRange(request.range);
    require(request.key.tag < (1 << 14) &&
                (read ? request.payload.empty()
                      : (request.op == Op::Write &&
                         request.payload.size() == request.range.bytes)),
            "Invalid FLIT memory request tag or data");
    std::vector<uint8_t> out;
    out.reserve((wide ? 16 : 12) + request.payload.size());
    const auto type =
        read ? (wide ? Read64 : Read32) : (wide ? Write64 : Write32);
    common(out, type, request.trafficClass, false, request.range.bytes);
    write16(out, request.key.requester);
    write16(out, read ? request.key.tag : 0);
    if (wide) {
        write32(out, request.range.address >> 32);
    }
    write32(out, request.range.address & UINT32_MAX);
    out.insert(out.end(), request.payload.begin(), request.payload.end());
    return out;
}

std::vector<uint8_t>
encode(const FlitCompletion &tlp)
{
    const auto &c = tlp.completion;
    const auto status = encodeStatus(c.status);
    const auto data = c.payload.size();
    require(c.key.tag < (1 << 14) && c.byteCount && c.byteCount <= MaxBytes &&
                c.byteCount % 4 == 0 && c.lowerAddress < 128 &&
                c.lowerAddress % 4 == 0 &&
                (status ? !data : data && data <= c.byteCount),
            "Invalid full-DW FLIT Completion");
    std::vector<uint8_t> out;
    out.reserve((status ? 16 : 12) + data);
    common(out, status ? Cpl : CplD, c.trafficClass, status != 0, data);
    write16(out, tlp.completer);
    write16(out, (uint16_t(c.lowerAddress & 0x40) << 8) | c.key.tag);
    write16(out, c.key.requester);
    write16(out,
            (uint16_t(c.lowerAddress & 0x3c) << 10) | (c.byteCount & 0xfff));
    if (status) {
        write32(out, status);
    }
    out.insert(out.end(), c.payload.begin(), c.payload.end());
    return out;
}

} // anonymous namespace

std::vector<uint8_t>
encodeFlitTlp(const FlitTlp &tlp)
{
    return std::visit([](const auto &packet) { return encode(packet); }, tlp);
}

size_t
flitTlpSize(std::span<const uint8_t, 4> first_dw)
{
    if (first_dw[0] == Nop) {
        return 4;
    }

    require(!(first_dw[2] & 0xfc), "Unsupported FLIT attributes or trailer");
    const auto ohc = first_dw[1] & 0x1f;
    if (first_dw[0] == Cpl) {
        require(ohc == OhcA && !read16(first_dw, 2),
                "Unsupported FLIT Completion without Data");
        return 16;
    }
    require(!ohc, "Unsupported FLIT OHC");
    const auto payload = lengthBytes(first_dw);
    switch (first_dw[0]) {
        case Read32:
            return 12;
        case Read64:
            return 16;
        case Write32:
        case CplD:
            return 12 + payload;
        case Write64:
            return 16 + payload;
        default:
            throw std::invalid_argument("Unsupported FLIT TLP type");
    }
}

FlitTlp
decodeFlitTlp(std::span<const uint8_t> bytes)
{
    require(bytes.size() >= 4, "Truncated FLIT TLP");
    const auto first_dw = bytes.first<4>();
    require(bytes.size() == flitTlpSize(first_dw), "Wrong FLIT TLP size");
    const uint8_t type = bytes[0];
    require(type != Nop, "NOP does not carry a transaction");

    const uint8_t tc = bytes[1] >> 5;
    const uint16_t field = read16(bytes, 6);
    require(!(field & 0x8000), "Poisoned FLIT TLP is unsupported");
    const uint16_t tag = field & 0x3fff;
    if (type == Cpl || type == CplD) {
        const uint16_t third = read16(bytes, 10);
        const uint16_t byte_count = third & 0xfff;
        const uint8_t lower = ((field >> 8) & 0x40) | ((third >> 10) & 0x3c);
        require(byte_count % 4 == 0, "Non-full-DW Completion is unsupported");
        Completion result{{read16(bytes, 8), tag},
                          byte_count ? byte_count
                                     : static_cast<uint16_t>(MaxBytes),
                          lower,
                          CompletionStatus::Success,
                          {},
                          tc};
        if (type == Cpl) {
            require(!bytes[13] && !(bytes[14] & 0x80) && !(bytes[15] & 0x18),
                    "Unsupported FLIT Completion segment or low address");
            result.status = decodeStatus(bytes[15] & 7);
        } else {
            require(bytes.size() - 12 <= result.byteCount,
                    "Completion data exceeds Byte Count");
            result.payload.assign(bytes.begin() + 12, bytes.end());
        }
        return FlitCompletion{read16(bytes, 4), std::move(result)};
    }

    const bool wide = type == Read64 || type == Write64;
    const bool read = type == Read64 || type == Read32;
    const size_t header_size = wide ? 16 : 12;
    const uint64_t address =
        wide ? ((uint64_t(read32(bytes, 8)) << 32) | read32(bytes, 12))
             : read32(bytes, 8);
    require(!wide || address > UINT32_MAX, "Mem64 address below 4 GiB");
    const ByteRange range{address, lengthBytes(first_dw)};
    validateRange(range);
    return FlitMemoryRequest{read ? Op::Read : Op::Write,
                             {read16(bytes, 4), read ? tag : uint16_t{0}},
                             range,
                             tc,
                             {bytes.begin() + header_size, bytes.end()}};
}

std::vector<FlitTlp>
FlitTlpAssembler::receive(std::span<const uint8_t, PayloadBytes> area)
{
    // Publish neither partial data nor decoded transactions on failure.
    auto data = partial;
    auto size = pendingSize;
    unsigned first_half = 0;
    unsigned second_half = 0;
    size_t pos = 0;
    std::vector<FlitTlp> complete;
    while (pos < PayloadBytes) {
        if (data.empty() && area[pos] == Nop) {
            const auto boundary = std::min(PayloadBytes, (pos / 16 + 1) * 16);
            do {
                require(area[pos] == Nop, "TLP before end of FLIT NOP group");
                pos += 4;
            } while (pos < boundary);
            continue;
        }
        if (data.empty()) {
            size =
                flitTlpSize(std::span<const uint8_t, 4>(area.data() + pos, 4));
            data.reserve(size);
        }
        const auto end =
            pos + std::min(size - data.size(), PayloadBytes - pos);
        if (pos < 128) {
            require(++first_half <= 8, "Too many TLPs in first FLIT half");
        }
        if (end > 128) {
            require(++second_half <= 8, "Too many TLPs in second FLIT half");
        }
        data.insert(data.end(), area.begin() + pos, area.begin() + end);
        if (data.size() == size) {
            complete.push_back(decodeFlitTlp(data));
            data.clear();
            size = 0;
        }
        pos = end;
    }
    partial = std::move(data);
    pendingSize = size;
    return complete;
}

} // namespace gem5::pcie
