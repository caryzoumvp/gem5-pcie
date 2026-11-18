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


"""M1 reference regressions using the results from the locked offline lab."""

import hashlib
import json
import math
from pathlib import Path

from testlib import (
    config,
    constants,
    gem5_verify_config,
    verifier,
)

REPO = Path(config.base_dir)
FIXTURES = REPO / "tests/pcie_switch/fixtures"
HTML = "PCIe6_Scaleup_Lab_Offline.html"
CASES = tuple(f"S-{index:02}" for index in range(1, 8))


def load_json(path):
    return json.loads(path.read_text(encoding="utf-8"))


def check_golden(actual, expected, label):
    # Logical deadlines retain browser precision in reference mode. The
    # tolerance covers floating-point arithmetic, while counters, queue
    # decisions, and integral values must agree exactly.
    if isinstance(expected, dict):
        assert isinstance(actual, dict), f"{label}: expected an object"
        assert actual.keys() == expected.keys(), (
            f"{label}: missing keys {expected.keys() - actual.keys()}, "
            f"extra keys {actual.keys() - expected.keys()}"
        )
        for key in expected:
            check_golden(actual[key], expected[key], f"{label}.{key}")
        return
    if isinstance(expected, list):
        assert isinstance(actual, list), f"{label}: expected an array"
        assert len(actual) == len(
            expected
        ), f"{label}: expected {len(expected)} elements, got {len(actual)}"
        for index, (value, golden) in enumerate(zip(actual, expected)):
            check_golden(value, golden, f"{label}[{index}]")
        return

    message = f"{label}: expected {expected!r}, got {actual!r}"
    if isinstance(expected, float):
        assert type(actual) in (float, int) and math.isclose(
            actual, expected, rel_tol=1e-12, abs_tol=1e-10
        ), message
    elif type(expected) is int:
        assert type(actual) in (float, int) and actual == expected, message
    else:
        assert type(actual) is type(expected) and actual == expected, message


def check_bench(case_id, sizing, sim):
    """Apply the acceptance predicates from the HTML's runBench()."""
    fraction = sim["bw"] / sizing["protocol"]
    limiting_field = {
        "S-04": "tagStall",
        "S-05": "creditStall",
    }
    if case_id in ("S-01", "S-02", "S-03"):
        inflight = {"S-01": 500, "S-02": 8, "S-03": 63}
        assert sizing["inFlight"] == inflight[case_id], case_id
        assert fraction > 0.95, f"{case_id}: protocol fraction {fraction}"
        if case_id in ("S-02", "S-03"):
            assert len(sizing["completions"]) == 16, case_id
    elif case_id in limiting_field:
        field = limiting_field[case_id]
        assert sim["stats"][field] > 0, f"{case_id}: no {field}"
        assert fraction < 0.2, f"{case_id}: protocol fraction {fraction}"
    elif case_id == "S-06":
        assert sizing["ceilings"][0][0] == "端口数据面", case_id
        assert fraction < 0.2, f"{case_id}: protocol fraction {fraction}"
    elif case_id == "S-07":
        assert not sim["queuePass"], case_id
        assert sim["stats"]["egress"] > sizing["egressDepth"], case_id


class MatchReferenceBench(verifier.Verifier):
    def __init__(self, case_id):
        super().__init__()
        self.case_id = case_id

    def test(self, params):
        outdir = Path(params.fixtures[constants.tempdir_fixture_name].path)
        result = load_json(outdir / "reference.json")
        golden = load_json(FIXTURES / "reference_events.json")
        golden_sizing = load_json(FIXTURES / "reference_sizing.json")
        expected_sha = golden["source_sha256"]
        html = REPO / "docs/pcie_switch" / HTML
        actual_sha = hashlib.sha256(html.read_bytes()).hexdigest()
        assert actual_sha == expected_sha, "Offline HTML source hash changed"
        assert golden_sizing["source_sha256"] == expected_sha

        case = golden["cases"][self.case_id]
        experiment = result["experiment"]
        assert experiment["source_hashes"][HTML] == expected_sha
        assert result["execution_mode"] == "lab_reference"
        assert result["status"] == "simulated"
        assert result["protocol_validated"] is False
        check_golden(
            experiment["raw_inputs"], case["diff"], self.case_id + ".diff"
        )
        check_golden(
            experiment["effective_inputs"],
            case["inputs"],
            self.case_id + ".inputs",
        )
        sim = result["reference_sim"]
        check_golden(sim, case["sim"], self.case_id + ".sim")
        assert result["sample_count"] == sim["sampleCount"]
        assert result["payload_bytes"] == sim["stats"]["bytes"]
        assert result["dispatch_end_tick"] == math.ceil(
            case["sim"]["actualDuration"] * 1000
        )
        sizing = result["reference_sizing"]
        sizing_case = next(
            entry
            for entry in golden_sizing["cases"].values()
            if entry["inputs"] == case["diff"]
        )
        check_golden(sizing, sizing_case["expected"], self.case_id + ".sizing")
        check_bench(self.case_id, sizing, sim)
        if self.case_id == "S-01":
            calibration = load_json(outdir / "calibration.json")
            assert calibration["inputs"] == experiment["effective_inputs"]
            assert calibration["sim"] == sim
            metadata = calibration["metadata"]
            assert metadata["evidence_level"] == "lab_reference_simulation"
            assert metadata["experiment_hash"] == result["experiment_hash"]
            assert metadata["source_hashes"] == experiment["source_hashes"]


for case_id in CASES:
    gem5_verify_config(
        name=f"pcie-lab-reference-{case_id}",
        config=str(Path(__file__).parent / "configs/run_reference_case.py"),
        config_args=[case_id],
        verifiers=[MatchReferenceBench(case_id)],
        valid_isas=(constants.null_tag,),
        length=constants.quick_tag,
    )
