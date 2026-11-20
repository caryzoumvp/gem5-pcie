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

const assert = require("node:assert/strict");
const {webcrypto} = require("node:crypto");
const fs = require("node:fs");
const vm = require("node:vm");

const html =
    fs.readFileSync("docs/pcie_switch/PCIe6_Scaleup_Lab_Offline.html", "utf8");
const source = html.match(/<script>([\s\S]*?)<\/script>/)[1];

function createPage(storage = new Map(), mountHardware = false)
{
    const elements = new Map();
    const downloads = [];
    const urls = new Map();
    const storageApi = {
        getItem : key => storage.get(key) ?? null,
        setItem : (key, value) => storage.set(key, value),
    };
    const context = vm.createContext({
        crypto : webcrypto,
        TextEncoder,
        Blob,
        localStorage : storageApi,
        setTimeout(callback) { callback(); },
        requestAnimationFrame(callback) { callback(); },
        URL : {
            createObjectURL(blob) {
                const url = String(urls.size);
                urls.set(url, blob);
                return url;
            },
            revokeObjectURL() {},
        },
        document : {
            createElement() {
                return {
                    click() {
                        downloads.push([ this.download, urls.get(this.href) ]);
                    },
                };
            },
        },
    });
    const end = source.indexOf("modules['cluster-topologies.mjs']");
    vm.runInContext(source.slice(0, end), context);
    const page = {
        innerHTML :
            vm.runInContext("modules['cycle-exchange.mjs'].markup()", context),
        addEventListener() {},
        querySelector(selector) {
            assert.ok(
                this.innerHTML.includes('id="' + selector.slice(1) + '"'));
            if (!elements.has(selector)) {
                elements.set(selector, {
                    click() { this.clicked = true; },
                    addEventListener() {},
                    querySelectorAll : () => [],
                    elements : [],
                });
            }
            return elements.get(selector);
        },
    };
    const evidence = {
        querySelector(selector) { return elements.get(selector); },
    };
    for (const id of ["cycle-evidence-import", "cycle-evidence-file",
                      "cycle-evidence-status", "cycle-evidence-history"]) {
        assert.ok(html.includes('id="' + id + '"'));
        elements.set("#" + id, {click() { this.clicked = true; }});
    }
    context.page = page;
    context.evidence = evidence;
    const updateReference = vm.runInContext(
        mountHardware
            ? "modules['hardware-page.mjs'].mountHardware(page, evidence)"
            : "modules['cycle-exchange.mjs'].mount(page, undefined, evidence)",
        context);
    const hash =
        vm.runInContext("modules['cycle-exchange.mjs'].configHash", context);
    const get = id => elements.get("#" + id);
    async function importResult(result, prefix = "cycle")
    {
        get(prefix + "-import").onclick();
        const input = get(prefix + "-file");
        assert.equal(input.clicked, true);
        input.value = "result.json";
        input.files = [ new Blob([ JSON.stringify(result) ]) ];
        await input.onchange();
        assert.equal(input.value, "");
    }
    return {get, downloads, storage, hash, importResult, updateReference};
}

async function main()
{
    const page = createPage();
    if (process.argv[2] === "--external-result") {
        const result = JSON.parse(fs.readFileSync(process.argv[3], "utf8"));
        const editor = page.get("cycle-config");
        const original = editor.value;
        assert.notEqual(await page.hash(JSON.parse(original)),
                        result.config_hash);
        editor.value = "{";
        await page.importResult(result);
        assert.match(page.get("cycle-status").textContent, /已导入/);
        assert.equal(editor.value, "{");
        const history = page.get("cycle-evidence-history").innerHTML;
        assert.ok(history.includes(result.config_hash));
        assert.match(history, /P50/);
        const reopened = createPage(page.storage, true);
        assert.equal(reopened.get("cycle-evidence-history").innerHTML,
                     history);
        await reopened.importResult({...result, config_hash : "0".repeat(64)},
                                    "cycle-evidence");
        assert.match(reopened.get("cycle-evidence-status").textContent,
                     /哈希/);
        assert.equal(reopened.get("cycle-evidence-history").innerHTML,
                     history);
        await reopened.importResult(result, "cycle-evidence");
        assert.match(reopened.get("cycle-evidence-status").textContent,
                     /已导入/);
        assert.equal(reopened.get("cycle-history").innerHTML,
                     reopened.get("cycle-evidence-history").innerHTML);
        return;
    }
    await page.get("cycle-export").onclick();
    assert.equal(page.downloads.length, 1);
    assert.equal(page.downloads[0][0], "pcie6-experiment.json");
    const json = await page.downloads[0][1].text();
    const snapshot = JSON.parse(json);
    assert.equal(snapshot.format, "pcie6-cycle-experiment-v1");
    assert.deepEqual(snapshot.units, {time : "ps", payload : "B"});
    assert.equal(snapshot.topology.kind, "transparent_tree");
    assert.equal(snapshot.workload.flows[0].bytes, 64);
    assert.equal(snapshot.lab_inputs.payload, 64);
    assert.equal(snapshot.hardware.rcb, 128);
    if (process.argv[2] === "--export") {
        process.stdout.write(json);
        return;
    }
    const hash = await page.hash(snapshot);
    // Independent Python compiler oracle for numeric and Unicode encoding.
    assert.equal(
        await page.hash({
            "\u{10437}" : true,
            "\uE001" : [ 4, 5.5, null, -0 ],
            "م" : "x\u2028y",
        }),
        "53312c84ce5dda1d5d97c9181c9bbd1987896aa6bef97ec876bde6f096d2a315");
    assert.match(page.get("cycle-status").textContent, new RegExp(hash));
    const result =
        process.argv[2] === "--result"
            ? JSON.parse(fs.readFileSync(process.argv[3], "utf8"))
            : {
                  format : "pcie6-cycle-result-v1",
                  mode : "pcie6_timing",
                  units : snapshot.units,
                  input : snapshot,
                  config_hash : hash,
                  status : "pass",
                  measurement : {
                      ...snapshot.measurement,
                      completed_payload_bytes : 64,
                      window_complete : true,
                      completed_flows : 1,
                      unfinished_flows : 0,
                      samples : 1,
                      p50_ps : 23125,
                      p99_ps : null,
                      p999_ps : null,
                  },
                  topology : snapshot.topology,
                  flows : [ {id : "<script>", completed_payload_bytes : 64} ],
                  counters : {},
                  producer : {},
              };
    assert.equal(result.config_hash, hash);
    await page.importResult({...result, format : "pcie6-cycle-result-v0"});
    assert.match(page.get("cycle-status").textContent, /版本/);
    await page.importResult({...result, units : {time : "ns", payload : "B"}});
    assert.match(page.get("cycle-status").textContent, /单位/);
    await page.importResult({...result, mode : "lab_reference"});
    assert.match(page.get("cycle-status").textContent, /模式/);
    await page.importResult({...result, config_hash : "0".repeat(64)});
    assert.match(page.get("cycle-status").textContent, /哈希/);
    await page.importResult({
        ...result,
        measurement : {...result.measurement, window_ps : 1},
    });
    assert.match(page.get("cycle-status").textContent, /窗口/);
    const unknown = structuredClone(result);
    unknown.input.workload.flows[0].bytes = 128;
    unknown.config_hash = await page.hash(unknown.input);
    assert.match(page.get("cycle-history").innerHTML, /尚无/);

    const changed = JSON.stringify(unknown.input);
    page.get("cycle-config").value = changed;
    page.get("cycle-config").onchange();
    const reopened = createPage(page.storage);
    assert.equal(reopened.get("cycle-config").value, changed);
    await reopened.importResult(result);
    assert.match(reopened.get("cycle-status").textContent, /已导入/);
    const later = createPage(page.storage);
    assert.match(later.get("cycle-history").innerHTML, new RegExp(hash));
    assert.doesNotMatch(later.get("cycle-history").innerHTML, /<script>/);
    assert.match(later.get("cycle-history").innerHTML, /pcie6_timing/);
    later.updateReference(
        {actualDuration : 100, stats : {bytes : 128}, bw : 1.6});
    assert.match(later.get("cycle-comparison").innerHTML, /lab_reference/);
    assert.match(later.get("cycle-comparison").innerHTML, /pcie6_timing/);
    assert.match(later.get("cycle-comparison").innerHTML, /80 ns/);
    assert.match(later.get("cycle-comparison").innerHTML, /64 B/);
    const stored = Array.from(later.storage.values()).map(JSON.parse);
    assert.deepEqual(stored[0].results, [ result ]);
    const repeat = structuredClone(result);
    repeat.measurement.p50_ps = 12345;
    await later.importResult(repeat);
    const comparison = later.get("cycle-comparison").innerHTML;
    assert.match(comparison, /同配置 · 上次导入/);
    assert.match(comparison, /12,345 ps/);
    const incomplete = structuredClone(repeat);
    incomplete.status = "truncated";
    incomplete.measurement.window_complete = false;
    await later.importResult(incomplete);
    const truncatedComparison = later.get("cycle-comparison").innerHTML;
    assert.doesNotMatch(truncatedComparison, /上次导入/);
    assert.match(truncatedComparison, /窗口未结束/);
    await later.importResult(unknown);
    assert.match(later.get("cycle-status").textContent, /已导入/);
    const changedComparison = later.get("cycle-comparison").innerHTML;
    assert.doesNotMatch(changedComparison, /上次导入/);

    const assessment =
        process.argv[2] === "--performance"
            ? JSON.parse(fs.readFileSync(process.argv[3], "utf8"))
            : {
                  profile : {
                      name : "SW-LAT",
                      frozen : true,
                      source_hashes : {"offline.html" : "a".repeat(64)},
                  },
                  measurement : {
                      boundary : "legal_ingress_to_egress_accept",
                      latency_ns : 11,
                  },
                  evaluation : {
                      schema_version : 1,
                      target_id : "P-02",
                      target_status : "NOT_COMPARABLE",
                      correctness_status : "PASS",
                      evidence_level : "gem5_flit_cycle",
                      reason : "<missing evidence>",
                      metrics : {},
                  },
              };
    await later.importResult(result, "comp");
    assert.match(later.get("comp-status").textContent, /格式/);
    await later.importResult({
        ...assessment,
        evaluation : {...assessment.evaluation, target_status : true},
    },
                             "comp");
    assert.match(later.get("comp-status").textContent, /状态/);
    await later.importResult({
        ...assessment,
        profile :
            {...assessment.profile, source_hashes : {offline : "invalid"}},
    },
                             "comp");
    assert.match(later.get("comp-status").textContent, /SHA-256/);
    await later.importResult({
        ...assessment,
        evaluation : {
            ...assessment.evaluation,
            target_status : "PASS",
            correctness_status : "FAIL",
        },
    },
                             "comp");
    assert.match(later.get("comp-status").textContent, /PASS 缺少/);
    const cycleHistory = later.get("cycle-history").innerHTML;
    await later.importResult(assessment, "comp");
    assert.match(later.get("comp-status").textContent, /已导入/);
    const assessmentHistory = createPage(later.storage);
    const comp = assessmentHistory.get("comp-history").innerHTML;
    assert.match(comp, new RegExp(assessment.evaluation.target_status));
    const correctness = assessment.evaluation.correctness_status;
    assert.ok(comp.includes("正确性 " + correctness));
    if (assessment.measurement.boundary !== undefined) {
        if (assessment.measurement.boundary !== undefined) {
            assert.ok(comp.includes(assessment.measurement.boundary));
        } else {
            assert.ok(comp.includes('&quot;window_ns&quot;: ' +
                                    assessment.measurement.window_ns));
        }
    } else {
        assert.ok(comp.includes('&quot;window_ns&quot;: ' +
                                assessment.measurement.window_ns));
    }
    assert.doesNotMatch(comp, /<missing evidence>/);
    assert.equal(assessmentHistory.get("cycle-history").innerHTML,
                 cycleHistory);
    await later.importResult({
        ...assessment,
        evaluation : {
            ...assessment.evaluation,
            target_status : "INSUFFICIENT_SAMPLES",
        },
    },
                             "comp");
    assert.match(later.get("comp-history").innerHTML, /INSUFFICIENT_SAMPLES/);
    await later.importResult({
        ...assessment,
        evaluation : {
            ...assessment.evaluation,
            target_status : "INSUFFICIENT_SAMPLES",
        },
    },
                             "comp");
    assert.match(later.get("comp-history").innerHTML, /INSUFFICIENT_SAMPLES/);

    const hardware = createPage(new Map(), true);
    hardware.get("hw-run").onclick();
    hardware.get("hw-export").onclick();
    const [filename, blob] = hardware.downloads[0];
    const calibration = JSON.parse(await blob.text());
    assert.equal(filename, "pcie6-hardware-calibration.json");
    assert.equal(calibration.format, "pcie6-hardware-calibration-v1");
    assert.equal(calibration.inputs.payload, 64);
    assert.ok(calibration.sim.sampleCount > 0);
    assert.match(hardware.get("cycle-comparison").innerHTML, /lab_reference/);
    await hardware.importResult(calibration);
    assert.match(hardware.get("cycle-status").textContent, /版本/);
    assert.match(hardware.get("cycle-history").innerHTML, /尚无/);
}

main().catch(error => {
    console.error(error);
    process.exitCode = 1;
});
