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


"""Native FLIT transport, flow control, and replay checks."""

import re
from pathlib import Path

from testlib import (
    constants,
    gem5_verify_config,
    verifier,
)

for hops, scenario in (
    (1, "shared"),
    (1, "merged"),
    (2, "dedicated"),
    (2, "crc"),
    (1, "lost-ack"),
):
    gem5_verify_config(
        name=f"pcie-flit-{hops}-hop-{scenario}",
        config=str(Path(__file__).parent / "configs/run_flit_case.py"),
        config_args=[str(hops), scenario],
        verifiers=[
            verifier.MatchRegex(re.compile(r"^PCIe native FLIT test passed$"))
        ],
        valid_isas=(constants.null_tag,),
        length=constants.quick_tag,
    )

for hops, scenario in ((2, "crc"), (1, "lost-ack")):
    gem5_verify_config(
        name=f"pcie-flit-{hops}-hop-{scenario}-wire-feedback",
        config=str(Path(__file__).parent / "configs/run_flit_case.py"),
        config_args=[str(hops), scenario, "--wire-feedback"],
        verifiers=[
            verifier.MatchRegex(re.compile(r"^PCIe native FLIT test passed$"))
        ],
        valid_isas=(constants.null_tag,),
        length=constants.quick_tag,
    )

gem5_verify_config(
    name="pcie-flit-cycle-trace",
    config=str(Path(__file__).parent / "configs/run_flit_case.py"),
    config_args=["1", "shared", "--cycle-trace"],
    verifiers=[
        verifier.MatchRegex(re.compile(r"^PCIe native FLIT test passed$"))
    ],
    valid_isas=(constants.null_tag,),
    length=constants.quick_tag,
)

for operation in ("read", "write"):
    gem5_verify_config(
        name=f"pcie-flit-permutation-{operation}",
        config=str(
            Path(__file__).resolve().parents[3]
            / "configs/example/pcie/performance.py"
        ),
        config_args=["SW-PERM", "--verify-permutation"]
        + (["--write"] if operation == "write" else []),
        verifiers=[
            verifier.MatchRegex(
                re.compile(r"^PCIe native permutation routes passed$")
            )
        ],
        valid_isas=(constants.null_tag,),
        length=constants.quick_tag,
    )
