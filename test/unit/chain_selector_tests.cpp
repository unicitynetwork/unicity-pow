// Copyright (c) 2025 The Unicity Foundation
// Unit tests for chain/chain_selector.cpp - Chain selection logic
//
// These tests verify:
// - CBlockIndexWorkComparator ordering (chain work, height, hash tie-breaking)
// - Finding chain with most work
// - Adding/removing candidates (leaf-only invariant)
// - Pruning stale candidates
// - Best header tracking
// - Edge cases (empty sets, null pointers, invalid blocks)

#include "catch_amalgamated.hpp"
#include "chain/chain_selector.hpp"
#include "chain/block_manager.hpp"
#include "chain/block.hpp"
#include "chain/chainparams.hpp"
#include "util/arith_uint256.hpp"

using namespace unicity;
using namespace unicity::validation;
using namespace unicity::chain;

// Helper: Create a block header with specific values
static CBlockHeader CreateTestHeader(
    uint32_t nTime = 1234567890,
    uint32_t nBits = 0x1d00ffff,
    uint32_t nNonce = 0) {

    CBlockHeader header;
    header.nVersion = 1;
    header.hashPrevBlock.SetNull();
    header.minerAddress.SetNull();
    header.nTime = nTime;
    header.nBits = nBits;
    header.nNonce = nNonce;
    header.hashRandomX.SetNull();
    return header;
}

// Helper: Create a child header
static CBlockHeader CreateChildHeader(
    const uint256& prevHash,
    uint32_t nTime = 1234567890,
    uint32_t nBits = 0x1d00ffff) {

    CBlockHeader header = CreateTestHeader(nTime, nBits);
    header.hashPrevBlock = prevHash;
    return header;
}

// Helper: Create a block index with specific chain work and height
CBlockIndex* CreateTestBlockIndex(
    BlockManager& bm,
    const CBlockHeader& header,
    int32_t height,
    const arith_uint256& chainWork) {

    auto* pindex = bm.AddToBlockIndex(header);
    pindex->nHeight = height;
    pindex->nChainWork = chainWork;
    pindex->status.validation = BlockStatus::TREE;  // Mark as validated
    return pindex;
}

TEST_CASE("CBlockIndexWorkComparator - Ordering", "[chain][chain_selector][unit]") {
    // Create genesis for BlockManager
    CBlockHeader genesis = CreateTestHeader();

    BlockManager bm;
    bm.Initialize(genesis);

    CBlockIndexWorkComparator comp;

    SECTION("Higher chain work comes first") {
        auto header1 = CreateTestHeader(1000);
        auto header2 = CreateTestHeader(2000);

        auto* pindex1 = CreateTestBlockIndex(bm, header1, 1, arith_uint256(100));
        auto* pindex2 = CreateTestBlockIndex(bm, header2, 1, arith_uint256(200));

        // pindex2 has more work, so should come before pindex1
        REQUIRE(comp(pindex2, pindex1) == true);
        REQUIRE(comp(pindex1, pindex2) == false);
    }

    SECTION("Same work, higher height comes first") {
        auto header1 = CreateTestHeader(1000);
        auto header2 = CreateTestHeader(2000);

        auto* pindex1 = CreateTestBlockIndex(bm, header1, 5, arith_uint256(100));
        auto* pindex2 = CreateTestBlockIndex(bm, header2, 10, arith_uint256(100));

        // Same work, pindex2 has greater height
        REQUIRE(comp(pindex2, pindex1) == true);
        REQUIRE(comp(pindex1, pindex2) == false);
    }

    SECTION("Same work and height, lexicographic hash order") {
        auto header1 = CreateTestHeader(1000);
        auto header2 = CreateTestHeader(2000);

        auto* pindex1 = CreateTestBlockIndex(bm, header1, 5, arith_uint256(100));
        auto* pindex2 = CreateTestBlockIndex(bm, header2, 5, arith_uint256(100));

        auto hash1 = pindex1->GetBlockHash();
        auto hash2 = pindex2->GetBlockHash();

        // Lexicographic ordering
        if (hash1 < hash2) {
            REQUIRE(comp(pindex1, pindex2) == true);
            REQUIRE(comp(pindex2, pindex1) == false);
        } else if (hash2 < hash1) {
            REQUIRE(comp(pindex2, pindex1) == true);
            REQUIRE(comp(pindex1, pindex2) == false);
        } else {
            // Same hash - both should be false (not less than)
            REQUIRE(comp(pindex1, pindex2) == false);
            REQUIRE(comp(pindex2, pindex1) == false);
        }
    }

    SECTION("Strict weak ordering - irreflexivity") {
        auto header = CreateTestHeader();
        auto* pindex = CreateTestBlockIndex(bm, header, 1, arith_uint256(100));

        // a < a should be false
        REQUIRE(comp(pindex, pindex) == false);
    }
}

TEST_CASE("ChainSelector - Construction", "[chain][chain_selector][unit]") {
    ChainSelector selector;

    REQUIRE(selector.GetCandidateCount() == 0);
    REQUIRE(selector.GetBestHeader() == nullptr);
    REQUIRE(selector.FindMostWorkChain() == nullptr);
}

TEST_CASE("ChainSelector - FindMostWorkChain", "[chain][chain_selector][unit]") {
    CBlockHeader genesis = CreateTestHeader();
    BlockManager bm;
    bm.Initialize(genesis);

    ChainSelector selector;

    SECTION("Empty candidate set") {
        REQUIRE(selector.FindMostWorkChain() == nullptr);
    }

    SECTION("Single candidate") {
        auto header1 = CreateTestHeader(1000);
        auto* pindex1 = CreateTestBlockIndex(bm, header1, 1, arith_uint256(100));

        selector.AddCandidateUnchecked(pindex1);
        REQUIRE(selector.GetCandidateCount() == 1);

        auto* best = selector.FindMostWorkChain();
        REQUIRE(best == pindex1);
    }

    SECTION("Multiple candidates - returns most work") {
        auto header1 = CreateTestHeader(1000);
        auto header2 = CreateTestHeader(2000);
        auto header3 = CreateTestHeader(3000);

        auto* pindex1 = CreateTestBlockIndex(bm, header1, 1, arith_uint256(100));
        auto* pindex2 = CreateTestBlockIndex(bm, header2, 2, arith_uint256(300));
        auto* pindex3 = CreateTestBlockIndex(bm, header3, 3, arith_uint256(200));

        selector.AddCandidateUnchecked(pindex1);
        selector.AddCandidateUnchecked(pindex2);
        selector.AddCandidateUnchecked(pindex3);

        REQUIRE(selector.GetCandidateCount() == 3);

        // pindex2 has most work (300)
        auto* best = selector.FindMostWorkChain();
        REQUIRE(best == pindex2);
    }

    SECTION("Invalid candidates are skipped") {
        auto header1 = CreateTestHeader(1000);
        auto header2 = CreateTestHeader(2000);

        auto* pindex1 = CreateTestBlockIndex(bm, header1, 1, arith_uint256(200));
        auto* pindex2 = CreateTestBlockIndex(bm, header2, 2, arith_uint256(100));

        // Mark pindex1 as failed (highest work but invalid)
        pindex1->status.MarkFailed();

        selector.AddCandidateUnchecked(pindex1);
        selector.AddCandidateUnchecked(pindex2);

        // Should skip pindex1 and return pindex2
        auto* best = selector.FindMostWorkChain();
        REQUIRE(best == pindex2);
    }

    SECTION("All candidates invalid") {
        auto header1 = CreateTestHeader(1000);
        auto* pindex1 = CreateTestBlockIndex(bm, header1, 1, arith_uint256(100));

        // Mark as failed
        pindex1->status.MarkFailed();

        selector.AddCandidateUnchecked(pindex1);

        // Should return nullptr
        REQUIRE(selector.FindMostWorkChain() == nullptr);
    }
}

TEST_CASE("ChainSelector - TryAddBlockIndexCandidate", "[chain][chain_selector][unit]") {
    CBlockHeader genesis = CreateTestHeader();
    BlockManager bm;
    bm.Initialize(genesis);

    ChainSelector selector;

    SECTION("Add nullptr") {
        selector.TryAddBlockIndexCandidate(nullptr, bm);
        REQUIRE(selector.GetCandidateCount() == 0);
    }

    SECTION("Add valid leaf block") {
        auto header1 = CreateChildHeader(genesis.GetHash(), 1000);
        auto* pindex1 = CreateTestBlockIndex(bm, header1, 1, arith_uint256(100));
        pindex1->pprev = bm.GetTip();

        selector.TryAddBlockIndexCandidate(pindex1, bm);
        REQUIRE(selector.GetCandidateCount() == 1);

        auto* best = selector.FindMostWorkChain();
        REQUIRE(best == pindex1);
    }

    SECTION("Do not add unvalidated block") {
        auto header1 = CreateChildHeader(genesis.GetHash(), 1000);
        auto* pindex1 = CreateTestBlockIndex(bm, header1, 1, arith_uint256(100));

        // Mark as not validated
        pindex1->status.validation = BlockStatus::HEADER;  // Not TREE level

        selector.TryAddBlockIndexCandidate(pindex1, bm);
        REQUIRE(selector.GetCandidateCount() == 0);
    }

    SECTION("Do not add block with children") {
        // Create parent -> child chain
        auto header1 = CreateChildHeader(genesis.GetHash(), 1000);
        auto* parent = CreateTestBlockIndex(bm, header1, 1, arith_uint256(100));
        parent->pprev = bm.GetTip();

        auto header2 = CreateChildHeader(header1.GetHash(), 2000);
        auto* child = CreateTestBlockIndex(bm, header2, 2, arith_uint256(200));
        child->pprev = parent;

        // Try to add parent (which has child) - should be rejected
        selector.TryAddBlockIndexCandidate(parent, bm);
        REQUIRE(selector.GetCandidateCount() == 0);
    }

    SECTION("Adding child removes parent from candidates") {
        auto header1 = CreateChildHeader(genesis.GetHash(), 1000);
        auto* parent = CreateTestBlockIndex(bm, header1, 1, arith_uint256(100));
        parent->pprev = bm.GetTip();

        // Add parent first
        selector.TryAddBlockIndexCandidate(parent, bm);
        REQUIRE(selector.GetCandidateCount() == 1);

        // Now create and add child
        auto header2 = CreateChildHeader(header1.GetHash(), 2000);
        auto* child = CreateTestBlockIndex(bm, header2, 2, arith_uint256(200));
        child->pprev = parent;

        selector.TryAddBlockIndexCandidate(child, bm);

        // Parent should be removed, only child remains
        REQUIRE(selector.GetCandidateCount() == 1);
        auto* best = selector.FindMostWorkChain();
        REQUIRE(best == child);
    }
}

TEST_CASE("ChainSelector - PruneBlockIndexCandidates", "[chain][chain_selector][unit]") {
    CBlockHeader genesis = CreateTestHeader();
    BlockManager bm;
    bm.Initialize(genesis);

    ChainSelector selector;

    SECTION("Empty candidate set") {
        selector.PruneBlockIndexCandidates(bm);
        REQUIRE(selector.GetCandidateCount() == 0);
    }

    SECTION("Remove candidates with less work than tip") {
        // Set up active chain: genesis -> block1 (work=200)
        auto header1 = CreateChildHeader(genesis.GetHash(), 1000);
        auto* block1 = CreateTestBlockIndex(bm, header1, 1, arith_uint256(200));
        block1->pprev = bm.GetTip();
        bm.SetActiveTip(*block1);

        // Create alternative block with less work
        auto header2 = CreateChildHeader(genesis.GetHash(), 2000);
        auto* block2 = CreateTestBlockIndex(bm, header2, 1, arith_uint256(100));
        block2->pprev = bm.GetTip()->pprev;

        selector.AddCandidateUnchecked(block2);
        REQUIRE(selector.GetCandidateCount() == 1);

        selector.PruneBlockIndexCandidates(bm);

        // block2 should be pruned (less work than tip)
        REQUIRE(selector.GetCandidateCount() == 0);
    }

    SECTION("Remove current tip from candidates") {
        auto header1 = CreateChildHeader(genesis.GetHash(), 1000);
        auto* block1 = CreateTestBlockIndex(bm, header1, 1, arith_uint256(200));
        block1->pprev = bm.GetTip();
        bm.SetActiveTip(*block1);

        // Add tip to candidates
        selector.AddCandidateUnchecked(block1);
        REQUIRE(selector.GetCandidateCount() == 1);

        selector.PruneBlockIndexCandidates(bm);

        // Tip should be pruned
        REQUIRE(selector.GetCandidateCount() == 0);
    }

    SECTION("Remove ancestors of tip from candidates") {
        // Build chain: genesis -> block1 -> block2
        auto header1 = CreateChildHeader(genesis.GetHash(), 1000);
        auto* block1 = CreateTestBlockIndex(bm, header1, 1, arith_uint256(100));
        block1->pprev = bm.GetTip();

        auto header2 = CreateChildHeader(header1.GetHash(), 2000);
        auto* block2 = CreateTestBlockIndex(bm, header2, 2, arith_uint256(200));
        block2->pprev = block1;

        bm.SetActiveTip(*block2);

        // Add block1 (ancestor of tip) to candidates
        selector.AddCandidateUnchecked(block1);
        REQUIRE(selector.GetCandidateCount() == 1);

        selector.PruneBlockIndexCandidates(bm);

        // block1 should be pruned (ancestor of tip)
        REQUIRE(selector.GetCandidateCount() == 0);
    }

    SECTION("Keep valid competing chain tip") {
        // Active chain: genesis -> block1 (work=200)
        auto header1 = CreateChildHeader(genesis.GetHash(), 1000);
        auto* block1 = CreateTestBlockIndex(bm, header1, 1, arith_uint256(200));
        block1->pprev = bm.GetTip();
        bm.SetActiveTip(*block1);

        // Alternative chain with MORE work
        auto header2 = CreateChildHeader(genesis.GetHash(), 2000);
        auto* block2 = CreateTestBlockIndex(bm, header2, 1, arith_uint256(300));
        block2->pprev = bm.GetTip()->pprev;

        selector.AddCandidateUnchecked(block2);
        REQUIRE(selector.GetCandidateCount() == 1);

        selector.PruneBlockIndexCandidates(bm);

        // block2 should remain (more work than tip)
        REQUIRE(selector.GetCandidateCount() == 1);
        auto* best = selector.FindMostWorkChain();
        REQUIRE(best == block2);
    }
}

TEST_CASE("ChainSelector - UpdateBestHeader", "[chain][chain_selector][unit]") {
    CBlockHeader genesis = CreateTestHeader();
    BlockManager bm;
    bm.Initialize(genesis);

    ChainSelector selector;

    SECTION("Update from nullptr") {
        REQUIRE(selector.GetBestHeader() == nullptr);

        auto header1 = CreateTestHeader(1000);
        auto* pindex1 = CreateTestBlockIndex(bm, header1, 1, arith_uint256(100));

        selector.UpdateBestHeader(pindex1);
        REQUIRE(selector.GetBestHeader() == pindex1);
    }

    SECTION("Update with higher work") {
        auto header1 = CreateTestHeader(1000);
        auto* pindex1 = CreateTestBlockIndex(bm, header1, 1, arith_uint256(100));
        selector.UpdateBestHeader(pindex1);

        auto header2 = CreateTestHeader(2000);
        auto* pindex2 = CreateTestBlockIndex(bm, header2, 2, arith_uint256(200));
        selector.UpdateBestHeader(pindex2);

        REQUIRE(selector.GetBestHeader() == pindex2);
    }

    SECTION("Do not update with lower work") {
        auto header1 = CreateTestHeader(1000);
        auto* pindex1 = CreateTestBlockIndex(bm, header1, 1, arith_uint256(200));
        selector.UpdateBestHeader(pindex1);

        auto header2 = CreateTestHeader(2000);
        auto* pindex2 = CreateTestBlockIndex(bm, header2, 2, arith_uint256(100));
        selector.UpdateBestHeader(pindex2);

        // Should remain pindex1
        REQUIRE(selector.GetBestHeader() == pindex1);
    }

    SECTION("Update with nullptr is ignored") {
        auto header1 = CreateTestHeader(1000);
        auto* pindex1 = CreateTestBlockIndex(bm, header1, 1, arith_uint256(100));
        selector.UpdateBestHeader(pindex1);

        selector.UpdateBestHeader(nullptr);

        // Should remain pindex1
        REQUIRE(selector.GetBestHeader() == pindex1);
    }
}

TEST_CASE("ChainSelector - AddCandidateUnchecked", "[chain][chain_selector][unit]") {
    CBlockHeader genesis = CreateTestHeader();
    BlockManager bm;
    bm.Initialize(genesis);

    ChainSelector selector;

    SECTION("Add valid candidate") {
        auto header1 = CreateTestHeader(1000);
        auto* pindex1 = CreateTestBlockIndex(bm, header1, 1, arith_uint256(100));

        selector.AddCandidateUnchecked(pindex1);
        REQUIRE(selector.GetCandidateCount() == 1);
    }

    SECTION("Add multiple candidates") {
        auto header1 = CreateTestHeader(1000);
        auto header2 = CreateTestHeader(2000);

        auto* pindex1 = CreateTestBlockIndex(bm, header1, 1, arith_uint256(100));
        auto* pindex2 = CreateTestBlockIndex(bm, header2, 2, arith_uint256(200));

        selector.AddCandidateUnchecked(pindex1);
        selector.AddCandidateUnchecked(pindex2);

        REQUIRE(selector.GetCandidateCount() == 2);
    }

    SECTION("Add nullptr") {
        selector.AddCandidateUnchecked(nullptr);
        REQUIRE(selector.GetCandidateCount() == 0);
    }
}

TEST_CASE("ChainSelector - ClearCandidates", "[chain][chain_selector][unit]") {
    CBlockHeader genesis = CreateTestHeader();
    BlockManager bm;
    bm.Initialize(genesis);

    ChainSelector selector;

    SECTION("Clear empty set") {
        selector.ClearCandidates();
        REQUIRE(selector.GetCandidateCount() == 0);
    }

    SECTION("Clear with candidates") {
        auto header1 = CreateTestHeader(1000);
        auto header2 = CreateTestHeader(2000);

        auto* pindex1 = CreateTestBlockIndex(bm, header1, 1, arith_uint256(100));
        auto* pindex2 = CreateTestBlockIndex(bm, header2, 2, arith_uint256(200));

        selector.AddCandidateUnchecked(pindex1);
        selector.AddCandidateUnchecked(pindex2);
        REQUIRE(selector.GetCandidateCount() == 2);

        selector.ClearCandidates();
        REQUIRE(selector.GetCandidateCount() == 0);
        REQUIRE(selector.FindMostWorkChain() == nullptr);
    }
}

TEST_CASE("ChainSelector - RemoveCandidate", "[chain][chain_selector][unit]") {
    CBlockHeader genesis = CreateTestHeader();
    BlockManager bm;
    bm.Initialize(genesis);

    ChainSelector selector;

    SECTION("Remove existing candidate") {
        auto header1 = CreateTestHeader(1000);
        auto* pindex1 = CreateTestBlockIndex(bm, header1, 1, arith_uint256(100));

        selector.AddCandidateUnchecked(pindex1);
        REQUIRE(selector.GetCandidateCount() == 1);

        selector.RemoveCandidate(pindex1);
        REQUIRE(selector.GetCandidateCount() == 0);
    }

    SECTION("Remove non-existing candidate") {
        auto header1 = CreateTestHeader(1000);
        auto* pindex1 = CreateTestBlockIndex(bm, header1, 1, arith_uint256(100));

        // Don't add, just try to remove
        selector.RemoveCandidate(pindex1);
        REQUIRE(selector.GetCandidateCount() == 0);
    }

    SECTION("Remove nullptr") {
        selector.RemoveCandidate(nullptr);
        REQUIRE(selector.GetCandidateCount() == 0);
    }

    SECTION("Remove one of multiple") {
        auto header1 = CreateTestHeader(1000);
        auto header2 = CreateTestHeader(2000);
        auto header3 = CreateTestHeader(3000);

        auto* pindex1 = CreateTestBlockIndex(bm, header1, 1, arith_uint256(100));
        auto* pindex2 = CreateTestBlockIndex(bm, header2, 2, arith_uint256(200));
        auto* pindex3 = CreateTestBlockIndex(bm, header3, 3, arith_uint256(300));

        selector.AddCandidateUnchecked(pindex1);
        selector.AddCandidateUnchecked(pindex2);
        selector.AddCandidateUnchecked(pindex3);
        REQUIRE(selector.GetCandidateCount() == 3);

        selector.RemoveCandidate(pindex2);
        REQUIRE(selector.GetCandidateCount() == 2);

        // pindex3 should be best (most work)
        auto* best = selector.FindMostWorkChain();
        REQUIRE(best == pindex3);
    }
}

TEST_CASE("ChainSelector - SetBestHeader", "[chain][chain_selector][unit]") {
    CBlockHeader genesis = CreateTestHeader();
    BlockManager bm;
    bm.Initialize(genesis);

    ChainSelector selector;

    SECTION("Set best header directly") {
        auto header1 = CreateTestHeader(1000);
        auto* pindex1 = CreateTestBlockIndex(bm, header1, 1, arith_uint256(100));

        selector.SetBestHeader(pindex1);
        REQUIRE(selector.GetBestHeader() == pindex1);
    }

    SECTION("Overwrite existing best header") {
        auto header1 = CreateTestHeader(1000);
        auto header2 = CreateTestHeader(2000);

        auto* pindex1 = CreateTestBlockIndex(bm, header1, 1, arith_uint256(100));
        auto* pindex2 = CreateTestBlockIndex(bm, header2, 2, arith_uint256(200));

        selector.SetBestHeader(pindex1);
        REQUIRE(selector.GetBestHeader() == pindex1);

        selector.SetBestHeader(pindex2);
        REQUIRE(selector.GetBestHeader() == pindex2);
    }
}

TEST_CASE("ChainSelector - Edge Cases", "[chain][chain_selector][unit]") {
    CBlockHeader genesis = CreateTestHeader();
    BlockManager bm;
    bm.Initialize(genesis);

    ChainSelector selector;

    SECTION("Multiple candidates with same work and height") {
        auto header1 = CreateTestHeader(1000);
        auto header2 = CreateTestHeader(2000);

        auto* pindex1 = CreateTestBlockIndex(bm, header1, 5, arith_uint256(100));
        auto* pindex2 = CreateTestBlockIndex(bm, header2, 5, arith_uint256(100));

        selector.AddCandidateUnchecked(pindex1);
        selector.AddCandidateUnchecked(pindex2);

        REQUIRE(selector.GetCandidateCount() == 2);

        // Should return one of them (deterministic based on hash)
        auto* best = selector.FindMostWorkChain();
        REQUIRE(best != nullptr);
        REQUIRE((best == pindex1 || best == pindex2));
    }

    SECTION("Clear and re-add candidates") {
        auto header1 = CreateTestHeader(1000);
        auto* pindex1 = CreateTestBlockIndex(bm, header1, 1, arith_uint256(100));

        selector.AddCandidateUnchecked(pindex1);
        REQUIRE(selector.GetCandidateCount() == 1);

        selector.ClearCandidates();
        REQUIRE(selector.GetCandidateCount() == 0);

        selector.AddCandidateUnchecked(pindex1);
        REQUIRE(selector.GetCandidateCount() == 1);
    }

    SECTION("Prune with no tip set") {
        auto header1 = CreateTestHeader(1000);
        auto* pindex1 = CreateTestBlockIndex(bm, header1, 1, arith_uint256(100));

        // Create fresh BlockManager with no tip set beyond genesis
        CBlockHeader empty_genesis = CreateTestHeader();
        BlockManager bm_empty;
        bm_empty.Initialize(empty_genesis);

        selector.AddCandidateUnchecked(pindex1);
        REQUIRE(selector.GetCandidateCount() == 1);

        // Should not crash or prune (genesis is tip)
        selector.PruneBlockIndexCandidates(bm_empty);

        // Candidate should be pruned if it has less work than genesis
        // or kept if it has more work
    }
}

TEST_CASE("ChainSelector - Integration Scenario", "[chain][chain_selector][unit]") {
    CBlockHeader genesis = CreateTestHeader();
    BlockManager bm;
    bm.Initialize(genesis);

    ChainSelector selector;

    SECTION("Build competing chains and select best") {
        // Chain A: genesis -> A1 -> A2 (total work: 300)
        auto headerA1 = CreateChildHeader(genesis.GetHash(), 1000);
        auto* blockA1 = CreateTestBlockIndex(bm, headerA1, 1, arith_uint256(100));
        blockA1->pprev = bm.GetTip();

        auto headerA2 = CreateChildHeader(headerA1.GetHash(), 2000);
        auto* blockA2 = CreateTestBlockIndex(bm, headerA2, 2, arith_uint256(300));
        blockA2->pprev = blockA1;

        // Chain B: genesis -> B1 (total work: 250)
        auto headerB1 = CreateChildHeader(genesis.GetHash(), 3000);
        auto* blockB1 = CreateTestBlockIndex(bm, headerB1, 1, arith_uint256(250));
        blockB1->pprev = bm.GetTip();

        // Add both tips
        selector.AddCandidateUnchecked(blockA2);
        selector.AddCandidateUnchecked(blockB1);

        REQUIRE(selector.GetCandidateCount() == 2);

        // Chain A should win (more work)
        auto* best = selector.FindMostWorkChain();
        REQUIRE(best == blockA2);

        // Set as active tip and prune
        bm.SetActiveTip(*blockA2);
        selector.PruneBlockIndexCandidates(bm);

        // blockA2 (active tip) and blockB1 (less work) should be pruned
        REQUIRE(selector.GetCandidateCount() == 0);
    }
}
