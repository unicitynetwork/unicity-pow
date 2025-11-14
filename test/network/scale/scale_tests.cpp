// Scale tests (ported to test2; skipped by default)

#include "catch_amalgamated.hpp"
#include "infra/simulated_network.hpp"
#include "infra/simulated_node.hpp"
#include <memory>

using namespace unicity;
using namespace unicity::test;

static void SetZeroLatency(SimulatedNetwork& network) {
    SimulatedNetwork::NetworkConditions c; c.latency_min=c.latency_max=std::chrono::milliseconds(0); c.jitter_max=std::chrono::milliseconds(0); network.SetNetworkConditions(c);
}

TEST_CASE("ScaleTest - HundredNodes", "[.][scaletest][network]") {
    SimulatedNetwork network(28001);
    SetZeroLatency(network);
    std::vector<std::unique_ptr<SimulatedNode>> nodes;
    for (int i = 0; i < 100; i++) nodes.push_back(std::make_unique<SimulatedNode>(i+1, &network));
    for (size_t i = 0; i < nodes.size(); i++) {
        for (int j = 0; j < 8; j++) {
            int peer_id = 1 + (rand() % 100);
            if (peer_id != static_cast<int>(i+1)) nodes[i]->ConnectTo(peer_id);
        }
    }
    uint64_t t=5000; network.AdvanceTime(t);
    nodes[0]->MineBlock();
    t+=10000; network.AdvanceTime(t);
    int synced=0; for (const auto& n : nodes) if (n->GetTipHeight()>=1) synced++;
    CHECK(synced > 90);
}

TEST_CASE("ScaleTest - ThousandNodeStressTest", "[.][scaletest]") {
    SimulatedNetwork network(28002);
    std::vector<std::unique_ptr<SimulatedNode>> nodes;
    for (int i = 0; i < 1000; i++) nodes.push_back(std::make_unique<SimulatedNode>(i+1, &network));
    for (size_t i = 0; i < nodes.size(); i++) {
        for (int j = 0; j < 4; j++) {
            int peer_id = 1 + (rand() % 1000);
            if (peer_id != static_cast<int>(i+1)) nodes[i]->ConnectTo(peer_id);
        }
    }
    network.AdvanceTime(10000);
    nodes[0]->MineBlock();
    network.AdvanceTime(30000);
    int synced=0; for (const auto& n : nodes) if (n->GetTipHeight()>=1) synced++;
    CHECK(synced > 800);
}
