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


"""Run matched native SW-TAIL trials and emit the P-08 verdict."""

import argparse
import json
import subprocess
import sys
from bisect import bisect_right
from collections import (
    Counter,
    defaultdict,
)
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO))

from util.pcie_lab.contracts import source_hashes
from util.pcie_lab.performance import (
    CORE_BOUNDARY,
    PerformanceCase,
    evaluate_performance,
)

CORE_STAGES = (
    "rx",
    "parse_start",
    "parse_done",
    "core_ready",
    "write_start",
    "rx_release",
    "fabric_grant",
    "fabric_done",
    "tx_ready",
    "tx_accepted",
)
PORTS = 9
PAYLOAD = 64
CLOCK_NS = 1
ENDPOINT_CYCLES = 7
PERIOD_NS = {"baseline": 640, "loaded": 10}
TICKS_PER_NS = 1000
START_NS = [1 + 64 * port for port in range(PORTS)]
WARMUP_PERIODS = 64
WINDOW_PERIODS = 400
WINDOWS = 3
REQUESTS = WARMUP_PERIODS + WINDOW_PERIODS * WINDOWS + 64


def percentile_ns(ticks):
    if not ticks:
        return None
    rank = (99 * len(ticks) + 99) // 100
    return sorted(ticks)[rank - 1] / TICKS_PER_NS


def measure_trace(trace, bounds):
    """Measure a half-open ingress cohort through the fixed drain deadline."""
    tlps = defaultdict(dict)
    for switch, stage, tick, tlp, resource in trace:
        if switch != 0:
            raise ValueError(f"Unexpected switch {switch}")
        if stage > 9:
            continue
        stages = tlps[tlp]
        if stage in stages:
            raise ValueError(f"Duplicate stage {stage} for TLP {tlp}")
        stages[stage] = (tick, resource)

    def flow_counts():
        return {
            "ingress": [0] * (len(bounds) - 1),
            "egress": [0] * (len(bounds) - 1),
            "backlog": [0] * len(bounds),
            "samples": 0,
            "censored": 0,
            "traced": 0,
        }

    flows = [flow_counts() for _ in range(PORTS)]
    latency = defaultdict(list)
    occupancy = Counter()
    for tlp, stages in tlps.items():
        if 0 not in stages:
            raise ValueError(f"Missing ingress for TLP {tlp}")
        rx, port = stages[0]
        if not 0 <= port < PORTS:
            raise ValueError(f"Unexpected ingress port {port}")
        flow = flows[port]
        flow["traced"] += 1
        occupancy[rx] += 1
        tx = None
        if 9 in stages:
            tx, egress = stages[9]
            ticks = [stages[stage][0] for stage in sorted(stages)]
            if (
                len(ticks) != 10
                or ticks != sorted(ticks)
                or egress != (port + 3) % PORTS
            ):
                raise ValueError(f"Invalid stages or route for TLP {tlp}")
            occupancy[tx] -= 1

        for i, boundary in enumerate(bounds):
            if rx < boundary and (tx is None or tx >= boundary):
                flow["backlog"][i] += 1
        for tick, label in ((rx, "ingress"), (tx, "egress")):
            if tick is not None and bounds[0] <= tick < bounds[-1]:
                flow[label][bisect_right(bounds, tick) - 1] += 1
        if not bounds[0] <= rx < bounds[-1]:
            continue
        if tx is None:
            flow["censored"] += 1
            continue
        flow["samples"] += 1
        latency["core"].append(tx - rx)
        for stage, (start, end) in enumerate(
            zip(CORE_STAGES, CORE_STAGES[1:])
        ):
            step = f"{start}_to_{end}"
            latency[step].append(stages[stage + 1][0] - stages[stage][0])

    peak = current = 0
    for tick in sorted(occupancy):
        current += occupancy[tick]
        peak = max(peak, current)
    return {
        "samples": len(latency["core"]),
        "censored": sum(flow["censored"] for flow in flows),
        "p99_ns": percentile_ns(latency["core"]),
        "stage_p99_ns": {
            stage: percentile_ns(ticks)
            for stage, ticks in latency.items()
            if stage != "core"
        },
        "peak_core_inflight": peak,
        "flows": flows,
    }


def run_trial(trial, wire_feedback):
    import m5
    from m5.objects import (
        PcieBenchmark,
        PcieTiming,
        Root,
        SrcClockDomain,
        System,
        VoltageDomain,
    )

    period = PERIOD_NS[trial]
    extent = (REQUESTS * PAYLOAD + 4095) // 4096 * 4096
    base = 0x1000
    system = System(mem_mode="timing")
    system.voltage_domain = VoltageDomain()
    system.clk_domain = SrcClockDomain(
        clock=f"{CLOCK_NS}ns", voltage_domain=system.voltage_domain
    )
    # Benchmark starts at its first clock edge and then issues in ticks.
    # Stagger sources to isolate baseline transfers with the same route matrix.
    system.source_clocks = [
        SrcClockDomain(
            clock=f"{delay}ns", voltage_domain=system.voltage_domain
        )
        for delay in START_NS
    ]
    settings = dict(
        base=base,
        window=f"{extent * PORTS}B",
        hops=1,
        outstanding=64,
        mps=256,
        mrrs=512,
        rcb=128,
        tags=128,
        max_transfer=PAYLOAD,
        flit_detail=True,
        flit_wire_feedback=wire_feedback,
        shared_fc=True,
        link_width=128,
        header_credits=64,
        data_credits=256,
        endpoint_cycles=ENDPOINT_CYCLES,
        replay_flits=64,
        fabric_channels=9,
        fabric_cells=5,
        ports_per_slice=3,
        cross_slice_delay="2ns",
        ingress_cdc_cycles=2,
        egress_cdc_cycles=2,
        core_trace_limit=PORTS * REQUESTS * 12,
        bidirectional_endpoints=True,
    )
    system.dut = PcieTiming(**settings)
    destinations = [(port + 3) % PORTS for port in range(PORTS)]
    system.sources = [
        PcieBenchmark(
            clk_domain=system.source_clocks[port],
            address=base + destination * extent,
            transfer_bytes=PAYLOAD,
            request_count=REQUESTS,
            window=64,
            write=True,
            period=f"{period}ns",
        )
        for port, destination in enumerate(destinations)
    ]
    for source in system.sources:
        source.port = system.dut.cpu_side
    root = Root(full_system=False, system=system)
    m5.instantiate()
    dut = root.system.dut

    bounds = [
        (max(START_NS) + period * (WARMUP_PERIODS + WINDOW_PERIODS * index))
        * TICKS_PER_NS
        for index in range(WINDOWS + 1)
    ]
    snapshots = []
    for tick in bounds:
        m5.simulate(tick - m5.curTick())
        if m5.curTick() != tick:
            raise RuntimeError("SW-TAIL did not reach its window boundary")
        snapshots.append(dut.report())

    deadline = (max(START_NS) + period * REQUESTS + 2000) * TICKS_PER_NS
    m5.simulate(deadline - m5.curTick())
    final = dut.report()
    evidence = measure_trace(dut.coreTrace(), bounds)
    source_checks = [
        {
            "finished": source.finished(),
            "verify_writes": dut.idle() and source.verifyWrites(),
            "first_accepted_tick": source.firstAcceptedTick(),
        }
        for source in system.sources
    ]
    waits = dut.waitGraph()
    correct = (
        m5.curTick() == deadline
        and dut.idle()
        and not waits
        and not final["core_trace_dropped"]
        and final["switch_0_transmissions"] == PORTS * REQUESTS
        and all(
            check["finished"]
            and check["verify_writes"]
            and check["first_accepted_tick"] == START_NS[port] * TICKS_PER_NS
            and evidence["flows"][port]["traced"] == REQUESTS
            and final[f"source_{port}_completed_payload_bytes"]
            == REQUESTS * PAYLOAD
            for port, check in enumerate(source_checks)
        )
    )
    for port, flow in enumerate(evidence["flows"]):
        accepted_key = f"source_{port}_accepted_packets"
        bytes_key = f"source_{port}_completed_payload_bytes"
        flow["accepted"] = [
            end[accepted_key] - start[accepted_key]
            for start, end in zip(snapshots, snapshots[1:])
        ]
        flow["endpoint_departures"] = [
            (end[bytes_key] - start[bytes_key]) // PAYLOAD
            for start, end in zip(snapshots, snapshots[1:])
        ]
        # Include work queued before core ingress and after core egress.
        flow["endpoint_backlog"] = [
            snap[accepted_key] - snap[bytes_key] // PAYLOAD
            for snap in snapshots
        ]

    profile = {
        "name": "SW-TAIL",
        "frozen": True,
        "operation": "MemWr",
        "payload_bytes": PAYLOAD,
        "routes": destinations,
        "clock_ns": CLOCK_NS,
        "first_issue_ns": START_NS,
        "requests_per_flow": REQUESTS,
        "source_window": 64,
        "warmup_periods": WARMUP_PERIODS,
        "window_periods": WINDOW_PERIODS,
        "windows": WINDOWS,
        "drain_guard_ns": 2000,
        "settings": settings,
        "source_hashes": source_hashes(REPO),
        "r_ref": min(118 / (PAYLOAD + 12), 1 / ENDPOINT_CYCLES),
        "reference_source": (
            "Per-flow min of x16 FLIT TLP bytes (118 B/ns), aligned "
            "MemWr (64 + 12 B), and one endpoint service per 7 ns"
        ),
        "reference_independent": True,
        "rate_unit": "transactions/ns/flow",
    }
    report = {
        "trial": trial,
        "profile": profile,
        "period_ns": period,
        "boundary_ticks": bounds,
        "deadline_tick": deadline,
        "correctness": correct,
        "evidence": evidence,
        "snapshots": snapshots,
        "source_checks": source_checks,
        "waits": waits,
        "idle": dut.idle(),
        "final": final,
    }
    output = Path(m5.options.outdir) / "tail-trial.json"
    write_json(output, report)


def write_json(path, report):
    path.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n")


def count_stable(flow, expected):
    """Require equal ingress, egress and full-path counts in every window."""
    return all(
        all(value == expected for value in flow[key])
        for key in ("ingress", "egress", "accepted", "endpoint_departures")
    ) and all(
        len(set(flow[key])) == 1 for key in ("backlog", "endpoint_backlog")
    )


def compare_trials(baseline, loaded):
    profile = loaded["profile"]
    left, right = baseline["evidence"], loaded["evidence"]
    matched = (
        baseline["profile"] == profile
        and baseline["trial"] == "baseline"
        and loaded["trial"] == "loaded"
        and baseline["period_ns"] == PERIOD_NS["baseline"]
        and loaded["period_ns"] == PERIOD_NS["loaded"]
        and left["peak_core_inflight"] == 1
        and all(count_stable(flow, WINDOW_PERIODS) for flow in left["flows"])
    )
    stable = all(count_stable(flow, WINDOW_PERIODS) for flow in right["flows"])
    measurement = {
        "boundary": CORE_BOUNDARY,
        "cohort_matches": matched,
        "offered_rate": 1 / loaded["period_ns"],
        "steady_state": stable and not right["censored"],
        "percentile": 99,
        "samples": right["samples"],
        "baseline_samples": left["samples"],
        "latency_ns": right["p99_ns"],
        "baseline_ns": left["p99_ns"],
    }
    correct = baseline["correctness"] and loaded["correctness"]
    return evaluate_performance(
        PerformanceCase(
            "P-08", profile, "gem5_flit_cycle", True, correct, measurement
        )
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("gem5", type=Path)
    parser.add_argument("output_directory", type=Path)
    parser.add_argument("--wire-feedback", action="store_true")
    args = parser.parse_args()
    args.output_directory.mkdir(parents=True, exist_ok=True)
    trials = {}
    for trial in PERIOD_NS:
        folder = args.output_directory / trial
        subprocess.run(
            [
                str(args.gem5.resolve()),
                "-d",
                str(folder),
                str(Path(__file__).resolve()),
                trial,
            ]
            + (["--wire-feedback"] if args.wire_feedback else []),
            check=True,
        )
        trials[trial] = json.loads((folder / "tail-trial.json").read_text())
    evaluation = compare_trials(trials["baseline"], trials["loaded"])
    write_json(args.output_directory / "tail-result.json", evaluation)
    print(json.dumps(evaluation))
    return 0 if evaluation["target_status"] == "PASS" else 1


if __name__ == "__m5_main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trial", choices=tuple(PERIOD_NS))
    parser.add_argument("--wire-feedback", action="store_true")
    args = parser.parse_args()
    run_trial(args.trial, args.wire_feedback)
elif __name__ == "__main__":
    sys.exit(main())
