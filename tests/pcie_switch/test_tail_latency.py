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


import unittest

from configs.example.pcie.tail_latency import (
    count_stable,
    measure_trace,
)


def trace_for(tlp, ingress, start, end):
    return [(0, stage, start, tlp, ingress) for stage in range(9)] + [
        (0, 9, end, tlp, (ingress + 3) % 9)
    ]


class TailLatencyTest(unittest.TestCase):
    def test_arrival_cohort_and_backlog(self):
        trace = trace_for(1, 0, 9, 13) + trace_for(2, 0, 14, 21)
        result = measure_trace(trace, (10, 15, 20))
        self.assertEqual(result["samples"], 1)
        self.assertEqual(result["p99_ns"], 0.007)
        self.assertEqual(result["flows"][0]["backlog"], [1, 1, 1])
        self.assertEqual(result["flows"][0]["ingress"], [1, 0])
        self.assertEqual(result["flows"][0]["egress"], [1, 0])

    def test_censored_and_invalid_trace(self):
        trace = trace_for(1, 0, 11, 12)
        result = measure_trace(trace[:-1], (10, 15, 20))
        self.assertEqual(result["samples"], 0)
        self.assertEqual(result["censored"], 1)
        self.assertEqual(result["flows"][0]["backlog"], [0, 1, 1])
        with self.assertRaises(ValueError):
            measure_trace(trace + trace[-1:], (10, 15, 20))

    def test_queue_growth_fails_even_if_switch_rates_match(self):
        flow = {
            "ingress": [400, 400, 400],
            "egress": [400, 400, 400],
            "backlog": [2, 2, 2, 2],
            "accepted": [400, 400, 400],
            "endpoint_departures": [400, 400, 400],
            "endpoint_backlog": [4, 4, 4, 4],
        }
        self.assertTrue(count_stable(flow, 400))
        flow["endpoint_departures"] = [399, 399, 399]
        flow["endpoint_backlog"] = [4, 5, 6, 7]
        self.assertFalse(count_stable(flow, 400))
