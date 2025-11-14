// DoS: Per-IP limit connections test

#include "catch_amalgamated.hpp"
#include "simulated_network.hpp"
#include "simulated_node.hpp"
#include "test_orchestrator.hpp"
#include "network/peer_lifecycle_manager.hpp"
#include "chain/chainparams.hpp"

using namespace unicity;
using namespace unicity::test;

TEST_CASE("Per-IP limit enforces max inbound connections from same IP", "[dos][network][per-ip]") {
    // Demonstrate vulnerability: many connections from same IP fill slots
    SimulatedNetwork network(2020);
    TestOrchestrator orchestrator(&network);

    const int VICTIM_ID = 100;
    const int BASE_IP_SUFFIX = 10; // 127.0.0.10
    const int NUM_ATTACKERS = 60;

    SimulatedNode victim(VICTIM_ID, &network);

    // Create attackers whose node_id maps to the same IP via (id % 255) == BASE_IP_SUFFIX
    std::vector<std::unique_ptr<SimulatedNode>> attackers;
    attackers.reserve(NUM_ATTACKERS);
    for (int i = 0; i < NUM_ATTACKERS; ++i) {
        int attacker_id = BASE_IP_SUFFIX + 255 * (i + 1); // 10, 265, 520, ... => all map to 127.0.0.10
        attackers.push_back(std::make_unique<SimulatedNode>(attacker_id, &network));
    }

    // Connect all attackers to victim
    for (auto &att : attackers) {
        REQUIRE(att->ConnectTo(VICTIM_ID));
        REQUIRE(orchestrator.WaitForConnection(victim, *att));
    }

    // Allow some processing time
    orchestrator.AdvanceTime(std::chrono::seconds(2));

    // With per-IP limit enforced, inbound peers from the same IP should be capped
    auto &pm = victim.GetNetworkManager().peer_manager();
    auto inbound = pm.get_inbound_peers();
    
    const int PER_IP_LIMIT = 2;
    REQUIRE(static_cast<int>(inbound.size()) <= PER_IP_LIMIT);

    // All inbound peers should report the same IP address (127.0.0.10)
    std::string expected_ip = "127.0.0." + std::to_string(BASE_IP_SUFFIX);
    int same_ip_count = 0;
    for (const auto &p : inbound) {
        REQUIRE(p != nullptr);
        if (p->address() == expected_ip) {
            same_ip_count++;
        }
    }

    // Policy: enforce a small per-IP limit (e.g., 2).
    REQUIRE(same_ip_count <= PER_IP_LIMIT);
}
