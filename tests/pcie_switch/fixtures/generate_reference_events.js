// Copyright (c) 2026 cary
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met: redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer;
// redistributions in binary form must reproduce the above copyright
// notice, this list of conditions and the following disclaimer in the
// documentation and/or other materials provided with the distribution;
// neither the name of the copyright holders nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

const crypto = require("node:crypto");
const fs = require("node:fs");
const vm = require("node:vm");

const html =
    fs.readFileSync("docs/pcie_switch/PCIe6_Scaleup_Lab_Offline.html", "utf8");
const sizingStart = html.indexOf("modules['hardware-sizing.mjs'] =");
const simEnd = html.indexOf("modules['hardware-page.mjs'] =", sizingStart);
const benchStart = html.indexOf("function runBench(){", simEnd);
const benchEnd = html.indexOf("return cases.map", benchStart);

if ([ sizingStart, simEnd, benchStart, benchEnd ].some(pos => pos === -1)) {
    throw new Error("Offline lab modules or benchmark list changed");
}

const definitions = html.slice(benchStart, benchEnd) + "return cases;}\n";
const results =
    vm.runInNewContext(html.slice(sizingStart, simEnd) + definitions + `
    const {hardwareDefaults, sizeHardware} = modules['hardware-sizing.mjs'];
    const {simulateHardware} = modules['hardware-sim.mjs'];
    const cases = runBench();
    Object.fromEntries(cases.map(({name, diff, check}, index) => {
        const inputs = {...hardwareDefaults, ...diff};
        const sizing = sizeHardware(inputs);
        const sim = simulateHardware(inputs, sizing);
        if (sim.errors || !check(sizing, sim)) throw new Error(name);
        return ['S-0' + (index + 1), {name, diff, inputs, sim}];
    }));
    `,
                       {modules : {}}, {timeout : 5000});
console.log(JSON.stringify({
    source_sha256 : crypto.createHash("sha256").update(html).digest("hex"),
    cases : results,
},
                           null, 2));
