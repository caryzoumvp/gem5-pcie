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


"""Run the public reference config with the browser benchmark's input diff."""

import json
import runpy
import sys
from pathlib import Path

import m5

REPO = Path(__file__).resolve().parents[4]
FIXTURE = REPO / "tests/pcie_switch/fixtures/reference_events.json"
CONFIG = REPO / "configs/example/pcie/reference.py"


if __name__ == "__m5_main__":
    case_id = sys.argv[1]
    case = json.loads(FIXTURE.read_text(encoding="utf-8"))["cases"][case_id]
    outdir = Path(m5.options.outdir)
    experiment = outdir / "experiment.json"
    experiment.write_text(
        json.dumps({"schema_version": 1, "inputs": case["diff"]}) + "\n",
        encoding="utf-8",
    )
    sys.argv = [str(CONFIG), str(experiment)]
    if case_id == "S-01":
        sys.argv.extend(
            ["--calibration-output", str(outdir / "calibration.json")]
        )
    runpy.run_path(str(CONFIG), run_name="__m5_main__")
