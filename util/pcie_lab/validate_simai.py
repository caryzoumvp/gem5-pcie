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


"""Run native SimAI communication workloads through gem5 PCIe."""

import argparse
import copy
import csv
import hashlib
import json
import sys
import tempfile
from collections import (
    Counter,
    defaultdict,
)
from decimal import (
    ROUND_CEILING,
    Decimal,
)
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from util.pcie_lab.cli import repository_metadata
from util.pcie_lab.validate import (
    GEM5,
    ROOT,
    run,
)

CYCLE = ROOT / "configs/example/pcie/cycle.py"
TENSOR = 32 * 1024


def write_json(path, document):
    path.write_text(json.dumps(document, indent=2) + "\n")
    return path


def window_spec(ranks):
    path = ROOT / "util/pcie_lab/examples/cycle_star.json"
    document = json.loads(path.read_text())
    nodes = document["topology"]["nodes"]
    links = document["topology"]["links"]
    document["workload"]["flows"].clear()
    for rank in range(2, ranks):
        node = {
            **nodes[2],
            "id": f"dst{rank - 1}",
            "requester_id": rank,
            "base": rank * 4096,
        }
        edge = copy.deepcopy(links[1])
        edge["id"] = f"link{rank}"
        edge["a"]["port"] = rank
        edge["b"]["node"] = node["id"]
        nodes.append(node)
        links.append(edge)
    document["measurement"] = {
        "warmup_ps": 0,
        "window_ps": 100_000_000,
        "timeout_ps": 100_000_000,
    }
    return document


def ring_fixture(output, ranks, *, slow=False):
    workload = output / f"ring-{ranks}.txt"
    workload.write_text(
        f"HYBRID_TRANSFORMER_FWD_IN_BCKWD model_parallel_NPU_group: {ranks} "
        f"ep: 1 pp: 1 vpp: 1 ga: 1 all_gpus: {ranks} checkpoints: 0 "
        "checkpoint_initiates: 0\n"
        "2\n"
        f"first -1 100 ALLREDUCE {TENSOR} 1 NONE 0 1 NONE 0 1\n"
        f"second -1 100 ALLREDUCE {TENSOR} 1 NONE 0 1 NONE 0 1\n"
    )
    return session_config(window_spec(ranks), ranks, workload, slow=slow)


def session_config(document, ranks, workload, *, slow=False):
    if slow:
        for edge in document["topology"]["links"]:
            for direction in ("forward", "reverse"):
                edge[direction]["cycle_ps"] *= 8
    document["simai"] = {
        "rank_endpoints": ["root", *(f"dst{i}" for i in range(ranks - 1))],
        "segment_bytes": 1024,
        "slots_per_rank": 4,
        "pending_limit": 256,
        "flow_limit": 4096,
        "workload_sha256": hashlib.sha256(workload.read_bytes()).hexdigest(),
    }
    return document, workload


def dependency_fixture(output, name, ranks, rows, *, slow=False):
    workload = output / f"{name}.txt"
    workload.write_text("PCIE_DAG_V1\n" + "\n".join(rows) + "\n")
    document = window_spec(ranks)
    if name == "matching":
        topology = document["topology"]
        topology["nodes"].append(
            {
                "id": "sw1",
                "kind": "switch",
                "port_budget": 2,
                "lane_budget": 32,
            }
        )
        downstream = topology["links"][1]
        upstream = copy.deepcopy(downstream)
        upstream["id"] = "inter-switch"
        upstream["b"] = {"node": "sw1", "port": 0}
        downstream["a"] = {"node": "sw1", "port": 1}
        topology["links"].append(upstream)
    return session_config(document, ranks, workload, slow=slow)


def check_dependency_workloads(output, library):
    # Compute: rank duration_ps predecessors...
    # Message: sender receiver scope channel tag bytes read recv_delay_ps
    #          predecessors...
    scenarios = {
        "clock": (
            2,
            ["T 0 FS 1500", "T 1 NS 0.1", "T 0 US 0.001 0", "T 1 NS 0 1"],
        ),
        "matching": (
            2,
            [
                "M 0 1 0 0 7 64 0 0",
                "M 0 1 0 0 7 64 0 3000000",
                "M 0 1 1 0 7 64 0 0",
                "M 0 1 0 1 7 64 0 0",
                "M 0 1 0 0 8 4096 1 3000 0",
            ],
        ),
        "tree": (
            4,
            [
                "M 2 1 0 0 1 4096 0 0",
                "M 3 1 0 0 2 4096 0 0",
                "C 1 4000 0 1",
                "M 1 0 0 0 3 4096 0 0 2",
                "C 0 2000 3",
                "M 0 1 0 0 4 4096 0 0 4",
                "M 1 2 0 0 5 4096 0 0 5",
                "M 1 3 0 0 6 4096 0 0 5",
            ],
        ),
        "incast": (
            9,
            [f"M {rank} 0 0 0 {rank} 4096 0 0" for rank in range(1, 9)],
        ),
        "host-staging": (
            3,
            ["M 1 0 0 0 1 4096 1 0", "C 0 2000 0", "M 0 2 0 0 2 4096 0 0 1"],
        ),
        "prefill": (
            2,
            [
                "C 0 1000000",
                "M 0 1 0 0 1 32768 0 0 0",
                "C 1 80000 1",
                "M 1 0 0 0 2 4096 0 0 2",
            ],
        ),
        "decode": (
            2,
            [
                "C 0 12000",
                "M 0 1 0 0 1 64 0 0 0",
                "C 1 20000 1",
                "M 1 0 0 0 2 64 0 0 2",
            ],
        ),
        "diffusion": (
            2,
            [
                "C 0 200000",
                "M 0 1 0 0 1 16384 0 0 0",
                "C 1 200000 1",
                "M 1 0 0 0 2 16384 0 0 2",
            ],
        ),
        "moe": (
            4,
            [
                "M 0 2 0 0 1 1024 0 0",
                "M 1 2 0 0 2 3072 0 0",
                "M 3 2 0 0 3 2048 0 0",
                "M 3 0 0 0 4 1024 0 0",
                "C 2 60000 0 1 2",
                "C 0 10000 3",
                "M 2 0 0 0 5 1024 0 0 4",
                "M 2 1 0 0 6 3072 0 0 4",
                "M 2 3 0 0 7 2048 0 0 4",
                "M 0 3 0 0 8 1024 0 0 5",
            ],
        ),
    }
    summaries = {}
    for name, (ranks, rows) in scenarios.items():
        runs = {}
        slowdown = name in ("prefill", "decode", "diffusion", "moe")
        for slow in (False, True) if slowdown else (False,):
            case = name + ("-slow" if slow else "")
            document, workload = dependency_fixture(
                output, case, ranks, rows, slow=slow
            )
            result = run_case(
                output,
                case,
                document,
                "--simai-library",
                library,
                "--simai-workload",
                workload,
            )
            runs[case] = check_dag(output, case, rows, result)
            if name in ("matching", "incast", "host-staging"):
                runs[case]["matched_standalone_transfers"] = check_replay(
                    output, case, result
                )
        if len(runs) == 2:
            assert (
                runs[name + "-slow"]["last_task_ps"]
                > runs[name]["last_task_ps"]
            )
        summaries.update(runs)
    return summaries


def check_dag(output, name, rows, result):
    starts = {}
    ends = {}
    with (output / name / "simai-tasks.tsv").open() as stream:
        for row in csv.DictReader(stream, delimiter="\t"):
            task, tick = int(row["task"]), int(row["tick_ps"])
            table = starts if row["event"] == "start" else ends
            assert task not in table, row
            table[task] = tick
    assert set(starts) == set(ends) == set(range(len(rows)))
    expected = Counter()
    for task, row in enumerate(rows):
        kind, *values = row.split()
        if kind == "T":
            _, unit, delta, *deps = values
            scales = {"FS": "0.001", "NS": "1000", "US": "1000000"}
            duration = (
                Decimal(delta) * Decimal(scales[unit])
            ).to_integral_value(rounding=ROUND_CEILING)
            assert ends[task] - starts[task] == duration
            deps = list(map(int, deps))
        elif kind == "C":
            numbers = list(map(int, values))
            _, duration, *deps = numbers
            assert ends[task] - starts[task] == duration
        else:
            numbers = list(map(int, values))
            src, dst, scope, channel, tag, size, read, delay, *deps = numbers
            expected[src, dst, scope, channel, tag, size, read] += 1
            assert ends[task] > starts[task]
            assert ends[task] >= starts[task] + delay
        assert starts[task] == max((ends[dep] for dep in deps), default=0)

    observed = Counter()
    deliveries = defaultdict(list)
    with (output / name / "simai-messages.tsv").open() as stream:
        for row in csv.DictReader(stream, delimiter="\t"):
            if row["event"] == "send":
                keys = (
                    "src",
                    "dst",
                    "scope",
                    "channel",
                    "tag",
                    "bytes",
                    "read",
                )
                observed[tuple(int(row[key]) for key in keys)] += 1
            if row["event"] == "delivered":
                keys = ("src", "dst", "scope", "channel", "tag")
                key = tuple(int(row[key]) for key in keys)
                deliveries[key].append(int(row["message"]))
    assert observed == expected, (observed, expected)
    assert all(order == sorted(order) for order in deliveries.values())
    assert sum(map(len, deliveries.values())) == sum(expected.values())
    expected_bytes = sum(
        size * count for (*_, size, read), count in expected.items()
    )
    assert result["measurement"]["completed_payload_bytes"] == expected_bytes
    read_flows = [
        flow for flow in result["flows"] if flow["operation"] == "read"
    ]
    assert sum(flow["bytes"] for flow in read_flows) == sum(
        size * count for (*_, size, read), count in expected.items() if read
    )
    if name == "matching":
        assert all(
            flow["path"] == ["dst0", "sw1", "sw0", "root"]
            for flow in read_flows
        )
        for flow in read_flows:
            counts = Counter()
            for link in flow["observed_links"]:
                counts[link["class"]] += link["tlps"]
            # Each aligned 1 KiB transfer uses one MemRd and four 256 B
            # CplDs in this profile, across three links each way.
            assert counts == {"nonposted": 3, "completion": 12}, counts
    return {
        "tasks": len(rows),
        "bytes": expected_bytes,
        "last_task_ps": max(ends.values()),
    }


def run_case(output, name, document, *options, env=()):
    experiment = write_json(output / f"{name}.json", document)
    case = output / name
    run(
        output,
        name,
        "env",
        *env,
        GEM5,
        "-d",
        case,
        CYCLE,
        experiment,
        *options,
        cwd=output,
    )
    result = json.loads((case / "cycle-result.json").read_text())
    assert (
        result["status"] == "pass"
        and result["drained"]
        and not result["waits"]
    )
    return result


def check_ring(output, name, document, result):
    ranks = len(document["simai"]["rank_endpoints"])
    expected_bytes = 2 * (ranks - 1) * TENSOR // ranks
    messages = defaultdict(dict)
    issued = {}
    finished = {}
    with (output / name / "simai-messages.tsv").open() as stream:
        for row in csv.DictReader(stream, delimiter="\t"):
            event = row.pop("event")
            values = {key: int(value) for key, value in row.items()}
            if event == "submit":
                transfer = values["transfer"]
                assert transfer not in issued, transfer
                issued[transfer] = values
            elif event == "finish":
                rank = values["src"]
                assert rank not in finished, rank
                finished[rank] = values["tick_ps"]
            else:
                events = messages[values["message"]]
                assert event not in events, (event, values)
                events[event] = values
    assert set(finished) == set(range(ranks)), finished

    endpoints = document["simai"]["rank_endpoints"]
    flows = result["flows"]
    assert len(flows) == len(issued)
    for index, flow in enumerate(flows):
        transfer = issued[index]
        assert (
            flow["src"] == endpoints[transfer["src"]]
            and flow["dst"] == endpoints[transfer["dst"]]
            and flow["bytes"] == transfer["bytes"]
            and flow["release_ps"] == transfer["tick_ps"]
        ), flow

    totals = Counter()
    phase_bytes = Counter()
    first_send = defaultdict(list)
    last_receive = defaultdict(list)
    tags_per_layer = ranks * 2 * (ranks - 1)
    for events in messages.values():
        assert set(events) == {"send", "recv", "sent", "visible", "delivered"}
        send, recv = events["send"], events["recv"]
        sent, visible = events["sent"], events["visible"]
        delivered = events["delivered"]
        assert send["algorithm"] == 1, send
        rank, target = send["src"], send["dst"]
        assert target == (rank + 1) % ranks, send
        assert (
            send["bytes"] == recv["bytes"] == delivered["bytes"]
            and send["tick_ps"] <= sent["tick_ps"] <= visible["tick_ps"]
            and max(visible["tick_ps"], recv["tick_ps"])
            <= delivered["tick_ps"]
            and delivered["tick_ps"] <= finished[target]
        ), events
        totals[rank] += send["bytes"]
        phase = send["tag"] // tags_per_layer
        assert phase in (0, 1), send
        phase_bytes[rank, phase] += send["bytes"]
        first_send[rank, phase].append(send["tick_ps"])
        last_receive[target, phase].append(delivered["tick_ps"])
    assert totals == {
        rank: 2 * expected_bytes for rank in range(ranks)
    }, totals
    for rank in range(ranks):
        assert phase_bytes[rank, 0] == phase_bytes[rank, 1] == expected_bytes
        assert min(first_send[rank, 1]) > max(last_receive[rank, 0])
    assert result["measurement"]["completed_payload_bytes"] == sum(
        totals.values()
    )
    assert (output / name / "EndToEnd.csv").stat().st_size > 0
    return {
        "messages": len(messages),
        "transfers": len(issued),
        "bytes_sent_per_rank": dict(totals),
        "first_release_ps": min(row["tick_ps"] for row in issued.values()),
        "second_layer_release_ps": min(
            min(first_send[rank, 1]) for rank in range(ranks)
        ),
        "last_rank_finish_ps": max(finished.values()),
    }


def check_replay(output, name, result):
    document = copy.deepcopy(result["input"])
    del document["simai"]
    fields = (
        "src",
        "dst",
        "address",
        "bytes",
        "operation",
        "pattern",
        "release_ps",
        "depends_on",
    )
    document["workload"]["flows"] = [
        {"id": f"replay-{index:06d}", **{key: flow[key] for key in fields}}
        for index, flow in enumerate(result["flows"])
    ]
    replay = run_case(output, f"{name}-replay", document)
    observations = (
        "accepted_ps",
        "responded_ps",
        "completed_ps",
        "observed_links",
        "path",
        "status",
    )
    for actual, expected in zip(result["flows"], replay["flows"], strict=True):
        assert all(actual[key] == expected[key] for key in observations), (
            actual,
            expected,
        )
    assert result["counters"] == replay["counters"]
    return len(replay["flows"])


def check_lifetime_errors(output, library):
    name = "unmatched-timeout"
    document, workload = dependency_fixture(
        output, name, 2, ["M 0 1 0 0 7 64 0 200000000"]
    )
    case = output / name
    run(
        output,
        name,
        GEM5,
        "-d",
        case,
        CYCLE,
        write_json(output / f"{name}.json", document),
        "--simai-library",
        library,
        "--simai-workload",
        workload,
        cwd=output,
        expect_error="(truncated)",
    )
    result = json.loads((case / "cycle-result.json").read_text())
    assert result["status"] == "truncated" and not result["drained"]
    assert result["measurement"]["completed_payload_bytes"] == 64
    assert result["simai"]["missing_receive"] == 1
    assert result["simai"]["unfinished_ranks"] == [0, 1]

    name = "capacity-exceeded"
    document, workload = dependency_fixture(
        output, name, 2, [f"M 0 1 0 0 {tag} 64 0 3000000" for tag in range(3)]
    )
    document["simai"].update(pending_limit=2, slots_per_rank=1)
    run(
        output,
        name,
        GEM5,
        "-d",
        output / name,
        CYCLE,
        write_json(output / f"{name}.json", document),
        "--simai-library",
        library,
        "--simai-workload",
        workload,
        cwd=output,
        expect_error="bounded Astra message capacity exceeded",
    )
    for name, delay in (
        ("negative-delay", "-1"),
        ("overflowing-delay", "1e20"),
    ):
        document, workload = dependency_fixture(
            output, name, 2, [f"T 0 NS {delay}"]
        )
        run(
            output,
            name,
            GEM5,
            "-d",
            output / name,
            CYCLE,
            write_json(output / f"{name}.json", document),
            "--simai-library",
            library,
            "--simai-workload",
            workload,
            cwd=output,
            expect_error="invalid or overflowing Astra delay",
        )


def check_legacy(output, simai, analytical, ns3):
    analytical_dir = output / "analytical"
    (analytical_dir / "results").mkdir(parents=True)
    astra = simai / "astra-sim-alibabacloud"
    (analytical_dir / astra.name).symlink_to(astra, target_is_directory=True)
    run(
        output,
        "analytical",
        analytical,
        "-w",
        simai / "example/workload_analytical.txt",
        "-g",
        "9216",
        "-nv",
        "360",
        "-nic",
        "48.5",
        "-n_p_s",
        "8",
        "-g_p_s",
        "8",
        "-r",
        "m4b-",
        cwd=analytical_dir,
    )
    analytics = analytical_dir / "results/m4b-EndToEnd.csv"
    analytical_hash = hashlib.sha256(analytics.read_bytes()).hexdigest()
    assert analytical_hash == (
        "6a28b5746c2a0e097cd9c8d8b9489d5e4ba14189ca5c398a87abd5c85f0a8eb2"
    )

    ns3_dir = output / "ns3"
    ns3_dir.mkdir()
    run(
        output,
        "ns3-topology",
        sys.executable,
        astra / "inputs/topo/gen_Topo_Template.py",
        "--ro",
        "-g",
        "8",
        "-gt",
        "H20",
        "-bw",
        "200Gbps",
        "-nvbw",
        "2400Gbps",
        cwd=ns3_dir,
    )
    topology = "Rail_Opti_SingleToR_8g_8gps_200Gbps_H20"
    config = (astra / "inputs/config/SimAI.conf").read_text()
    (ns3_dir / "ns3.conf").write_text(
        config.replace("/etc/astra-sim/simulation/", "")
    )
    for name in ("flow1.txt", "trace1.txt"):
        (ns3_dir / name).write_text("0\n")
    log = run(
        output,
        "ns3",
        "env",
        "AS_SEND_LAT=3",
        ns3,
        "-t",
        "1",
        "-w",
        simai / "example/microAllReduce.txt",
        "-n",
        topology,
        "-c",
        "ns3.conf",
        cwd=ns3_dir,
    ).read_text()
    for rank in range(8):
        assert f"All data sent from node {rank} is 146800640\n" in log
        assert f"All data received by node {rank} is 146800640\n" in log
    csv_result = ns3_dir / "ncclFlowModel_EndToEnd.csv"
    ns3_hash = hashlib.sha256(csv_result.read_bytes()).hexdigest()
    assert ns3_hash == (
        "63f865f93bff61ab02739c9314c48202afd337470741fe4389ee275d2b204f27"
    )
    return {"analytical_sha256": analytical_hash, "ns3_sha256": ns3_hash}


def validate(output, simai, jobs, library, *, legacy=None):
    if library is None:
        build = output / "build"
        run(
            output,
            "configure",
            "cmake",
            "-S",
            simai / "astra-sim-alibabacloud/build/simai_pcie",
            "-B",
            build,
            f"-DGEM5_ROOT={ROOT}",
            "-DCMAKE_BUILD_TYPE=Release",
        )
        run(output, "build", "cmake", "--build", build, "-j", str(jobs))
        library = build / "libSimAI_pcie.so"

    fixture = output / "native-fixture.so"
    test_sources = ROOT / "tests/gem5/pcie/configs"
    run(
        output,
        "compile-native-fixture",
        "c++",
        "-std=c++17",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-shared",
        "-fPIC",
        "-I",
        ROOT / "src",
        test_sources / "workload_client.cc",
        "-o",
        fixture,
    )
    run(
        output,
        "native-fixture",
        GEM5,
        "-d",
        output / "native-fixture",
        test_sources / "run_cycle_case.py",
        "external",
        fixture,
    )

    summary = {
        "gem5": repository_metadata(ROOT),
        "simai": repository_metadata(simai),
        "simccl": repository_metadata(simai / "SimCCL"),
    }
    for ranks, slow in ((2, False), (8, False), (8, True)):
        name = f"ring-{ranks}" + ("-slow" if slow else "")
        document, workload = ring_fixture(output, ranks, slow=slow)
        result = run_case(
            output,
            name,
            document,
            "--simai-library",
            library,
            "--simai-workload",
            workload,
            env=("AS_NVLS_ENABLE=1", "AS_NVLS_MIN_BYTES=1"),
        )
        summary[name] = check_ring(output, name, document, result)
        if not slow:
            summary[name]["matched_standalone_transfers"] = check_replay(
                output, name, result
            )
    assert (
        summary["ring-8-slow"]["last_rank_finish_ps"]
        > summary["ring-8"]["last_rank_finish_ps"]
    )
    assert (
        summary["ring-8-slow"]["second_layer_release_ps"]
        > summary["ring-8"]["second_layer_release_ps"]
    )
    summary.update(check_dependency_workloads(output, library))
    check_lifetime_errors(output, library)
    run(
        output,
        "html-import-simai",
        "node",
        ROOT / "tests/pcie_switch/fixtures/test_cycle_html.js",
        "--external-result",
        output / "ring-8/cycle-result.json",
    )
    if legacy:
        summary["legacy"] = check_legacy(output, simai, *legacy)
    report = write_json(output / "summary.json", summary)
    print(f"Native SimAI checks passed. See {report}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--simai-root", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, default=Path.cwd())
    parser.add_argument("--library", type=Path)
    parser.add_argument("--analytical-bin", type=Path)
    parser.add_argument("--ns3-bin", type=Path)
    parser.add_argument("--jobs", type=int, default=8)
    args = parser.parse_args()
    if bool(args.analytical_bin) != bool(args.ns3_bin):
        parser.error("provide both legacy backend binaries")
    if args.jobs < 1:
        parser.error("--jobs requires a positive integer")
    legacy = (
        (args.analytical_bin.resolve(), args.ns3_bin.resolve())
        if args.analytical_bin
        else None
    )
    parent = args.output_dir.resolve()
    parent.mkdir(parents=True, exist_ok=True)
    output = Path(tempfile.mkdtemp(prefix="gem5-pcie-simai-", dir=parent))
    print(f"Validation output: {output}", flush=True)
    validate(
        output,
        args.simai_root.resolve(),
        args.jobs,
        args.library.resolve() if args.library else None,
        legacy=legacy,
    )


if __name__ == "__main__":
    main()
