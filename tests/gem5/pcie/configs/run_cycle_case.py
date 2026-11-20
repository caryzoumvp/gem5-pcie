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

"""Check standalone cycle topology, traffic, and completion."""

import copy
import json
import sys
from pathlib import Path

import m5

REPO = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(REPO))

from util.pcie_lab.contracts import ConfigError
from util.pcie_lab.cycle_config import compile_cycle

A = 0x1000
B = 0x5000
PAYLOAD = 0x5A


def experiment():
    return {
        "format": "pcie6-cycle-experiment-v1",
        "units": {"time": "ps", "payload": "B"},
        "hardware": {
            "mps": 256,
            "mrrs": 512,
            "rcb": 128,
            "core_cycle_ps": 1000,
            "flit_replay_timeout_ps": 10_000_000,
        },
        "topology": {"kind": "transparent_tree", "nodes": [], "links": []},
        "workload": {"flows": []},
        "measurement": {
            "warmup_ps": 0,
            "window_ps": 20_000_000,
            "timeout_ps": 20_000_000,
        },
    }


def leaf(topology, name, requester, base, kind="endpoint"):
    topology["nodes"].append(
        {
            "id": name,
            "kind": kind,
            "requester_id": requester,
            "base": base,
            "size": 0x4000,
        }
    )


def switch(topology, name, ports, **resources):
    topology["nodes"].append(
        {
            "id": name,
            "kind": "switch",
            "port_budget": ports,
            "lane_budget": ports * 4,
            "resources": resources,
        }
    )


def link(topology, name, a, ap, b, bp, **forward):
    budget = {
        "width": 16,
        "cycle_ps": 1000,
        "propagation_ps": 3000,
        "credit_delay_ps": 5000,
    }
    topology["links"].append(
        {
            "id": name,
            "a": {"node": a, "port": ap},
            "b": {"node": b, "port": bp},
            "lanes": 4,
            "forward": {**budget, **forward},
            "reverse": budget,
        }
    )


def flow(
    workload, name, op, src, dst, addr, size, after=(), release=0, byte=PAYLOAD
):
    workload["flows"].append(
        {
            "id": name,
            "src": src,
            "dst": dst,
            "operation": op,
            "address": addr,
            "bytes": size,
            "release_ps": release,
            "depends_on": list(after),
            "pattern": byte,
        }
    )


def linear(case):
    spec = experiment()
    topology, workload = spec["topology"], spec["workload"]
    branch = case == "branch"
    chain = case in ("chain", "branch")
    leaf(topology, "root", 4, A, "root")
    leaf(topology, "target", 3, B)
    switch(topology, "s0", 3 if branch else 2, core_descriptors=11)
    link(topology, "0-up", "root", 0, "s0", 0)
    down = "s0"
    path = ["root", "s0"]
    if chain:
        switch(topology, "s1", 2, fabric_channels=3)
        link(topology, "1-isl", "s0", 1, "s1", 0)
        down = "s1"
        path.append("s1")
    path.append("target")
    final_budget = {}
    if case == "width":
        final_budget = {"width": 8, "cycle_ps": 2000}
    if case == "delay":
        final_budget = {"propagation_ps": 500_000}
    if case == "endpoint-delay":
        spec["hardware"]["endpoint_cycles"] = 100
    link(topology, "2-down", down, 1, "target", 0, **final_budget)
    if branch:
        leaf(topology, "idle", 9, 0x9000)
        link(topology, "3-idle", "s0", 2, "idle", 0)
    size = 4096 if case == "chain" else 64
    flow(workload, "write", "write", "root", "target", B, size, release=123)
    flow(workload, "read", "read", "root", "target", B, size, after=["write"])
    return spec, path, size


def final_stats(result):
    assert result["status"] == "pass", result
    assert result["drained"] and not result["waits"], result["waits"]
    window = result["measurement"]
    counters = result["counters"]
    assert counters["start"]["tick"] == window["warmup_ps"]
    assert counters["end"]["tick"] == window["warmup_ps"] + window["window_ps"]
    assert (
        window["samples"] == window["completed_flows"] == len(result["flows"])
    )
    assert window["unfinished_flows"] == 0
    assert window["completed_payload_bytes"] == sum(
        flow["bytes"] for flow in result["flows"]
    )
    assert window["p99_ps"] is None and window["p999_ps"] is None
    stats = counters["final"]
    for key in (
        "pending_packets",
        "pending_tlps",
        "target_pending_completions",
        "target_rx_waiting",
        "source_completion_reservation_bytes",
    ):
        assert stats[key] == 0, (key, stats[key])
    return stats


def edge_counts(stats, edge, forward, reverse):
    labels = ("posted_tlps", "nonposted_tlps", "completion_tlps")
    for direction, expected in (("forward", forward), ("reverse", reverse)):
        prefix = f"edge_{edge}_{direction}_"
        assert tuple(stats[prefix + label] for label in labels) == expected, (
            prefix,
            expected,
            {label: stats[prefix + label] for label in labels},
        )


def check_flow_links(row, expected):
    actual = [
        (item["link"], item["direction"], item["class"], item["tlps"])
        for item in row["observed_links"]
    ]
    assert sorted(actual) == sorted(expected), (row["id"], actual, expected)


def check_widths(spec, result, stats):
    effective = result["topology"]["links"]
    declared = sorted(spec["topology"]["links"], key=lambda link: link["id"])
    assert effective == declared, (effective, declared)
    for edge, link_spec in enumerate(declared):
        for direction in ("forward", "reverse"):
            config = link_spec[direction]
            prefix = f"edge_{edge}_{direction}_"
            ticks = (256 + config["width"] - 1) // config["width"]
            ticks *= config["cycle_ps"]
            original = stats[prefix + "flit_original"]
            control = stats[prefix + "flit_control"]
            actual = stats[prefix + "flit_serialized_ticks"]
            assert stats[prefix + "flit_replayed"] == 0, prefix
            assert actual == ticks * (original + control), (prefix, actual)
            assert stats[prefix + "flit_control_ticks"] == ticks * control


def check_linear(case, spec, path, size, result):
    stats = final_stats(result)
    records = {row["id"]: row for row in result["flows"]}
    read, write = records["read"], records["write"]
    for row in (read, write):
        assert row["path"] == path, row
        assert row["bytes"] == size, row
        assert row["status"] == "pass", row
    assert write["accepted_ps"] == 123, write
    assert (
        write["accepted_ps"] <= write["responded_ps"] < write["completed_ps"]
    )
    assert read["accepted_ps"] == write["completed_ps"], (read, write)
    assert read["accepted_ps"] < read["responded_ps"] == read["completed_ps"]
    expected = (16, 8, 16) if size == 4096 else (1, 1, 1)
    routes = ["0-up", "2-down"]
    if len(path) == 4:
        routes.insert(1, "1-isl")
    check_flow_links(
        write, [(route, "forward", "posted", expected[0]) for route in routes]
    )
    check_flow_links(
        read,
        [
            row
            for route in routes
            for row in (
                (route, "forward", "nonposted", expected[1]),
                (route, "reverse", "completion", expected[2]),
            )
        ],
    )
    for edge in range(len(path) - 1):
        edge_counts(stats, edge, (*expected[:2], 0), (0, 0, expected[2]))
        for direction in ("forward", "reverse"):
            prefix = f"edge_{edge}_{direction}_"
            assert stats[prefix + "data_bytes"] == size
            assert stats[prefix + "flit_original"] > 0
            assert stats[prefix + "flit_control"] > 0
    if case == "branch":
        edge_counts(stats, 3, (0, 0, 0), (0, 0, 0))
        assert stats["edge_3_forward_flit_serialized_ticks"] == 0
        assert stats["edge_3_reverse_flit_serialized_ticks"] == 0
    assert stats["accepted_packets"] == 2
    assert stats["visible_write_tlps"] == expected[0]
    assert stats["completed_read_packets"] == 1
    check_widths(spec, result, stats)
    # Even without serialization or queueing, write visibility requires
    # traversal of both physical links and the target's service cycles.
    minimum_delay = (
        3000
        + (500_000 if case == "delay" else 3000)
        + (100_000 if case == "endpoint-delay" else 3000)
    )
    assert write["completed_ps"] >= 123 + minimum_delay, write
    assert read["accepted_ps"] >= 123 + minimum_delay, read


def incast():
    spec = experiment()
    topology, workload = spec["topology"], spec["workload"]
    leaf(topology, "root", 0, A, "root")
    switch(topology, "s0", 9)
    link(topology, "0-target", "s0", 0, "root", 0)
    for index in range(1, 9):
        name = f"source{index}"
        leaf(topology, name, index, index * 0x4000 + A)
        link(topology, str(index), name, 0, "s0", index)
        address = A + index * 64
        flow(
            workload,
            f"write{index}",
            "write",
            name,
            "root",
            address,
            64,
            byte=index,
        )
        flow(
            workload,
            f"read{index}",
            "read",
            name,
            "root",
            address,
            64,
            after=[f"write{index}"],
            byte=index,
        )
    return spec


def check_incast(result):
    stats = final_stats(result)
    edge_counts(stats, 0, (8, 8, 0), (0, 0, 8))
    records = {row["id"]: row for row in result["flows"]}
    for index in range(1, 9):
        edge_counts(stats, index, (1, 1, 0), (0, 0, 1))
        write, read = records[f"write{index}"], records[f"read{index}"]
        assert read["path"] == [f"source{index}", "s0", "root"], read
        assert read["accepted_ps"] == write["completed_ps"], (read, write)
        assert write["status"] == read["status"] == "pass", (read, write)
        check_flow_links(
            write,
            [
                ("0-target", "forward", "posted", 1),
                (str(index), "forward", "posted", 1),
            ],
        )
    assert stats["accepted_packets"] == 16
    assert stats["visible_write_tlps"] == stats["completed_read_packets"] == 8


def collective():
    spec = experiment()
    topology, workload = spec["topology"], spec["workload"]
    leaf(topology, "root", 0, A, "root")
    leaf(topology, "left", 1, B)
    leaf(topology, "right", 2, 0x9000)
    switch(topology, "s0", 3)
    link(topology, "0", "root", 0, "s0", 0)
    link(topology, "1", "left", 0, "s0", 1)
    link(topology, "2", "right", 0, "s0", 2)
    transfers = [
        ("root", "left", B),
        ("left", "right", 0x9000),
        ("right", "root", A),
    ]
    # Two explicit rounds of a three-participant exchange. All first-round
    # writes must be visible before any second-round Read can be accepted.
    writes = [f"write{idx}" for idx in range(3)]
    for idx, (src, dst, addr) in enumerate(transfers):
        flow(workload, writes[idx], "write", src, dst, addr, 64, byte=idx + 1)
        flow(
            workload,
            f"read{idx}",
            "read",
            src,
            dst,
            addr,
            64,
            after=writes,
            byte=idx + 1,
        )
    return spec


def check_collective(result):
    stats = final_stats(result)
    records = {row["id"]: row for row in result["flows"]}
    barrier = max(records[f"write{idx}"]["completed_ps"] for idx in range(3))
    for idx in range(3):
        edge_counts(stats, idx, (1, 1, 1), (1, 1, 1))
        read, write = records[f"read{idx}"], records[f"write{idx}"]
        assert read["accepted_ps"] == barrier, read
        assert write["status"] == read["status"] == "pass", (read, write)
    assert stats["accepted_packets"] == 6
    assert stats["visible_write_tlps"] == stats["completed_read_packets"] == 3


def timeout():
    spec, _, _ = linear("star")
    spec["measurement"] = {
        "warmup_ps": 1_000,
        "window_ps": 5_000,
        "timeout_ps": 10_000,
    }
    spec["workload"]["flows"][0]["release_ps"] = 9_999
    return spec


def check_timeout(result):
    assert result["status"] == "truncated" and not result["drained"], result
    window = result["measurement"]
    assert window["window_complete"]
    assert window["samples"] == window["completed_flows"] == 0
    assert window["completed_payload_bytes"] == 0
    assert window["unfinished_flows"] == 2
    assert all(window[f"{key}_ps"] is None for key in ("p50", "p99", "p999"))
    counters = result["counters"]
    for key, tick in (("start", 1_000), ("end", 6_000), ("final", 10_000)):
        assert counters[key]["tick"] == tick, counters[key]
    assert counters["end"]["accepted_packets"] == 0
    assert counters["final"]["accepted_packets"] == 1
    records = {row["id"]: row for row in result["flows"]}
    assert records["write"]["accepted_ps"] == 9_999
    assert records["read"]["accepted_ps"] is None
    for row in records.values():
        assert row["status"] == "pending" and row["completed_ps"] is None, row


def invalid():
    spec, _, _ = linear("star")
    topo = spec["topology"]
    cases = (
        (topo["links"][1]["a"], {"port": 0}, "duplicate"),
        (topo["nodes"][1], {"base": A}, "overlap"),
        (topo["nodes"][2], {"lane_budget": 7}, "lanes"),
        (topo["links"][1]["b"], {"node": "missing"}, "identity"),
    )
    for obj, changes, expected in cases:
        original = copy.deepcopy(obj)
        obj.update(changes)
        try:
            compile_cycle(spec)
        except ConfigError as error:
            assert error.code == expected, error
        else:
            raise AssertionError(f"Accepted invalid configuration: {changes}")
        obj.update(original)
    assert m5.curTick() == 0


def external(library=""):
    from cycle import build_root

    spec, _, size = linear("chain")
    spec["workload"]["flows"].clear()
    root, topology = build_root(
        compile_cycle(spec), external_flow_limit=2, external_library=library
    )
    m5.instantiate()
    driver = root.system.workload_driver
    dut = root.system.dut
    source = topology["endpoints"]["root"]["index"]
    target = topology["endpoints"]["target"]["index"]
    if not library:
        m5.simulate(123)
        assert not driver.submitExternal(
            source, target, 1, size, True, PAYLOAD
        )
        assert not driver.submitExternal(
            source, source, 0, size, True, PAYLOAD
        )
        assert driver.submitExternal(source, target, 0, size, True, PAYLOAD)
        # Independent archived M4A chain profile from the static driver.
        m5.simulate(443_000 - m5.curTick())
        assert driver.submitExternal(source, target, 0, size, False, PAYLOAD)
        assert not driver.submitExternal(
            source, target, 0, size, False, PAYLOAD
        )
    m5.simulate(spec["measurement"]["window_ps"] - m5.curTick())
    write, read = driver.flowTrace()
    assert tuple(write[:3]) == (123, 1000, 443_000)
    assert read[0] == write[2] and read[0] < read[1] == read[2]
    assert read[2] == 990_000
    assert write[3] == read[3] == 1
    assert driver.finished() and dut.idle() and not dut.waitGraph()
    for edge in range(3):
        edge_counts(dut.report(), edge, (16, 8, 0), (0, 0, 16))


def main():
    case = sys.argv[1]
    if case == "invalid":
        invalid()
    else:
        sys.path.insert(0, str(REPO / "configs/example/pcie"))
        from cycle import run_cycle

        if case == "external":
            external(sys.argv[2] if len(sys.argv) > 2 else "")
            print("PCIe cycle external passed")
            return
        special_cases = {
            "incast": (incast, check_incast),
            "collective": (collective, check_collective),
            "timeout": (timeout, check_timeout),
        }
        if case in special_cases:
            make_spec, verify = special_cases[case]
            spec = make_spec()
            result = run_cycle(compile_cycle(spec))
            verify(result)
        else:
            spec, path, size = linear(case)
            result = run_cycle(compile_cycle(spec))
            check_linear(case, spec, path, size, result)
        outdir = Path(m5.options.outdir)
        for filename, data in (
            ("experiment.json", spec),
            ("result.json", result),
        ):
            (outdir / filename).write_text(
                json.dumps(data, ensure_ascii=False, indent=2) + "\n",
                encoding="utf-8",
            )
    print(f"PCIe cycle {case} passed")


if __name__ == "__m5_main__":
    main()
