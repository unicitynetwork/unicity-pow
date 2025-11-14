// Miner template and regeneration tests using test hooks (no hashing)

#include "catch_amalgamated.hpp"
#include "chain/miner.hpp"
#include "test_chainstate_manager.hpp"
#include "chain/chainparams.hpp"
#include "chain/block.hpp"
#include "chain/validation.hpp"

using namespace unicity;
using namespace unicity::chain;
using unicity::test::TestChainstateManager;

TEST_CASE("CPUMiner - DebugCreateBlockTemplate and DebugShouldRegenerateTemplate", "[miner]") {
    auto params = ChainParams::CreateRegTest();
    TestChainstateManager csm(*params);
    REQUIRE(csm.Initialize(params->GenesisBlock()));

    mining::CPUMiner miner(*params, csm);

    SECTION("Template reflects tip and MTP constraint") {
        auto tmpl1 = miner.DebugCreateBlockTemplate();
        REQUIRE(tmpl1.nHeight == 1);
        REQUIRE(tmpl1.hashPrevBlock == params->GenesisBlock().GetHash());
        REQUIRE(tmpl1.header.nTime > static_cast<uint32_t>(params->GenesisBlock().nTime));

        // Advance chain by one header
        CBlockHeader h; h.nVersion=1; h.hashPrevBlock = params->GenesisBlock().GetHash(); h.minerAddress = uint160(); h.nTime = tmpl1.header.nTime + 120; h.nBits = tmpl1.nBits; h.nNonce = 0; h.hashRandomX.SetNull();
validation::ValidationState st; REQUIRE(csm.ProcessNewBlockHeader(h, st, /*min_pow_checked=*/true));

        auto tmpl2 = miner.DebugCreateBlockTemplate();
        REQUIRE(tmpl2.nHeight == 2);
        REQUIRE(tmpl2.hashPrevBlock == h.GetHash());
    }

    SECTION("InvalidateTemplate triggers one-shot regeneration request") {
        auto tmpl = miner.DebugCreateBlockTemplate();
        // No change → false
        REQUIRE_FALSE(miner.DebugShouldRegenerateTemplate(tmpl.hashPrevBlock));
        // Invalidate → true once
        miner.InvalidateTemplate();
        REQUIRE(miner.DebugShouldRegenerateTemplate(tmpl.hashPrevBlock));
        // Consumed → false again
        REQUIRE_FALSE(miner.DebugShouldRegenerateTemplate(tmpl.hashPrevBlock));
    }
}
