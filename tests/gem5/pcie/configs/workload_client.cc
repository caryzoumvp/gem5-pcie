/*
 * Copyright (c) 2026 cary
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <memory>

#include "dev/pci/pcie/workload_client.h"

namespace
{

struct Fixture
{
    const Gem5PcieHost host;
    unsigned order = 0;
    unsigned completions = 0;

    static void
    scheduleNext(void *arg)
    {
        auto &state = *static_cast<Fixture *>(arg);
        if (++state.order != 3) {
            state.fail("same-tick nested callback violated FIFO order");
        }
    }

    static void
    issueWrite(void *arg)
    {
        auto &state = *static_cast<Fixture *>(arg);
        if (++state.order != 1 || state.host.now(state.host.context) != 123 ||
            !state.host.schedule(state.host.context, 123, scheduleNext, arg) ||
            state.order != 1 ||
            !state.host.submit(state.host.context, 0, 1, 0, 4096, 1, 0x5a,
                               writeAccepted, writeVisible, arg)) {
            state.fail("could not issue Write asynchronously at tick 123");
        }
    }

    static void
    checkFifo(void *arg)
    {
        auto &state = *static_cast<Fixture *>(arg);
        if (++state.order != 2) {
            state.fail("same-tick preexisting callback violated FIFO order");
        }
    }

    static void
    writeAccepted(void *arg, uint64_t tick, int success)
    {
        auto &state = *static_cast<Fixture *>(arg);
        if (!success || tick != 1000 || state.order != 3 ||
            ++state.completions != 1) {
            state.fail("incorrect source-buffer release");
        }
    }

    static void
    writeVisible(void *arg, uint64_t tick, int success)
    {
        auto &state = *static_cast<Fixture *>(arg);
        if (!success || tick != 443000 || ++state.completions != 2 ||
            !state.host.submit(state.host.context, 0, 1, 0, 4096, 0, 0x5a,
                               readCompleted, readVisible, arg)) {
            state.fail("incorrect destination visibility");
        }
    }

    static void
    readCompleted(void *arg, uint64_t tick, int success)
    {
        auto &state = *static_cast<Fixture *>(arg);
        if (!success || tick != 990000 || ++state.completions != 3) {
            state.fail("incorrect assembled Read response");
        }
    }

    static void
    readVisible(void *arg, uint64_t tick, int success)
    {
        auto &state = *static_cast<Fixture *>(arg);
        if (!success || tick != 990000 || ++state.completions != 4) {
            state.fail("incorrect Read destination callback");
        }
    }

    void
    fail(const char *reason)
    {
        host.fail(host.context, reason);
    }
};

} // anonymous namespace

extern "C" int
gem5_pcie_open(const Gem5PcieHost *host, const char *, Gem5PcieClient *client)
{
    if (host->abi != GEM5_PCIE_WORKLOAD_ABI || host->endpoint_count != 2 ||
        host->ticks_per_second != 1000000000000ULL) {
        return 0;
    }
    auto state = std::make_unique<Fixture>(Fixture{*host});
    if (!host->schedule(host->context, 123, Fixture::issueWrite,
                        state.get()) ||
        !host->schedule(host->context, 123, Fixture::checkFifo, state.get()) ||
        host->schedule(host->context, 123, Fixture::checkFifo, state.get())) {
        state->fail("callback capacity was not enforced");
    }
    *client = {
        state.release(),
        [](void *arg) {
            return static_cast<Fixture *>(arg)->completions == 4 ? 1 : 0;
        },
        [](void *arg) { delete static_cast<Fixture *>(arg); },
    };
    return 1;
}
