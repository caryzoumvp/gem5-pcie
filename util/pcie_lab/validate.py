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


"""Build and validate the standalone gem5 PCIe switch."""

import argparse
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path
from xml.etree import ElementTree

ROOT = Path(__file__).resolve().parents[2]
GEM5 = ROOT / "build/NULL/gem5.fast"
GTESTS = [
    ROOT / f"build/NULL/dev/pci/pcie/pcie_{name}.test.opt"
    for name in (
        "request",
        "link",
        "core_resources",
        "ordering",
        "endpoint_memory",
        "switch_core",
        "flit_tlp",
        "flit_packer",
        "flit_replay",
        "flit_integrity",
        "shared_fc",
        "flit_link",
        "tree_topology",
    )
]
PERFORMANCE = ROOT / "configs/example/pcie/performance.py"
MEASURED = {"PASS", "FAIL"}
PERFORMANCE_CASES = [
    ("bulk-read", "PASS", "SW-BULK"),
    ("bulk-write", "PASS", "SW-BULK --write"),
    ("bulk-read-mrrs4096", "PASS", "SW-BULK --mrrs 4096"),
    ("bulk-read-replay16", "PASS", "SW-BULK --replay-flits 16"),
    ("bulk-read-wire", MEASURED, "SW-BULK --wire-feedback"),
    ("bulk-write-wire", "PASS", "SW-BULK --wire-feedback --write"),
    ("small-read", MEASURED, "SW-SMALL"),
    ("small-write", "PASS", "SW-SMALL --write"),
    ("small-read-8", MEASURED, "SW-SMALL --sources 8"),
    ("small-write-8", "PASS", "SW-SMALL --sources 8 --write"),
    ("small-read-wire", MEASURED, "SW-SMALL --wire-feedback"),
    ("small-write-wire", "PASS", "SW-SMALL --wire-feedback --write"),
    ("perm-write", "PASS", "SW-PERM --write"),
    ("perm-read", MEASURED, "SW-PERM"),
    ("perm-write-local", "PASS", "SW-PERM --write --same-slice"),
    ("perm-write-wire", "PASS", "SW-PERM --write --wire-feedback"),
    (
        "perm-write-3-channels",
        "FAIL",
        "SW-PERM --write --fabric-channels 3 --fabric-cells 3",
    ),
    ("rtt-64", "NOT_COMPARABLE", "RTT-64 --wire-feedback"),
]


def run(output, name, *argv, cwd=ROOT, expect_error=None):
    log = output / f"{name}.log"
    print(f"{name}  ({log})", flush=True)
    with log.open("w") as stream:
        result = subprocess.run(
            argv, cwd=cwd, stdout=stream, stderr=subprocess.STDOUT
        )
    if expect_error is None and result.returncode == 0:
        return log
    if (
        expect_error is not None
        and result.returncode > 0
        and expect_error in log.read_text(errors="replace")
    ):
        return log
    raise RuntimeError(f"{name} exited with {result.returncode}. See {log}")


def check_result(path, expected):
    result = json.loads(path.read_text())
    if path.name == "performance.json":
        result = result["evaluation"]
    status = result["target_status"]
    acceptable = {expected} if isinstance(expected, str) else expected
    if result["correctness_status"] != "PASS" or status not in acceptable:
        raise RuntimeError(f"Unexpected result in {path}: {result}")
    return status


def validate(output, jobs, skip_build):
    if not skip_build:
        run(
            output,
            "build",
            "scons",
            "-j",
            str(jobs),
            *(path.relative_to(ROOT) for path in (GEM5, *GTESTS)),
        )
    for path in (GEM5, *GTESTS):
        if not path.is_file() or not os.access(path, os.X_OK):
            raise RuntimeError(
                f"Missing executable {path}. Run with build enabled."
            )

    run(
        output,
        "host",
        sys.executable,
        "-m",
        "unittest",
        "discover",
        "-s",
        "tests/pcie_switch",
    )
    for example in ("reference", "strict_contract"):
        log = run(
            output,
            f"contract-{example}",
            sys.executable,
            "-m",
            "util.pcie_lab.cli",
            f"util/pcie_lab/examples/{example}.json",
            "--root",
            ".",
        )
        contract = json.loads(log.read_text())
        if contract["status"] != "not_run" or contract["protocol_validated"]:
            raise RuntimeError(f"Unexpected contract status in {log}")

    oracle = run(
        output,
        "html-oracle",
        "node",
        "tests/pcie_switch/fixtures/generate_reference_events.js",
    )
    fixture = ROOT / "tests/pcie_switch/fixtures/reference_events.json"
    if json.loads(oracle.read_text()) != json.loads(fixture.read_text()):
        raise RuntimeError(f"HTML oracle differs from {fixture}. See {oracle}")
    for test in GTESTS:
        run(output, test.name, test)

    testlib = output / "testlib"
    testlib.mkdir()
    run(
        output,
        "testlib",
        ROOT / "tests/main.py",
        "run",
        "--build-dir",
        ROOT / "build",
        "--base-dir",
        ROOT,
        "--skip-build",
        "--isa=NULL",
        "--variant=fast",
        "-t",
        str(jobs),
        ROOT / "tests/gem5/pcie",
        cwd=testlib,
    )
    results = testlib / "testing-results/results.xml"
    cases = ElementTree.parse(results).findall(".//testcase")
    if not cases or any(case.get("status") != "Passed" for case in cases):
        raise RuntimeError(
            f"Expected PCIe TestLib cases without skips in {results}"
        )

    browser = ROOT / "tests/pcie_switch/fixtures/test_cycle_html.js"
    experiment = run(output, "cycle-html-export", "node", browser, "--export")
    cycle_output = output / "cycle-html"
    run(
        output,
        "cycle-html-gem5",
        GEM5,
        "-d",
        cycle_output,
        ROOT / "configs/example/pcie/cycle.py",
        experiment,
    )
    run(
        output,
        "cycle-html-import",
        "node",
        browser,
        "--result",
        cycle_output / "cycle-result.json",
    )

    run(
        output,
        "no-shared-tx",
        GEM5,
        "-d",
        output / "no-shared-tx",
        ROOT / "tests/gem5/pcie/configs/run_flit_case.py",
        "1",
        "shared",
        "--no-shared-tx",
        expect_error="invalid FLIT capability or flow-control profile",
    )

    matrix = list(PERFORMANCE_CASES)
    for size in (64, 256):
        for write in (False, True):
            for local in (False, True):
                op = "write" if write else "read"
                scope = "local" if local else "cross"
                args = f"SW-LAT --wire-feedback --bytes {size}"
                if write:
                    args += " --write"
                if local:
                    args += " --same-slice"
                matrix.append((f"lat-{op}-{size}-{scope}", "PASS", args))

    verdicts = {}
    for name, expected, args in matrix:
        folder = output / name
        run(output, name, GEM5, "-d", folder, PERFORMANCE, *args.split())
        verdicts[name] = check_result(folder / "performance.json", expected)
    for name, flags in (
        ("tail-delay", ()),
        ("tail-wire", ("--wire-feedback",)),
    ):
        folder = output / name
        run(
            output,
            name,
            sys.executable,
            ROOT / "configs/example/pcie/tail_latency.py",
            GEM5,
            folder,
            *flags,
        )
        verdicts[name] = check_result(folder / "tail-result.json", "PASS")
    run(
        output,
        "cycle-html-competitor",
        "node",
        browser,
        "--performance",
        output / "bulk-read-wire" / "performance.json",
    )
    summary = output / "performance-verdicts.json"
    summary.write_text(json.dumps(verdicts, indent=2) + "\n")
    print(f"M0–M4A validation passed. Performance verdicts: {summary}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=ROOT,
        help="Output parent directory (default: repository root)",
    )
    parser.add_argument(
        "--skip-build",
        action="store_true",
        help="Use existing NULL gem5 and PCIe GTest binaries",
    )
    parser.add_argument(
        "-j",
        "--jobs",
        type=int,
        default=4,
        help="SCons jobs and TestLib threads (default: 4)",
    )
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs requires a positive integer")
    try:
        parent = args.output_dir.expanduser().resolve()
        parent.mkdir(parents=True, exist_ok=True)
        output = Path(
            tempfile.mkdtemp(prefix="gem5-pcie-validation-", dir=parent)
        )
        print(f"Validation output: {output}", flush=True)
        validate(output, args.jobs, args.skip_build)
    except (
        OSError,
        ValueError,
        KeyError,
        RuntimeError,
        ElementTree.ParseError,
    ) as error:
        parser.exit(1, f"Validation failed: {error}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
