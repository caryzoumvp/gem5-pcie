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
    VectorResponsePort,
)
from m5.SimObject import PyBindMethod


class PcieTiming(ClockedObject):
    type = "PcieTiming"
    cxx_class = "gem5::pcie::PcieTiming"
    cxx_header = "dev/pci/pcie/timing.hh"
    cxx_exports = [
        PyBindMethod("idle"),
        PyBindMethod("visibleWrites"),
        PyBindMethod("completedReads"),
        PyBindMethod("report"),
        PyBindMethod("waitGraph"),
        PyBindMethod("coreTrace"),
    ]

    cpu_side = VectorResponsePort("Timing requests from a synthetic Requester")
    hops = Param.Unsigned(1, "Number of transparent switches in the chain")
    bidirectional_endpoints = Param.Bool(
        False, "Give each requester port an independent target window"
    )
    base = Param.Addr(0x1000, "Start of the downstream address window")
    window = Param.MemorySize("16KiB", "Size of the downstream memory")
    outstanding = Param.Unsigned(8, "Maximum retained requests per Requester")
    max_transfer = Param.Unsigned(
        65536, "Maximum application bytes per Packet"
    )
    mps = Param.Unsigned(256, "Maximum payload bytes per TLP")
    mrrs = Param.Unsigned(512, "Maximum bytes per Memory Read TLP")
    rcb = Param.Unsigned(128, "Root Complex Read Completion Boundary")
    tags = Param.Unsigned(8, "Requester Tag budget")
    completion_slots = Param.Unsigned(64, "Completer pending Cpl slots")
    endpoint_cycles = Param.Cycles(
        3, "Endpoint port service cycles per request"
    )
    endpoint_hold_cycles = Param.Cycles(
        0, "One-time target pause after its first arrival"
    )
    link_width = Param.Unsigned(16, "TLP budget bytes per link cycle")
    link_delay = Param.Latency("3ns", "One-way physical propagation")
    credit_delay = Param.Latency("5ns", "Delay from release to credit update")
    header_credits = Param.Unsigned(
        4, "Dedicated RX headers per class on a link"
    )
    data_credits = Param.Unsigned(32, "Dedicated RX 16 B units per class")
    core_descriptors = Param.Unsigned(8, "Core descriptors per ingress")
    core_sectors = Param.Unsigned(256, "64 B core storage sectors per ingress")
    active_voqs = Param.Unsigned(3, "Number of simultaneous VOQs per ingress")
    voq_cells = Param.Unsigned(64, "Cells per VOQ")
    staging_cells = Param.Unsigned(64, "Cells per egress")
    fabric_channels = Param.Unsigned(2, "Parallel switch channels")
    ports_per_slice = Param.Unsigned(1, "Contiguous ports in a switch slice")
    fabric_cells = Param.Unsigned(1, "Cells served per channel cycle")
    cross_slice_delay = Param.Latency("0ns", "Additional cross-slice delay")
    flit_detail = Param.Bool(False, "Serialize and recover 256B FLITs")
    shared_fc = Param.Bool(False, "Target links advertise Shared FC backing")
    shared_fc_tx_capable = Param.Bool(True, "Transmitter supports Shared FC")
    shared_merge_posted_completion = Param.Bool(
        False, "Merge the Posted and Completion shared pools"
    )
    replay_flits = Param.Unsigned(8, "FLIT replay buffer entries")
    flit_flush = Param.Latency("1ns", "FLIT packing flush deadline")
    flit_feedback = Param.Latency("15ns", "Abstract reverse DLP delay")
    flit_wire_feedback = Param.Bool(
        False, "Serialize ACK/NAK NOP FLITs on the opposite direction"
    )
    flit_replay_timeout = Param.Latency("160ns", "FLIT replay watchdog")
    flit_fec_delay = Param.Latency("0ns", "Abstract FLIT integrity delay")
    flit_link_period = Param.Latency(
        "0ns", "FLIT clock; 0 uses the core clock"
    )
    parser_tlps_per_cycle = Param.Unsigned(2, "Header parser slots per port")
    parser_cycles = Param.Unsigned(1, "Parser latency in link cycles")
    ingress_cdc_cycles = Param.Unsigned(0, "Ingress CDC in core cycles")
    egress_cdc_cycles = Param.Unsigned(0, "Egress CDC in link cycles")
    core_trace_limit = Param.Unsigned(0, "Maximum recorded core stage events")
    corrupt_first_flit = Param.Bool(
        False, "Flip one bit on the first requester data FLIT"
    )
    drop_first_ack = Param.Bool(
        False, "Drop the first requester ACK in the detailed link"
    )
