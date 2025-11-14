#include "catch_amalgamated.hpp"
#include "infra/simulated_network.hpp"
#include "infra/simulated_node.hpp"
#include "test_orchestrator.hpp"
#include "network/protocol.hpp"
#include "network/peer_discovery_manager.hpp"

using namespace unicity;
using namespace unicity::test;
using namespace unicity::protocol;

static protocol::NetworkAddress make_address(const std::string& ip, uint16_t port) {
    protocol::NetworkAddress addr;
    addr.services = NODE_NETWORK;
    addr.port = port;
    // IPv4-mapped IPv6
    for (int i = 0; i < 10; ++i) addr.ip[i] = 0;
    addr.ip[10] = 0xFF; addr.ip[11] = 0xFF;
    int a,b,c,d; sscanf(ip.c_str(), "%d.%d.%d.%d", &a,&b,&c,&d);
    addr.ip[12] = (uint8_t)a; addr.ip[13] = (uint8_t)b; addr.ip[14] = (uint8_t)c; addr.ip[15] = (uint8_t)d;
    return addr;
}

TEST_CASE("Feeler connects and auto-disconnects; no outbound slot consumed", "[network][feeler]") {
    SimulatedNetwork net(3601);
    TestOrchestrator orch(&net);

    SimulatedNode n1(1, &net);
    SimulatedNode n2(2, &net);

    // Seed n2 in n1's new table
    auto addr2 = make_address(n2.GetAddress(), n2.GetPort());
    n1.GetNetworkManager().discovery_manager().Add(addr2);

    size_t outbound_before = n1.GetNetworkManager().outbound_peer_count();

    // Trigger feeler
    n1.GetNetworkManager().attempt_feeler_connection();

    // Process events/time for handshake to complete and feeler to disconnect
    for (int i = 0; i < 20; ++i) orch.AdvanceTime(std::chrono::milliseconds(100));

    CHECK(n1.GetPeerCount() == 0);
    CHECK(n2.GetPeerCount() == 0);

    // Outbound peers should remain unchanged (feelers don't count)
    size_t outbound_after = n1.GetNetworkManager().outbound_peer_count();
    CHECK(outbound_after == outbound_before);
}
