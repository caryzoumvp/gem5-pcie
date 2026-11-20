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

"""Check a posted-write DAG and independent-source retry."""

import m5
from m5.objects import (
    PcieTiming,
    PcieWorkload,
    Root,
    SrcClockDomain,
    System,
    VoltageDomain,
)


def main():
    system = System(mem_mode="timing")
    system.clk_domain = SrcClockDomain(
        clock="1GHz", voltage_domain=VoltageDomain()
    )
    system.dut = PcieTiming(
        window="128KiB",
        outstanding=1,
        mps=128,
        mrrs=128,
        endpoint_cycles=20,
        endpoint_hold_cycles=250,
    )
    system.workload_driver = PcieWorkload(
        dut=system.dut,
        flow_ids=["write", "retry", "read", "wrong", "blocked"],
        source_indices=[0, 0, 1, 1, 1],
        addresses=[0x1000, 0x4000, 0x1000, 0x1000, 0x4000],
        transfer_bytes=[4096, 64, 4096, 64, 64],
        writes=[True, True, False, False, False],
        patterns=[0x5A, 0xA5, 0x5A, 0x01, 0xA5],
        releases=[10, 10, 0, 0, 0],
        dependency_offsets=[0, 0, 0, 1, 2, 3],
        dependency_indices=[0, 2, 3],
    )
    for _ in range(2):
        system.workload_driver.ports = system.dut.cpu_side

    Root(full_system=False, system=system)
    m5.instantiate()
    m5.simulate(20_000_000)
    assert system.workload_driver.finished()
    assert system.dut.idle()
    assert not system.dut.waitGraph()
    write, retry, read, wrong, blocked = system.workload_driver.flowTrace()
    assert write[0] == 10
    assert write[0] <= write[1] < write[2]
    assert retry[0] >= write[2]
    assert read[0] == write[2]
    assert read[0] < read[1] == read[2]
    assert wrong[0] == read[2]
    assert [row[3] for row in [write, retry, read, wrong, blocked]] == [
        1,
        1,
        1,
        2,
        2,
    ]
    assert blocked[:3] == [(1 << 64) - 1] * 3
    stats = system.dut.report()
    assert stats["accepted_packets"] == 4
    assert stats["visible_write_tlps"] == 33
    assert stats["completed_read_packets"] == 2
    assert stats["request_stalls"] >= 1
    print("PCIe standalone workload test passed")


if __name__ == "__m5_main__":
    main()
