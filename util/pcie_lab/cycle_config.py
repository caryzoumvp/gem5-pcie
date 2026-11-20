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

"""Compile a standalone PCIe cycle experiment before gem5 instantiation."""

import copy
import hashlib
import json
import math
import struct
from collections import deque

from .contracts import (
    check_keys,
    fail,
    integer,
    object_value,
)

FORMAT = "pcie6-cycle-experiment-v1"
UNITS = {"time": "ps", "payload": "B"}
MAX_SAFE_INTEGER = (1 << 53) - 1
UINT32_MAX = (1 << 32) - 1
HARDWARE_DEFAULTS = {
    "outstanding": 8,
    "tags": 8,
    "max_transfer": 65536,
    "completion_slots": 64,
    "endpoint_cycles": 3,
    "header_credits": 4,
    "data_credits": 32,
    "replay_flits": 8,
    "flit_flush_ps": 1000,
    "flit_feedback_ps": 15000,
    "flit_replay_timeout_ps": 160000,
    "shared_fc": True,
    "flit_wire_feedback": True,
}
RESOURCE_DEFAULTS = {
    "core_descriptors": 8,
    "core_sectors": 256,
    "active_voqs": 3,
    "voq_cells": 64,
    "staging_cells": 64,
    "fabric_channels": 2,
    "fabric_cells": 1,
    "ports_per_slice": 1,
    "cross_slice_delay_ps": 0,
    "parser_tlps_per_cycle": 2,
    "parser_cycles": 1,
    "ingress_cdc_cycles": 0,
    "egress_cdc_cycles": 0,
}


def _typed_json(value):
    if value is None:
        return ["null"]
    if isinstance(value, bool):
        return ["bool", value]
    if isinstance(value, str):
        _validate_string(value)
        return ["string", value]
    if type(value) in (int, float):
        if abs(value) > MAX_SAFE_INTEGER or not math.isfinite(value):
            fail("experiment", "range", "expected a finite JSON-safe number")
        return ["number", struct.pack(">d", value or 0).hex()]
    if isinstance(value, list):
        return ["array", [_typed_json(item) for item in value]]
    if isinstance(value, dict):
        for key in value:
            _validate_string(key)
        keys = sorted(value, key=lambda key: key.encode("utf-16-be"))
        return ["object", [[key, _typed_json(value[key])] for key in keys]]
    fail("experiment", "type", "expected plain JSON data")


def _validate_string(value):
    if not isinstance(value, str):
        fail("experiment", "type", "expected JSON string keys")
    if any(0xD800 <= ord(char) <= 0xDFFF for char in value):
        fail("experiment", "unicode", "strings must contain Unicode scalars")


def config_hash(document):
    """Digest the input document using a browser-compatible encoding."""
    canonical = json.dumps(
        _typed_json(document), ensure_ascii=False, separators=(",", ":")
    )
    return hashlib.sha256(canonical.encode("utf-8")).hexdigest()


def _number(value, field, minimum=0, maximum=MAX_SAFE_INTEGER):
    integer(value, field, minimum)
    if value > maximum:
        fail(field, "range", f"expected integer <= {maximum}")
    return value


def _array(value, field):
    if not isinstance(value, list):
        fail(field, "type", "expected an array")
    return value


def _identifier(value, field):
    if not isinstance(value, str) or not value:
        fail(field, "identity", "expected a nonempty string ID")
    return value


def _aligned(value, field, alignment, minimum=0, maximum=MAX_SAFE_INTEGER):
    _number(value, field, minimum, maximum)
    if value % alignment:
        fail(field, "alignment", f"expected a multiple of {alignment} bytes")
    return value


def _compile_hardware(raw):
    field = "hardware"
    required = ("mps", "mrrs", "rcb", "core_cycle_ps")
    check_keys(raw, (*required, *HARDWARE_DEFAULTS), field)
    hardware = {**HARDWARE_DEFAULTS, **raw}
    for key in ("mps", "mrrs"):
        value = _number(hardware.get(key), f"{field}.{key}", 128, 4096)
        if value & (value - 1):
            fail(f"{field}.{key}", "size", "expected a power of two")
    if _number(hardware.get("rcb"), "hardware.rcb") != 128:
        fail("hardware.rcb", "unsupported", "RCB must be 128 B")
    _number(hardware.get("core_cycle_ps"), "hardware.core_cycle_ps", 1)
    count_limits = {
        "outstanding": UINT32_MAX,
        "tags": 768,
        "completion_slots": UINT32_MAX,
        "endpoint_cycles": UINT32_MAX,
        "header_credits": 65536,
        "data_credits": 1048576,
        "replay_flits": 511,
    }
    for name, limit in count_limits.items():
        _number(hardware[name], f"hardware.{name}", 1, limit)
    _aligned(
        hardware["max_transfer"], "hardware.max_transfer", 4, 4, UINT32_MAX
    )
    for name in (
        "flit_flush_ps",
        "flit_feedback_ps",
        "flit_replay_timeout_ps",
    ):
        _number(hardware[name], f"hardware.{name}", 1)
    for name in ("shared_fc", "flit_wire_feedback"):
        if type(hardware[name]) is not bool:
            fail(f"hardware.{name}", "type", "expected a boolean")
    max_data = min(hardware["mps"], hardware["max_transfer"])
    if hardware["data_credits"] * 16 < max_data:
        fail(
            "hardware.data_credits", "capacity", "cannot hold one maximum TLP"
        )
    max_completions = 0
    for offset in range(0, hardware["rcb"], 4):
        remaining = min(
            hardware["mrrs"], hardware["max_transfer"], 4096 - offset
        )
        count = 1
        while remaining > hardware["mps"]:
            length = hardware["mps"] - offset % hardware["rcb"]
            offset += length
            remaining -= length
            count += 1
        max_completions = max(max_completions, count)
    if hardware["completion_slots"] < max_completions:
        fail(
            "hardware.completion_slots",
            "capacity",
            "cannot hold one maximum Read's Completion fragments",
        )
    return hardware


def _compile_resources(raw, field, hardware):
    check_keys(raw, RESOURCE_DEFAULTS, field)
    resources = {**RESOURCE_DEFAULTS, **raw}
    zero_allowed = {
        "cross_slice_delay_ps",
        "ingress_cdc_cycles",
        "egress_cdc_cycles",
    }
    for name, value in resources.items():
        maximum = MAX_SAFE_INTEGER if name.endswith("_ps") else UINT32_MAX
        _number(
            value, f"{field}.{name}", int(name not in zero_allowed), maximum
        )
    max_payload = min(hardware["mps"], hardware["max_transfer"])
    minimums = {
        "core_sectors": (max_payload + 63) // 64,
        "voq_cells": (max_payload + 16 + 63) // 64,
        "staging_cells": (max_payload + 16 + 63) // 64,
    }
    for name, minimum in minimums.items():
        if resources[name] < minimum:
            fail(f"{field}.{name}", "capacity", "cannot hold one maximum TLP")
    return resources


def _compile_nodes(raw, hardware):
    by_id = {}
    requester_ids = set()
    windows = []
    root = None
    for index, node in enumerate(_array(raw, "topology.nodes")):
        field = f"topology.nodes[{index}]"
        object_value(node, field)
        node_id = _identifier(node.get("id"), f"{field}.id")
        if node_id in by_id:
            fail(f"{field}.id", "duplicate", "duplicate node ID")
        kind = node.get("kind")
        if kind == "switch":
            check_keys(
                node,
                ("id", "kind", "lane_budget", "port_budget", "resources"),
                field,
            )
            _number(
                node.get("port_budget"), f"{field}.port_budget", 2, UINT32_MAX
            )
            _number(
                node.get("lane_budget"), f"{field}.lane_budget", 1, UINT32_MAX
            )
            resources = _compile_resources(
                node.get("resources", {}), f"{field}.resources", hardware
            )
            by_id[node_id] = {**node, "resources": resources}
        elif kind in ("root", "endpoint"):
            if kind == "root":
                if root is not None:
                    fail(f"{field}.kind", "count", "exactly one root required")
                root = node_id
            check_keys(
                node, ("id", "kind", "requester_id", "base", "size"), field
            )
            requester = _number(
                node.get("requester_id"), f"{field}.requester_id", 0, 65535
            )
            if requester in requester_ids:
                fail(
                    f"{field}.requester_id",
                    "duplicate",
                    "duplicate Requester ID",
                )
            requester_ids.add(requester)
            base = _aligned(node.get("base"), f"{field}.base", 4096)
            size = _aligned(node.get("size"), f"{field}.size", 4096, 4096)
            _number(base + size - 1, f"{field}.size")
            windows.append((base, base + size, field))
            by_id[node_id] = node.copy()
        else:
            fail(f"{field}.kind", "enum", "expected root, switch or endpoint")
    if (
        root is None
        or len(requester_ids) < 2
        or len(by_id) == len(requester_ids)
    ):
        fail(
            "topology.nodes",
            "count",
            "one root, at least one switch and one endpoint required",
        )
    windows.sort()
    for (_, end, _), (next_base, _, field) in zip(windows, windows[1:]):
        if end > next_base:
            fail(f"{field}.base", "overlap", "overlapping target windows")
    return by_id, root


def _compile_direction(raw, field, lanes, hardware):
    keys = ("width", "cycle_ps", "propagation_ps", "credit_delay_ps")
    check_keys(raw, keys, field)
    width = _number(raw.get("width"), f"{field}.width", 1, UINT32_MAX)
    cycle = _number(raw.get("cycle_ps"), f"{field}.cycle_ps", 1)
    propagation = _number(raw.get("propagation_ps"), f"{field}.propagation_ps")
    _number(raw.get("credit_delay_ps"), f"{field}.credit_delay_ps")
    if 1000 * width > lanes * 8 * cycle:
        fail(f"{field}.width", "bandwidth", "exceeds Gen6 lane wire budget")
    if hardware["flit_replay_timeout_ps"] <= (
        propagation + hardware["flit_feedback_ps"]
    ):
        fail(
            f"{field}.propagation_ps",
            "timeout",
            "propagation plus feedback exceeds the replay timeout",
        )
    return raw.copy()


def _compile_links(raw, nodes, hardware):
    links = {}
    neighbors = {node: {} for node in nodes}
    lane_use = dict.fromkeys(nodes, 0)
    for index, link in enumerate(_array(raw, "topology.links")):
        field = f"topology.links[{index}]"
        check_keys(
            link, ("id", "a", "b", "lanes", "forward", "reverse"), field
        )
        link_id = _identifier(link.get("id"), f"{field}.id")
        if link_id in links:
            fail(f"{field}.id", "duplicate", "duplicate link ID")
        lanes = _number(link.get("lanes"), f"{field}.lanes", 1, 16)
        if lanes not in (1, 2, 4, 8, 16):
            fail(f"{field}.lanes", "lanes", "unsupported PCIe link width")
        local = {}
        for key in ("a", "b"):
            point = link.get(key)
            point_field = f"{field}.{key}"
            check_keys(point, ("node", "port"), point_field)
            node = _identifier(point.get("node"), f"{point_field}.node")
            if node not in nodes:
                fail(f"{point_field}.node", "identity", "unknown node ID")
            port = _number(
                point.get("port"), f"{point_field}.port", 0, UINT32_MAX
            )
            if port in neighbors[node]:
                fail(f"{point_field}.port", "duplicate", "port already in use")
            local[key] = {"node": node, "port": port}
        first, second = local["a"]["node"], local["b"]["node"]
        if first == second or second in neighbors[first].values():
            fail(
                field, "tree", "self link or parallel link in transparent tree"
            )
        for peer, point in ((second, local["a"]), (first, local["b"])):
            node, port = point["node"], point["port"]
            lane_use[node] += lanes
            neighbors[node][port] = peer
        links[link_id] = {
            **link,
            **local,
            **{
                name: _compile_direction(
                    link.get(name), f"{field}.{name}", lanes, hardware
                )
                for name in ("forward", "reverse")
            },
        }
    for name, ports in neighbors.items():
        node = nodes[name]
        field = f"topology.nodes[{name}]"
        if node["kind"] == "switch":
            if not 2 <= len(ports) <= node["port_budget"]:
                fail(
                    field,
                    "ports",
                    "switch degree exceeds its port budget "
                    "or has no downstream port",
                )
            if lane_use[name] > node["lane_budget"]:
                fail(field, "lanes", "active lanes exceed switch lane budget")
        elif len(ports) != 1:
            fail(field, "leaf", "root and endpoints require one link")
        if set(ports) != set(range(len(ports))):
            fail(field, "ports", "port indices must be dense from zero")
    if len(links) != len(nodes) - 1:
        fail("topology.links", "tree", "transparent topology must be a tree")
    return links, neighbors


def _paths_from(first, neighbors):
    paths = {first: [first]}
    stack = [first]
    while stack:
        current = stack.pop()
        for peer in neighbors[current].values():
            if peer not in paths:
                paths[peer] = [*paths[current], peer]
                stack.append(peer)
    return paths


def _compile_topology(raw, hardware):
    check_keys(raw, ("kind", "nodes", "links"), "topology")
    if raw.get("kind") != "transparent_tree":
        fail("topology.kind", "unsupported", "expected transparent_tree")
    nodes, root = _compile_nodes(raw.get("nodes"), hardware)
    links, neighbors = _compile_links(raw.get("links"), nodes, hardware)
    if len(_paths_from(root, neighbors)) != len(nodes):
        fail("topology.links", "connected", "unreachable nodes or cycle")
    leaves = {
        key: node
        for key, node in sorted(nodes.items())
        if node["kind"] != "switch"
    }
    paths = {}
    for first in leaves:
        routes = _paths_from(first, neighbors)
        paths[first] = {
            second: routes[second] for second in leaves if second != first
        }
    return {
        "kind": raw["kind"],
        "nodes": [nodes[key] for key in sorted(nodes)],
        "links": [links[key] for key in sorted(links)],
        "paths": paths,
    }, leaves


def _compile_measurement(raw):
    field = "measurement"
    check_keys(raw, ("timeout_ps", "warmup_ps", "window_ps"), field)
    timeout = _number(raw.get("timeout_ps"), f"{field}.timeout_ps", 1)
    warmup = _number(raw.get("warmup_ps"), f"{field}.warmup_ps", 0, timeout)
    _number(raw.get("window_ps"), f"{field}.window_ps", 1, timeout - warmup)
    return raw.copy()


def _compile_workload(raw, leaves, hardware, measurement):
    check_keys(raw, ("flows",), "workload")
    flows = {}
    for index, flow in enumerate(_array(raw.get("flows"), "workload.flows")):
        field = f"workload.flows[{index}]"
        check_keys(
            flow,
            (
                "id",
                "src",
                "dst",
                "operation",
                "address",
                "bytes",
                "release_ps",
                "depends_on",
                "pattern",
            ),
            field,
        )
        flow_id = _identifier(flow.get("id"), f"{field}.id")
        if flow_id in flows:
            fail(f"{field}.id", "duplicate", "duplicate flow ID")
        source = _identifier(flow.get("src"), f"{field}.src")
        target = _identifier(flow.get("dst"), f"{field}.dst")
        if source not in leaves:
            fail(f"{field}.src", "identity", "unknown Requester")
        if target not in leaves or target == source:
            fail(f"{field}.dst", "identity", "expected a different Completer")
        operation = flow.get("operation")
        if operation not in ("read", "write"):
            fail(f"{field}.operation", "unsupported", "expected read or write")
        addr = _aligned(flow.get("address"), f"{field}.address", 4)
        size = _aligned(
            flow.get("bytes"), f"{field}.bytes", 4, 4, hardware["max_transfer"]
        )
        window = leaves[target]
        if (
            addr < window["base"]
            or addr + size > window["base"] + window["size"]
        ):
            fail(
                f"{field}.address",
                "window",
                "flow must fit inside its Completer's window",
            )
        _number(
            flow.get("release_ps"),
            f"{field}.release_ps",
            0,
            measurement["timeout_ps"] - 1,
        )
        dependencies = [
            _identifier(dep, f"{field}.depends_on")
            for dep in _array(flow.get("depends_on"), f"{field}.depends_on")
        ]
        if len(dependencies) != len(set(dependencies)):
            fail(f"{field}.depends_on", "duplicate", "duplicate dependency")
        pattern = (
            flow.get("pattern")
            if operation == "write"
            else flow.get("pattern", 0)
        )
        _number(pattern, f"{field}.pattern", 0, 255)
        flows[flow_id] = {
            **flow,
            "depends_on": sorted(dependencies),
            "pattern": pattern,
        }
    successors = {key: [] for key in flows}
    pending = {}
    for key, flow in flows.items():
        pending[key] = len(flow["depends_on"])
        for dependency in flow["depends_on"]:
            if dependency not in flows:
                fail(
                    f"workload.flows[{key}].depends_on",
                    "identity",
                    f"unknown predecessor {dependency!r}",
                )
            successors[dependency].append(key)
    ready = deque(key for key, count in pending.items() if count == 0)
    visited = 0
    while ready:
        visited += 1
        for successor in successors[ready.popleft()]:
            pending[successor] -= 1
            if pending[successor] == 0:
                ready.append(successor)
    if visited != len(flows):
        fail("workload.flows", "cycle", "dependencies must form a DAG")
    return {"flows": [flows[key] for key in sorted(flows)]}


def compile_cycle(document):
    """Validate inputs and compile stable topology and workload snapshots."""
    allowed = (
        "format",
        "units",
        "hardware",
        "topology",
        "workload",
        "measurement",
        "lab_inputs",
        "simai",
    )
    check_keys(document, allowed, "experiment")
    if document.get("format") != FORMAT:
        fail("format", "version", f"expected {FORMAT}")
    if document.get("units") != UNITS:
        fail("units", "units", "expected time in ps and payload in B")
    if "lab_inputs" in document:
        object_value(document["lab_inputs"], "lab_inputs")
    digest = config_hash(document)
    hardware = _compile_hardware(document.get("hardware"))
    topology, leaves = _compile_topology(document.get("topology"), hardware)
    measurement = _compile_measurement(document.get("measurement"))
    workload = _compile_workload(
        document.get("workload"), leaves, hardware, measurement
    )
    simai = document.get("simai")
    if "simai" in document:
        check_keys(
            simai,
            (
                "rank_endpoints",
                "segment_bytes",
                "slots_per_rank",
                "pending_limit",
                "flow_limit",
                "workload_sha256",
            ),
            "simai",
        )
        ranks = _array(simai.get("rank_endpoints"), "simai.rank_endpoints")
        if (
            len(ranks) < 2
            or any(
                not isinstance(rank, str) or rank not in leaves
                for rank in ranks
            )
            or len(set(ranks)) != len(ranks)
        ):
            fail("simai.rank_endpoints", "identity", "invalid rank placement")
        segment = _aligned(
            simai.get("segment_bytes"),
            "simai.segment_bytes",
            4,
            4,
            hardware["max_transfer"],
        )
        pending = _number(
            simai.get("pending_limit"),
            "simai.pending_limit",
            len(ranks),
            65536,
        )
        slots = _number(
            simai.get("slots_per_rank"), "simai.slots_per_rank", 1, pending
        )
        _number(simai.get("flow_limit"), "simai.flow_limit", pending, 1 << 20)
        if any(leaves[rank]["size"] < segment * slots for rank in ranks):
            fail(
                "simai.slots_per_rank", "window", "slot capacity exceeds size"
            )
        if workload["flows"]:
            fail("workload.flows", "unsupported", "SimAI supplies the flows")
        checksum = simai.get("workload_sha256")
        if (
            not isinstance(checksum, str)
            or len(checksum) != 64
            or set(checksum) - set("0123456789abcdef")
        ):
            fail("simai.workload_sha256", "hash", "expected a SHA-256 digest")
    return {
        "config_hash": digest,
        "input": copy.deepcopy(document),
        "hardware": hardware,
        "topology": topology,
        "workload": workload,
        "measurement": measurement,
    }
