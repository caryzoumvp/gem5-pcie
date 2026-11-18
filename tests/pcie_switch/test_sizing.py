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


import hashlib
import json
import math
import unittest
from pathlib import Path

from util.pcie_lab.sizing import size_hardware

ROOT = Path(__file__).resolve().parents[2]
FIXTURE = Path(__file__).with_name("fixtures") / "reference_sizing.json"


class SizingTest(unittest.TestCase):
    def assert_matches(self, actual, expected, path):
        if isinstance(expected, dict):
            self.assertIsInstance(actual, dict, path)
            self.assertEqual(actual.keys(), expected.keys(), path)
            for key, value in expected.items():
                self.assert_matches(actual[key], value, f"{path}.{key}")
        elif isinstance(expected, list):
            self.assertIsInstance(actual, list, path)
            self.assertEqual(len(actual), len(expected), path)
            for i, (a, e) in enumerate(zip(actual, expected)):
                self.assert_matches(a, e, f"{path}[{i}]")
        elif isinstance(expected, float):
            self.assertTrue(
                math.isclose(actual, expected, rel_tol=1e-12, abs_tol=1e-10),
                f"{path}: {actual!r} != {expected!r}",
            )
        else:
            if type(expected) is bool:
                self.assertIs(type(actual), type(expected), path)
            if type(expected) is int:
                self.assertIn(type(actual), (int, float), path)
            self.assertEqual(actual, expected, path)

    def test_reference_cases(self):
        fixture = json.loads(FIXTURE.read_text())
        source = ROOT / "docs/pcie_switch/PCIe6_Scaleup_Lab_Offline.html"
        self.assertEqual(
            hashlib.sha256(source.read_bytes()).hexdigest(),
            fixture["source_sha256"],
        )
        for name, case in fixture["cases"].items():
            with self.subTest(case=name):
                self.assert_matches(
                    size_hardware(case["inputs"]), case["expected"], name
                )

    def test_write_split_at_page_and_mps(self):
        sizing = size_hardware({"op": "write", "payload": 516, "offset": 4092})
        self.assertEqual(
            [(r["addr"], r["size"]) for r in sizing["reqs"]],
            [(4092, 4), (4096, 256), (4352, 256)],
        )
        self.assertEqual(sizing["qBytes"], 516 + 3 * 16)
        self.assertEqual(sizing["completions"], [])
        self.assertIsNone(sizing["inFlight"])

    def test_read_split_at_page_and_preserve_completion_data(self):
        sizing = size_hardware({"payload": 4096, "offset": 4092})
        self.assertEqual(
            [(r["addr"], r["size"]) for r in sizing["reqs"]],
            [(4092, 4), (4096, 4092)],
        )
        self.assertEqual(
            [sum(c["size"] for c in r["completions"]) for r in sizing["reqs"]],
            [4, 4092],
        )
        self.assertEqual(sizing["qBytes"], 32)

    def test_reference_window_rounding_and_explicit_override(self):
        # 128 GB/s / 64 B = 2 requests/ns. The 185 ns link loop,
        # 20% margin and two VCs give 222 headers and 888 data units
        # for 64 B completions. Credit windows are not rounded to powers
        # of two. Replay needs ceil(128*185/256*1.2) = 111 FLITs.
        auto = size_hardware({"poolKiB": 0})
        link = auto["resources"][0]
        self.assertEqual(link["creditRtt"], 185)
        self.assertEqual(link["forward"]["actualH"], 222)
        self.assertEqual(link["forward"]["actualD"], 0)
        self.assertEqual(link["reverse"]["actualD"], 888)
        self.assertEqual((link["replayNeed"], link["replay"]), (111, 128))
        self.assertEqual(auto["portWidth"], 2112)
        self.assertEqual(auto["fabricWidth"], 1472)
        self.assertEqual(auto["voqDepth"], 2048)
        self.assertEqual(auto["egressDepth"], 2048)
        self.assertEqual(auto["poolCells"], 8 * 2048)

        limited = size_hardware(
            {"headerCredits": 3, "dataCredits": 1, "replayFlits": 3}
        )["resources"][0]
        self.assertEqual(limited["reverse"]["actualH"], 3)
        self.assertEqual(limited["reverse"]["actualD"], 1)
        self.assertEqual(limited["replay"], 3)
