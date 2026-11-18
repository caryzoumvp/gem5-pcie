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

"""Exercise bounded 8:1 incast through native gem5 timing ports."""

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

SOURCE_COUNT = 8
PAUSED_TICK = 150_000
DEADLINE_TICK = 20_000_000


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("hops", type=int, choices=(1, 2))
    variant = parser.add_mutually_exclusive_group()
    variant.add_argument("--one-cell", action="store_true")
    variant.add_argument("--tiny-baseline", action="store_true")
    args = parser.parse_args()

    system = System(mem_mode="timing")
    system.clk_domain = SrcClockDomain(
        clock="1GHz", voltage_domain=VoltageDomain()
    )
    system.dut = PcieTiming(
        hops=args.hops,
        window="128KiB",
        outstanding=1,
        mps=128,
        mrrs=128,
        header_credits=1,
        data_credits=8,
        endpoint_cycles=20,
        endpoint_hold_cycles=250,
    )
    tiny = args.one_cell or args.tiny_baseline
    data_size = 32 if tiny else 516
    if tiny:
        system.dut.max_transfer = data_size
    if args.one_cell:
        system.dut.staging_cells = 1
    system.sources = [
        PcieTrafficTester(
            address_offset=source * 0x4000,
            data_size=data_size,
            visibility_delay=5000,
            exit_on_finish=False,
        )
        for source in range(SOURCE_COUNT)
    ]
    for source in system.sources:
        source.port = system.dut.cpu_side
    root = Root(full_system=False, system=system)
    m5.instantiate()

    paused_event = m5.simulate(PAUSED_TICK)
    if (
        m5.curTick() != PAUSED_TICK
        or paused_event.getCause() != "simulate() limit reached"
        or paused_event.getCode() != 0
    ):
        raise RuntimeError(
            f"PCIe incast stopped before snapshot: {paused_event.getCause()}"
        )
    paused = root.system.dut.report()
    waits = root.system.dut.waitGraph()
    event = m5.simulate(DEADLINE_TICK - PAUSED_TICK)
    final = root.system.dut.report()
    result = {
        "paused": paused,
        "paused_waits": waits,
        "final": final,
        "final_waits": root.system.dut.waitGraph(),
    }
    out = Path(m5.options.outdir) / "incast-result.json"
    out.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")

    if (
        m5.curTick() != DEADLINE_TICK
        or event.getCause() != "simulate() limit reached"
        or event.getCode() != 0
    ):
        raise RuntimeError(f"PCIe incast stopped early: {event.getCause()}")
    if not (
        paused["target_pause_active"] == 1
        and paused["visible_write_tlps"] == 0
        and paused["target_rx_waiting"] > 0
        and paused["target_link_credit_stalls"] > 0
        and any("target pause" in edge for edge in waits)
    ):
        raise RuntimeError("PCIe incast did not back up behind the held sink")
    staged = paused[f"switch_{args.hops - 1}_staged_cells"]
    if args.one_cell and (staged != 1 or paused["switch_0_staged_cells"] != 1):
        raise RuntimeError("PCIe incast did not fill the final staging cell")
    # First-switch ingress domains are independent. At the last switch,
    # the trunk merges Posted traffic into a single ordering domain.
    if args.tiny_baseline and paused["switch_0_staged_cells"] <= 1:
        raise RuntimeError("PCIe incast baseline did not use extra staging")
    if (
        final["target_pause_active"]
        or not root.system.dut.idle()
        or result["final_waits"]
    ):
        raise RuntimeError("PCIe incast did not drain after sink release")
    if not all(source.finished() for source in system.sources):
        raise RuntimeError("PCIe incast did not verify all requester payloads")

    # Each path splits 32 B into 4 + 28 or 516 B into 4 + 4*128.
    # The 16 B retry write uses one TLP and each MemRd yields one CplD.
    data_fragments = 2 if tiny else 5
    expected = {
        "accepted_packets": 3,
        "visible_write_tlps": data_fragments + 1,
        "completed_reads": 1,
    }
    for source in range(SOURCE_COUNT):
        for counter, count in expected.items():
            actual = final[f"source_{source}_{counter}"]
            if actual != count:
                raise RuntimeError(
                    f"PCIe requester {source} {counter}: {actual} != {count}"
                )

    expected_transfers = SOURCE_COUNT * (3 * data_fragments + 1)
    for hop in range(args.hops):
        for counter in ("grants", "transmissions"):
            actual = final[f"switch_{hop}_{counter}"]
            if actual != expected_transfers:
                raise RuntimeError(f"PCIe switch {hop} {counter}: {actual}")

    if final["response_stalls"] != SOURCE_COUNT:
        raise RuntimeError("PCIe incast did not exercise each response retry")
    print("PCIe timing incast test passed")


if __name__ == "__m5_main__":
    main()
