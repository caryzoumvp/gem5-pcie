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


"""Evaluate performance targets from externally collected evidence."""

from dataclasses import dataclass
from decimal import Decimal
from math import (
    isclose,
    isfinite,
)


@dataclass(frozen=True)
class PerformanceCase:
    """One frozen profile and its optional measurement and readiness checks."""

    target_id: str
    profile: dict
    evidence_level: str | None = None
    capabilities_ready: bool | None = None
    correctness_passed: bool | None = None
    measurement: dict | None = None
    schema_version: int = 1


PROFILES = {
    "P-02": "SW-LAT",
    "P-04": "RTT-64",
    "P-05": "SW-BULK",
    "P-06": "SW-SMALL",
    "P-07": "SW-PERM",
    "P-08": "SW-TAIL",
    "P-13": "CT-PIN",
}
CORE_BOUNDARY = "legal_ingress_to_egress_accept"
LATENCY_RULES = {
    "P-02": (CORE_BOUNDARY, ("stage_breakdown_complete",), 100),
    "P-04": (
        "source_submit_to_data_consumable",
        ("phy_evidence", "endpoint_evidence", "budget_closed"),
        250,
    ),
    "P-13": ("pin_first_to_last_bit", ("phy_evidence", "budget_closed"), 90),
}


def _nonnegative(value):
    return (
        type(value) in (int, float)
        and value >= 0
        and (type(value) is int or isfinite(value))
    )


def _positive(value):
    return _nonnegative(value) and value > 0


def _count(value):
    return type(value) is int and value >= 0


def _reference_ready(profile):
    source = profile.get("reference_source")
    return (
        profile.get("reference_independent") is True
        and isinstance(source, str)
        and bool(source.strip())
    )


def _latency(case):
    measurement = case.measurement
    boundary, required_evidence, limit = LATENCY_RULES[case.target_id]
    latency = measurement.get("latency_ns")
    if (
        measurement.get("boundary") != boundary
        or not all(measurement.get(flag) is True for flag in required_evidence)
        or not _nonnegative(latency)
    ):
        return "NOT_COMPARABLE", "missing matched latency evidence", {}
    inclusive = case.target_id == "P-13"
    if inclusive and measurement.get("forwarding") != "cut_through":
        return "NOT_COMPARABLE", "missing cut-through evidence", {}
    meets_limit = latency <= limit if inclusive else latency < limit
    return (
        "PASS" if meets_limit else "FAIL",
        "latency threshold",
        {"latency_ns": latency, "limit_ns": limit},
    )


def _throughput(case):
    profile = case.profile
    measurement = case.measurement
    window = measurement.get("window_ns")
    refs = profile.get("b_ref_GBps")
    flow_ids = profile.get("flow_ids")
    payload = measurement.get("completed_payload_bytes")
    unfinished = measurement.get("unfinished")
    if (
        not _reference_ready(profile)
        or not isinstance(flow_ids, (list, tuple))
        or not flow_ids
        or not all(isinstance(flow, str) and flow for flow in flow_ids)
        or len(set(flow_ids)) != len(flow_ids)
        or not all(
            isinstance(values, dict) and values.keys() == set(flow_ids)
            for values in (refs, payload, unfinished)
        )
        or not all(_positive(amount) for amount in refs.values())
        or not all(_count(amount) for amount in payload.values())
        or not all(_count(amount) for amount in unfinished.values())
        or not _positive(window)
        or measurement.get("window_complete") is not True
        or measurement.get("service_breakdown_complete") is not True
    ):
        return "NOT_COMPARABLE", "missing reference or flow evidence", {}

    flows = {}
    for flow in flow_ids:
        # A byte per ns is a decimal GB/s.
        denominator = Decimal(str(refs[flow])) * Decimal(str(window))
        fraction = Decimal(payload[flow]) / denominator
        flows[flow] = {
            "completed_payload_GBps": payload[flow] / window,
            "b_ref_GBps": refs[flow],
            "unfinished": unfinished[flow],
            "pass": fraction >= Decimal("0.95"),
        }
    passed = all(metrics["pass"] for metrics in flows.values())
    return (
        "PASS" if passed else "FAIL",
        "per-flow throughput threshold",
        {"flows": flows, "window_ns": window},
    )


def _tail(case):
    profile = case.profile
    measurement = case.measurement
    ref_rate = profile.get("r_ref")
    offered = measurement.get("offered_rate")
    if (
        not _reference_ready(profile)
        or not _positive(ref_rate)
        or not _positive(offered)
        # Allow floating-point rounding of the frozen offered rate.
        or not isclose(offered, 0.70 * ref_rate, rel_tol=1e-12)
        or measurement.get("boundary") != CORE_BOUNDARY
        or measurement.get("cohort_matches") is not True
    ):
        return "NOT_COMPARABLE", "missing matched cohort at 70% R_ref", {}
    steady = measurement.get("steady_state")
    if steady is False:
        return "FAIL", "backlog grew during the measurement window", {}
    if steady is not True:
        return "NOT_COMPARABLE", "missing steady-state evidence", {}

    percentile = measurement.get("percentile")
    samples = measurement.get("samples")
    baseline_samples = measurement.get("baseline_samples")
    latency = measurement.get("latency_ns")
    baseline = measurement.get("baseline_ns")
    if (
        percentile not in (99, 99.9)
        or not _count(samples)
        or not _count(baseline_samples)
        or not _nonnegative(latency)
        or not _positive(baseline)
    ):
        return "NOT_COMPARABLE", "missing percentile evidence", {}
    metrics = {
        "percentile": percentile,
        "samples": samples,
        "baseline_samples": baseline_samples,
        "latency_ns": latency,
        "baseline_ns": baseline,
    }
    minimum = 10_000 if percentile == 99 else 100_000
    if min(samples, baseline_samples) < minimum:
        return "INSUFFICIENT_SAMPLES", "below sample minimum", metrics
    if percentile == 99.9:
        return "NOT_COMPARABLE", "P-08 sets a P99 target", metrics
    return (
        "PASS" if latency <= 2 * baseline else "FAIL",
        "twice the unloaded P99",
        metrics,
    )


def evaluate_performance(case: PerformanceCase) -> dict:
    """Report correctness, target verdict and caller-supplied evidence level.

    A profile snapshot must identify the matching named profile. The caller
    attests that its experiment inputs are frozen and that the measurements,
    including the independent reference, use that snapshot.
    """
    target = case.target_id
    if case.correctness_passed is True:
        correctness = "PASS"
    elif case.correctness_passed is False:
        correctness = "FAIL"
    else:
        correctness = "NOT_RUN"

    metrics = {}
    if case.schema_version != 1 or target not in PROFILES:
        status, reason = "NOT_COMPARABLE", "unsupported case version or target"
    elif case.capabilities_ready is False:
        status, reason = (
            "MISSING_CAPABILITY",
            "required capability unavailable",
        )
    elif case.measurement is None:
        status, reason = "NOT_RUN", "no measurement supplied"
    elif (
        case.profile.get("name") != PROFILES[target]
        or case.profile.get("frozen") is not True
        or case.capabilities_ready is not True
        or correctness != "PASS"
        or not case.evidence_level
    ):
        status, reason = "NOT_COMPARABLE", "missing profile or run evidence"
    elif target in LATENCY_RULES:
        status, reason, metrics = _latency(case)
    elif target == "P-08":
        status, reason, metrics = _tail(case)
    else:
        status, reason, metrics = _throughput(case)
    return {
        "schema_version": case.schema_version,
        "target_id": target,
        "target_status": status,
        "correctness_status": correctness,
        "evidence_level": case.evidence_level,
        "reason": reason,
        "metrics": metrics,
    }
