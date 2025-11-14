// DoS: Low-work headers ignored without penalty

#include "catch_amalgamated.hpp"
#include "infra/simulated_network.hpp"
#include "infra/simulated_node.hpp"
#include "infra/node_simulator.hpp"
#include "test_orchestrator.hpp"
#include "network_observer.hpp"
#include "chain/chainparams.hpp"

using namespace unicity;
using namespace unicity::chain;
using namespace unicity::test;

static struct TestSetup {
    TestSetup() { chain::GlobalChainParams::Select(chain::ChainType::REGTEST); }
} test_setup_low_work;

TEST_CASE("DoS: Low-work headers ignored without penalty", "[dos][network]") {
    // Bitcoin Core behavior: Ignore low-work headers without disconnecting
    // Rationale: Could be legitimate (network partition, different view)
    // Protection: Don't store or fully validate, just ignore

    SimulatedNetwork network(789);
    TestOrchestrator orchestrator(&network);
    NetworkObserver observer;
    AutoDumpOnFailure auto_dump(observer);

    SimulatedNode victim(1, &network);
    NodeSimulator attacker(2, &network);

    observer.OnCustomEvent("TEST_START", -1, "Low-work header spam test");

    // Victim builds high-work chain
    observer.OnCustomEvent("PHASE", -1, "Building high-work chain (100 blocks)");
    victim.SetBypassPOWValidation(true);
    for (int i = 0; i < 100; i++) {
        victim.MineBlock();
    }
    auto victim_tip_before = victim.GetTipHash();

    // Attacker builds low-work chain
    observer.OnCustomEvent("PHASE", -1, "Attacker building low-work chain (10 blocks)");
    attacker.SetBypassPOWValidation(true);
    std::vector<uint256> attacker_chain;
    for (int i = 0; i < 10; i++) {
        auto hash = attacker.MineBlockPrivate();
        attacker_chain.push_back(hash);
    }

    // Connect
    attacker.ConnectTo(1);
    REQUIRE(orchestrator.WaitForConnection(victim, attacker));
    // Ensure handshake completes before sending adversarial messages
    for (int i = 0; i < 20; ++i) orchestrator.AdvanceTime(std::chrono::milliseconds(100));

    // Attack: Spam low-work headers
    observer.OnCustomEvent("PHASE", -1, "Spamming low-work headers (20 batches)");
    for (int i = 0; i < 20; i++) {
        attacker.SendLowWorkHeaders(1, attacker_chain);
        observer.OnCustomEvent("ATTACK", 2, "Low-work batch " + std::to_string(i + 1));
        orchestrator.AdvanceTime(std::chrono::milliseconds(100));
    }

    orchestrator.AdvanceTime(std::chrono::seconds(2));

    // Verify Bitcoin Core behavior:
    observer.OnCustomEvent("PHASE", -1, "Verifying Bitcoin Core behavior");

    // 1. Nodes should STAY connected (no disconnect)
    orchestrator.AssertPeerCount(victim, 1);
    observer.OnCustomEvent("VERIFY", -1, "✓ Peer still connected");

    // 2. Victim chain unchanged (low-work ignored)
    orchestrator.AssertHeight(victim, 100);
    observer.OnCustomEvent("VERIFY", -1, "✓ Victim chain unchanged");

    // 3. Peer should NOT be discouraged (not malicious behavior)
    orchestrator.AssertPeerNotDiscouraged(victim, attacker);
    observer.OnCustomEvent("VERIFY", -1, "✓ Peer not discouraged");

    observer.OnCustomEvent("TEST_END", -1, "PASSED - Low-work correctly ignored");
    auto_dump.MarkSuccess();
}
