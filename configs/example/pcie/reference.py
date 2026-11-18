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


"""Run the offline PCIe lab reference model on a gem5 event queue."""

import argparse
import json
import sys
from pathlib import Path

import m5
from m5.objects import Root

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO))

from util.pcie_lab.calibration import (
    export_calibration,
    split_document,
)
from util.pcie_lab.cli import repository_metadata
from util.pcie_lab.contracts import (
    compile_experiment,
    fail,
    make_result,
    source_hashes,
)
from util.pcie_lab.reference import (
    PcieLabReference,
    ReferenceEvents,
)
from util.pcie_lab.sizing import size_hardware


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "input", type=Path, help="experiment or calibration JSON"
    )
    parser.add_argument("--output", type=Path)
    parser.add_argument("--calibration-output", type=Path)
    args = parser.parse_args()
    output = args.output or (Path(m5.options.outdir) / "reference.json")
    if (
        args.calibration_output
        and args.calibration_output.resolve() == output.resolve()
    ):
        parser.error("calibration output must differ from the result path")

    try:
        document, imported = split_document(json.loads(args.input.read_text()))
        sources = source_hashes(REPO)
        lock = json.loads(
            (REPO / "util/pcie_lab/source_lock.json").read_text()
        )
        if sources != lock["sha256"]:
            fail("source_hashes", "source_mismatch", "offline sources changed")
        experiment = compile_experiment(document, sources=sources)
        if experiment["execution_mode"] != "lab_reference":
            fail("execution_mode", "reference_only", "requires lab_reference")
        # These declarations have no representation in the browser model.
        for field in ("topology", "protocol", "resources"):
            if document.get(field):
                fail(field, "unsupported", "use the reference hardware fields")
        inputs = experiment["effective_inputs"]
        sizing = size_hardware(inputs)
        events = ReferenceEvents(m5.event)
        model = PcieLabReference(inputs, sizing, events.add)
    except (ValueError, OSError) as exc:
        parser.error(str(exc))

    m5.ticks.setGlobalFrequency("1THz")
    root = Root(full_system=False)
    m5.instantiate()
    model.start()
    events.start(model.duration)
    m5.simulate(events.end_tick)
    if not events.complete or m5.curTick() != events.end_tick:
        raise RuntimeError("gem5 exited before the reference window ended")

    sim = model.result()
    unfinished = sim["stats"]["arrivals"] * len(sizing["reqs"])
    unfinished -= sim["stats"]["completed"]
    result = make_result(experiment, unfinished=unfinished)
    result.update(
        status="simulated",
        sample_count=sim["sampleCount"],
        payload_bytes=sim["stats"]["bytes"],
        latency_ns={key: sim[key] for key in ("p50", "p99", "p999")},
        measurement_ps=(model.duration - model.warmup) * 1000,
        reference_sim=sim,
        reference_sizing=sizing,
        backend="gem5-reference-v1",
        dispatch_end_tick=m5.curTick(),
        producer=repository_metadata(REPO),
    )
    if imported is not None:
        result["imported_calibration"] = imported
    output.write_text(json.dumps(result, indent=2, allow_nan=False) + "\n")
    if args.calibration_output:
        calibration = export_calibration(
            experiment, sizing, result["producer"], sim=sim
        )
        args.calibration_output.write_text(
            json.dumps(calibration, indent=2, allow_nan=False) + "\n"
        )


if __name__ == "__m5_main__":
    main()
