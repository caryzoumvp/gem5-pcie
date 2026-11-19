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


import unittest
from dataclasses import replace

from util.pcie_lab.performance import (
    PerformanceCase,
    evaluate_performance,
)


class PerformanceTest(unittest.TestCase):
    def evaluate(self, target, profile, measurement, **profile_fields):
        case = PerformanceCase(
            target_id=target,
            profile={"name": profile, "frozen": True, **profile_fields},
            evidence_level="MODEL",
            capabilities_ready=True,
            correctness_passed=True,
            measurement=measurement,
        )
        return evaluate_performance(case)

    def test_core_limit_and_stage_boundary(self):
        measurement = {
            "boundary": "legal_ingress_to_egress_accept",
            "stage_breakdown_complete": True,
            "latency_ns": 99,
        }

        def evaluate():
            return self.evaluate("P-02", "SW-LAT", measurement)

        self.assertEqual(evaluate()["target_status"], "PASS")
        measurement["latency_ns"] = 100
        self.assertEqual(evaluate()["target_status"], "FAIL")
        measurement["stage_breakdown_complete"] = False
        self.assertEqual(evaluate()["target_status"], "NOT_COMPARABLE")

    def test_rtt_and_cut_through_require_distinct_boundaries(self):
        measurement = {
            "boundary": "source_submit_to_data_consumable",
            "latency_ns": 249,
            "phy_evidence": True,
            "endpoint_evidence": True,
            "budget_closed": True,
        }
        result = self.evaluate("P-04", "RTT-64", measurement)
        self.assertEqual(result["target_status"], "PASS")
        measurement["latency_ns"] = 250
        self.assertEqual(
            self.evaluate("P-04", "RTT-64", measurement)["target_status"],
            "FAIL",
        )
        measurement["endpoint_evidence"] = False
        self.assertEqual(
            self.evaluate("P-04", "RTT-64", measurement)["target_status"],
            "NOT_COMPARABLE",
        )
        measurement.update(
            boundary="pin_first_to_last_bit",
            latency_ns=90,
            forwarding="store_and_forward",
        )
        self.assertEqual(
            self.evaluate("P-13", "CT-PIN", measurement)["target_status"],
            "NOT_COMPARABLE",
        )
        measurement["forwarding"] = "cut_through"
        self.assertEqual(
            self.evaluate("P-13", "CT-PIN", measurement)["target_status"],
            "PASS",
        )

    def test_throughput_requires_each_flow_and_independent_reference(self):
        measurement = {
            "window_ns": 10,
            "window_complete": True,
            "service_breakdown_complete": True,
            "completed_payload_bytes": {"a": 950, "b": 1000},
            "unfinished": {"a": 1, "b": 0},
        }
        refs = {"a": 100, "b": 100}
        for target, profile in (
            ("P-05", "SW-BULK"),
            ("P-06", "SW-SMALL"),
            ("P-07", "SW-PERM"),
        ):
            with self.subTest(target=target):
                measurement["completed_payload_bytes"]["a"] = 950
                fields = {
                    "b_ref_GBps": refs,
                    "flow_ids": ["a", "b"],
                    "reference_source": "test model",
                    "reference_independent": True,
                }

                def evaluate():
                    return self.evaluate(
                        target, profile, measurement, **fields
                    )

                self.assertEqual(evaluate()["target_status"], "PASS")
                measurement["completed_payload_bytes"]["a"] = 949
                result = evaluate()
                self.assertEqual(result["target_status"], "FAIL")
                flow = result["metrics"]["flows"]["a"]
                self.assertEqual(flow["unfinished"], 1)
                self.assertFalse(flow["pass"])
                fields["reference_independent"] = False
                self.assertEqual(evaluate()["target_status"], "NOT_COMPARABLE")
                fields["reference_independent"] = True
                fields["reference_source"] = ""
                self.assertEqual(evaluate()["target_status"], "NOT_COMPARABLE")
        measurement["completed_payload_bytes"].pop("b")
        self.assertEqual(
            self.evaluate(
                "P-07",
                "SW-PERM",
                measurement,
                b_ref_GBps=refs,
                flow_ids=["a", "b"],
                reference_source="test model",
                reference_independent=True,
            )["target_status"],
            "NOT_COMPARABLE",
        )

    def test_tail_needs_sample_cohort_rate_and_steady_state(self):
        measurement = {
            "boundary": "legal_ingress_to_egress_accept",
            "cohort_matches": True,
            "steady_state": True,
            "offered_rate": 70,
            "percentile": 99,
            "samples": 10000,
            "baseline_samples": 10000,
            "latency_ns": 120,
            "baseline_ns": 60,
        }

        def evaluate():
            return self.evaluate(
                "P-08",
                "SW-TAIL",
                measurement,
                r_ref=100,
                reference_source="test model",
                reference_independent=True,
            )

        self.assertEqual(evaluate()["target_status"], "PASS")
        measurement["latency_ns"] = 121
        self.assertEqual(evaluate()["target_status"], "FAIL")
        measurement["steady_state"] = False
        self.assertEqual(evaluate()["target_status"], "FAIL")
        self.assertNotIn("latency_ns", evaluate()["metrics"])
        measurement.update(steady_state=True, offered_rate=69)
        self.assertEqual(evaluate()["target_status"], "NOT_COMPARABLE")
        measurement.update(offered_rate=70, samples=9999)
        self.assertEqual(evaluate()["target_status"], "INSUFFICIENT_SAMPLES")
        measurement.update(percentile=99.9, samples=100000)
        self.assertEqual(evaluate()["target_status"], "INSUFFICIENT_SAMPLES")
        measurement["baseline_samples"] = 100000
        self.assertEqual(evaluate()["target_status"], "NOT_COMPARABLE")

    def test_statuses_and_evidence_are_separate(self):
        case = PerformanceCase("P-02", {"name": "SW-LAT", "frozen": True})
        result = evaluate_performance(case)
        self.assertEqual(result["target_status"], "NOT_RUN")
        self.assertEqual(result["correctness_status"], "NOT_RUN")
        blocked = replace(case, capabilities_ready=False)
        self.assertEqual(
            evaluate_performance(blocked)["target_status"],
            "MISSING_CAPABILITY",
        )
        failed = replace(
            case,
            capabilities_ready=True,
            correctness_passed=False,
            evidence_level="MODEL",
            measurement={},
        )
        result = evaluate_performance(failed)
        self.assertEqual(result["correctness_status"], "FAIL")
        self.assertEqual(result["target_status"], "NOT_COMPARABLE")
        self.assertEqual(result["evidence_level"], "MODEL")
