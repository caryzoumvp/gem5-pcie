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
    def run_cli(self, value, source_root=ROOT):
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
