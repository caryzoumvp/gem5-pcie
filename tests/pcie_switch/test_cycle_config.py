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
import unittest

from util.pcie_lab import cycle_config
from util.pcie_lab.contracts import ConfigError


def tree(branch=False):
    direction = {
        "width": 16,
        "cycle_ps": 1000,
        "propagation_ps": 3000,
        "credit_delay_ps": 5000,
    }

    def link(name, a, ap, b, bp):
        return {
            "id": name,
            "a": {"node": a, "port": ap},
            "b": {"node": b, "port": bp},
            "lanes": 4,
            "forward": direction.copy(),
            "reverse": {**direction, "width": 8},
        }

    result = {
        "format": "pcie6-cycle-experiment-v1",
        "units": {"time": "ps", "payload": "B"},
        "hardware": {
            "mps": 256,
            "mrrs": 512,
            "rcb": 128,
            "core_cycle_ps": 1000,
        },
        "topology": {
            "kind": "transparent_tree",
            "nodes": [
                {
                    "id": "root",
                    "kind": "root",
                    "requester_id": 1,
                    "base": 0x1000,
                    "size": 0x1000,
                },
                {
                    "id": "s1",
                    "kind": "switch",
                    "port_budget": 4,
                    "lane_budget": 16,
                },
                {
                    "id": "e1",
                    "kind": "endpoint",
                    "requester_id": 2,
                    "base": 0x4000,
                    "size": 0x1000,
                },
            ],
            "links": [
                link("u", "root", 0, "s1", 0),
                link("d1", "s1", 1, "e1", 0),
            ],
        },
        "workload": {
            "flows": [
                {
                    "id": "put",
                    "src": "root",
                    "dst": "e1",
                    "operation": "write",
                    "address": 0x4000,
                    "bytes": 64,
                    "release_ps": 0,
                    "depends_on": [],
                    "pattern": 23,
                },
                {
                    "id": "get",
                    "src": "root",
                    "dst": "e1",
                    "operation": "read",
                    "address": 0x4000,
                    "bytes": 64,
                    "release_ps": 0,
                    "depends_on": ["put"],
                    "pattern": 23,
                },
            ],
        },
        "measurement": {
            "timeout_ps": 100000,
            "warmup_ps": 0,
            "window_ps": 100000,
        },
    }
    if branch:
        result["topology"]["nodes"].extend(
            [
                {
                    "id": "s2",
                    "kind": "switch",
                    "port_budget": 2,
                    "lane_budget": 8,
                    "resources": {"core_descriptors": 17},
                },
                {
                    "id": "e2",
                    "kind": "endpoint",
                    "requester_id": 3,
                    "base": 0x8000,
                    "size": 0x1000,
                },
            ]
        )
        result["topology"]["links"].extend(
            [
                link("isl", "s1", 2, "s2", 0),
                link("d2", "s2", 1, "e2", 0),
            ]
        )
    return result


class CycleCompilerTest(unittest.TestCase):
    def test_paths_and_defaults(self):
        experiment = tree(True)
        snapshot = copy.deepcopy(experiment)
        compiled = cycle_config.compile_cycle(experiment)
        self.assertEqual(experiment, snapshot)
        self.assertEqual(compiled["input"], snapshot)
        self.assertEqual(
            compiled["topology"]["paths"]["e2"]["root"],
            ["e2", "s2", "s1", "root"],
        )
        self.assertEqual(
            compiled["topology"]["paths"]["e1"]["e2"],
            ["e1", "s1", "s2", "e2"],
        )
        nodes = {node["id"]: node for node in compiled["topology"]["nodes"]}
        self.assertEqual(nodes["s2"]["resources"]["core_descriptors"], 17)
        self.assertEqual(nodes["s1"]["resources"]["core_descriptors"], 8)
        link = compiled["topology"]["links"][0]
        self.assertEqual(link["forward"]["width"], 16)
        self.assertEqual(link["reverse"]["width"], 8)

    def test_hash_is_key_order_independent(self):
        experiment = tree()
        reordered = dict(reversed(list(experiment.items())))
        result = cycle_config.compile_cycle(experiment)
        self.assertEqual(
            result["config_hash"],
            cycle_config.compile_cycle(reordered)["config_hash"],
        )
        reordered["hardware"] = {**experiment["hardware"], "tags": 4}
        self.assertNotEqual(
            result["config_hash"],
            cycle_config.compile_cycle(reordered)["config_hash"],
        )

    def test_browser_digest_vector(self):
        data = {
            "\U00010437": True,
            "\ue001": [4, 5.5, None, -0.0],
            "\u0645": "x\u2028y",
        }
        self.assertEqual(
            cycle_config.config_hash(data),
            "53312c84ce5dda1d5d97c9181c9bbd1987896aa6bef97ec876bde6f096d2a315",
        )
        for invalid in ("\ud800", 2**53, float("nan")):
            with self.subTest(invalid=invalid), self.assertRaises(ConfigError):
                cycle_config.config_hash({"lab_inputs": {"invalid": invalid}})

    def test_chain_routes_after_input_reordering(self):
        experiment = tree(True)
        nodes = experiment["topology"]["nodes"]
        links = experiment["topology"]["links"]
        nodes[:] = [node for node in nodes if node["id"] != "e1"]
        links[:] = [link for link in links if link["id"] != "d1"]
        for link in links:
            if link["id"] == "isl":
                link["a"]["port"] = 1
        for flow in experiment["workload"]["flows"]:
            flow.update(dst="e2", address=0x8000)
        first = cycle_config.compile_cycle(experiment)
        self.assertEqual(
            first["topology"]["paths"]["root"]["e2"],
            ["root", "s1", "s2", "e2"],
        )
        nodes.reverse()
        links.reverse()
        experiment["workload"]["flows"].reverse()
        second = cycle_config.compile_cycle(experiment)
        self.assertEqual(first["topology"], second["topology"])
        self.assertEqual(first["workload"], second["workload"])
        self.assertNotEqual(first["config_hash"], second["config_hash"])

    def test_validation_before_instantiation(self):
        cases = [
            (("topology", "links", 1, "a", "port"), 0, "duplicate"),
            (("topology", "links", 1, "a", "port"), 2, "ports"),
            (("topology", "nodes", 1, "lane_budget"), 7, "lanes"),
            (("topology", "nodes", 2, "base"), 0x1000, "overlap"),
            (("topology", "nodes", 2, "requester_id"), 1, "duplicate"),
            (("topology", "links", 0, "forward", "width"), 64, "bandwidth"),
            (("workload", "flows", 0, "bytes"), 4097, "alignment"),
            (("workload", "flows", 0, "bytes"), 4100, "window"),
            (("workload", "flows", 0, "depends_on"), ["get"], "cycle"),
            (("workload", "flows", 1, "depends_on"), ["other"], "identity"),
            (("workload", "flows", 0, "pattern"), True, "range"),
            (("hardware", "replay_flits"), 512, "range"),
            (("topology", "links", 0, "lanes"), 12, "lanes"),
        ]
        for path, value, code in cases:
            with self.subTest(path=path, value=value):
                experiment = tree()
                parent = experiment
                for key in path[:-1]:
                    parent = parent[key]
                parent[path[-1]] = value
                with self.assertRaises(ConfigError) as error:
                    cycle_config.compile_cycle(experiment)
                self.assertEqual(error.exception.code, code)

    def test_endpoint_can_write_to_root(self):
        experiment = tree()
        experiment["workload"]["flows"][0].update(
            src="e1", dst="root", address=0x1000
        )
        compiled = cycle_config.compile_cycle(experiment)
        self.assertEqual(
            compiled["topology"]["paths"]["e1"]["root"],
            ["e1", "s1", "root"],
        )
