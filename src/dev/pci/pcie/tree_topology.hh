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

#ifndef __DEV_PCI_PCIE_TREE_TOPOLOGY_HH__
#define __DEV_PCI_PCIE_TREE_TOPOLOGY_HH__

#include <cstddef>
#include <vector>

namespace gem5::pcie
{

/** A physical tree with leaf ordinals followed by switch ordinals. */
class TreeTopology
{
  public:
    struct Port
    {
        size_t node;
        size_t number;
    };

    struct Edge
    {
        Port a;
        Port b;
    };

    struct Direction
    {
        size_t edge;
        bool forward;
        bool operator==(const Direction &) const = default;
    };

    TreeTopology(size_t leaves, size_t switches, std::vector<Edge> edges);
    const Port &receiver(Direction direction) const;
    const Port &sender(Direction direction) const;
    size_t degree(size_t node) const;
    Direction outgoing(size_t node, size_t port) const;
    Direction route(size_t switch_index, size_t destination) const;

  private:
    const size_t leafCount;
    const std::vector<Edge> edges;
    std::vector<std::vector<Direction>> ports;
    std::vector<std::vector<Direction>> routes;
};

} // namespace gem5::pcie

#endif // __DEV_PCI_PCIE_TREE_TOPOLOGY_HH__
