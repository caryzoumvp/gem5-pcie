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


"""Measure fixed PCIe switch cases on the native FLIT timing model."""

import argparse
import json
import sys
from pathlib import Path

import m5
from m5.objects import (
    PcieBenchmark,
    PcieTiming,
    Root,
    SrcClockDomain,
    System,
    VoltageDomain,
)

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO))

from util.pcie_lab.contracts import source_hashes
from util.pcie_lab.performance import (
    CORE_BOUNDARY,
    PerformanceCase,
    evaluate_performance,
)


def reference_limit(size, write, mps, mrrs, sources):
    """Independent wire/endpoint ceiling for aligned low-address requests."""
    if write:
        requests = (size + mps - 1) // mps
        wire_bytes = size + 12 * requests
    else:
        requests = (size + mrrs - 1) // mrrs
        full, tail = divmod(size, mrrs)
        completions = full * (mrrs // mps) + (tail + mps - 1) // mps
        wire_bytes = max(12 * requests, size + 12 * completions)
    # 128 B/ns on x16, 236 of 256 B for TLP bytes, and one memory
    # request issued per ns on the shared target. Apply the target cut.
    return min(118 * size / wire_bytes, size / requests) / sources


def read_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "profile",
        choices=("SW-LAT", "RTT-64", "SW-BULK", "SW-SMALL", "SW-PERM"),
    )
    parser.add_argument("--write", action="store_true")
    parser.add_argument("--bytes", type=int, choices=(64, 256, 4096))
    parser.add_argument("--sources", type=int, choices=(1, 8), default=1)
    parser.add_argument("--same-slice", action="store_true")
    parser.add_argument("--mrrs", type=int, choices=(512, 4096), default=512)
    parser.add_argument(
        "--fabric-channels", type=int, choices=(3, 9), default=9
    )
    parser.add_argument("--fabric-cells", type=int, choices=(3, 5), default=5)
    parser.add_argument(
        "--replay-flits", type=int, choices=(16, 64), default=64
    )
    parser.add_argument("--wire-feedback", action="store_true")
    parser.add_argument("--verify-permutation", action="store_true")
    args = parser.parse_args()
    if args.verify_permutation and args.profile != "SW-PERM":
        parser.error("--verify-permutation requires SW-PERM")
    if args.profile == "SW-PERM":
        args.sources = 9
    if args.profile == "RTT-64":
        args.bytes = args.bytes or 64
        if args.sources != 1 or args.bytes != 64 or args.write:
            parser.error("RTT-64 requires one requester and one 64 B Read")
    elif args.profile == "SW-LAT":
        args.bytes = args.bytes or 64
        if args.sources != 1 or args.bytes not in (64, 256):
            parser.error("SW-LAT requires one requester and 64/256 B")
    else:
        expected = 4096 if args.profile == "SW-BULK" else 64
        if args.bytes not in (None, expected):
            parser.error(f"{args.profile} requires {expected} B")
        args.bytes = expected
    return args


def service_window(start, end, edges, window_ns):
    links = {}
    for edge in range(edges):
        for direction in ("forward", "reverse"):
            name = f"edge_{edge}_{direction}"
            totals = {
                key.removeprefix(name + "_"): end[key] - start[key]
                for key in end
                if key.startswith(name + "_") and key in start
            }
            if (
                not {
                    "data_bytes",
                    "tlp_bytes",
                    "nonposted_tlps",
                    "completion_tlps",
                    "posted_tlps",
                    "flit_serialized_ticks",
                }
                <= totals.keys()
            ):
                return {}
            totals["serialized_time_fraction"] = totals[
                "flit_serialized_ticks"
            ] / (window_ns * 1000)
            for cls in ("nonposted", "posted", "completion"):
                totals[f"{cls}_tlps_per_second"] = (
                    totals[f"{cls}_tlps"] * 1e9 / window_ns
                )
            links[name] = totals
    core = {
        key.removeprefix("switch_0_"): end[key] - start[key]
        for key in end
        if key == "switch_0_grants"
        or key == "switch_0_transmissions"
        or key.startswith("switch_0_")
        and key.endswith(("_stalls", "_beats"))
    }
    return (
        {"links": links, "core": core}
        if {"grants", "transmissions", "read_beats", "write_beats"}
        <= core.keys()
        else {}
    )


def main():
    args = read_args()
    lat = args.profile == "SW-LAT"
    rtt = args.profile == "RTT-64"
    perm = args.profile == "SW-PERM"
    trace_routes = args.verify_permutation
    requests = (
        1
        if lat or rtt or trace_routes
        else (1024 if args.bytes == 4096 else 8192)
    )
    warmup_ns, window_ns = 1000, 3000
    flow_names = [f"source_{i}" for i in range(args.sources)]
    payload_extent = requests * args.bytes
    extent = (payload_extent + 4095) // 4096 * 4096
    base = 0x1000
    if perm and args.same_slice:
        destinations = [i // 3 * 3 + (i + 1) % 3 for i in range(args.sources)]
    elif perm:
        destinations = [(i + 3) % args.sources for i in range(args.sources)]
    else:
        destinations = list(range(args.sources))
    ports_per_slice = (
        3 if perm else (args.sources + 1 if args.same_slice else 1)
    )
    system = System(mem_mode="timing")
    system.clk_domain = SrcClockDomain(
        clock="1GHz", voltage_domain=VoltageDomain()
    )
    system.dut = PcieTiming(
        base=base,
        window=f"{extent * args.sources}B",
        outstanding=1 if rtt else 64,
        mps=256,
        mrrs=args.mrrs,
        rcb=128,
        tags=128,
        max_transfer=args.bytes,
        flit_detail=True,
        flit_wire_feedback=args.wire_feedback,
        shared_fc=True,
        link_width=128,
        header_credits=64,
        data_credits=256,
        endpoint_cycles=1,
        replay_flits=args.replay_flits,
        fabric_channels=args.fabric_channels,
        fabric_cells=args.fabric_cells,
        ports_per_slice=ports_per_slice,
        cross_slice_delay="2ns",
        ingress_cdc_cycles=2,
        egress_cdc_cycles=2,
        core_trace_limit=1024 if lat or rtt or trace_routes else 0,
        **({"bidirectional_endpoints": True} if perm else {}),
    )
    system.sources = [
        PcieBenchmark(
            address=base + destination * extent,
            transfer_bytes=args.bytes,
            request_count=requests,
            window=1 if rtt else 64,
            write=args.write,
        )
        for destination in destinations
    ]
    for source in system.sources:
        source.port = system.dut.cpu_side
    root = Root(full_system=False, system=system)
    m5.instantiate()

    profile = {
        "name": args.profile,
        "frozen": True,
        "operation": "MemWr" if args.write else "MemRd",
        "bytes": args.bytes,
        "sources": args.sources,
        "sources_layout": {
            name: {
                "base": base + destinations[i] * extent,
                "extent": extent,
                "egress": destinations[i] if perm else args.sources,
            }
            for i, name in enumerate(flow_names)
        },
        "requests_per_source": requests,
        "window_per_source": 1 if rtt else 64,
        "arrival": "one shot" if lat or rtt else "backlogged",
        "mps": 256,
        "mrrs": args.mrrs,
        "rcb": 128,
        "raw_GBps": 128,
        "shared_fc": True,
        "dedicated_header_credits": 64,
        "dedicated_data_credits": 256,
        "requester_tags": 128,
        "core_clock_Hz": 1_000_000_000,
        "link_clock_Hz": 1_000_000_000,
        "parser_tlps_per_cycle": 2,
        "parser_cycles": 1,
        "fabric_channels": args.fabric_channels,
        "cells_per_channel_per_cycle": args.fabric_cells,
        "replay_flits": args.replay_flits,
        "flit_flush_ns": 1,
        "flit_feedback_ns": 15,
        "flit_replay_timeout_ns": 160,
        "feedback_transport": (
            "reverse_nop_flit" if args.wire_feedback else "delay"
        ),
        "endpoint_cycles": 1,
        "endpoint_link_propagation_ns": 3,
        "same_slice": args.same_slice,
        "ports_per_slice": ports_per_slice,
        "cdc_cycles": [2, 2],
        "cross_slice_delay_ns": 2,
        "warmup_ns": 0 if lat or rtt else warmup_ns,
        "window_ns": 0 if lat or rtt else window_ns,
        "source_hashes": source_hashes(REPO),
    }
    dut = root.system.dut
    start = end = {}
    if not lat and not rtt and not trace_routes:
        m5.simulate(warmup_ns * 1000)
        start = dut.report()
        m5.simulate(window_ns * 1000)
        end = dut.report()
    deadline = m5.curTick() + 1_000_000_000
    while m5.curTick() < deadline and (
        not all(source.finished() for source in system.sources)
        or not dut.idle()
    ):
        m5.simulate(min(10_000_000, deadline - m5.curTick()))
    final = dut.report()
    correct = dut.idle() and all(
        source.finished() and (not args.write or source.verifyWrites())
        for source in system.sources
    )
    correct = correct and all(
        final[f"{flow}_completed_payload_bytes"] == payload_extent
        for flow in flow_names
    )
    if trace_routes:
        rx = {
            tlp: port
            for _, stage, _, tlp, port in dut.coreTrace()
            if stage == 0
        }
        tx = {
            tlp: port
            for _, stage, _, tlp, port in dut.coreTrace()
            if stage == 9
        }
        direct = list(enumerate(destinations))
        expected = (
            direct if args.write else direct + [(b, a) for a, b in direct]
        )
        assert (
            correct and not final["core_trace_dropped"] and not dut.waitGraph()
        )
        assert rx.keys() == tx.keys()
        assert sorted((rx[tlp], tx[tlp]) for tlp in rx) == sorted(expected)
        print("PCIe native permutation routes passed")
        return

    if rtt:
        source = system.sources[0]
        measurement = {
            "boundary": "source_submit_to_data_consumable",
            "latency_ns": (
                (source.lastRetiredTick() - source.firstAcceptedTick()) / 1000
                if correct
                else None
            ),
            "trace": dut.coreTrace(),
            "phy_evidence": False,
            "endpoint_evidence": False,
            "budget_closed": False,
        }
        target = "P-04"
    elif lat:
        trace = dut.coreTrace()
        stages = {}
        milestones = [row for row in trace if row[1] < 12]
        for hop, stage, tick, tlp, _ in milestones:
            stages.setdefault((hop, tlp), {})[stage] = tick
        breakdown = (
            bool(stages)
            and all(
                events.keys() == set(range(12)) for events in stages.values()
            )
            and len(milestones) == 12 * len(stages)
            and not final["core_trace_dropped"]
        )
        measurement = {
            "boundary": CORE_BOUNDARY,
            "stage_breakdown_complete": breakdown,
            "latency_ns": (
                max(events[9] - events[0] for events in stages.values()) / 1000
                if breakdown
                else None
            ),
            "trace": trace,
        }
        target = "P-02"
    else:
        target = {"SW-BULK": "P-05", "SW-SMALL": "P-06", "SW-PERM": "P-07"}[
            args.profile
        ]
        b_ref = reference_limit(
            args.bytes, args.write, 256, args.mrrs, 1 if perm else args.sources
        )
        profile.update(
            flow_ids=flow_names,
            b_ref_GBps=dict.fromkeys(flow_names, b_ref),
            reference_source="Base Spec 236/256, headers and endpoint cut",
            reference_independent=True,
        )
        service = service_window(
            start, end, args.sources + (0 if perm else 1), window_ns
        )
        completed_bytes = {
            flow: end[f"{flow}_completed_payload_bytes"]
            - start[f"{flow}_completed_payload_bytes"]
            for flow in flow_names
        }
        measurement = {
            "window_ns": window_ns,
            "window_complete": (
                end["tick"] - start["tick"] == window_ns * 1000
                and all(
                    end[f"{flow}_accepted_packets"] < requests
                    for flow in flow_names
                )
            ),
            "service_breakdown_complete": bool(service),
            "service": service,
            "completed_payload_bytes": completed_bytes,
            "completed_iops": {
                flow: payload / args.bytes * 1e9 / window_ns
                for flow, payload in completed_bytes.items()
            },
            "unfinished": {
                flow: end[f"{flow}_pending_packets"] for flow in flow_names
            },
        }

    evaluated = evaluate_performance(
        PerformanceCase(
            target, profile, "gem5_flit_cycle", True, correct, measurement
        )
    )
    out = Path(m5.options.outdir) / "performance.json"
    out.write_text(
        json.dumps(
            {
                "profile": profile,
                "measurement": measurement,
                "final": final,
                "start": start,
                "end": end,
                "evaluation": evaluated,
                "waits": dut.waitGraph(),
            },
            indent=2,
            allow_nan=False,
        )
        + "\n"
    )
    print(json.dumps(evaluated))


if __name__ == "__m5_main__":
    main()
