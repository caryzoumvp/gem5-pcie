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


"""Build and run a configured transparent PCIe tree in native gem5."""

import argparse
import csv
import hashlib
import json
import sys
from pathlib import Path

import m5

from _m5 import core as gem5_core

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))

from util.pcie_lab.cli import repository_metadata
from util.pcie_lab.contracts import (
    ConfigError,
    source_hashes,
)
from util.pcie_lab.cycle_config import (
    UNITS,
    compile_cycle,
)

RESULT_FORMAT = "pcie6-cycle-result-v1"
MAX_TICK = (1 << 64) - 1


def build_root(
    compiled, *, external_flow_limit=0, external_library="", external_config=""
):
    from m5.objects import (
        PcieTiming,
        PcieTreeEndpoint,
        PcieTreeLink,
        PcieTreeSwitch,
        PcieWorkload,
        Root,
        SrcClockDomain,
        System,
        VoltageDomain,
    )

    topology = compiled["topology"]
    leaves = [node for node in topology["nodes"] if node["kind"] != "switch"]
    switches = [node for node in topology["nodes"] if node["kind"] == "switch"]
    node_ids = {
        node["id"]: index for index, node in enumerate(leaves + switches)
    }
    endpoint_params = [
        PcieTreeEndpoint(
            base=node["base"],
            size=node["size"],
            requester_id=node["requester_id"],
        )
        for node in leaves
    ]
    switch_params = [
        PcieTreeSwitch(
            port_budget=node["port_budget"],
            lane_budget=node["lane_budget"],
            **{
                name.removesuffix("_ps"): value
                for name, value in node["resources"].items()
            },
        )
        for node in switches
    ]
    link_params = [
        PcieTreeLink(
            node_a=node_ids[edge["a"]["node"]],
            port_a=edge["a"]["port"],
            node_b=node_ids[edge["b"]["node"]],
            port_b=edge["b"]["port"],
            lanes=edge["lanes"],
            **{
                f"{direction}_{name.removesuffix('_ps')}": value
                for direction in ("forward", "reverse")
                for name, value in edge[direction].items()
            },
        )
        for edge in topology["links"]
    ]
    hardware = compiled["hardware"].copy()
    core_tick = hardware.pop("core_cycle_ps")
    hardware = {
        name.removesuffix("_ps"): (
            f"{value}ps" if name.endswith("_ps") else value
        )
        for name, value in hardware.items()
    }
    system = System(mem_mode="timing")
    system.clk_domain = SrcClockDomain(
        clock=f"{core_tick}ps", voltage_domain=VoltageDomain()
    )
    system.dut = PcieTiming(
        flit_detail=True,
        tree_endpoints=endpoint_params,
        tree_switches=switch_params,
        tree_links=link_params,
        **hardware,
    )

    flows = compiled["workload"]["flows"]
    flow_ids = {flow["id"]: index for index, flow in enumerate(flows)}
    offsets, indices = [0], []
    for flow in flows:
        indices.extend(flow_ids[dep] for dep in flow["depends_on"])
        offsets.append(len(indices))
    system.workload_driver = PcieWorkload(
        dut=system.dut,
        flow_ids=list(flow_ids),
        source_indices=[node_ids[flow["src"]] for flow in flows],
        addresses=[flow["address"] for flow in flows],
        transfer_bytes=[flow["bytes"] for flow in flows],
        writes=[flow["operation"] == "write" for flow in flows],
        patterns=[flow["pattern"] for flow in flows],
        releases=[flow["release_ps"] for flow in flows],
        dependency_offsets=offsets,
        dependency_indices=indices,
        external_flow_limit=external_flow_limit,
        external_library=str(external_library),
        external_config=str(external_config),
    )
    for _ in leaves:
        system.workload_driver.ports = system.dut.cpu_side
    root = Root(full_system=False, system=system)
    runtime = {
        key: {
            node["id"]: {"index": index, "profile": profiles[index].path()}
            for index, node in enumerate(configured)
        }
        for key, configured, profiles in (
            ("endpoints", leaves, endpoint_params),
            ("switches", switches, switch_params),
            ("links", topology["links"], link_params),
        )
    }
    return root, runtime


def measure_window(config, flows, end_tick):
    begin, stop = (
        config["warmup_ps"],
        config["warmup_ps"] + config["window_ps"],
    )
    cohort = [
        flow
        for flow in flows
        if flow["status"] == "pass" and begin <= flow["completed_ps"] < stop
    ]
    latencies = sorted(
        flow["completed_ps"] - flow["accepted_ps"] for flow in cohort
    )
    samples = len(latencies)
    result = {
        **config,
        "window_complete": end_tick >= stop,
        "cohort": (
            "successful completions in " "[warmup_ps, warmup_ps + window_ps)"
        ),
        "completed_payload_bytes": sum(flow["bytes"] for flow in cohort),
        "completed_flows": samples,
        "unfinished_flows": sum(
            flow["completed_ps"] is None or flow["completed_ps"] >= stop
            for flow in flows
        ),
        "samples": samples,
        "latency_boundary": "packet_acceptance_to_destination_or_response",
        "percentile_status": {},
    }
    for key, numerator, denominator, minimum in (
        ("p50", 1, 2, 1),
        ("p99", 99, 100, 10_000),
        ("p999", 999, 1000, 100_000),
    ):
        rank = (samples * numerator + denominator - 1) // denominator
        enough = samples >= minimum and result["window_complete"]
        result[f"{key}_ps"] = latencies[rank - 1] if enough else None
        status = "measured"
        if not result["window_complete"]:
            status = "window_incomplete"
        elif not samples:
            status = "no_samples"
        elif samples < minimum:
            status = "insufficient_samples"
        result["percentile_status"][key] = status
    return result


def native_summary(output, rank_count):
    waiting_send, waiting_recv, finished = set(), set(), {}
    messages = output / "simai-messages.tsv"
    with messages.open() as stream:
        for row in csv.DictReader(stream, delimiter="\t"):
            event = row["event"]
            message = int(row["message"])
            if event == "send":
                waiting_send.add(message)
            elif event == "recv":
                waiting_recv.add(message)
            elif event == "delivered":
                waiting_send.remove(message)
                waiting_recv.remove(message)
            elif event == "finish":
                finished[int(row["src"])] = int(row["tick_ps"])

    summary = {
        "unfinished_ranks": sorted(set(range(rank_count)) - finished.keys()),
        "rank_finish_ps": finished,
        "missing_send": len(waiting_recv - waiting_send),
        "missing_receive": len(waiting_send - waiting_recv),
        "pending_messages": len(waiting_send | waiting_recv),
        "traces": {},
    }
    for path in (
        messages,
        output / "simai-tasks.tsv",
        output / "EndToEnd.csv",
    ):
        if path.is_file():
            fingerprint = hashlib.sha256()
            with path.open("rb") as stream:
                for chunk in iter(lambda: stream.read(65536), b""):
                    fingerprint.update(chunk)
            summary["traces"][path.name] = fingerprint.hexdigest()
    return summary


def run_cycle(compiled, *, simai_library=None, simai_workload=None):
    simai = compiled["input"].get("simai")
    external = {}
    if simai is not None:
        output = Path(m5.options.outdir).resolve()
        config = output / "simai-client.conf"
        leaves = [
            node["id"]
            for node in compiled["topology"]["nodes"]
            if node["kind"] != "switch"
        ]
        placement = [leaves.index(name) for name in simai["rank_endpoints"]]
        config.write_text(
            f"{simai_workload}\n{output}\n"
            f"{len(placement)} {simai['segment_bytes']} "
            f"{simai['slots_per_rank']} {simai['pending_limit']}\n"
            + " ".join(str(index) for index in placement)
            + "\n"
        )
        external = {
            "external_flow_limit": simai["flow_limit"],
            "external_library": simai_library,
            "external_config": config,
        }
    root, runtime = build_root(compiled, **external)
    m5.instantiate()
    dut = root.system.dut
    driver = root.system.workload_driver
    window = compiled["measurement"]
    end_window = window["warmup_ps"] + window["window_ps"]
    if window["warmup_ps"]:
        m5.simulate(window["warmup_ps"])
    start = dut.report()
    if m5.curTick() < end_window:
        m5.simulate(end_window - m5.curTick())
    end = dut.report()

    timeout = window["timeout_ps"]
    while m5.curTick() < timeout and not (driver.finished() and dut.idle()):
        before = m5.curTick()
        m5.simulate(min(100_000, timeout - before))
        if m5.curTick() <= before:
            break
    final = dut.report()
    paths = compiled["topology"]["paths"]
    rows = driver.flowTrace()
    observed = [[] for _ in rows]
    for flow, edge, forward, tlp_class, count in driver.linkTrace():
        link = compiled["topology"]["links"][edge]
        observed[flow].append(
            {
                "link": link["id"],
                "direction": "forward" if forward else "reverse",
                "class": ("posted", "nonposted", "completion")[tlp_class],
                "tlps": count,
            }
        )
    offered_flows = compiled["workload"]["flows"]
    if simai is not None:
        offered_flows = [
            {
                "id": f"pcie-transfer-{index}",
                "src": leaves[source],
                "dst": leaves[dest],
                "address": address,
                "bytes": size,
                "operation": "write" if write else "read",
                "pattern": pattern,
                "release_ps": release,
                "depends_on": [],
            }
            for index, (
                source,
                dest,
                address,
                size,
                write,
                pattern,
                release,
            ) in enumerate(driver.externalFlows())
        ]
    flows = []
    for flow, row, links in zip(offered_flows, rows, observed, strict=True):
        accepted, responded, completed, outcome = row
        flows.append(
            {
                **flow,
                "path": paths[flow["src"]][flow["dst"]],
                "observed_links": links,
                "accepted_ps": None if accepted == MAX_TICK else accepted,
                "responded_ps": None if responded == MAX_TICK else responded,
                "completed_ps": None if completed == MAX_TICK else completed,
                "latency_ps": (completed - accepted if outcome == 1 else None),
                "status": ("pending", "pass", "fail")[outcome],
            }
        )
    measurement = measure_window(window, flows, end["tick"])
    drained = driver.finished() and dut.idle()
    status = "truncated"
    if drained and measurement["window_complete"]:
        status = (
            "pass" if all(f["status"] == "pass" for f in flows) else "fail"
        )
    result = {
        "format": RESULT_FORMAT,
        "units": UNITS,
        "mode": "pcie6_timing",
        "link_fidelity": "gem5_flit_cycle",
        "status": status,
        "config_hash": compiled["config_hash"],
        "input": compiled["input"],
        "hardware": compiled["hardware"],
        "topology": {**compiled["topology"], "runtime": runtime},
        "measurement": measurement,
        "flows": flows,
        "counters": {"start": start, "end": end, "final": final},
        "waits": dut.waitGraph(),
        "drained": drained,
        "producer": {
            **repository_metadata(ROOT),
            "source_hashes": source_hashes(ROOT),
            "gem5_version": gem5_core.gem5Version,
            "gem5_compile_date": gem5_core.compileDate,
        },
        "scope": [
            "initialized Gen6, VC0, store-and-forward Memory requests",
            "coordinated FC updates; analytical reference is separate",
        ],
    }
    if simai is not None:
        result["producer"]["simai_client_sha256"] = hashlib.sha256(
            simai_library.read_bytes()
        ).hexdigest()
        result["producer"]["simai"] = json.loads(
            (output / "simai-build.json").read_text()
        )
        result["scope"].append("SimAI workload, abstract payload")
        result["simai"] = native_summary(output, len(placement))
        for key in (
            "unfinished_ranks",
            "missing_send",
            "missing_receive",
            "pending_messages",
        ):
            if result["simai"][key]:
                result["waits"].append(f"simai.{key}={result['simai'][key]}")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("experiment", type=Path)
    parser.add_argument("--simai-library", type=Path)
    parser.add_argument("--simai-workload", type=Path)
    args = parser.parse_args()
    try:
        compiled = compile_cycle(json.loads(args.experiment.read_text()))
        simai = compiled["input"].get("simai")
        if simai is not None:
            if not args.simai_library or not args.simai_workload:
                parser.error(
                    "SimAI requires --simai-library and --simai-workload"
                )
            args.simai_workload = args.simai_workload.resolve(strict=True)
            args.simai_library = args.simai_library.resolve(strict=True)
            digest = hashlib.sha256(
                args.simai_workload.read_bytes()
            ).hexdigest()
            if digest != simai["workload_sha256"]:
                parser.error("SimAI workload does not match workload_sha256")
        elif args.simai_library or args.simai_workload:
            parser.error(
                "the ExperimentSpec is missing the simai configuration"
            )
    except (ConfigError, OSError, json.JSONDecodeError) as error:
        parser.exit(2, f"PCIe experiment rejected: {error}\n")
    result = run_cycle(
        compiled,
        simai_library=args.simai_library,
        simai_workload=args.simai_workload,
    )
    output = Path(m5.options.outdir) / "cycle-result.json"
    output.write_text(json.dumps(result, indent=2, allow_nan=False) + "\n")
    print(f"PCIe cycle result: {output} ({result['status']})")
    return 0 if result["status"] == "pass" else 1


if __name__ == "__m5_main__":
    raise SystemExit(main())
