// Miner Start/Stop coverage without hashing

#include "catch_amalgamated.hpp"
#include "chain/miner.hpp"
#include "test_chainstate_manager.hpp"
#include "chain/chainparams.hpp"

using namespace unicity;
using namespace unicity::chain;
using unicity::test::TestChainstateManager;

TEST_CASE("CPUMiner - Start/Stop and idempotency", "[miner]") {
    auto params = ChainParams::CreateRegTest();
    TestChainstateManager csm(*params);
    REQUIRE(csm.Initialize(params->GenesisBlock()));

    mining::CPUMiner miner(*params, csm);

    SECTION("Start spawns worker and Stop joins") {
        REQUIRE(miner.Start(/*target_height=*/-1));
        REQUIRE(miner.IsMining());
        // Immediately stop; this exercises join path without requiring a found block
        miner.Stop();
        REQUIRE_FALSE(miner.IsMining());
    }

    SECTION("Double Start prevented and double Stop safe") {
        REQUIRE(miner.Start());
        REQUIRE_FALSE(miner.Start()); // already mining
        miner.Stop();
        miner.Stop(); // idempotent
        REQUIRE_FALSE(miner.IsMining());
    }
}
