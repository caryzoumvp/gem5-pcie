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

import copy
import json
import math
import unittest
from pathlib import Path

from util.pcie_lab import contracts as dut


class HardwareFieldsTest(unittest.TestCase):
    def test_independent_defaults(self):
        values = dut.normalize_inputs()
        self.assertEqual(len(values), 63)
        self.assertEqual(values["payload"], 64)
        self.assertEqual(values["rcb"], 64)
        self.assertEqual(values["slices"], 9)
        self.assertEqual(values["poolKiB"], 128)
        self.assertEqual(values["simUs"], 12)

    def test_every_boundary_enum_and_step(self):
        for name, spec in dut.FIELDS.items():
            with self.subTest(name=name):
                if "enum" in spec:
                    for value in spec["enum"]:
                        # Cross-field Tag rules are checked separately.
                        extra = {"tagBudget": 1} if name == "tagBits" else {}
                        self.assertEqual(
                            dut.normalize_inputs({**extra, name: value})[name],
                            value,
                        )
                    self.assert_invalid({name: "__invalid__"}, name)
                else:
                    for value in [spec["minimum"], spec["maximum"]]:
                        extra = {"op": "write"} if name == "tagBudget" else {}
                        self.assertEqual(
                            dut.normalize_inputs({**extra, name: value})[name],
                            value,
                        )
                    self.assert_invalid(
                        {name: spec["minimum"] - spec["step"]}, name
                    )
                    self.assert_invalid(
                        {name: spec["maximum"] + spec["step"]}, name
                    )
                    self.assert_invalid(
                        {name: spec["minimum"] + spec["step"] / 2}, name
                    )
                self.assert_invalid({name: True}, name)
                if "enum" not in spec:
                    for invalid in ["1", None, math.nan, math.inf, 10**400]:
                        self.assert_invalid({name: invalid}, name)

    def assert_invalid(self, values, field):
        with self.assertRaises(dut.ConfigError) as caught:
            dut.normalize_inputs(values)
        self.assertIn(field, str(caught.exception))

    def test_hidden_values_survive_and_input_not_mutated(self):
        raw = {"op": "write", "fixedRtt": 999, "mrrs": 512, "islLanes": 4}
        before = copy.deepcopy(raw)
        result = dut.normalize_inputs(raw)
        self.assertEqual(raw, before)
        for name, value in raw.items():
            self.assertEqual(result[name], value)

    def test_override_precedence_and_unknown_keys(self):
        result = dut.normalize_inputs({"mps": 512}, {"mps": 1024, "slices": 3})
        self.assertEqual(result["mps"], 512)
        self.assertEqual(result["slices"], 3)
        self.assert_invalid({"unknown": 1}, "unknown")
        self.assert_invalid({"tagBits": 5, "tagBudget": 33}, "tagBudget")


class ExperimentTest(unittest.TestCase):
    def test_default_contract_and_zero_is_unresolved_auto(self):
        result = dut.compile_experiment({})
        self.assertEqual(result["execution_mode"], "lab_reference")
        self.assertEqual(result["effective_inputs"]["portWidth"], 0)
        self.assertIn("portWidth", result["unresolved_auto_fields"])
        self.assertNotIn("requestLimit", result["unresolved_auto_fields"])
        self.assertFalse(result["backend_implemented"])
        self.assertEqual(set(result["input_units"]), set(dut.FIELDS))
        self.assertEqual(result["input_units"]["fixedRtt"], "ns")
        self.assertEqual(result["input_units"]["poolKiB"], "KiB")
        self.assertEqual(result["input_units"]["target"], "GB/s")
        self.assertEqual(result, dut.compile_experiment({}))
        self.assertEqual(json.loads(json.dumps(result)), result)

    def test_raw_effective_and_hash(self):
        raw = {
            "inputs": {"mps": 512},
            "profile": {
                "name": "competitor_target",
                "overrides": {"mps": 1024},
            },
        }
        frozen = copy.deepcopy(raw)
        result = dut.compile_experiment(raw)
        self.assertEqual(raw, frozen)
        self.assertEqual(result["raw_inputs"], {"mps": 512})
        self.assertEqual(result["effective_inputs"]["mps"], 512)
        self.assertEqual(result["override_history"][0]["source"], "profile")
        self.assertEqual(result["override_history"][-1]["source"], "user")
        self.assertNotEqual(
            result["config_hash"], dut.compile_experiment({})["config_hash"]
        )

    def test_unknown_schema_and_incompatible_modes(self):
        for raw in [
            {"schema_version": 2},
            {"seed": True},
            {"seed": -1},
            {"execution_mode": "mystery"},
            {"accidental": 1},
            {"link_fidelity": "flit_detail"},
            {"interface_model": "controller_detail"},
            {"profile": {"name": "x", "overrides": {"typo": 1}}},
            {"inputs": {1: 4}},
            {"inputs": {"reqLanes": 1.0}},
            {"inputs": []},
            {"inputs": None},
            {"profile": {"overrides": None}},
            {"resources": {"hiddenCapacity": 9}},
        ]:
            with self.subTest(raw=raw), self.assertRaises(dut.ConfigError):
                dut.compile_experiment(raw)

    def test_strict_requires_explicit_role_and_resources(self):
        with self.assertRaisesRegex(dut.ConfigError, "protocol"):
            dut.compile_experiment({"execution_mode": "pcie6_timing"})
        protocol = {
            "rcb_role": "endpoint",
            "shared_fc_tx": True,
            "shared_fc_rx": False,
        }
        with self.assertRaisesRegex(dut.ConfigError, "rcb"):
            dut.compile_experiment(
                {"execution_mode": "pcie6_timing", "protocol": protocol}
            )
        protocol["rcb_role"] = "root_complex"
        with self.assertRaisesRegex(dut.ConfigError, "topology"):
            dut.compile_experiment(
                {"execution_mode": "pcie6_timing", "protocol": protocol}
            )

    def test_strict_topology_and_finite_resource_contract(self):
        raw = strict_example()
        result = dut.compile_experiment(raw)
        self.assertEqual(result["topology"]["kind"], "transparent_tree")
        self.assertEqual(result["effective_inputs"]["slices"], 9)
        self.assertNotIn("forwarding_slices", result["resources"])
        for mutate in [
            lambda x: x["topology"]["links"].append(["ep", "root"]),
            lambda x: x["topology"]["links"].append(["sw", "missing"]),
            lambda x: x["topology"]["nodes"].append(
                {"id": "ep", "kind": "endpoint"}
            ),
            lambda x: x["resources"].update(rx_header_slots=0),
            lambda x: x["resources"].update(descriptor_slots=True),
            lambda x: x["protocol"].update(shared_fc_tx=False),
            lambda x: x["inputs"].update(chipPorts=2, reqPorts=2),
        ]:
            changed = copy.deepcopy(raw)
            mutate(changed)
            with self.subTest(raw=changed), self.assertRaises(dut.ConfigError):
                dut.compile_experiment(changed)

    def test_topology_requires_explicit_destination(self):
        raw = strict_example()
        raw["topology"]["nodes"].pop()
        raw["topology"]["links"].pop()
        with self.assertRaises(dut.ConfigError) as caught:
            dut.compile_experiment(raw)
        self.assertEqual(caught.exception.code, "count")
        self.assertIn("endpoint", str(caught.exception))

    def test_uniform_fanout_uses_path_count_not_total_switch_count(self):
        raw = strict_example()
        raw["inputs"]["switches"] = 2
        raw["topology"]["nodes"] += [
            {"id": "branch_a", "kind": "switch"},
            {"id": "branch_b", "kind": "switch"},
            {"id": "ep_b", "kind": "endpoint"},
        ]
        raw["topology"]["links"] = [
            ["root", "sw"],
            ["sw", "branch_a"],
            ["sw", "branch_b"],
            ["branch_a", "ep"],
            ["branch_b", "ep_b"],
        ]
        self.assertEqual(
            len(dut.compile_experiment(raw)["topology"]["nodes"]), 6
        )
        raw["topology"]["nodes"].append({"id": "ep_c", "kind": "endpoint"})
        raw["topology"]["links"].append(["sw", "ep_c"])
        with self.assertRaisesRegex(dut.ConfigError, "path_count"):
            dut.compile_experiment(raw)

    def test_transparent_tree_is_not_arbitrary_connected_graph(self):
        raw = strict_example()
        raw["topology"]["links"] = [["root", "sw"], ["root", "ep"]]
        # No endpoint attached to the declared switch and wrong switch degree.
        with self.assertRaises(dut.ConfigError):
            dut.compile_experiment(raw)


class ResultTest(unittest.TestCase):
    def test_empty_unfinished_and_failed_results(self):
        exp = dut.compile_experiment({})
        for status in ["not_run", "failed", "incomplete"]:
            result = dut.make_result(exp, status=status, unfinished=3)
            self.assertEqual(result["experiment_hash"], exp["config_hash"])
            self.assertIsNone(result["latency_ns"])
            self.assertEqual(result["unfinished"], 3)
            self.assertEqual(result, json.loads(json.dumps(result)))
            self.assertFalse(result["protocol_validated"])
        with self.assertRaises(dut.ConfigError):
            dut.make_result(exp, status="passed")
        with self.assertRaises(dut.ConfigError):
            dut.make_result(exp, unfinished=-1)

    def test_provenance_changes_identity_and_survives_result(self):
        first = dut.compile_experiment({}, sources={"html": "a" * 64})
        second = dut.compile_experiment({}, sources={"html": "b" * 64})
        self.assertNotEqual(first["config_hash"], second["config_hash"])
        result = dut.make_result(first)
        self.assertEqual(
            result["experiment"]["source_hashes"], {"html": "a" * 64}
        )
        with self.assertRaises(dut.ConfigError):
            dut.compile_experiment({}, sources={"html": "not-a-sha256"})

    def test_metadata_has_no_fabricated_measurement(self):
        result = dut.make_result(dut.compile_experiment({}))
        self.assertEqual(result["sample_count"], 0)
        self.assertIsNone(result["payload_bytes"])
        self.assertIsNone(result["measurement_ps"])


def strict_example():
    return {
        "execution_mode": "pcie6_timing",
        "inputs": {"rcb": 128},
        "protocol": {
            "rcb_role": "endpoint",
            "shared_fc_tx": True,
            "shared_fc_rx": False,
        },
        "topology": {
            "kind": "transparent_tree",
            "nodes": [
                {"id": "root", "kind": "root"},
                {"id": "sw", "kind": "switch"},
                {"id": "ep", "kind": "endpoint"},
            ],
            "links": [["root", "sw"], ["sw", "ep"]],
        },
        "resources": {
            "rx_header_slots": 8,
            "rx_data_bytes": 4096,
            "descriptor_slots": 8,
        },
    }


if __name__ == "__main__":
    unittest.main()
