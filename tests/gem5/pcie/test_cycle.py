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

"""Register standalone PCIe cycle experiments with TestLib."""

import re
from pathlib import Path

from testlib import (
    constants,
    gem5_verify_config,
    verifier,
)

for case in (
    "star",
    "chain",
    "branch",
    "width",
    "delay",
    "endpoint-delay",
    "incast",
    "collective",
    "timeout",
    "invalid",
    "external",
):
    gem5_verify_config(
        name=f"pcie-cycle-{case}",
        config=str(Path(__file__).parent / "configs/run_cycle_case.py"),
        config_args=[case],
        verifiers=[
            verifier.MatchRegex(re.compile(rf"^PCIe cycle {case} passed$"))
        ],
        valid_isas=(constants.null_tag,),
        length=constants.quick_tag,
    )

gem5_verify_config(
    name="pcie-cycle-workload-driver",
    config=str(Path(__file__).parent / "configs/run_workload_case.py"),
    config_args=[],
    verifiers=[
        verifier.MatchRegex(
            re.compile(r"^PCIe standalone workload test passed$")
        )
    ],
    valid_isas=(constants.null_tag,),
    length=constants.quick_tag,
)
