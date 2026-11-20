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

#ifndef __DEV_PCI_PCIE_WORKLOAD_CLIENT_H__
#define __DEV_PCI_PCIE_WORKLOAD_CLIENT_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GEM5_PCIE_WORKLOAD_ABI 1

typedef void (*Gem5PcieEvent)(void *argument);
typedef void (*Gem5PcieCompletion)(void *argument, uint64_t tick, int success);

/*
 * Single-threaded, one instance per gem5 System. Use the host to schedule
 * all asynchronous work. Callbacks never execute inline. Zero indicates
 * rejection for submit/schedule; the client retains the callback arguments.
 */
struct Gem5PcieHost
{
    uint32_t abi;
    void *context;
    uint32_t endpoint_count;
    uint64_t ticks_per_second;
    uint64_t (*now)(void *context);
    int (*schedule)(void *context, uint64_t when, Gem5PcieEvent callback,
                    void *argument);
    int (*submit)(void *context, uint32_t source, uint32_t destination,
                  uint64_t offset, uint32_t bytes, int write, uint8_t pattern,
                  Gem5PcieCompletion source_done,
                  Gem5PcieCompletion destination_done, void *argument);
    void (*fail)(void *context, const char *reason);
};

struct Gem5PcieClient
{
    void *context;
    /* Return 1 on complete, 0 on pending and -1 on unrecoverable error. */
    int (*status)(void *context);
    void (*destroy)(void *context);
};

/*
 * Symbol supplied by the client shared library. The client may copy the host
 * table and may start scheduling here. Return 1 on successful setup.
 */
typedef int (*Gem5PcieOpen)(const struct Gem5PcieHost *host,
                            const char *config, struct Gem5PcieClient *client);

#ifdef __cplusplus
}
#endif

#endif // __DEV_PCI_PCIE_WORKLOAD_CLIENT_H__
