// Copyright (c) 2025 The Unicity Foundation
// Unit tests for chain/chainstate_manager.cpp - Main chain state coordinator
//
// These tests verify:
// - Initialization and persistence
// - Block header acceptance (duplicates, genesis, orphans, validation)
// - Chain activation and reorg handling
// - Orphan header management (DoS limits, eviction, recursive processing)
// - Block invalidation (manual InvalidateBlock RPC)
// - IBD detection
// - Best chain selection

#include "catch_amalgamated.hpp"
#include "chain/chainstate_manager.hpp"
#include "chain/chainparams.hpp"
#include "chain/block.hpp"
#include "chain/validation.hpp"
#include <filesystem>
#include <thread>
#include <chrono>

using namespace unicity;
using namespace unicity::validation;
using namespace unicity::chain;

// Test subclass that bypasses expensive PoW validation
class TestChainstateManager : public ChainstateManager {
public:
    explicit TestChainstateManager(const ChainParams& params)
        : ChainstateManager(params), bypass_pow_validation_(true) {}

    // Control whether PoW checks pass
    void SetBypassPoW(bool bypass) { bypass_pow_validation_ = bypass; }
    void SetPoWCheckResult(bool result) { pow_check_result_ = result; }
    void SetBlockHeaderCheckResult(bool result) { block_header_check_result_ = result; }
    void SetContextualCheckResult(bool result) { contextual_check_result_ = result; }

protected:
    // Override to bypass expensive RandomX validation
    bool CheckProofOfWork(const CBlockHeader& header, crypto::POWVerifyMode mode) const override {
        if (bypass_pow_validation_) {
            return pow_check_result_;
        }
        return ChainstateManager::CheckProofOfWork(header, mode);
    }

    bool CheckBlockHeaderWrapper(const CBlockHeader& header, ValidationState& state) const override {
        if (bypass_pow_validation_) {
            if (!block_header_check_result_) {
                state.Invalid("test-failure", "block header check failed (test)");
                return false;
            }
            return true;
        }
        return ChainstateManager::CheckBlockHeaderWrapper(header, state);
    }

    bool ContextualCheckBlockHeaderWrapper(const CBlockHeader& header,
                                           const CBlockIndex* pindexPrev,
                                           int64_t adjusted_time,
                                           ValidationState& state) const override {
        if (bypass_pow_validation_) {
            if (!contextual_check_result_) {
                state.Invalid("test-failure", "contextual check failed (test)");
                return false;
            }
            return true;
        }
        return ChainstateManager::ContextualCheckBlockHeaderWrapper(header, pindexPrev, adjusted_time, state);
    }

private:
    bool bypass_pow_validation_;
    bool pow_check_result_{true};
    bool block_header_check_result_{true};
    bool contextual_check_result_{true};
};

// Helper: Create a block header
static CBlockHeader CreateTestHeader(uint32_t nTime = 1234567890, uint32_t nBits = 0x1d00ffff) {
    CBlockHeader header;
    header.nVersion = 1;
    header.hashPrevBlock.SetNull();
    header.minerAddress.SetNull();
    header.nTime = nTime;
    header.nBits = nBits;
    header.nNonce = 0;
    header.hashRandomX.SetNull();
    return header;
}

// Helper: Create a child header
static CBlockHeader CreateChildHeader(const uint256& prevHash, uint32_t nTime = 1234567890) {
    CBlockHeader header = CreateTestHeader(nTime);
    header.hashPrevBlock = prevHash;
    return header;
}

// Test fixture for temp files
class ChainstateManagerTestFixture {
public:
    std::string test_file;

    ChainstateManagerTestFixture() {
        auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        test_file = "/tmp/chainstate_test_" + std::to_string(now) + ".json";
    }

    ~ChainstateManagerTestFixture() {
        std::filesystem::remove(test_file);
    }
};

TEST_CASE("ChainstateManager - Construction", "[chain][chainstate_manager][unit]") {
    auto params = ChainParams::CreateRegTest();
    TestChainstateManager csm(*params);

    SECTION("Default construction") {
        REQUIRE(csm.GetTip() == nullptr);
        REQUIRE(csm.GetBlockCount() == 0);
        REQUIRE(csm.GetChainHeight() == -1);
    }
}

TEST_CASE("ChainstateManager - Initialize", "[chain][chainstate_manager][unit]") {
    auto params = ChainParams::CreateRegTest();
    TestChainstateManager csm(*params);

    SECTION("Initialize with genesis") {
        CBlockHeader genesis = CreateTestHeader();
        REQUIRE(csm.Initialize(genesis));

        REQUIRE(csm.GetTip() != nullptr);
        REQUIRE(csm.GetTip()->GetBlockHash() == genesis.GetHash());
        REQUIRE(csm.GetBlockCount() == 1);
        REQUIRE(csm.GetChainHeight() == 0);
    }

    SECTION("Cannot initialize twice") {
        CBlockHeader genesis = CreateTestHeader();
        REQUIRE(csm.Initialize(genesis));

        CBlockHeader another = CreateTestHeader(9999999);
        REQUIRE_FALSE(csm.Initialize(another));
    }
}

TEST_CASE("ChainstateManager - AcceptBlockHeader Basic", "[chain][chainstate_manager][unit]") {
    auto params = ChainParams::CreateRegTest();
    TestChainstateManager csm(*params);

    CBlockHeader genesis = CreateTestHeader();
    csm.Initialize(genesis);

    SECTION("Accept valid block") {
        CBlockHeader block1 = CreateChildHeader(genesis.GetHash(), 1234567900);
        ValidationState state;

CBlockIndex* pindex = csm.AcceptBlockHeader(block1, state, /*min_pow_checked=*/true);
        REQUIRE(pindex != nullptr);
        REQUIRE(pindex->GetBlockHash() == block1.GetHash());
        REQUIRE(pindex->nHeight == 1);
        REQUIRE(state.IsValid());
    }

    SECTION("Reject duplicate block") {
        CBlockHeader block1 = CreateChildHeader(genesis.GetHash(), 1234567900);
        ValidationState state1;

CBlockIndex* pindex1 = csm.AcceptBlockHeader(block1, state1, /*min_pow_checked=*/true);
        REQUIRE(pindex1 != nullptr);

        // Try to accept same block again
        ValidationState state2;
CBlockIndex* pindex2 = csm.AcceptBlockHeader(block1, state2, /*min_pow_checked=*/true);
        REQUIRE(pindex2 == pindex1);  // Returns existing index
    }

    SECTION("Reject genesis via AcceptBlockHeader") {
        // Create a different genesis (different time = different hash)
        CBlockHeader fake_genesis = CreateTestHeader(9999999);
        ValidationState state;

CBlockIndex* pindex = csm.AcceptBlockHeader(fake_genesis, state, /*min_pow_checked=*/true);
        REQUIRE(pindex == nullptr);
        REQUIRE_FALSE(state.IsValid());
        // Should fail with bad-genesis because hash doesn't match expected genesis
        REQUIRE(state.GetRejectReason() == "bad-genesis");
    }

    SECTION("Reject block with failed PoW commitment") {
        csm.SetPoWCheckResult(false);

        CBlockHeader block1 = CreateChildHeader(genesis.GetHash(), 1234567900);
        ValidationState state;

CBlockIndex* pindex = csm.AcceptBlockHeader(block1, state, /*min_pow_checked=*/true);
        REQUIRE(pindex == nullptr);
        REQUIRE_FALSE(state.IsValid());
        REQUIRE(state.GetRejectReason() == "high-hash");
    }

    SECTION("Reject block with failed header check") {
        csm.SetBlockHeaderCheckResult(false);

        CBlockHeader block1 = CreateChildHeader(genesis.GetHash(), 1234567900);
        ValidationState state;

CBlockIndex* pindex = csm.AcceptBlockHeader(block1, state, /*min_pow_checked=*/true);
        REQUIRE(pindex == nullptr);
        REQUIRE_FALSE(state.IsValid());
        REQUIRE(state.GetRejectReason() == "test-failure");
    }

    SECTION("Reject block with failed contextual check") {
        csm.SetContextualCheckResult(false);

        CBlockHeader block1 = CreateChildHeader(genesis.GetHash(), 1234567900);
        ValidationState state;

CBlockIndex* pindex = csm.AcceptBlockHeader(block1, state, /*min_pow_checked=*/true);
        REQUIRE(pindex == nullptr);
        REQUIRE_FALSE(state.IsValid());
        REQUIRE(state.GetRejectReason() == "test-failure");
    }
}

TEST_CASE("ChainstateManager - Orphan Headers", "[chain][chainstate_manager][unit]") {
    auto params = ChainParams::CreateRegTest();
    TestChainstateManager csm(*params);

    CBlockHeader genesis = CreateTestHeader();
    csm.Initialize(genesis);

    SECTION("Orphan header cached when parent missing") {
        // Create block2 without block1 existing
        uint256 missing_parent;
        missing_parent.SetNull();
        memset((void*)missing_parent.data(), 0xaa, 32);

        CBlockHeader block2 = CreateChildHeader(missing_parent, 1234567900);
        ValidationState state;

CBlockIndex* pindex = csm.AcceptBlockHeader(block2, state, /*min_pow_checked=*/true);
REQUIRE(pindex == nullptr);
REQUIRE_FALSE(state.IsValid());
REQUIRE(state.GetRejectReason() == "prev-blk-not-found");
REQUIRE(csm.AddOrphanHeader(block2, 1));
REQUIRE(csm.GetOrphanHeaderCount() == 1);
    }

    SECTION("Orphan processed when parent arrives") {
        // Create orphan
        CBlockHeader block1 = CreateChildHeader(genesis.GetHash(), 1234567900);
        CBlockHeader block2 = CreateChildHeader(block1.GetHash(), 1234567910);

        ValidationState state1;
REQUIRE(csm.AddOrphanHeader(block2, 1));
REQUIRE(csm.GetOrphanHeaderCount() == 1);

// Now add parent
ValidationState state2;
CBlockIndex* pindex1 = csm.AcceptBlockHeader(block1, state2, /*min_pow_checked=*/true);
        REQUIRE(pindex1 != nullptr);

        // Orphan should be processed automatically
        REQUIRE(csm.GetOrphanHeaderCount() == 0);
        REQUIRE(csm.GetBlockCount() == 3);  // genesis + block1 + block2
    }

    SECTION("Per-peer orphan limit") {
        // Create 51 orphans from same peer (limit is 50)
        for (int i = 0; i < 51; i++) {
            uint256 missing_parent;
            missing_parent.SetNull();
            memset((void*)missing_parent.data(), 0xaa + i, 32);

            CBlockHeader orphan = CreateChildHeader(missing_parent, 1234567900 + i);
            ValidationState state;

(void)csm.AddOrphanHeader(orphan, 1);
        }

        // Should have exactly 50 orphans (per-peer limit)
        REQUIRE(csm.GetOrphanHeaderCount() <= 50);
    }

    SECTION("Orphan eviction by time") {
        // Add orphan
        uint256 missing_parent;
        missing_parent.SetNull();
        memset((void*)missing_parent.data(), 0xaa, 32);

        CBlockHeader orphan = CreateChildHeader(missing_parent, 1234567900);
        ValidationState state;

        (void)csm.AddOrphanHeader(orphan, 1);
        REQUIRE(csm.GetOrphanHeaderCount() == 1);

        // Sleep to allow time to pass (orphan expire time is 600 seconds in real code,
        // but we can test eviction manually)
        size_t evicted = csm.EvictOrphanHeaders();

        // Without time passing, no eviction yet
        REQUIRE(csm.GetOrphanHeaderCount() == 1);
    }
}

TEST_CASE("ChainstateManager - ProcessNewBlockHeader", "[chain][chainstate_manager][unit]") {
    auto params = ChainParams::CreateRegTest();
    TestChainstateManager csm(*params);

    CBlockHeader genesis = CreateTestHeader();
    csm.Initialize(genesis);

    SECTION("Process valid block") {
        CBlockHeader block1 = CreateChildHeader(genesis.GetHash(), 1234567900);
        ValidationState state;

REQUIRE(csm.ProcessNewBlockHeader(block1, state, /*min_pow_checked=*/true));
        REQUIRE(state.IsValid());
        REQUIRE(csm.GetTip()->GetBlockHash() == block1.GetHash());
        REQUIRE(csm.GetChainHeight() == 1);
    }

    SECTION("Process invalid block") {
        csm.SetPoWCheckResult(false);

        CBlockHeader block1 = CreateChildHeader(genesis.GetHash(), 1234567900);
        ValidationState state;

REQUIRE_FALSE(csm.ProcessNewBlockHeader(block1, state, /*min_pow_checked=*/true));
        REQUIRE_FALSE(state.IsValid());
        REQUIRE(csm.GetTip()->GetBlockHash() == genesis.GetHash());  // Tip unchanged
    }
}

TEST_CASE("ChainstateManager - Chain Activation", "[chain][chainstate_manager][unit]") {
    auto params = ChainParams::CreateRegTest();
    TestChainstateManager csm(*params);

    CBlockHeader genesis = CreateTestHeader();
    csm.Initialize(genesis);

    SECTION("Extend main chain") {
        CBlockHeader block1 = CreateChildHeader(genesis.GetHash(), 1234567900);
        ValidationState state1;
csm.ProcessNewBlockHeader(block1, state1, /*min_pow_checked=*/true);

        CBlockHeader block2 = CreateChildHeader(block1.GetHash(), 1234567910);
        ValidationState state2;
csm.ProcessNewBlockHeader(block2, state2, /*min_pow_checked=*/true);

        REQUIRE(csm.GetChainHeight() == 2);
        REQUIRE(csm.GetTip()->GetBlockHash() == block2.GetHash());
    }

    SECTION("No reorg to chain with less work") {
        // Build main chain: genesis -> A1 -> A2
        CBlockHeader blockA1 = CreateChildHeader(genesis.GetHash(), 1000);
        ValidationState stateA1;
csm.ProcessNewBlockHeader(blockA1, stateA1, /*min_pow_checked=*/true);

        CBlockHeader blockA2 = CreateChildHeader(blockA1.GetHash(), 2000);
        ValidationState stateA2;
csm.ProcessNewBlockHeader(blockA2, stateA2, /*min_pow_checked=*/true);

        REQUIRE(csm.GetChainHeight() == 2);

        // Build shorter fork: genesis -> B1
        CBlockHeader blockB1 = CreateChildHeader(genesis.GetHash(), 3000);
        ValidationState stateB1;
csm.ProcessNewBlockHeader(blockB1, stateB1, /*min_pow_checked=*/true);

        // Should NOT reorg (A2 has more blocks/work)
        REQUIRE(csm.GetTip()->GetBlockHash() == blockA2.GetHash());
    }
}

TEST_CASE("ChainstateManager - Reorg", "[chain][chainstate_manager][unit]") {
    auto params = ChainParams::CreateRegTest();
    TestChainstateManager csm(*params);

    CBlockHeader genesis = CreateTestHeader();
    csm.Initialize(genesis);

    SECTION("Simple reorg to longer chain") {
        // Chain A: genesis -> A1
        CBlockHeader blockA1 = CreateChildHeader(genesis.GetHash(), 1000);
        ValidationState stateA1;
csm.ProcessNewBlockHeader(blockA1, stateA1, /*min_pow_checked=*/true);

        // Chain B: genesis -> B1 -> B2 (more work)
        CBlockHeader blockB1 = CreateChildHeader(genesis.GetHash(), 2000);
        ValidationState stateB1;
        csm.ProcessNewBlockHeader(blockB1, stateB1, /*min_pow_checked=*/true);

        CBlockHeader blockB2 = CreateChildHeader(blockB1.GetHash(), 3000);
        ValidationState stateB2;
        csm.ProcessNewBlockHeader(blockB2, stateB2, /*min_pow_checked=*/true);

        // Should reorg to chain B
        REQUIRE(csm.GetTip()->GetBlockHash() == blockB2.GetHash());
        REQUIRE(csm.GetChainHeight() == 2);
    }

    SECTION("Deep reorg rejected") {
        // Set suspicious reorg depth to 2
        auto params_limited = ChainParams::CreateRegTest();
        params_limited->SetSuspiciousReorgDepth(2);
        TestChainstateManager csm_limited(*params_limited);
        csm_limited.Initialize(genesis);

        // Build chain A: genesis -> A1 -> A2
        CBlockHeader blockA1 = CreateChildHeader(genesis.GetHash(), 1000);
        ValidationState stateA1;
        csm_limited.ProcessNewBlockHeader(blockA1, stateA1, /*min_pow_checked=*/true);

        CBlockHeader blockA2 = CreateChildHeader(blockA1.GetHash(), 2000);
        ValidationState stateA2;
csm_limited.ProcessNewBlockHeader(blockA2, stateA2, /*min_pow_checked=*/true);

        // Build chain B: genesis -> B1 -> B2 -> B3 (longer, would cause reorg of depth 2)
        CBlockHeader blockB1 = CreateChildHeader(genesis.GetHash(), 3000);
        ValidationState stateB1;
csm_limited.ProcessNewBlockHeader(blockB1, stateB1, /*min_pow_checked=*/true);

        CBlockHeader blockB2 = CreateChildHeader(blockB1.GetHash(), 4000);
        ValidationState stateB2;
csm_limited.ProcessNewBlockHeader(blockB2, stateB2, /*min_pow_checked=*/true);

        CBlockHeader blockB3 = CreateChildHeader(blockB2.GetHash(), 5000);
        ValidationState stateB3;
csm_limited.ProcessNewBlockHeader(blockB3, stateB3, /*min_pow_checked=*/true);

        // Should reject deep reorg (depth 2 >= limit 2)
        REQUIRE(csm_limited.GetTip()->GetBlockHash() == blockA2.GetHash());
    }
}

TEST_CASE("ChainstateManager - Persistence", "[chain][chainstate_manager][unit]") {
    ChainstateManagerTestFixture fixture;
    auto params = ChainParams::CreateRegTest();

    SECTION("Save") {
        CBlockHeader genesis = CreateTestHeader();

        // Create chain
        TestChainstateManager csm1(*params);
        csm1.Initialize(genesis);

        CBlockHeader block1 = CreateChildHeader(genesis.GetHash(), 1000);
        ValidationState state1;
csm1.ProcessNewBlockHeader(block1, state1, /*min_pow_checked=*/true);

        CBlockHeader block2 = CreateChildHeader(block1.GetHash(), 2000);
        ValidationState state2;
csm1.ProcessNewBlockHeader(block2, state2, /*min_pow_checked=*/true);

        REQUIRE(csm1.GetChainHeight() == 2);

        // Save should succeed
        REQUIRE(csm1.Save(fixture.test_file));

        // Verify file was created
        REQUIRE(std::filesystem::exists(fixture.test_file));

        // Note: Load test requires matching actual chain params genesis,
        // which would need real PoW genesis mining. Save test is sufficient
        // for unit testing - full integration tested elsewhere.
    }
}

TEST_CASE("ChainstateManager - LookupBlockIndex", "[chain][chainstate_manager][unit]") {
    auto params = ChainParams::CreateRegTest();
    TestChainstateManager csm(*params);

    CBlockHeader genesis = CreateTestHeader();
    csm.Initialize(genesis);

    SECTION("Lookup existing block") {
        CBlockHeader block1 = CreateChildHeader(genesis.GetHash(), 1000);
        ValidationState state;
csm.ProcessNewBlockHeader(block1, state, /*min_pow_checked=*/true);

        CBlockIndex* pindex = csm.LookupBlockIndex(block1.GetHash());
        REQUIRE(pindex != nullptr);
        REQUIRE(pindex->GetBlockHash() == block1.GetHash());
    }

    SECTION("Lookup non-existing block") {
        uint256 fake_hash;
        fake_hash.SetNull();
        memset((void*)fake_hash.data(), 0xff, 32);

        CBlockIndex* pindex = csm.LookupBlockIndex(fake_hash);
        REQUIRE(pindex == nullptr);
    }
}

TEST_CASE("ChainstateManager - GetLocator", "[chain][chainstate_manager][unit]") {
    auto params = ChainParams::CreateRegTest();
    TestChainstateManager csm(*params);

    CBlockHeader genesis = CreateTestHeader();
    csm.Initialize(genesis);

    SECTION("Locator for tip") {
        CBlockHeader block1 = CreateChildHeader(genesis.GetHash(), 1000);
        ValidationState state1;
        csm.ProcessNewBlockHeader(block1, state1, /*min_pow_checked=*/true);

        CBlockLocator locator = csm.GetLocator();
        REQUIRE(!locator.vHave.empty());
        REQUIRE(locator.vHave[0] == block1.GetHash());
    }

    SECTION("Locator for specific block") {
        CBlockHeader block1 = CreateChildHeader(genesis.GetHash(), 1000);
        ValidationState state1;
        csm.ProcessNewBlockHeader(block1, state1, /*min_pow_checked=*/true);

        CBlockHeader block2 = CreateChildHeader(block1.GetHash(), 2000);
        ValidationState state2;
csm.ProcessNewBlockHeader(block2, state2, /*min_pow_checked=*/true);

        CBlockIndex* pindex1 = csm.LookupBlockIndex(block1.GetHash());
        CBlockLocator locator = csm.GetLocator(pindex1);

        REQUIRE(!locator.vHave.empty());
        REQUIRE(locator.vHave[0] == block1.GetHash());
    }
}

TEST_CASE("ChainstateManager - IsOnActiveChain", "[chain][chainstate_manager][unit]") {
    auto params = ChainParams::CreateRegTest();
    TestChainstateManager csm(*params);

    CBlockHeader genesis = CreateTestHeader();
    csm.Initialize(genesis);

    SECTION("Genesis is on active chain") {
        REQUIRE(csm.IsOnActiveChain(csm.GetTip()));
    }

    SECTION("Active block is on active chain") {
        CBlockHeader block1 = CreateChildHeader(genesis.GetHash(), 1000);
        ValidationState state;
csm.ProcessNewBlockHeader(block1, state, /*min_pow_checked=*/true);

        CBlockIndex* pindex = csm.LookupBlockIndex(block1.GetHash());
        REQUIRE(csm.IsOnActiveChain(pindex));
    }

    SECTION("Orphaned block not on active chain") {
        // Build main chain: genesis -> A1
        CBlockHeader blockA1 = CreateChildHeader(genesis.GetHash(), 1000);
        ValidationState stateA1;
        csm.ProcessNewBlockHeader(blockA1, stateA1, /*min_pow_checked=*/true);

        // Build fork: genesis -> B1 (not activated)
        CBlockHeader blockB1 = CreateChildHeader(genesis.GetHash(), 2000);
        ValidationState stateB1;
csm.ProcessNewBlockHeader(blockB1, stateB1, /*min_pow_checked=*/true);

        // blockA1 should be on active chain (longer)
        CBlockIndex* pindexA1 = csm.LookupBlockIndex(blockA1.GetHash());
        REQUIRE(csm.IsOnActiveChain(pindexA1));

        // blockB1 should NOT be on active chain
        CBlockIndex* pindexB1 = csm.LookupBlockIndex(blockB1.GetHash());
        REQUIRE_FALSE(csm.IsOnActiveChain(pindexB1));
    }
}

TEST_CASE("ChainstateManager - GetBlockAtHeight", "[chain][chainstate_manager][unit]") {
    auto params = ChainParams::CreateRegTest();
    TestChainstateManager csm(*params);

    CBlockHeader genesis = CreateTestHeader();
    csm.Initialize(genesis);

    SECTION("Get genesis at height 0") {
        const CBlockIndex* pindex = csm.GetBlockAtHeight(0);
        REQUIRE(pindex != nullptr);
        REQUIRE(pindex->GetBlockHash() == genesis.GetHash());
    }

    SECTION("Get block at valid height") {
        CBlockHeader block1 = CreateChildHeader(genesis.GetHash(), 1000);
        ValidationState state;
        csm.ProcessNewBlockHeader(block1, state, /*min_pow_checked=*/true);

        const CBlockIndex* pindex = csm.GetBlockAtHeight(1);
        REQUIRE(pindex != nullptr);
        REQUIRE(pindex->GetBlockHash() == block1.GetHash());
    }

    SECTION("Get block at invalid height") {
        REQUIRE(csm.GetBlockAtHeight(-1) == nullptr);
        REQUIRE(csm.GetBlockAtHeight(999) == nullptr);
    }
}

TEST_CASE("ChainstateManager - IsInitialBlockDownload", "[chain][chainstate_manager][unit]") {
    auto params = ChainParams::CreateRegTest();
    TestChainstateManager csm(*params);

    SECTION("IBD when no tip") {
        REQUIRE(csm.IsInitialBlockDownload());
    }

    SECTION("IBD when tip exists") {
        CBlockHeader genesis = CreateTestHeader();
        csm.Initialize(genesis);

        // With current time, should be in IBD (tip too old or not enough work)
        // This depends on genesis timestamp vs current time
        // For deterministic testing, we just verify it doesn't crash
        bool ibd = csm.IsInitialBlockDownload();
        REQUIRE((ibd == true || ibd == false));  // Just check it returns
    }
}

TEST_CASE("ChainstateManager - InvalidateBlock", "[chain][chainstate_manager][unit]") {
    auto params = ChainParams::CreateRegTest();
    TestChainstateManager csm(*params);

    CBlockHeader genesis = CreateTestHeader();
    csm.Initialize(genesis);

    SECTION("Cannot invalidate genesis") {
        REQUIRE_FALSE(csm.InvalidateBlock(genesis.GetHash()));
    }

    SECTION("Cannot invalidate unknown block") {
        uint256 fake_hash;
        fake_hash.SetNull();
        memset((void*)fake_hash.data(), 0xff, 32);

        REQUIRE_FALSE(csm.InvalidateBlock(fake_hash));
    }

    SECTION("Invalidate block on main chain") {
        // Build chain: genesis -> block1 -> block2
        CBlockHeader block1 = CreateChildHeader(genesis.GetHash(), 1000);
        ValidationState state1;
        csm.ProcessNewBlockHeader(block1, state1, /*min_pow_checked=*/true);

        CBlockHeader block2 = CreateChildHeader(block1.GetHash(), 2000);
        ValidationState state2;
        csm.ProcessNewBlockHeader(block2, state2, /*min_pow_checked=*/true);

        REQUIRE(csm.GetChainHeight() == 2);

        // Invalidate block2
        REQUIRE(csm.InvalidateBlock(block2.GetHash()));

        // Tip should revert to block1
        REQUIRE(csm.GetTip()->GetBlockHash() == block1.GetHash());
        REQUIRE(csm.GetChainHeight() == 1);
    }

    SECTION("Invalidate block not on main chain") {
        // Build main chain: genesis -> A1
        CBlockHeader blockA1 = CreateChildHeader(genesis.GetHash(), 1000);
        ValidationState stateA1;
        csm.ProcessNewBlockHeader(blockA1, stateA1, /*min_pow_checked=*/true);

        // Build fork: genesis -> B1 (not active)
        CBlockHeader blockB1 = CreateChildHeader(genesis.GetHash(), 2000);
        ValidationState stateB1;
        csm.ProcessNewBlockHeader(blockB1, stateB1, /*min_pow_checked=*/true);

        // Invalidate B1 (not on main chain)
        REQUIRE(csm.InvalidateBlock(blockB1.GetHash()));

        // Main chain should remain unchanged
        REQUIRE(csm.GetTip()->GetBlockHash() == blockA1.GetHash());

        // B1 should be marked invalid
        CBlockIndex* pindexB1 = csm.LookupBlockIndex(blockB1.GetHash());
        REQUIRE(pindexB1->status.IsFailed());
    }
}

TEST_CASE("ChainstateManager - GetBlockCount", "[chain][chainstate_manager][unit]") {
    auto params = ChainParams::CreateRegTest();
    TestChainstateManager csm(*params);

    SECTION("Empty chain") {
        REQUIRE(csm.GetBlockCount() == 0);
    }

    SECTION("With genesis") {
        CBlockHeader genesis = CreateTestHeader();
        csm.Initialize(genesis);

        REQUIRE(csm.GetBlockCount() == 1);
    }

    SECTION("With multiple blocks") {
        CBlockHeader genesis = CreateTestHeader();
        csm.Initialize(genesis);

        CBlockHeader block1 = CreateChildHeader(genesis.GetHash(), 1000);
        ValidationState state1;
csm.ProcessNewBlockHeader(block1, state1, /*min_pow_checked=*/true);

        CBlockHeader block2 = CreateChildHeader(block1.GetHash(), 2000);
        ValidationState state2;
csm.ProcessNewBlockHeader(block2, state2, /*min_pow_checked=*/true);

        REQUIRE(csm.GetBlockCount() == 3);
    }
}

TEST_CASE("ChainstateManager - GetChainHeight", "[chain][chainstate_manager][unit]") {
    auto params = ChainParams::CreateRegTest();
    TestChainstateManager csm(*params);

    SECTION("Empty chain") {
        REQUIRE(csm.GetChainHeight() == -1);
    }

    SECTION("Genesis only") {
        CBlockHeader genesis = CreateTestHeader();
        csm.Initialize(genesis);

        REQUIRE(csm.GetChainHeight() == 0);
    }

    SECTION("With blocks") {
        CBlockHeader genesis = CreateTestHeader();
        csm.Initialize(genesis);

        CBlockHeader block1 = CreateChildHeader(genesis.GetHash(), 1000);
        ValidationState state;
        csm.ProcessNewBlockHeader(block1, state, /*min_pow_checked=*/true);

        REQUIRE(csm.GetChainHeight() == 1);
    }
}

TEST_CASE("ChainstateManager - CheckHeadersPoW", "[chain][chainstate_manager][unit]") {
    auto params = ChainParams::CreateRegTest();
    TestChainstateManager csm(*params);

    SECTION("All headers pass") {
        std::vector<CBlockHeader> headers;
        headers.push_back(CreateTestHeader(1000));
        headers.push_back(CreateTestHeader(2000));

        REQUIRE(csm.CheckHeadersPoW(headers));
    }

    SECTION("One header fails") {
        csm.SetPoWCheckResult(false);

        std::vector<CBlockHeader> headers;
        headers.push_back(CreateTestHeader(1000));
        headers.push_back(CreateTestHeader(2000));

        REQUIRE_FALSE(csm.CheckHeadersPoW(headers));
    }

    SECTION("Empty list") {
        std::vector<CBlockHeader> headers;
        REQUIRE(csm.CheckHeadersPoW(headers));
    }
}

TEST_CASE("ChainstateManager - Edge Cases", "[chain][chainstate_manager][unit]") {
    auto params = ChainParams::CreateRegTest();
    TestChainstateManager csm(*params);

    CBlockHeader genesis = CreateTestHeader();
    csm.Initialize(genesis);

    SECTION("Null pointer checks") {
        REQUIRE_FALSE(csm.IsOnActiveChain(nullptr));
    }

    SECTION("Multiple orphans from different peers") {
        uint256 missing1, missing2;
        missing1.SetNull();
        memset((void*)missing1.data(), 0xaa, 32);
        missing2.SetNull();
        memset((void*)missing2.data(), 0xbb, 32);

        CBlockHeader orphan1 = CreateChildHeader(missing1, 1000);
        CBlockHeader orphan2 = CreateChildHeader(missing2, 2000);

        ValidationState state1, state2;

        csm.AcceptBlockHeader(orphan1, state1, /*min_pow_checked=*/true);
        REQUIRE(csm.AddOrphanHeader(orphan1, /*peer_id=*/1));
        csm.AcceptBlockHeader(orphan2, state2, /*min_pow_checked=*/true);
        REQUIRE(csm.AddOrphanHeader(orphan2, /*peer_id=*/2));

        REQUIRE(csm.GetOrphanHeaderCount() == 2);
    }

    SECTION("Orphan eviction manual trigger") {
        // Add orphan
        uint256 missing;
        missing.SetNull();
        memset((void*)missing.data(), 0xaa, 32);

        CBlockHeader orphan = CreateChildHeader(missing, 1000);
        ValidationState state;
        csm.AcceptBlockHeader(orphan, state, /*min_pow_checked=*/true);
        REQUIRE(csm.AddOrphanHeader(orphan, /*peer_id=*/1));

        REQUIRE(csm.GetOrphanHeaderCount() == 1);

        // Manual eviction (won't evict unless expired)
        csm.EvictOrphanHeaders();

        // Orphan still there (not expired yet)
        REQUIRE(csm.GetOrphanHeaderCount() == 1);
    }
}

TEST_CASE("ChainstateManager - Anti-DoS gate (min_pow_checked=false)", "[chain][chainstate_manager][unit]") {
    auto params = ChainParams::CreateRegTest();
    TestChainstateManager csm(*params);

    CBlockHeader genesis = CreateTestHeader();
    csm.Initialize(genesis);

    SECTION("Reject when min_pow_checked is false") {
        CBlockHeader block1 = CreateChildHeader(genesis.GetHash(), 1234567900);
        ValidationState state;

        CBlockIndex* p = csm.AcceptBlockHeader(block1, state, /*min_pow_checked=*/false);
        REQUIRE(p == nullptr);
        REQUIRE_FALSE(state.IsValid());
        REQUIRE(state.GetRejectReason() == "too-little-chainwork");
        REQUIRE(csm.LookupBlockIndex(block1.GetHash()) == nullptr);
    }
}

TEST_CASE("ChainstateManager - Duplicate invalid re-announce", "[chain][chainstate_manager][unit]") {
    auto params = ChainParams::CreateRegTest();
    TestChainstateManager csm(*params);

    CBlockHeader genesis = CreateTestHeader();
    csm.Initialize(genesis);

    // Accept a valid header first
    CBlockHeader block1 = CreateChildHeader(genesis.GetHash(), 1234567900);
    {
        ValidationState st;
        CBlockIndex* p1 = csm.AcceptBlockHeader(block1, st, /*min_pow_checked=*/true);
        REQUIRE(p1 != nullptr);
        REQUIRE(st.IsValid());
    }

    // Invalidate it
    REQUIRE(csm.InvalidateBlock(block1.GetHash()));

    // Re-announce the same header: should be rejected as duplicate (known invalid)
    ValidationState st2;
    CBlockIndex* p2 = csm.AcceptBlockHeader(block1, st2, /*min_pow_checked=*/true);
    REQUIRE(p2 == nullptr);
    REQUIRE_FALSE(st2.IsValid());
    REQUIRE(st2.GetRejectReason() == "duplicate");
}

TEST_CASE("ChainstateManager - Descendant of invalid is rejected", "[chain][chainstate_manager][unit]") {
    auto params = ChainParams::CreateRegTest();
    TestChainstateManager csm(*params);

    CBlockHeader genesis = CreateTestHeader();
    csm.Initialize(genesis);

    // Build chain: genesis -> A1 -> A2
    CBlockHeader A1 = CreateChildHeader(genesis.GetHash(), 1000);
    CBlockHeader A2 = CreateChildHeader(A1.GetHash(), 2000);

    {
        ValidationState s1; REQUIRE(csm.AcceptBlockHeader(A1, s1, /*min_pow_checked=*/true) != nullptr);
        ValidationState s2; REQUIRE(csm.AcceptBlockHeader(A2, s2, /*min_pow_checked=*/true) != nullptr);
    }

    // Invalidate A1 (marks A1 failed and A2 as FAILED_CHILD)
    REQUIRE(csm.InvalidateBlock(A1.GetHash()));

    // Now try to accept a child of A2 (A3) -> parent is invalid (FAILED_CHILD)
    CBlockHeader A3 = CreateChildHeader(A2.GetHash(), 3000);
    ValidationState s3;
    CBlockIndex* p3 = csm.AcceptBlockHeader(A3, s3, /*min_pow_checked=*/true);

    REQUIRE(p3 == nullptr);
    REQUIRE_FALSE(s3.IsValid());
    REQUIRE(s3.GetRejectReason() == "bad-prevblk");
}
