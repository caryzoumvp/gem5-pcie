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

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class CliTest(unittest.TestCase):
    def run_cli(self, value, source_root=ROOT, *, options=()):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "input.json"
            path.write_text(json.dumps(value))
            return subprocess.run(
                [
                    sys.executable,
                    "-m",
                    "util.pcie_lab.cli",
                    str(path),
                    "--root",
                    str(source_root),
                    *options,
                ],
                cwd=ROOT,
                capture_output=True,
                text=True,
                timeout=5,
            )

    def test_valid_input_is_not_a_simulation_claim(self):
        result = self.run_cli({})
        self.assertEqual(result.returncode, 0, result.stderr)
        envelope = json.loads(result.stdout)
        self.assertEqual(envelope["status"], "not_run")
        self.assertFalse(envelope["experiment"]["backend_implemented"])
        self.assertEqual(len(envelope["experiment"]["source_hashes"]), 2)
        self.assertRegex(
            envelope["producer"]["git_revision"], r"^[0-9a-f]{40}$"
        )
        self.assertIsInstance(envelope["producer"]["git_dirty"], bool)

    def test_sizing_preserves_input_and_reports_resolved_resources(self):
        result = self.run_cli(
            {"inputs": {"portWidth": 256, "replayFlits": 0}},
            options=("--size",),
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        envelope = json.loads(result.stdout)
        self.assertEqual(envelope["status"], "not_run")
        self.assertEqual(
            envelope["experiment"]["raw_inputs"]["replayFlits"], 0
        )
        sizing = envelope["reference_sizing"]
        self.assertEqual(sizing["portWidth"], 256)
        self.assertEqual(sizing["resources"][0]["replay"], 128)
        self.assertEqual(sizing["ceilings"][0][0], "端口数据面")
        self.assertIn(["发起端请求率", None], sizing["ceilings"])

    def test_sizing_rejects_strict_mode(self):
        example = ROOT / "util/pcie_lab/examples/strict_contract.json"
        result = self.run_cli(
            json.loads(example.read_text()), options=("--size",)
        )
        self.assertEqual(result.returncode, 2)
        self.assertEqual(json.loads(result.stderr)["code"], "reference_only")

    def test_import_and_reexport_browser_calibration(self):
        fixture = (
            ROOT / "tests/pcie_switch/fixtures/reference_calibration.json"
        )
        cases = json.loads(fixture.read_text())["cases"]
        for name, original in cases.items():
            with self.subTest(case=name):
                result = self.run_cli(original, options=("--size",))
                self.assertEqual(result.returncode, 0, result.stderr)
                envelope = json.loads(result.stdout)
                self.assertEqual(envelope["status"], "not_run")
                self.assertEqual(
                    envelope["experiment"]["raw_inputs"], original["inputs"]
                )
                self.assertEqual(envelope["imported_calibration"], original)
                self.assertIsNone(envelope["payload_bytes"])
                result = self.run_cli(
                    original, options=("--export-calibration",)
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(json.loads(result.stdout), original)

    def test_export_new_calibration_matches_page_and_reimports(self):
        fixture = (
            ROOT / "tests/pcie_switch/fixtures/reference_calibration.json"
        )
        for name, diff in (
            ("writeNoRun", {"op": "write", "poolKiB": 0}),
            ("readNoRun", {"payload": 4096, "mrrs": 512, "switches": 2}),
        ):
            with self.subTest(case=name):
                oracle = json.loads(fixture.read_text())
                result = self.run_cli(
                    {"inputs": diff, "seed": 17},
                    options=("--export-calibration",),
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                export = json.loads(result.stdout)
                metadata = export.pop("metadata")
                self.assertEqual(metadata["schema_version"], 1)
                self.assertEqual(
                    metadata["source_hashes"][
                        "PCIe6_Scaleup_Lab_Offline.html"
                    ],
                    oracle["source_sha256"],
                )
                self.assertEqual(export, oracle["cases"][name])
                export["metadata"] = metadata
                result = self.run_cli(export, options=("--size",))
                self.assertEqual(result.returncode, 0, result.stderr)
                envelope = json.loads(result.stdout)
                self.assertEqual(envelope["imported_calibration"], export)
                self.assertEqual(envelope["experiment"]["seed"], 17)

    def test_workspace_and_unknown_calibration_versions_are_distinguished(
        self,
    ):
        for document, code in (
            (
                {"schema": 1, "config": {}, "versions": [], "evidence": []},
                "workspace_not_hardware",
            ),
            ({"format": "pcie6-hardware-calibration-v2"}, "version"),
            ({"format": "pcie6-hardware-calibration-v1"}, "required"),
        ):
            with self.subTest(code=code):
                result = self.run_cli(document)
                self.assertEqual(result.returncode, 2)
                self.assertEqual(json.loads(result.stderr)["code"], code)

    def test_stale_source_snapshot_is_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            source_root = Path(folder)
            source = source_root / "docs" / "pcie_switch"
            source.mkdir(parents=True)
            for name in (
                "PCIe6_Scaleup_Lab_Offline.html",
                "PCI-Express-Base-Specification-Revision-6.0.pdf",
            ):
                (source / name).write_text("stale source")
            result = self.run_cli({}, source_root)
        self.assertEqual(result.returncode, 2)
        self.assertEqual(json.loads(result.stderr)["code"], "source_mismatch")
        self.assertEqual(result.stdout, "")

    def test_invalid_input_has_structured_diagnostic_and_no_output(self):
        result = self.run_cli({"inputs": {"payload": 0}})
        self.assertEqual(result.returncode, 2)
        self.assertEqual(result.stdout, "")
        diagnostic = json.loads(result.stderr)
        self.assertEqual(diagnostic["field"], "payload")
        self.assertEqual(diagnostic["code"], "range")


if __name__ == "__main__":
    unittest.main()
