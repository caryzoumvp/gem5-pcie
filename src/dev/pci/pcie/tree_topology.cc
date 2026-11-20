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

#include "dev/pci/pcie/tree_topology.hh"

#include <stdexcept>
#include <utility>

namespace gem5::pcie
{

TreeTopology::TreeTopology(size_t leaves, size_t switches,
                           std::vector<Edge> edges)
    : leafCount(leaves),
      edges(std::move(edges)),
      ports(leaves + switches),
      routes(switches, std::vector<Direction>(leaves))
{
    if (leaves < 2 || !switches || this->edges.size() != ports.size() - 1) {
        throw std::invalid_argument("invalid PCIe tree node/edge count");
    }
    std::vector<size_t> degrees(ports.size());
    for (const auto &edge : this->edges) {
        if (edge.a.node >= ports.size() || edge.b.node >= ports.size() ||
            edge.a.node == edge.b.node) {
            throw std::invalid_argument("invalid PCIe tree node");
        }
        ++degrees[edge.a.node];
        ++degrees[edge.b.node];
    }
    const Direction empty{this->edges.size(), false};
    for (size_t node = 0; node < ports.size(); ++node) {
        if (node < leafCount ? degrees[node] != 1 : degrees[node] < 2) {
            throw std::invalid_argument("invalid PCIe tree node degree");
        }
        ports[node].assign(degrees[node], empty);
    }
    for (size_t index = 0; index < this->edges.size(); ++index) {
        for (bool forward : {true, false}) {
            const Direction wire{index, forward};
            const auto &port = sender(wire);
            auto &node = ports[port.node];
            if (port.number >= node.size() || node[port.number] != empty) {
                throw std::invalid_argument("invalid or reused PCIe port");
            }
            node[port.number] = wire;
        }
    }

    for (size_t leaf = 0; leaf < leafCount; ++leaf) {
        std::vector<bool> visited(ports.size());
        std::vector<size_t> pending{leaf};
        visited[leaf] = true;
        for (size_t next = 0; next < pending.size(); ++next) {
            for (const auto wire : ports[pending[next]]) {
                const auto adjacent = receiver(wire).node;
                if (visited[adjacent]) {
                    continue;
                }
                visited[adjacent] = true;
                pending.push_back(adjacent);
                if (adjacent >= leafCount) {
                    routes[adjacent - leafCount][leaf] = {wire.edge,
                                                          !wire.forward};
                }
            }
        }
        if (pending.size() != ports.size()) {
            throw std::invalid_argument("disconnected PCIe tree");
        }
    }
}

const TreeTopology::Port &
TreeTopology::receiver(Direction wire) const
{
    const auto &edge = edges.at(wire.edge);
    return wire.forward ? edge.b : edge.a;
}

const TreeTopology::Port &
TreeTopology::sender(Direction wire) const
{
    return receiver({wire.edge, !wire.forward});
}

size_t
TreeTopology::degree(size_t node) const
{
    return ports.at(node).size();
}

TreeTopology::Direction
TreeTopology::outgoing(size_t node, size_t port) const
{
    return ports.at(node).at(port);
}

TreeTopology::Direction
TreeTopology::route(size_t switch_index, size_t destination) const
{
    return routes.at(switch_index).at(destination);
}

} // namespace gem5::pcie
