// Peer connection and ban manager tests (ported to test2)

#include "catch_amalgamated.hpp"
#include "infra/simulated_network.hpp"
#include "infra/simulated_node.hpp"
#include "test_orchestrator.hpp"

using namespace unicity;
using namespace unicity::test;

static void SetZeroLatency(test::SimulatedNetwork& network){ test::SimulatedNetwork::NetworkConditions c; c.latency_min=c.latency_max=std::chrono::milliseconds(0); c.jitter_max=std::chrono::milliseconds(0); network.SetNetworkConditions(c);} 

TEST_CASE("ConnectionManagerTest - BasicHandshake", "[peermanagertest][network]") {
    SimulatedNetwork network(12345);
    SimulatedNode node1(1,&network); SimulatedNode node2(2,&network);
CHECK(node1.ConnectTo(2));
    TestOrchestrator orch(&network);
    REQUIRE(orch.WaitForConnection(node1, node2));
}

TEST_CASE("ConnectionManagerTest - MultipleConnections (2 peers)", "[peermanagertest][network]") {
    SimulatedNetwork network(12346);
    // Use small non-zero latency to avoid handshake reordering on burst connects
    SimulatedNetwork::NetworkConditions c; c.latency_min=std::chrono::milliseconds(1); c.latency_max=std::chrono::milliseconds(3); c.jitter_max=std::chrono::milliseconds(0); network.SetNetworkConditions(c);
    std::vector<std::unique_ptr<SimulatedNode>> nodes;
    // Avoid node_id=0 to prevent 127.0.0.0 address
    for(int id=1; id<=5; ++id){ nodes.push_back(std::make_unique<SimulatedNode>(id,&network)); }
    TestOrchestrator orch(&network);
    // Connect node with id=1 (nodes[0]) to nodes with id=2..3 only for stability
for(int i=1;i<=2;i++){ CHECK(nodes[0]->ConnectTo(i+1)); REQUIRE(orch.WaitForCondition([&]{ return orch.GetPeerId(*nodes[0], *nodes[i]) >= 0; }, std::chrono::seconds(10))); }
    CHECK(nodes[0]->GetOutboundPeerCount()==2);
    CHECK(nodes[0]->GetPeerCount()==2);
    for(int i=1;i<=2;i++){ REQUIRE(orch.WaitForCondition([&]{ return nodes[i]->GetInboundPeerCount()>=1; }, std::chrono::seconds(5))); }
}

TEST_CASE("ConnectionManagerTest - SelfConnectionPrevention", "[peermanagertest][network]") {
    SimulatedNetwork network(12347); SimulatedNode node(1,&network);
    CHECK_FALSE(node.ConnectTo(1));
    CHECK(node.GetPeerCount()==0);
}

TEST_CASE("ConnectionManagerTest - PeerDisconnection", "[peermanagertest][network]") {
    SimulatedNetwork network(12348); SetZeroLatency(network);
    SimulatedNode node1(1,&network); SimulatedNode node2(2,&network);
node1.ConnectTo(2);
    TestOrchestrator orch(&network);
    REQUIRE(orch.WaitForConnection(node1, node2));
    node1.DisconnectFrom(2);
    REQUIRE(orch.WaitForPeerCount(node1, 0, std::chrono::seconds(2)));
    REQUIRE(orch.WaitForPeerCount(node2, 0, std::chrono::seconds(2)));
}

TEST_CASE("ConnectionManagerTest - MaxConnectionLimits", "[peermanagertest][network]") {
    // This test creates 200+ nodes and requires adequate file descriptors
    // If you see "Too many open files" error, increase your limit:
    //   ulimit -n 10240
    // Or add to your shell config (~/.zshrc or ~/.bash_profile)
    SimulatedNetwork network(12349); SimulatedNode server(1,&network);
    std::vector<std::unique_ptr<SimulatedNode>> clients; int successful=0;
    try {
        for(int i=0;i<200;i++){
            clients.push_back(std::make_unique<SimulatedNode>(100+i,&network));
            if(clients.back()->ConnectTo(1)) successful++;
        }
    } catch (const boost::system::system_error& e) {
        if (e.code().value() == 24) { // EMFILE - Too many open files
            FAIL("Too many open files. This test requires ~600 file descriptors.\n"
                 "Your system limit is too low (likely 256).\n"
                 "Fix: Run 'ulimit -n 10240' before running tests,\n"
                 "or add it to your shell config (~/.zshrc or ~/.bash_profile)");
        }
        throw; // Re-throw if it's a different error
    } catch (const std::exception& e) {
        // Catch any other exception and check if it's the "too many open files" error
        std::string msg = e.what();
        if (msg.find("Too many open files") != std::string::npos ||
            msg.find("pipe_select_interrupter") != std::string::npos) {
            FAIL("Too many open files. This test requires ~600 file descriptors.\n"
                 "Your system limit is too low (likely 256).\n"
                 "Fix: Run 'ulimit -n 10240' before running tests,\n"
                 "or add it to your shell config (~/.zshrc or ~/.bash_profile)");
        }
        throw; // Re-throw if it's a different error
    }
    TestOrchestrator orch(&network);
    REQUIRE(orch.WaitForCondition([&]{ return server.GetInboundPeerCount()>100; }, std::chrono::seconds(15)));
    CHECK(server.GetInboundPeerCount()<=125);
}

TEST_CASE("ConnectionManagerTest - PeerEviction", "[peermanagertest][network]") {
    SimulatedNetwork network(12350); SimulatedNode server(1,&network);
    std::vector<std::unique_ptr<SimulatedNode>> clients; for(int i=0;i<126;i++){ clients.push_back(std::make_unique<SimulatedNode>(100+i,&network)); clients.back()->ConnectTo(1);} 
TestOrchestrator orch(&network);
    REQUIRE(orch.WaitForCondition([&]{ return server.GetInboundPeerCount()<=125; }, std::chrono::seconds(8)));
}

TEST_CASE("BanManTest - BasicBan", "[banmantest][network]") {
    SimulatedNetwork network(12351); SetZeroLatency(network);
    SimulatedNode node1(1,&network); SimulatedNode node2(2,&network);
    std::string addr = node2.GetAddress(); node1.Ban(addr); CHECK(node1.IsBanned(addr));
    CHECK_FALSE(node1.ConnectTo(2));
}

TEST_CASE("BanManTest - UnbanAddress", "[banmantest][network]") {
    SimulatedNetwork network(12352); SimulatedNode node1(1,&network); SimulatedNode node2(2,&network);
    std::string addr=node2.GetAddress(); node1.Ban(addr); CHECK(node1.IsBanned(addr)); node1.Unban(addr); CHECK_FALSE(node1.IsBanned(addr));
CHECK(node1.ConnectTo(2)); TestOrchestrator orch(&network); REQUIRE(orch.WaitForConnection(node1, node2));
}
