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


"""Exchange hardware calibration JSON with the offline lab."""

from .contracts import (
    FIELDS,
    fail,
    object_value,
)

FORMAT = "pcie6-hardware-calibration-v1"
NOTES = (
    "Calibrate a workload-level network model with payload capacity "
    "and same-measurement-point latency."
)
REQUIRED = (
    "inputs",
    "readRttNs",
    "memRequests",
    "protocolPayloadGBpsPerRequester",
    "estimatedPayloadGBpsPerRequester",
    "switchSpec",
    "sim",
    "notes",
)


def split_document(document):
    """Return runnable input and any separately retained browser evidence."""
    object_value(document, "document")
    if "format" not in document:
        if "schema" in document and "config" in document:
            if type(document["schema"]) is not int or document["schema"] != 1:
                fail("schema", "version", "expected workspace schema 1")
            fail(
                "schema",
                "workspace_not_hardware",
                "workspace is distinct from hardware calibration",
            )
        return document, None
    if document["format"] != FORMAT:
        fail("format", "version", f"expected {FORMAT}")
    for field in REQUIRED:
        if field not in document:
            fail(field, "required", "missing calibration field")
    inputs = object_value(document["inputs"], "inputs")
    missing = sorted(FIELDS.keys() - inputs.keys())
    if missing:
        fail(
            "inputs",
            "required",
            f"missing hardware fields: {', '.join(missing)}",
        )
    experiment = {"inputs": inputs}
    if "metadata" in document:
        metadata = object_value(document["metadata"], "metadata")
        version = metadata.get("schema_version")
        if type(version) is not int or version != 1:
            fail("metadata.schema_version", "version", "expected 1")
        if "seed" in metadata:
            experiment["seed"] = metadata["seed"]
    # Preserve estimates, measurements and extension keys as evidence.
    return experiment, document


def link_spec(link):
    forward, reverse = link["forward"], link["reverse"]
    return {
        "name": link["name"],
        "rawGBps": link["line"],
        "creditRttNs": link["creditRtt"],
        "ackRttNs": link["ackRtt"],
        "replayFlits": link["replay"],
        "forward": {"H": forward["actualH"], "D": forward["actualD"]},
        "reverse": (
            {"H": reverse["actualH"], "D": reverse["actualD"]}
            if reverse is not None
            else None
        ),
    }


def export_calibration(experiment, sizing, producer, *, sim=None):
    """Project the analytical result into the legacy field names and units."""
    c = experiment["effective_inputs"]
    return {
        "format": FORMAT,
        "inputs": c,
        "readRttNs": sizing["rtt"] if sizing["read"] else None,
        "memRequests": [
            {
                "payloadBytes": req["size"],
                "cplPayloadBytes": [
                    packet["size"] for packet in req["completions"]
                ],
            }
            for req in sizing["reqs"]
        ],
        "protocolPayloadGBpsPerRequester": sizing["protocol"],
        "estimatedPayloadGBpsPerRequester": min(
            c["target"], sizing["ceilings"][0][1]
        ),
        "switchSpec": {
            "portChannels": c["channels"],
            "portWidthBits": sizing["portWidth"],
            "fabricSlices": c["slices"],
            "fabricWidthBits": sizing["fabricWidth"],
            "clockMHz": c["clock"],
            "vcPerPort": c["vc"],
            "logicalVoq": sizing["voqCount"],
            "activeVoqPerIngress": sizing["activeVoq"],
            "voqDepthCells": sizing["voqDepth"],
            "ingressPoolCells": sizing["poolCells"],
            "egressCellsPerVc": sizing["egressDepth"],
            "fabricSlotsPerSlice": sizing["fabricSlots"],
            "trackingEntriesPerRequester": sizing["trackDepth"],
            "links": [link_spec(link) for link in sizing["resources"]],
        },
        "sim": sim,
        "notes": NOTES,
        "metadata": {
            "schema_version": 1,
            "execution_mode": experiment["execution_mode"],
            "link_fidelity": experiment["link_fidelity"],
            "source_hashes": experiment["source_hashes"],
            "experiment_hash": experiment["config_hash"],
            "producer": producer,
            "backend_version": (
                "reference-sizing-v1" if sim is None else "gem5-reference-v1"
            ),
            "seed": experiment["seed"],
            "input_units": experiment["input_units"],
            "units": {"time": "ns", "bandwidth": "GB/s", "payload": "B"},
            "measurement": (
                None
                if sim is None
                else {
                    "duration_ns": sim["actualDuration"],
                    "warmup_fraction": 0.2,
                    "latency_interval": "first_link_launch_to_request_finish",
                    "sample_unit": "request",
                }
            ),
            "evidence_level": (
                "analytical" if sim is None else "lab_reference_simulation"
            ),
        },
    }
