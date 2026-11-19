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


"""Native timing packet, retry and payload checks for the PCIe chain."""

import re
from pathlib import Path

from testlib import (
    constants,
    gem5_verify_config,
    verifier,
)

for hops, suffix, options in (
    (1, "", []),
    (2, "", []),
    (2, "-drain", ["--drain-early"]),
    (1, "-64b", ["--bytes", "64"]),
    (2, "-4k", ["--bytes", "4096"]),
):
    gem5_verify_config(
        name=f"pcie-timing-{hops}-hop{suffix}",
        config=str(Path(__file__).parent / "configs/run_timing_case.py"),
        config_args=[str(hops), *options],
        verifiers=[
            verifier.MatchRegex(
                re.compile(r"^PCIe timing packet test passed$")
            )
        ],
        valid_isas=(constants.null_tag,),
        length=constants.quick_tag,
    )

for hops, suffix, extra in (
    (1, "", []),
    (2, "", []),
    (2, "-one-cell", ["--one-cell"]),
    (2, "-tiny-baseline", ["--tiny-baseline"]),
    (2, "-one-cell-flit", ["--one-cell", "--flit"]),
):
    gem5_verify_config(
        name=f"pcie-timing-incast-{hops}-hop{suffix}",
        config=str(Path(__file__).parent / "configs/run_incast_case.py"),
        config_args=[str(hops), *extra],
        verifiers=[
            verifier.MatchRegex(
                re.compile(r"^PCIe timing incast test passed$")
            )
        ],
        valid_isas=(constants.null_tag,),
        length=constants.quick_tag,
    )
