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

"""M0 configuration contracts; no timing backend or sizing implementation."""

import copy
import hashlib
import json
import math
from decimal import Decimal
from pathlib import Path

FIELDS = json.loads(
    Path(__file__).with_name("hardware_fields.json").read_text()
)
INPUT_UNITS = {name: "count" for name in FIELDS}
for names, unit in [
    ("op cplSplit rttMode trackMode simArrival", "enum"),
    (
        "payload offset mrrs mps rcb reqHeader cplHeader cell meta descriptor",
        "B",
    ),
    ("target", "GB/s"),
    ("rate", "GT/s"),
    ("reqMeters cplMeters islMeters", "m"),
    ("nsMeter", "ns/m"),
    (
        "retimerNs switchNs endpointNs fixedRtt creditFixed ackFixed fabricCreditNs burstNs",
        "ns",
    ),
    ("packing margin util sramFactor", "%"),
    ("tagBits trackExtra portWidth fabricWidth", "bit"),
    ("requestLimit cplLimit", "MTLP/s"),
    ("clock", "MHz"),
    ("poolKiB", "KiB"),
    ("simUs", "us"),
    ("burst", "ratio"),
]:
    for name in names.split():
        INPUT_UNITS[name] = unit

AUTO_FIELDS = (
    "portWidth",
    "fabricWidth",
    "headerCredits",
    "dataCredits",
    "replayFlits",
    "egressCells",
    "voqDepth",
    "poolKiB",
)
RESOURCE_FIELDS = (
    "rx_header_slots",
    "rx_data_bytes",
    "descriptor_slots",
    "forwarding_slices",
    "banks_per_slice",
    "sector_bytes",
)


class ConfigError(ValueError):
    """An input contract violation with a stable field and diagnostic code."""

    def __init__(self, field, code, message):
        self.field = field
        self.code = code
        super().__init__(f"{field}: {code}: {message}")


def fail(field, code, message):
    raise ConfigError(field, code, message)


def object_value(value, field):
    if not isinstance(value, dict):
        fail(field, "type", "expected an object")
    return value


def check_keys(value, allowed, field):
    object_value(value, field)
    if any(not isinstance(key, str) for key in value):
        fail(field, "type", "object keys must be strings")
    unknown = value.keys() - set(allowed)
    if unknown:
        fail(field, "unknown_field", ", ".join(sorted(unknown)))


def integer(value, field, minimum=0):
    if type(value) is not int or value < minimum:
        fail(field, "range", f"expected integer >= {minimum}")
    return value


def normalize_inputs(user=None, profile=None):
    """Defaults -> profile -> user; retain all hidden fields and auto zeros."""
    user = {} if user is None else user
    profile = {} if profile is None else profile
    for name, values in [("profile.overrides", profile), ("inputs", user)]:
        check_keys(values, FIELDS, name)
    result = {name: spec["default"] for name, spec in FIELDS.items()}
    result.update(profile)
    result.update(user)
    for name, spec in FIELDS.items():
        value = result[name]
        if isinstance(value, bool):
            fail(name, "type", "boolean is not a field value")
        if "enum" in spec:
            if not any(
                type(value) is type(option) and value == option
                for option in spec["enum"]
            ):
                fail(name, "enum", f'expected one of {spec["enum"]}')
        else:
            if not isinstance(value, (int, float)) or (
                isinstance(value, float) and not math.isfinite(value)
            ):
                fail(name, "type", "expected a finite number")
            number = Decimal(str(value))
            low, high = (Decimal(str(spec[k])) for k in ("minimum", "maximum"))
            if not low <= number <= high:
                fail(name, "range", f"expected {low} <= value <= {high}")
            steps = (number - low) / Decimal(str(spec["step"]))
            if steps != steps.to_integral_value():
                fail(name, "step", f'expected step {spec["step"]} from {low}')
    if result["op"] == "read":
        ceiling = {5: 32, 8: 256, 10: 768, 14: 15360}[result["tagBits"]]
        if result["tagBudget"] > ceiling:
            fail("tagBudget", "tag_budget", "exceeds reference Tag ceiling")
    return copy.deepcopy(result)


def validate_protocol(protocol, inputs, strict):
    allowed = ("rcb_role", "shared_fc_tx", "shared_fc_rx")
    check_keys(protocol, allowed, "protocol")
    if not strict and not protocol:
        return
    if protocol.get("rcb_role") not in ("root_complex", "endpoint"):
        fail("protocol.rcb_role", "required", "declare RCB recipient role")
    for name in ("shared_fc_tx", "shared_fc_rx"):
        if type(protocol.get(name)) is not bool:
            fail(f"protocol.{name}", "type", "declare boolean capability")
    if strict and not protocol["shared_fc_tx"]:
        fail(
            "protocol.shared_fc_tx",
            "capability",
            "FLIT transmitter requires Shared FC support",
        )
    if (
        strict
        and inputs["op"] == "read"
        and protocol["rcb_role"] == "endpoint"
        and inputs["rcb"] != 128
    ):
        fail("rcb", "role", "non-RC recipient requires RCB 128")


def validate_topology(topology, inputs, strict):
    check_keys(topology, ("kind", "nodes", "links"), "topology")
    if not topology:
        if strict:
            fail("topology", "required", "declare a transparent tree")
        return
    if topology.get("kind") != "transparent_tree":
        fail(
            "topology.kind",
            "unsupported",
            "M0 structural contract supports transparent_tree only",
        )
    nodes, links = topology.get("nodes"), topology.get("links")
    if not isinstance(nodes, list) or not isinstance(links, list):
        fail("topology", "type", "nodes and links must be arrays")
    by_id = {}
    for node in nodes:
        check_keys(node, ("id", "kind"), "topology.nodes")
        node_id = node.get("id")
        if not isinstance(node_id, str) or not node_id or node_id in by_id:
            fail(
                "topology.nodes.id",
                "identity",
                "IDs must be unique nonempty strings",
            )
        if node.get("kind") not in ("root", "switch", "endpoint"):
            fail(
                "topology.nodes.kind", "enum", "expected root/switch/endpoint"
            )
        by_id[node_id] = node["kind"]
    roots = [n for n, kind in by_id.items() if kind == "root"]
    switches = [n for n, kind in by_id.items() if kind == "switch"]
    if len(roots) != 1 or not switches or "endpoint" not in by_id.values():
        fail(
            "topology.nodes",
            "count",
            "one root, at least one switch and one endpoint required",
        )
    adjacent = {name: set() for name in by_id}
    for edge in links:
        if (
            not isinstance(edge, list)
            or len(edge) != 2
            or any(not isinstance(n, str) or n not in by_id for n in edge)
        ):
            fail(
                "topology.links", "identity", "edge must name two known nodes"
            )
        first, second = edge
        if first == second or second in adjacent[first]:
            fail("topology.links", "duplicate", "self/duplicate edge")
        adjacent[first].add(second)
        adjacent[second].add(first)
    if len(links) != len(nodes) - 1:
        fail("topology.links", "tree", "transparent topology must be a tree")
    visited, pending = set(), list(roots)
    while pending:
        node = pending.pop()
        if node in visited:
            continue
        visited.add(node)
        pending.extend(adjacent[node] - visited)
    if len(visited) != len(nodes):
        fail("topology.links", "connected", "unreachable node")
    for node, kind in by_id.items():
        degree = len(adjacent[node])
        if kind != "switch" and degree != 1:
            fail(
                "topology.nodes",
                "leaf",
                "root and endpoints must be leaves in this path contract",
            )
        if kind == "switch" and not 2 <= degree <= inputs["chipPorts"]:
            fail(
                "topology.nodes",
                "ports",
                "switch degree exceeds port budget or has no downstream",
            )
    # The HTML describes a uniform root-to-leaf path, not chip count.
    work = [(roots[0], None, 0)]
    while work:
        node, parent, count = work.pop()
        count += by_id[node] == "switch"
        if by_id[node] == "endpoint" and count != inputs["switches"]:
            fail(
                "topology.links",
                "path_count",
                "each root-to-endpoint path must match switches",
            )
        work.extend(
            (child, node, count) for child in adjacent[node] if child != parent
        )
    # Port/lane wiring and addresses require the later physical profile.


def compile_experiment(document, sources=None):
    """Validate/normalize M0 input. Never imply that a backend is implemented."""
    allowed = (
        "schema_version",
        "inputs",
        "profile",
        "execution_mode",
        "link_fidelity",
        "interface_model",
        "seed",
        "protocol",
        "topology",
        "resources",
    )
    check_keys(document, allowed, "experiment")
    version = document.get("schema_version", 1)
    if type(version) is not int or version != 1:
        fail("schema_version", "version", "expected 1")
    seed = integer(document.get("seed", 0), "seed")
    mode = document.get("execution_mode", "lab_reference")
    fidelity = document.get("link_fidelity", "tlp_budget")
    interface = document.get("interface_model", "transaction")
    if mode not in ("lab_reference", "pcie6_timing"):
        fail("execution_mode", "enum", "unknown mode")
    if fidelity not in ("tlp_budget", "flit_detail"):
        fail("link_fidelity", "enum", "unknown fidelity")
    if interface not in ("transaction", "controller_detail"):
        fail("interface_model", "enum", "unknown interface")
    if mode == "lab_reference" and (
        fidelity != "tlp_budget" or interface != "transaction"
    ):
        fail(
            "execution_mode",
            "combination",
            "reference mode fixes budget/transaction semantics",
        )
    profile = document.get("profile", {})
    check_keys(profile, ("name", "overrides"), "profile")
    profile_name = profile.get("name", "html_default")
    if profile_name not in ("html_default", "competitor_target", "custom"):
        fail("profile.name", "enum", "unknown scenario profile")
    overrides = profile.get("overrides", {})
    raw = document.get("inputs", {})
    object_value(raw, "inputs")
    object_value(overrides, "profile.overrides")
    values = normalize_inputs(raw, overrides)
    strict = mode == "pcie6_timing"
    protocol = document.get("protocol", {})
    validate_protocol(protocol, values, strict)
    topology = document.get("topology", {})
    validate_topology(topology, values, strict)
    resources = document.get("resources", {})
    check_keys(resources, RESOURCE_FIELDS, "resources")
    for name, value in resources.items():
        integer(value, f"resources.{name}", 1)
    if strict:
        for name in RESOURCE_FIELDS[:3]:
            if name not in resources:
                fail(
                    f"resources.{name}",
                    "required",
                    "declare finite RX/core backing",
                )
        if values["reqPorts"] + values["cplPorts"] > values["chipPorts"]:
            fail("chipPorts", "ports", "active ports exceed budget")
        lanes = (
            values["reqPorts"] * values["reqLanes"]
            + values["cplPorts"] * values["cplLanes"]
        )
        if values["switches"] == 1 and lanes > values["chipLanes"]:
            fail("chipLanes", "lanes", "active lanes exceed budget")
    history = []
    prior = {name: spec["default"] for name, spec in FIELDS.items()}
    for source, update in [("profile", overrides), ("user", raw)]:
        for name in sorted(update):
            history.append(
                {
                    "source": source,
                    "field": name,
                    "before": prior[name],
                    "after": update[name],
                }
            )
            prior[name] = update[name]
    sources = {} if sources is None else copy.deepcopy(sources)
    object_value(sources, "source_hashes")
    for name, value in sources.items():
        if (
            not isinstance(name, str)
            or not isinstance(value, str)
            or len(value) != 64
            or any(c not in "0123456789abcdef" for c in value)
        ):
            fail(
                "source_hashes",
                "hash",
                "expected source names and lowercase SHA256 hex",
            )
    result = {
        "source_hashes": sources,
        "contract_version": "m0-v1",
        "contract_sha256": hashlib.sha256(
            Path(__file__).read_bytes()
        ).hexdigest(),
        "manifest_sha256": hashlib.sha256(
            Path(__file__).with_name("hardware_fields.json").read_bytes()
        ).hexdigest(),
        "schema_version": 1,
        "kind": "pcie6-experiment",
        "seed": seed,
        "execution_mode": mode,
        "link_fidelity": fidelity,
        "interface_model": interface,
        "scenario_profile": profile_name,
        "raw_inputs": copy.deepcopy(raw),
        "profile_overrides": copy.deepcopy(overrides),
        "effective_inputs": values,
        "override_history": history,
        "unresolved_auto_fields": [
            name for name in AUTO_FIELDS if values[name] == 0
        ],
        "protocol": copy.deepcopy(protocol),
        "topology": copy.deepcopy(topology),
        "resources": copy.deepcopy(resources),
        "input_units": copy.deepcopy(INPUT_UNITS),
        "ir_units": {"time": "ps", "capacity": "B", "rate": "B/s"},
        "backend_implemented": False,
        "unvalidated_contracts": [
            "physical_port_lane_mapping",
            "address_routing",
            "complete_resource_backing",
            "protocol_execution",
        ],
    }
    result["config_hash"] = hashlib.sha256(
        json.dumps(
            result, sort_keys=True, separators=(",", ":"), allow_nan=False
        ).encode()
    ).hexdigest()
    return result


def make_result(experiment, status="not_run", unfinished=0):
    """M0 result envelope: do not fabricate samples or validated protocol."""
    if status not in ("not_run", "failed", "incomplete"):
        fail(
            "status", "unsupported", "M0 cannot claim simulated/passed results"
        )
    integer(unfinished, "unfinished")
    return {
        "schema_version": 1,
        "kind": "pcie6-result",
        "experiment_hash": experiment["config_hash"],
        "experiment": copy.deepcopy(experiment),
        "execution_mode": experiment["execution_mode"],
        "link_fidelity": experiment["link_fidelity"],
        "status": status,
        "sample_count": 0,
        "unfinished": unfinished,
        "payload_bytes": None,
        "latency_ns": None,
        "measurement_ps": None,
        "protocol_validated": False,
    }


def source_hashes(root):
    """Fingerprint required local sources without embedding licensed PDF text."""
    result = {}
    for name in (
        "PCIe6_Scaleup_Lab_Offline.html",
        "PCI-Express-Base-Specification-Revision-6.0.pdf",
    ):
        path = Path(root) / "docs" / "pcie_switch" / name
        digest = hashlib.sha256()
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(65536), b""):
                digest.update(chunk)
        result[name] = digest.hexdigest()
    return result
