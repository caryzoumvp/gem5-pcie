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

"""Exercise the PCIe timing adapter through native gem5 timing ports."""

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
    parser.add_argument("--drain-early", action="store_true")
    parser.add_argument(
        "--bytes", type=int, choices=(64, 516, 4096), default=516
    )
    args = parser.parse_args()
    if args.drain_early and args.bytes != 516:
        parser.error("the early-drain fixture uses the 516-byte request")

    system = System(mem_mode="timing")
    system.clk_domain = SrcClockDomain(
        clock="1GHz", voltage_domain=VoltageDomain()
    )
    system.dut = PcieTiming(hops=args.hops, outstanding=1, mps=256, mrrs=512)
    if args.bytes == 4096:
        system.dut.tags = 1
        system.dut.core_descriptors = 1
        system.dut.core_sectors = 4
        system.dut.fabric_channels = 1
    system.tester = PcieTrafficTester(data_size=args.bytes)
    system.tester.port = system.dut.cpu_side
    root = Root(full_system=False, system=system)
    m5.instantiate()

    if args.drain_early:
        m5.simulate(5_000)
        if system.dut.idle() or system.dut.visibleWrites() != 0:
            raise RuntimeError(
                "Drain test did not stop with a write in flight"
            )
        m5.drain()
        if not system.dut.idle() or system.dut.visibleWrites() != 3:
            raise RuntimeError(
                "Drain missed an accepted posted Write fragment"
            )

    event = m5.simulate(20_000_000)
    expected = "PCIe timing packet test passed"
    result = system.dut.report()
    out = Path(m5.options.outdir) / "timing-result.json"
    out.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    if event.getCause() != expected or event.getCode() != 0:
        raise RuntimeError(f"PCIe timing case stopped: {event.getCause()}")
    if not root.system.dut.idle():
        raise RuntimeError("PCIe timing adapter did not drain")
    if not system.tester.verifyLocalRead():
        raise RuntimeError("PCIe drained memory failed a functional read")
    write_tlps, read_tlps, cpl_tlps = {
        64: (3, 2, 2),
        516: (4, 2, 3),
        4096: (18, 9, 17),
    }[args.bytes]
    if (
        system.dut.visibleWrites() != write_tlps
        or system.dut.completedReads() != 1
    ):
        raise RuntimeError("PCIe timing adapter did not retire every TLP once")
    if not (
        0
        < result["first_write_response_tick"]
        < result["first_write_visible_tick"]
        < result["last_read_complete_tick"]
    ):
        raise RuntimeError("Unexpected PCIe acceptance/visibility ordering")
    if result["accepted_packets"] != 3 or result["response_stalls"] != 1:
        raise RuntimeError("PCIe Packet duplication or missing response retry")
    for hop in range(args.hops):
        if (
            result[f"switch_{hop}_transmissions"]
            != write_tlps + read_tlps + cpl_tlps
        ):
            raise RuntimeError(f"PCIe switch {hop} lost or duplicated a TLP")
    print(expected)


if __name__ == "__m5_main__":
    main()
