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


"""Exercise native PCIe FLIT transport through gem5 timing ports."""

import argparse
import json
from pathlib import Path

import m5
from m5.objects import (
    PcieTiming,
    PcieTrafficTester,
    Root,
    SrcClockDomain,
    System,
    VoltageDomain,
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("hops", type=int, choices=(1, 2))
    parser.add_argument(
        "scenario",
        choices=("shared", "merged", "dedicated", "crc", "lost-ack"),
    )
    parser.add_argument("--no-shared-tx", action="store_true")
    parser.add_argument("--cycle-trace", action="store_true")
    parser.add_argument("--wire-feedback", action="store_true")
    args = parser.parse_args()
    if args.cycle_trace and (args.hops != 1 or args.scenario != "shared"):
        parser.error("cycle trace expects the single-hop Shared fixture")

    system = System(mem_mode="timing")
    system.clk_domain = SrcClockDomain(
        clock="1GHz", voltage_domain=VoltageDomain()
    )
    system.dut = PcieTiming(
        hops=args.hops,
        outstanding=1,
        mps=256,
        mrrs=512,
        flit_detail=True,
        flit_wire_feedback=args.wire_feedback,
        shared_fc=args.scenario != "dedicated",
        shared_fc_tx_capable=not args.no_shared_tx,
        shared_merge_posted_completion=args.scenario == "merged",
        # No later cumulative ACK can mask the lost ACK at depth one.
        replay_flits=1 if args.scenario == "lost-ack" else 4,
        corrupt_first_flit=args.scenario == "crc",
        drop_first_ack=args.scenario == "lost-ack",
    )
    if args.cycle_trace:
        system.dut.flit_link_period = "2ns"
        system.dut.ingress_cdc_cycles = 2
        system.dut.egress_cdc_cycles = 2
        system.dut.core_trace_limit = 256
    system.tester = PcieTrafficTester(data_size=516)
    system.tester.port = system.dut.cpu_side
    root = Root(full_system=False, system=system)
    m5.instantiate()

    event = m5.simulate(20_000_000)
    dut = root.system.dut
    result = dut.report()
    output = Path(m5.options.outdir) / "flit-result.json"
    output.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")

    if (
        event.getCause() != "PCIe timing packet test passed"
        or event.getCode() != 0
    ):
        raise RuntimeError(f"PCIe FLIT case stopped: {event.getCause()}")
    if not dut.idle() or not system.tester.verifyLocalRead():
        raise RuntimeError("PCIe FLIT case failed drain or functional read")
    if dut.visibleWrites() != 4 or dut.completedReads() != 1:
        raise RuntimeError("PCIe FLIT case did not retire each transaction")
    if result["accepted_packets"] != 3 or result["response_stalls"] != 1:
        raise RuntimeError("PCIe Packet duplication or missing response retry")
    if not (
        0
        < result["first_write_response_tick"]
        < result["first_write_visible_tick"]
        < result["last_read_complete_tick"]
    ):
        raise RuntimeError("Unexpected PCIe acceptance/visibility ordering")

    # The workload produces four Write, two Read, and three Completion TLPs.
    for hop in range(args.hops):
        if result[f"switch_{hop}_transmissions"] != 9:
            raise RuntimeError(f"PCIe switch {hop} lost or duplicated a TLP")

    if result["flit_original"] <= 0:
        raise RuntimeError("PCIe FLIT case sent no original FLITs")
    control = result["flit_control"]
    if bool(control) != args.wire_feedback:
        raise RuntimeError("Expected control FLITs only on paired links")
    total_flits = result["flit_original"] + result["flit_replayed"] + control
    flit_ticks = 32_000 if args.cycle_trace else 16_000
    if result["flit_serialized_ticks"] != total_flits * flit_ticks:
        raise RuntimeError("Incorrect 256B FLIT wire occupancy")
    if result["flit_control_ticks"] != control * flit_ticks:
        raise RuntimeError("Incorrect control-only FLIT occupancy")
    if args.scenario == "dedicated":
        if (
            result["flit_shared_admissions"] != 0
            or result["flit_dedicated_admissions"] <= 0
        ):
            raise RuntimeError("Expected dedicated flow control admissions")
    elif result["flit_shared_admissions"] <= 0:
        raise RuntimeError("Expected shared flow control admissions")

    if result["flit_crc_errors"] != int(args.scenario == "crc"):
        raise RuntimeError("Unexpected PCIe FLIT CRC error count")
    if args.scenario == "lost-ack":
        if result["flit_timeouts"] <= 0 or result["flit_duplicates"] <= 0:
            raise RuntimeError("Lost ACK did not produce a timeout and replay")
    elif result["flit_timeouts"] or result["flit_duplicates"]:
        raise RuntimeError("Unexpected timeout or duplicate FLIT")
    if (result["flit_replayed"] > 0) != (args.scenario in ("crc", "lost-ack")):
        raise RuntimeError("Unexpected PCIe FLIT replay count")
    if args.cycle_trace:
        trace = dut.coreTrace()
        output.with_name("cycle-trace.json").write_text(
            json.dumps(trace, indent=2) + "\n"
        )
        first_id = trace[0][3]
        first = [
            (stage, tick, resource)
            for hop, stage, tick, tlp, resource in trace
            if hop == 0 and tlp == first_id
        ]
        # One short Write: 4ns launches, 32ns serialize plus 3ns
        # propagation. Parser uses 2ns, core uses 1ns, CDCs use 2 cycles.
        expected = [
            (0, 39000, 0),
            (1, 40000, 0),
            (2, 42000, 0),
            (3, 44000, 0),
            (4, 45000, 0),
            (5, 46000, 0),
            (6, 46000, 0),
            (11, 47000, 0),
            (7, 47000, 0),
            (8, 52000, 1),
            (9, 53000, 1),
            (10, 56000, 1),
        ]
        if result["core_trace_dropped"] or first != expected:
            raise RuntimeError(f"PCIe cycle trace mismatch: {first}")
    print("PCIe native FLIT test passed")


if __name__ == "__m5_main__":
    main()
