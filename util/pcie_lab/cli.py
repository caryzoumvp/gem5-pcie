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

"""Validate an experiment and optionally size the offline lab profile."""

import argparse
import json
import subprocess
import sys
from pathlib import Path

from .calibration import (
    export_calibration,
    split_document,
)
from .contracts import (
    ConfigError,
    compile_experiment,
    make_result,
    source_hashes,
)
from .sizing import size_hardware


def repository_metadata(root):
    """Report a checkout revision, or explicit unknowns for source archives."""
    try:
        revision = subprocess.run(
            ["git", "rev-parse", "HEAD"],
            cwd=root,
            check=True,
            capture_output=True,
            text=True,
            timeout=5,
        ).stdout.strip()
        dirty = subprocess.run(
            ["git", "status", "--porcelain"],
            cwd=root,
            check=True,
            capture_output=True,
            text=True,
            timeout=5,
        ).stdout
    except (OSError, subprocess.SubprocessError):
        return {"git_revision": None, "git_dirty": None}
    return {"git_revision": revision, "git_dirty": bool(dirty)}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "input", type=Path, help="experiment or calibration JSON"
    )
    parser.add_argument(
        "--root", type=Path, required=True, help="gem5 source root"
    )
    output = parser.add_mutually_exclusive_group()
    output.add_argument(
        "--size", action="store_true", help="include analytical lab sizing"
    )
    output.add_argument(
        "--export-calibration",
        action="store_true",
        help="emit the legacy hardware calibration JSON",
    )
    args = parser.parse_args(argv)
    try:
        document = json.loads(args.input.read_text())
        sources = source_hashes(args.root)
        lock = json.loads(
            Path(__file__).with_name("source_lock.json").read_text()
        )
        if sources != lock["sha256"]:
            raise ConfigError(
                "source_hashes",
                "source_mismatch",
                "source snapshot changed; audit and update the manifest/lock first",
            )
        spec, imported = split_document(document)
        experiment = compile_experiment(spec, sources=sources)
        result = make_result(experiment)
        producer = repository_metadata(args.root)
        result["producer"] = producer
        if imported is not None:
            result["imported_calibration"] = imported
        if args.export_calibration and imported is not None:
            result = imported
        elif args.size or args.export_calibration:
            if experiment["execution_mode"] != "lab_reference":
                raise ConfigError(
                    "execution_mode",
                    "reference_only",
                    "analytical sizing requires lab_reference",
                )
            sizing = size_hardware(experiment["effective_inputs"])
            if args.export_calibration:
                result = export_calibration(experiment, sizing, producer)
            else:
                result["reference_sizing"] = sizing
        serialized = json.dumps(
            result, ensure_ascii=False, indent=2, allow_nan=False
        )
    except (OSError, ValueError) as error:
        diagnostic = {"status": "invalid", "message": str(error)}
        if isinstance(error, ConfigError):
            diagnostic.update(field=error.field, code=error.code)
        print(json.dumps(diagnostic, ensure_ascii=False), file=sys.stderr)
        return 2
    print(serialized)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
