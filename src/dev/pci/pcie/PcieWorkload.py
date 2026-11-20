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
    VectorParam,
    VectorRequestPort,
)
from m5.proxy import Parent
from m5.SimObject import PyBindMethod


class PcieWorkload(ClockedObject):
    type = "PcieWorkload"
    cxx_header = "dev/pci/pcie/workload.hh"
    cxx_class = "gem5::pcie::Workload"
    cxx_exports = [
        PyBindMethod("finished"),
        PyBindMethod("flowTrace"),
        PyBindMethod("linkTrace"),
        PyBindMethod("submitExternal"),
        PyBindMethod("externalFlows"),
    ]

    ports = VectorRequestPort("Connections to PCIe timing requester ports")
    dut = Param.PcieTiming("Timing fabric supplying write visibility events")
    system = Param.System(Parent.any, "System assigning the requestor ID")
    flow_ids = VectorParam.String("Flow labels in result row order")
    source_indices = VectorParam.Unsigned("Requester port index per flow")
    addresses = VectorParam.Addr("Destination address per flow")
    transfer_bytes = VectorParam.Unsigned("Transfer size per flow")
    writes = VectorParam.Bool("Posted Write if true, Read otherwise")
    patterns = VectorParam.UInt8("Write fill byte or expected Read byte")
    releases = VectorParam.Tick("Earliest issue tick per flow")
    dependency_offsets = VectorParam.Unsigned(
        "Offsets into the predecessor index vector, with a final sentinel"
    )
    dependency_indices = VectorParam.Unsigned(
        "Predecessor flow indices in compressed row order"
    )
    external_flow_limit = Param.Unsigned(
        0, "Maximum number of dynamically submitted transfers in a run"
    )
    external_library = Param.String("", "Native PCIe workload client library")
    external_config = Param.String("", "Configuration path passed to client")
