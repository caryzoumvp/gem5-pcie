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
import hashlib
import json
import tempfile
import unittest
from pathlib import Path

from util.pcie_lab import (
    audit,
    contracts,
)

ROOT = Path(__file__).resolve().parents[2]


class AuditTest(unittest.TestCase):
    def test_every_field_matches_independent_html_oracle(self):
        actual = audit.extract_fields(
            ROOT / "docs/pcie_switch/PCIe6_Scaleup_Lab_Offline.html"
        )
        self.assertEqual(actual, contracts.FIELDS)
        self.assertEqual(len(actual), 63)
        # Ensure the oracle comparison catches a realistic stale default.
        stale = copy.deepcopy(contracts.FIELDS)
        stale["slices"]["default"] = 3
        self.assertNotEqual(actual, stale)

    def test_capabilities_have_sources_and_scenario_links(self):
        folder = ROOT / "util" / "pcie_lab"
        data = json.loads((folder / "capabilities.json").read_text())
        self.assertEqual(data["schema_version"], 1)
        self.assertEqual(len(data["capabilities"]), 9)
        scenario_doc = (
            ROOT / "docs/pcie_switch/PCIe6_Validation_Scenarios.md"
        ).read_text()
        for name, capability in data["capabilities"].items():
            with self.subTest(name=name):
                self.assertEqual(
                    capability["implementation_status"], "not_implemented"
                )
                self.assertTrue(capability["section"])
                for key in (
                    "normative_behavior",
                    "reference_behavior",
                    "unsupported",
                ):
                    self.assertTrue(capability[key])
                for scenario in capability["scenarios"]:
                    self.assertIn("| " + scenario + " |", scenario_doc)
        lock = json.loads((folder / "source_lock.json").read_text())
        self.assertEqual(lock["sha256"], contracts.source_hashes(ROOT))

    def test_fingerprints_are_content_hashes(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            folder = root / "docs" / "pcie_switch"
            folder.mkdir(parents=True)
            names = [
                "PCIe6_Scaleup_Lab_Offline.html",
                "PCI-Express-Base-Specification-Revision-6.0.pdf",
            ]
            for name in names:
                (folder / name).write_bytes(b"fixture bytes")
            result = contracts.source_hashes(root)
            self.assertEqual(set(result), set(names))
            self.assertEqual(
                set(result.values()),
                {hashlib.sha256(b"fixture bytes").hexdigest()},
            )
            (folder / names[0]).write_bytes(b"changed")
            self.assertNotEqual(result, contracts.source_hashes(root))


if __name__ == "__main__":
    unittest.main()
