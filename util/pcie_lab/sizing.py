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


"""Analytical sizing for the locked offline lab reference profile."""

from math import (
    ceil,
    floor,
    inf,
    isinf,
    log2,
)

from .contracts import normalize_inputs


def rounded_up(value):
    """Use the HTML tolerance at integer resource boundaries."""
    return ceil(value - min(1e-7, abs(value) * 1e-12))


def power_of_two(value):
    return 1 << (value - 1).bit_length() if value > 0 else 0


def packet(size, header, cell):
    tlp = size + header
    return {
        "size": size,
        "tlp": tlp,
        "credits": ceil(size / 16),
        "cells": ceil(tlp / cell),
    }


def split_transfer(c):
    read = c["op"] == "read"
    maximum = c["mrrs"] if read else c["mps"]
    addr, left = c["offset"], c["payload"]
    requests = []
    while left:
        size = min(left, 4096 - addr % 4096, maximum)
        completions = []
        if read:
            pos, remaining = addr, size
            while remaining:
                count = min(c["mps"], remaining)
                if c["cplSplit"] == "rcb":
                    count = min(count, c["rcb"] - pos % c["rcb"])
                elif count < remaining:
                    count -= (pos + count) % c["rcb"]
                completions.append(packet(count, c["cplHeader"], c["cell"]))
                pos += count
                remaining -= count
        requests.append(
            {
                "size": size,
                "addr": addr,
                "request": packet(
                    0 if read else size, c["reqHeader"], c["cell"]
                ),
                "completions": completions,
            }
        )
        addr += size
        left -= size
    return requests


def link_rows(c):
    rows = [
        ("req", "发起端链路", 1),
        ("cpl", "响应端链路", c["reqPorts"] / c["cplPorts"]),
    ]
    if c["switches"] > 1:
        rows.append(("isl", "每段级联链路", c["reqPorts"]))
    return [
        {
            "key": key,
            "name": name,
            "lanes": c[key + "Lanes"],
            "meters": c[key + "Meters"],
            "ports": c[key + "Ports"] if key != "isl" else 1,
            "share": share,
        }
        for key, name, share in rows
    ]


class Direction:
    def __init__(self, name, packets):
        self.name = name
        self.count = len(packets)
        self.bytes = sum(p["tlp"] for p in packets)
        self.cells = sum(p["cells"] for p in packets)
        self.credits = sum(p["credits"] for p in packets)
        self.max_data = max((p["credits"] for p in packets), default=0)
        self.max_cells = max((p["cells"] for p in packets), default=0)

    def window(self, c, share, credit_rtt, tlp_service, port_capacity):
        if not self.count:
            return None
        freq = c["target"] / c["payload"] * share
        safety = 1 + c["margin"] / 100
        vc = c["vc"]
        header = max(
            1, rounded_up(freq * self.count * credit_rtt * safety / vc)
        )
        data = max(
            self.max_data,
            rounded_up(freq * self.credits * credit_rtt * safety / vc),
        )
        actual_h = c["headerCredits"] or header
        actual_d = c["dataCredits"] or data
        ceiling = (
            c["payload"]
            / share
            / credit_rtt
            * min(
                actual_h * vc / self.count,
                actual_d * vc / self.credits if self.credits else inf,
            )
        )
        cell_rate = freq * self.cells
        physical_bytes = self.cells * (c["cell"] + c["meta"])
        service = (
            min(tlp_service / self.bytes, port_capacity / physical_bytes)
            * self.cells
        )
        return {
            "name": self.name,
            "h": header,
            "d": data,
            "actualH": actual_h,
            "actualD": actual_d,
            "fcCeiling": ceiling,
            "maxD": self.max_data,
            "maxCells": self.max_cells,
            "burstCells": rounded_up(
                max(0, cell_rate * c["burst"] - service) * c["burstNs"]
            ),
            "rate": freq * self.count * 1000,
            "tlp": freq * self.bytes,
            "cellRate": cell_rate,
        }


def link_resources(c, links, directions, packing, port_capacity):
    cell_physical = c["cell"] + c["meta"]
    resources = []
    for link in links:
        line = c["rate"] * link["lanes"] / 8
        tlp = line * packing
        share = link["share"]
        delay = link["meters"] * c["nsMeter"] + c["retimers"] * c["retimerNs"]
        credit_rtt = 2 * delay + c["creditFixed"]
        ack_rtt = 2 * delay + c["ackFixed"]
        wire_ceiling = (
            c["payload"]
            / share
            * min(tlp / d.bytes for d in directions if d.count)
        )
        port_ceiling = (
            c["payload"]
            / share
            * min(
                port_capacity / (d.cells * cell_physical)
                for d in directions
                if d.count
            )
        )
        replay_need = rounded_up(
            line * ack_rtt / 256 * (1 + c["margin"] / 100)
        )
        replay = c["replayFlits"] or power_of_two(replay_need)
        forward, reverse = (
            d.window(c, share, credit_rtt, tlp, port_capacity)
            for d in directions
        )
        resources.append(
            {
                **link,
                "line": line,
                "tlp": tlp,
                "linkDelay": delay,
                "creditRtt": credit_rtt,
                "ackRtt": ack_rtt,
                "wireCeiling": wire_ceiling,
                "portCeiling": port_ceiling,
                "replayNeed": replay_need,
                "replay": replay,
                "replayCeiling": wire_ceiling
                * min(1, replay * 256 / (line * ack_rtt)),
                "forward": forward,
                "reverse": reverse,
            }
        )
    return resources


def warnings_for(c, result, directions):
    warnings = []

    def add(condition, severity, message):
        if condition:
            warnings.append([severity, message])

    name, limit = result["ceilings"][0]
    target = c["target"]
    target_text = str(int(target)) if target == int(target) else str(target)
    add(
        limit < target - 1e-6,
        "bad",
        f"目标 {target_text} GB/s/端口超过当前估算上限，首个约束为 {name}。",
    )
    tags = result["tagPerFunction"]
    budget = c["tagBudget"]
    add(
        tags is not None and tags > budget,
        "bad",
        f"实际 MemRd 并发需要 {tags} Tag/Function，"
        f"超过已配置的 {int(budget)}。",
    )
    add(
        c["switches"] == 1
        and (
            c["reqPorts"] + c["cplPorts"] > c["chipPorts"]
            or c["reqPorts"] * c["reqLanes"] + c["cplPorts"] * c["cplLanes"]
            > c["chipLanes"]
        ),
        "bad",
        "活跃端口或 SerDes lanes 超出单颗 IP 配置。",
    )
    add(
        not c["chipPorts"] <= c["chipLanes"] <= 16 * c["chipPorts"],
        "warn",
        "逻辑端口与 lane 总量需核对合法的 PHY Bifurcation 组合。",
    )
    add(
        c["replayFlits"]
        and any(r["replay"] < r["replayNeed"] for r in result["resources"]),
        "warn",
        "已配置 Replay 深度小于至少一个端口的满线速窗口加裕量。",
    )
    add(
        any(d["actualD"] < d["maxD"] for d in directions),
        "bad",
        "D Credit 小于至少一个 TLP 的数据需求。该队列中的大包无法发送。",
    )
    add(
        c["activeVoq"] > result["activeVoq"],
        "warn",
        "活跃 VOQ 额度已按当前物理端口 / VC 组合封顶。",
    )
    need = result["voqNeed"]
    add(
        result["poolCells"] < need,
        "warn",
        "单入口 Payload 池小于热点 VOQ 突发估算。",
    )
    add(result["egressDepth"] < need, "warn", "出口暂存小于突发估算。")
    add(
        result["voqDepth"] < need,
        "warn",
        "已配置 VOQ 深度低于突发估算。请核对流量分布和共享池。",
    )
    transport_rtt = 2 * (result["oneWay"] + c["switches"] * c["switchNs"])
    add(
        result["read"]
        and c["rttMode"] == "fixed"
        and c["fixedRtt"] < transport_rtt,
        "warn",
        "手动 RTT 小于链路与 Switch 往返之和。仿真仍会计入传播与穿越延迟。",
    )
    return warnings


def size_hardware(inputs):
    """Return the page's JSON-safe analytical result from reference inputs."""
    c = normalize_inputs(inputs)
    read = c["op"] == "read"
    reqs = split_transfer(c)
    requests = [r["request"] for r in reqs]
    completions = [p for r in reqs for p in r["completions"]]
    request = Direction("NP / MemRd" if read else "P / MemWr", requests)
    response = Direction("Cpl / CplD", completions)
    links = link_rows(c)
    hops = c["switches"]
    one_way = (
        c["nsMeter"]
        * (c["reqMeters"] + c["cplMeters"] + (hops - 1) * c["islMeters"])
        + c["retimers"] * (hops + 1) * c["retimerNs"]
    )
    stages = [
        ["链路与 Retimer 往返", 2 * one_way],
        ["Switch 往返穿越", 2 * hops * c["switchNs"]],
        ["端点及其余预算", c["endpointNs"]],
    ]
    derived_rtt = sum(delay for _, delay in stages)
    rtt = c["fixedRtt"] if c["rttMode"] == "fixed" else derived_rtt
    packing = 236 / 256 * c["packing"] / 100
    safety = 1 + c["margin"] / 100
    cycle = c["clock"] / 1000 * c["util"] / 100
    ops = c["target"] / c["payload"]
    physical_cell = c["cell"] + c["meta"]
    line_rates = [c["rate"] * link["lanes"] / 8 for link in links]
    protocol = min(
        line
        * packing
        * c["payload"]
        / link["share"]
        / max(request.bytes, response.bytes)
        for line, link in zip(line_rates, links)
    )
    peak_link_cells = (
        protocol
        / c["payload"]
        * max(request.cells, response.cells)
        * physical_cell
        * max(link["share"] for link in links)
    )
    nominal_port_width = 32 * rounded_up(
        max(peak_link_cells, *line_rates) * 8 / (cycle * c["channels"]) / 32
    )
    port_width = c["portWidth"] or nominal_port_width
    port_capacity = port_width * c["channels"] / 8 * cycle
    chip_raw = c["rate"] * c["chipLanes"] / 8
    cells_per_op = request.cells + response.cells
    fabric_per_op = cells_per_op * physical_cell
    required_fabric = max(
        chip_raw,
        protocol / c["payload"] * cells_per_op * physical_cell * c["reqPorts"],
    )
    fabric_width = c["fabricWidth"] or 32 * rounded_up(
        required_fabric * safety * 8 / (cycle * c["slices"]) / 32
    )
    fabric_capacity = fabric_width * c["slices"] / 8 * cycle
    resources = link_resources(
        c, links, (request, response), packing, port_capacity
    )

    in_flight = rounded_up(ops * len(reqs) * rtt) if read else None
    ideal = rounded_up(ops * rtt) if read else None
    track_depth = (
        power_of_two(rounded_up(in_flight * safety)) if read else None
    )
    track_bits = 16 + c["tagBits"] + 13 + ceil(log2(c["chipPorts"]))
    track_bits += c["trackExtra"]
    tags = rounded_up(in_flight / c["functions"]) if read else None
    tag_bw = (
        c["tagBudget"] * c["functions"] * c["payload"] / (len(reqs) * rtt)
        if read and rtt
        else inf
    )
    req_bw = (
        c["requestLimit"] / 1000 * c["payload"] / len(reqs)
        if c["requestLimit"]
        else inf
    )
    cpl_bw = (
        c["cplLimit"]
        / 1000
        * c["payload"]
        / len(completions)
        * c["cplPorts"]
        / c["reqPorts"]
        if read and c["cplLimit"]
        else inf
    )
    directions = [
        r[key]
        for r in resources
        for key in ("forward", "reverse")
        if r[key] is not None
    ]
    ceilings = [
        ["FLIT / TLP 与链路切割", min(r["wireCeiling"] for r in resources)],
        ["可用 Tag", tag_bw],
        ["发起端请求率", req_bw],
        ["响应端 Completion 率", cpl_bw],
        ["端口数据面", min(r["portCeiling"] for r in resources)],
        [
            "Fabric Cell 数据面",
            fabric_capacity * c["payload"] / (fabric_per_op * c["reqPorts"]),
        ],
        ["Credit 窗口", min(d["fcCeiling"] for d in directions)],
        ["Replay 窗口", min(r["replayCeiling"] for r in resources)],
    ]
    ceilings.sort(key=lambda row: row[1])
    voq_need = max(max(d["maxCells"], d["burstCells"]) for d in directions)
    voq_depth = c["voqDepth"] or power_of_two(voq_need)
    voq_count = c["chipPorts"] * (c["chipPorts"] - 1) * 3 * c["vc"]
    active_voq = min(c["activeVoq"], voq_count // c["chipPorts"])
    sram_scale = 1 + c["sramFactor"] / 100
    pool_bytes = (
        c["poolKiB"] * 1024 or active_voq * voq_depth * physical_cell
    ) * sram_scale
    result = {
        "read": read,
        "reqs": reqs,
        "requests": requests,
        "completions": completions,
        "resources": resources,
        "stages": stages,
        "derivedRtt": derived_rtt,
        "rtt": rtt,
        "oneWay": one_way,
        "packing": packing,
        "cellPhysical": physical_cell,
        "ops": ops,
        "qBytes": request.bytes,
        "aBytes": response.bytes,
        "qCells": request.cells,
        "aCells": response.cells,
        "inFlight": in_flight,
        "ideal": ideal,
        "trackDepth": track_depth,
        "trackBits": track_bits,
        "tagPerFunction": tags,
        "portCapacity": port_capacity,
        "portWidth": port_width,
        "nominalPortWidth": nominal_port_width,
        "fabricWidth": fabric_width,
        "fabricCapacity": fabric_capacity,
        "chipRaw": chip_raw,
        "ceilings": ceilings,
        "protocol": protocol,
        "voqNeed": voq_need,
        "voqDepth": voq_depth,
        "voqCount": voq_count,
        "activeVoq": active_voq,
        "voqBytes": (
            c["chipPorts"]
            * active_voq
            * voq_depth
            * c["descriptor"]
            * sram_scale
        ),
        "poolBytes": pool_bytes,
        "poolCells": floor(pool_bytes / (physical_cell * sram_scale)),
        "egressDepth": c["egressCells"] or power_of_two(voq_need),
        "fabricSlots": rounded_up(
            fabric_capacity
            / c["slices"]
            * c["fabricCreditNs"]
            / physical_cell
            * safety
        ),
        "busyFabric": ops * fabric_per_op * c["reqPorts"],
        "trackBytes": (track_depth or 0) * ceil(track_bits / 8) * sram_scale,
        "maxPayload": max(r["size"] for r in reqs),
        "sramScale": sram_scale,
    }
    result["warnings"] = warnings_for(c, result, directions)
    result["ceilings"] = [
        [name, None if isinf(ceiling) else ceiling]
        for name, ceiling in ceilings
    ]
    return result
