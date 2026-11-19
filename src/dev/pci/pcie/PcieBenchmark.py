# Copyright (c) 2026 cary
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met: redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer;
# redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution;
# neither the name of the copyright holders nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

from m5.objects.ClockedObject import ClockedObject
from m5.params import (
    Param,
    RequestPort,
)
from m5.proxy import Parent
from m5.SimObject import PyBindMethod


class PcieBenchmark(ClockedObject):
    type = "PcieBenchmark"
    cxx_header = "dev/pci/pcie/benchmark.hh"
    cxx_class = "gem5::pcie::Benchmark"
    cxx_exports = [
        PyBindMethod("finished"),
        PyBindMethod("accepted"),
        PyBindMethod("retired"),
        PyBindMethod("firstAcceptedTick"),
        PyBindMethod("lastRetiredTick"),
        PyBindMethod("verifyWrites"),
    ]

    port = RequestPort("PCIe timing adapter connection")
    system = Param.System(Parent.any, "System assigning the requestor ID")
    address = Param.Addr(0x1000, "Starting address of the contiguous workload")
    transfer_bytes = Param.Unsigned(256, "Bytes per request")
    request_count = Param.Unsigned(64, "Number of requests")
    window = Param.Unsigned(32, "Maximum number of outstanding requests")
    period = Param.Latency("0ns", "Minimum interval between accepted requests")
    write = Param.Bool(False, "Issue Writes instead of Reads of zeroed memory")
