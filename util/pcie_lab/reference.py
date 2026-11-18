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


"""Offline lab event semantics, dispatched by the gem5 main event queue."""

from collections import deque
from dataclasses import (
    dataclass,
    field,
)
from math import (
    ceil,
    floor,
)

TICKS_PER_NS = 1000


class ReferenceEvents:
    """Resolve browser heap ties, dispatching each callback on a gem5 event.

    Floating-point deadlines preserve the browser's statistics. The gem5
    dispatch tick is rounded up so a callback never runs before its deadline.
    This policy belongs to the lab reference profile.
    """

    def __init__(self, event_api):
        self.api = event_api
        self.pending = []
        self.complete = False
        self.event = event_api.create(self.dispatch)

    def add(self, when, callback):
        entry = (when, callback)
        i = len(self.pending)
        self.pending.append(entry)
        while i:
            parent = (i - 1) // 2
            if self.pending[parent][0] <= when:
                break
            self.pending[i] = self.pending[parent]
            i = parent
        self.pending[i] = entry

    def pop(self):
        first = self.pending[0]
        last = self.pending.pop()
        if self.pending:
            i = 0
            while 2 * i + 1 < len(self.pending):
                child = 2 * i + 1
                if (
                    child + 1 < len(self.pending)
                    and self.pending[child + 1][0] < self.pending[child][0]
                ):
                    child += 1
                if self.pending[child][0] >= last[0]:
                    break
                self.pending[i] = self.pending[child]
                i = child
            self.pending[i] = last
        return first

    def start(self, duration):
        self.duration = duration
        self.end_tick = ceil(duration * TICKS_PER_NS)
        self.schedule_next()

    def schedule_next(self):
        if self.pending and self.pending[0][0] <= self.duration:
            when = ceil(self.pending[0][0] * TICKS_PER_NS)
            self.api.getEventQueue().schedule(self.event, when)
        else:
            self.complete = True

    def dispatch(self):
        when, callback = self.pop()
        callback(when)
        self.schedule_next()


@dataclass
class VcQueue:
    pending: deque = field(default_factory=deque)
    headers: int = 0
    data: int = 0
    cells: int = 0


class Pipe:
    """Unbounded enqueue with a bounded, time-recycled FC service window."""

    def __init__(self, model, link, reverse):
        self.model = model
        self.link = link
        self.spec = link["reverse" if reverse else "forward"]
        self.wire = (
            min(link["line"], link["replay"] * 256 / link["ackRtt"])
            * model.sizing["packing"]
        )
        self.queues = [VcQueue() for _ in range(int(model.inputs["vc"]))]
        self.next_vc = 0
        self.busy = False
        self.waiting = False

    def enqueue(self, vc, packet, now, done, launch):
        queue = self.queues[vc]
        queue.pending.append((packet, done, launch))
        queue.cells += packet["cells"]
        self.drain(now)
        stats = self.model.stats
        stats["egress"] = max(stats["egress"], queue.cells)

    def drain(self, now):
        if self.busy:
            return
        count = len(self.queues)
        pending = False
        for offset in range(count):
            vc = (self.next_vc + offset) % count
            queue = self.queues[vc]
            if not queue.pending:
                continue
            pending = True
            packet, done, launch = queue.pending[0]
            data = packet["credits"]
            if (
                queue.headers >= self.spec["actualH"]
                or queue.data + data > self.spec["actualD"]
            ):
                continue
            queue.pending.popleft()
            queue.cells -= packet["cells"]
            queue.headers += 1
            queue.data += data
            self.next_vc = (vc + 1) % count
            self.busy = True
            self.waiting = False
            if launch is not None:
                launch(now)
            wire_time = packet["tlp"] / self.wire
            sizing = self.model.sizing
            port_time = (
                packet["cells"]
                * sizing["cellPhysical"]
                / sizing["portCapacity"]
            )
            serialized = max(wire_time, port_time)

            def wire_free(time):
                self.busy = False
                self.drain(time)

            def return_credit(time):
                queue.headers -= 1
                queue.data -= data
                self.drain(time)

            add = self.model.add
            add(now + serialized, wire_free)
            add(now + max(serialized, self.link["creditRtt"]), return_credit)
            add(now + serialized + self.link["linkDelay"], done)
            return

        if pending and not self.waiting:
            self.model.stats["creditStall"] += 1
            self.waiting = True


@dataclass
class Fabric:
    free: float = 0
    queues: dict = field(default_factory=dict)
    ingress: dict = field(default_factory=dict)


@dataclass
class Ingress:
    cells: int = 0
    active: int = 0


@dataclass
class Requester:
    pending: deque = field(default_factory=deque)
    tags: int = 0
    waiting: bool = False


class PcieLabReference:
    def __init__(self, inputs, sizing, schedule):
        self.inputs = inputs
        self.sizing = sizing
        self.schedule = schedule
        self.read = sizing["read"]
        self.requested_duration = inputs["simUs"] * 1000
        self.burst_period = (
            inputs["burstNs"] * inputs["burst"]
            if inputs["simArrival"] == "burst"
            else 0
        )
        self.offer = sizing["ops"]
        if inputs["requestLimit"]:
            self.offer = min(
                self.offer,
                inputs["requestLimit"] / 1000 / len(sizing["requests"]),
            )
        packets = len(sizing["requests"]) + len(sizing["completions"])
        self.duration = min(
            self.requested_duration,
            30000
            / (
                self.offer
                * inputs["reqPorts"]
                * packets
                * (inputs["burst"] if self.burst_period else 1)
            ),
        )
        if self.duration < 1:
            raise ValueError("workload exceeds the offline lab event budget")
        self.warmup = self.duration * 0.2
        self.stats = dict.fromkeys(
            (
                "bytes",
                "arrivals",
                "issued",
                "completed",
                "tagStall",
                "creditStall",
                "queue",
                "pool",
                "egress",
                "activeVoq",
                "peakTags",
                "events",
            ),
            0,
        )
        self.samples = []
        self.fabric = [Fabric() for _ in range(int(inputs["switches"]))]
        self.participants = [
            Requester() for _ in range(int(inputs["reqPorts"]))
        ]
        self.completer_free = [0] * int(inputs["cplPorts"])
        self.extra_endpoint = inputs["endpointNs"]
        if inputs["rttMode"] == "fixed" and self.read:
            transport = 2 * (
                sizing["oneWay"] + inputs["switches"] * inputs["switchNs"]
            )
            self.extra_endpoint = max(0, sizing["rtt"] - transport)
        self.tag_limit = inputs["tagBudget"] * inputs["functions"]
        links = {link["key"]: link for link in sizing["resources"]}
        self.req = self.pairs(links["req"], inputs["reqPorts"])
        self.cpl = self.pairs(links["cpl"], inputs["cplPorts"])
        self.isl = (
            self.pairs(links["isl"], inputs["switches"] - 1)
            if inputs["switches"] > 1
            else []
        )

    def pairs(self, link, count):
        return [
            (
                Pipe(self, link, False),
                Pipe(self, link, True) if self.read else None,
            )
            for _ in range(int(count))
        ]

    def add(self, when, callback):
        def run(now):
            self.stats["events"] += 1
            callback(now)

        self.schedule(when, run)

    def crossbar(self, index, ingress, egress, vc, packet, now, done):
        fabric = self.fabric[index]
        key = (ingress, egress, vc)
        bank = fabric.ingress.setdefault(ingress, Ingress())
        before = fabric.queues.get(key, 0)
        cells = packet["cells"]
        queued = before + cells
        fabric.queues[key] = queued
        bank.cells += cells
        if not before:
            bank.active += 1
        start = max(now, fabric.free)
        sizing = self.sizing
        end = start + cells * sizing["cellPhysical"] / sizing["fabricCapacity"]
        fabric.free = end

        def grant(_):
            left = fabric.queues[key] - cells
            fabric.queues[key] = left
            bank.cells -= cells
            if not left:
                bank.active -= 1

        self.add(start, grant)
        if start > now:
            for label, value in (
                ("queue", queued),
                ("pool", bank.cells),
                ("activeVoq", bank.active),
            ):
                self.stats[label] = max(self.stats[label], value)
        self.add(end + self.inputs["switchNs"], done)

    def route(self, src, dst, vc, packet, now, back, done, launch=None):
        if back:
            segments = [
                self.cpl[dst][1],
                *(pairs[1] for pairs in reversed(self.isl)),
                self.req[src][1],
            ]
            in_id, out_id = ("cpl", dst), ("req", src)
        else:
            segments = [
                self.req[src][0],
                *(pairs[0] for pairs in self.isl),
                self.cpl[dst][0],
            ]
            in_id, out_id = ("req", src), ("cpl", dst)

        def hop(i, time):
            def arrived(arrival):
                switch = len(self.fabric) - 1 - i if back else i
                self.crossbar(
                    switch,
                    in_id if i == 0 else "isl",
                    out_id if i == len(segments) - 2 else "isl",
                    vc,
                    packet,
                    arrival,
                    lambda next_time: hop(i + 1, next_time),
                )

            segments[i].enqueue(
                vc,
                packet,
                time,
                done if i == len(segments) - 1 else arrived,
                launch if i == 0 else None,
            )

        hop(0, now)

    def issue(self, src, dst, vc, req, now):
        state = self.participants[src]
        first = None

        def launch(time):
            nonlocal first
            first = time

        def finish(time):
            self.stats["completed"] += 1
            if self.warmup <= time <= self.duration:
                self.stats["bytes"] += req["size"]
                self.samples.append(time - first)
            if self.read:
                state.tags -= 1
                self.admit(src, time)

        def arrive(time):
            if not self.read:
                finish(time)
                return
            remaining = len(req["completions"])
            send_at = max(time + self.extra_endpoint, self.completer_free[dst])

            def complete_piece(end):
                nonlocal remaining
                remaining -= 1
                if not remaining:
                    finish(end)

            cpl_limit = self.inputs["cplLimit"]
            for response in req["completions"]:
                self.add(
                    send_at,
                    lambda now, packet=response: self.route(
                        src, dst, vc, packet, now, True, complete_piece
                    ),
                )
                if cpl_limit:
                    send_at += 1000 / cpl_limit
            self.completer_free[dst] = send_at

        self.route(src, dst, vc, req["request"], now, False, arrive, launch)

    def admit(self, src, now):
        state = self.participants[src]
        while state.pending:
            if self.read and state.tags >= self.tag_limit:
                if not state.waiting:
                    self.stats["tagStall"] += 1
                state.waiting = True
                return
            req, dst, vc = state.pending.popleft()
            state.waiting = False
            self.stats["issued"] += 1
            if self.read:
                state.tags += 1
                self.stats["peakTags"] = max(
                    self.stats["peakTags"], state.tags
                )
            self.issue(src, dst, vc, req, now)

    def arrival(self, src, seq, now):
        dst = (src + seq) % len(self.cpl)
        vc = (seq // len(self.cpl)) % len(self.req[src][0].queues)
        self.stats["arrivals"] += 1
        self.participants[src].pending.extend(
            (request, dst, vc) for request in self.sizing["reqs"]
        )
        self.admit(src, now)

        interval = 1 / self.offer
        clock = (seq + 1 + src / len(self.req)) * interval
        next_time = clock
        if self.burst_period:
            period = self.burst_period
            next_time = (
                floor(clock / period) * period
                + (clock % period) / self.inputs["burst"]
            )
        if next_time < self.duration:
            self.add(next_time, lambda time: self.arrival(src, seq + 1, time))

    def start(self):
        interval = 1 / self.offer
        burst = self.inputs["burst"] if self.burst_period else 1
        for src in range(len(self.req)):
            self.add(
                src * interval / len(self.req) / burst,
                lambda time, i=src: self.arrival(i, 0, time),
            )

    def result(self):
        self.samples.sort()
        count = len(self.samples)
        sizing = self.sizing
        limits = {
            "queue": sizing["voqDepth"],
            "pool": sizing["poolCells"],
            "egress": sizing["egressDepth"],
            "activeVoq": sizing["activeVoq"],
        }
        result = {
            "stats": self.stats,
            "requestedDuration": self.requested_duration,
            "actualDuration": self.duration,
            "shortened": self.duration < self.requested_duration,
            "bw": self.stats["bytes"]
            / (self.duration - self.warmup)
            / len(self.req),
            "sampleCount": count,
            "queuePass": all(
                self.stats[key] <= limit for key, limit in limits.items()
            ),
        }
        for key, quantile in (("p50", 0.5), ("p99", 0.99), ("p999", 0.999)):
            result[key] = (
                self.samples[ceil(count * quantile) - 1] if count else None
            )
        return result
