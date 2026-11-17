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

"""Audit the committed M0 manifest against the trusted offline lab source."""

import json
import subprocess
from pathlib import Path


def extract_fields(html_path):
    """Run only the isolated browser-free field module, with a VM timeout."""
    source = Path(html_path).read_text()
    start = source.index("modules['hardware-sizing.mjs'] = (() => {")
    end = source.index("modules['hardware-sim.mjs']", start)
    script = """
const vm = require('vm');
let text = '';
process.stdin.setEncoding('utf8');
process.stdin.on('data', chunk => text += chunk);
process.stdin.on('end', () => {
    const context = {modules: {}};
    vm.runInNewContext(text, context, {timeout: 1000});
    process.stdout.write(JSON.stringify(
        context.modules['hardware-sizing.mjs'].hardwareFields));
});
"""
    result = subprocess.run(
        ["node", "-e", script],
        input=source[start:end],
        text=True,
        check=True,
        capture_output=True,
        timeout=5,
    )
    fields = {}
    for group, entries, *_ in json.loads(result.stdout):
        for key, label, default, *rest in entries:
            if key in fields:
                raise ValueError(f"duplicate HTML field: {key}")
            spec = {"label": label, "group": group, "default": default}
            if isinstance(rest[0], list):
                spec.update(
                    enum=[item[0] for item in rest[0]],
                    condition=rest[1] if len(rest) > 1 else "all",
                )
            else:
                low, high, step, *condition = rest
                spec.update(
                    minimum=low,
                    maximum=high,
                    step=step,
                    condition=condition[0] if condition else "all",
                )
            fields[key] = spec
    return fields
