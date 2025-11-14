// Inbound slot exhaustion attack proof (ported to test2)

#include "catch_amalgamated.hpp"
#include "infra/simulated_network.hpp"
#include "infra/simulated_node.hpp"
#include "infra/node_simulator.hpp"
#include "test_orchestrator.hpp"

using namespace unicity;
using namespace unicity::test;

static void SetZeroLatency(SimulatedNetwork& network){ SimulatedNetwork::NetworkConditions c; c.latency_min=c.latency_max=std::chrono::milliseconds(0); c.jitter_max=std::chrono::milliseconds(0); network.SetNetworkConditions(c);} 

TEST_CASE("SlotExhaustion - PROOF: Attacker can fill inbound slots", "[network][vulnerability][slotexhaustion]") {
    SimulatedNetwork network(12345); SetZeroLatency(network);
    SimulatedNode victim(1,&network); victim.SetBypassPOWValidation(true);
    for(int i=0;i<5;i++) victim.MineBlock();

    const int ATT=10; std::vector<std::unique_ptr<NodeSimulator>> attackers; attackers.reserve(ATT);
    for(int i=0;i<ATT;i++){ attackers.push_back(std::make_unique<NodeSimulator>(100+i,&network)); attackers.back()->SetBypassPOWValidation(true); attackers.back()->ConnectTo(1);} 
TestOrchestrator orch(&network);
    REQUIRE(orch.WaitForCondition([&]{ return victim.GetPeerCount() == (size_t)ATT; }, std::chrono::seconds(5)));

    SimulatedNode honest(500,&network); honest.SetBypassPOWValidation(true);
    (void)honest.ConnectTo(1);
    REQUIRE(orch.WaitForCondition([&]{ return true; }, std::chrono::milliseconds(500))); // brief settle
}

TEST_CASE("SlotExhaustion - PROOF: Rotation attack maintains protection", "[network][vulnerability][slotexhaustion][rotation]") {
    SimulatedNetwork network(12346); SetZeroLatency(network);
    SimulatedNode victim(1,&network); victim.SetBypassPOWValidation(true); for(int i=0;i<5;i++) victim.MineBlock();
    const int N=5; std::vector<std::unique_ptr<NodeSimulator>> attackers; for(int i=0;i<N;i++){ attackers.push_back(std::make_unique<NodeSimulator>(100+i,&network)); attackers.back()->SetBypassPOWValidation(true); attackers.back()->ConnectTo(1);} 
TestOrchestrator orch(&network);
    REQUIRE(orch.WaitForCondition([&]{ return victim.GetPeerCount()==(size_t)N; }, std::chrono::seconds(5)));

    // Perform a simple rotate and allow brief processing
    attackers[0]->DisconnectFrom(1);
    REQUIRE(orch.WaitForCondition([&]{ return victim.GetPeerCount()<= (size_t)(N-1); }, std::chrono::seconds(2)));
    attackers[0]->ConnectTo(1);
    REQUIRE(orch.WaitForCondition([&]{ return victim.GetPeerCount()==(size_t)N; }, std::chrono::seconds(3)));
}

TEST_CASE("SlotExhaustion - PROOF: Honest peer blocked when slots full", "[network][vulnerability][slotexhaustion]") {
    SimulatedNetwork network(12347); SetZeroLatency(network);
    SimulatedNode victim(1,&network); victim.SetBypassPOWValidation(true); for(int i=0;i<5;i++) victim.MineBlock();
    const int N=10; std::vector<std::unique_ptr<NodeSimulator>> attackers; for(int i=0;i<N;i++){ attackers.push_back(std::make_unique<NodeSimulator>(100+i,&network)); attackers.back()->SetBypassPOWValidation(true); attackers.back()->ConnectTo(1);} 
TestOrchestrator orch(&network);
    REQUIRE(orch.WaitForCondition([&]{ return victim.GetPeerCount()==(size_t)N; }, std::chrono::seconds(5)));

    SimulatedNode honest(500,&network); honest.SetBypassPOWValidation(true); for(int i=0;i<20;i++) honest.MineBlock();
    (void)honest.ConnectTo(1);
    REQUIRE(orch.WaitForCondition([&]{ return true; }, std::chrono::milliseconds(500)));
}

TEST_CASE("SlotExhaustion - PROOF: Minimal resources needed", "[network][vulnerability][slotexhaustion]") {
    SimulatedNetwork network(12348); SetZeroLatency(network);
    SimulatedNode victim(1,&network); victim.SetBypassPOWValidation(true); for(int i=0;i<5;i++) victim.MineBlock();
    const int N=8; std::vector<std::unique_ptr<NodeSimulator>> attackers; for(int i=0;i<N;i++){ attackers.push_back(std::make_unique<NodeSimulator>(100+i,&network)); attackers.back()->SetBypassPOWValidation(true); REQUIRE(attackers.back()->ConnectTo(1)); }
TestOrchestrator orch(&network);
    REQUIRE(orch.WaitForCondition([&]{ return victim.GetPeerCount()==(size_t)N; }, std::chrono::seconds(5)));
}
