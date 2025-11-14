// Copyright (c) 2025 The Unicity Foundation
// Unit tests for chain/block_manager.cpp - Block storage and retrieval
//
// These tests verify:
// - Initialization with genesis block
// - Block index management (add, lookup)
// - Active chain tracking
// - Persistence (save/load to disk)
// - Genesis validation
// - Error handling

#include "catch_amalgamated.hpp"
#include "chain/block_manager.hpp"
#include "chain/block.hpp"
#include "chain/chainparams.hpp"
#include <filesystem>
#include <fstream>

using namespace unicity::chain;

// Helper to create a test block header
static CBlockHeader CreateTestHeader(uint32_t nTime = 1234567890, uint32_t nBits = 0x1d00ffff, uint32_t nNonce = 0) {
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

// Helper to create a child header
static CBlockHeader CreateChildHeader(const uint256& prevHash, uint32_t nTime = 1234567890, uint32_t nBits = 0x1d00ffff) {
    CBlockHeader header = CreateTestHeader(nTime, nBits);
    header.hashPrevBlock = prevHash;
    return header;
}

// Test fixture for managing temporary files
class BlockManagerTestFixture {
public:
    std::string test_file;

    BlockManagerTestFixture() {
        auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        test_file = "/tmp/block_manager_test_" + std::to_string(now) + ".json";
    }

    ~BlockManagerTestFixture() {
        // Clean up test file
        std::filesystem::remove(test_file);
    }
};

TEST_CASE("BlockManager - Construction", "[chain][block_manager][unit]") {
    BlockManager bm;

    SECTION("Default construction") {
        REQUIRE(bm.GetBlockCount() == 0);
        REQUIRE(bm.GetTip() == nullptr);
    }
}

TEST_CASE("BlockManager - Initialize", "[chain][block_manager][unit]") {
    BlockManager bm;
    CBlockHeader genesis = CreateTestHeader();

    SECTION("Initialize with genesis") {
        bool result = bm.Initialize(genesis);

        REQUIRE(result);
        REQUIRE(bm.GetBlockCount() == 1);
        REQUIRE(bm.GetTip() != nullptr);
        REQUIRE(bm.GetTip()->GetBlockHash() == genesis.GetHash());
        REQUIRE(bm.GetTip()->nHeight == 0);
    }

    SECTION("Cannot initialize twice") {
        REQUIRE(bm.Initialize(genesis));

        // Try to initialize again
        CBlockHeader another_genesis = CreateTestHeader(9999999);
        REQUIRE_FALSE(bm.Initialize(another_genesis));

        // Should still have original genesis
        REQUIRE(bm.GetBlockCount() == 1);
        REQUIRE(bm.GetTip()->GetBlockHash() == genesis.GetHash());
    }

    SECTION("Genesis becomes active tip") {
        bm.Initialize(genesis);

        const CChain& chain = bm.ActiveChain();
        REQUIRE(chain.Height() == 0);
        REQUIRE(chain.Tip() != nullptr);
        REQUIRE(chain.Tip()->GetBlockHash() == genesis.GetHash());
    }
}

TEST_CASE("BlockManager - AddToBlockIndex", "[chain][block_manager][unit]") {
    BlockManager bm;
    CBlockHeader genesis = CreateTestHeader();
    bm.Initialize(genesis);

    SECTION("Add new block") {
        CBlockHeader block1 = CreateChildHeader(genesis.GetHash(), 1234567900);
        CBlockIndex* pindex = bm.AddToBlockIndex(block1);

        REQUIRE(pindex != nullptr);
        REQUIRE(pindex->GetBlockHash() == block1.GetHash());
        REQUIRE(pindex->nHeight == 1);
        REQUIRE(pindex->pprev != nullptr);
        REQUIRE(pindex->pprev->GetBlockHash() == genesis.GetHash());
        REQUIRE(bm.GetBlockCount() == 2);
    }

    SECTION("Add same block twice returns existing") {
        CBlockHeader block1 = CreateChildHeader(genesis.GetHash());

        CBlockIndex* pindex1 = bm.AddToBlockIndex(block1);
        CBlockIndex* pindex2 = bm.AddToBlockIndex(block1);

        REQUIRE(pindex1 == pindex2);  // Same pointer
        REQUIRE(bm.GetBlockCount() == 2);  // Still only 2 blocks
    }

    SECTION("Add orphan block (parent not found) - rejected") {
        // Test defensive behavior: orphans are rejected by BlockManager
        uint256 unknown_parent;
        unknown_parent.SetHex("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");

        CBlockHeader orphan = CreateChildHeader(unknown_parent);
        CBlockIndex* pindex = bm.AddToBlockIndex(orphan);

        // Orphans are now rejected (defensive fix)
        REQUIRE(pindex == nullptr);
        REQUIRE(bm.GetBlockCount() == 1);  // Only genesis
    }

    SECTION("Add chain of blocks") {
        CBlockHeader block1 = CreateChildHeader(genesis.GetHash(), 1000);
        CBlockHeader block2 = CreateChildHeader(block1.GetHash(), 2000);
        CBlockHeader block3 = CreateChildHeader(block2.GetHash(), 3000);

        CBlockIndex* p1 = bm.AddToBlockIndex(block1);
        CBlockIndex* p2 = bm.AddToBlockIndex(block2);
        CBlockIndex* p3 = bm.AddToBlockIndex(block3);

        REQUIRE(p1->nHeight == 1);
        REQUIRE(p2->nHeight == 2);
        REQUIRE(p3->nHeight == 3);

        REQUIRE(p1->pprev->GetBlockHash() == genesis.GetHash());
        REQUIRE(p2->pprev == p1);
        REQUIRE(p3->pprev == p2);

        REQUIRE(bm.GetBlockCount() == 4);  // Genesis + 3 blocks
    }

    SECTION("Chain work increases with each block") {
        CBlockHeader block1 = CreateChildHeader(genesis.GetHash());
        CBlockHeader block2 = CreateChildHeader(block1.GetHash());

        CBlockIndex* genesis_idx = bm.LookupBlockIndex(genesis.GetHash());
        CBlockIndex* p1 = bm.AddToBlockIndex(block1);
        CBlockIndex* p2 = bm.AddToBlockIndex(block2);

        REQUIRE(p1->nChainWork > genesis_idx->nChainWork);
        REQUIRE(p2->nChainWork > p1->nChainWork);
    }
}

TEST_CASE("BlockManager - LookupBlockIndex", "[chain][block_manager][unit]") {
    BlockManager bm;
    CBlockHeader genesis = CreateTestHeader();
    bm.Initialize(genesis);

    SECTION("Lookup existing block") {
        uint256 genesis_hash = genesis.GetHash();
        CBlockIndex* pindex = bm.LookupBlockIndex(genesis_hash);

        REQUIRE(pindex != nullptr);
        REQUIRE(pindex->GetBlockHash() == genesis_hash);
    }

    SECTION("Lookup non-existent block") {
        uint256 unknown_hash;
        unknown_hash.SetHex("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");

        CBlockIndex* pindex = bm.LookupBlockIndex(unknown_hash);
        REQUIRE(pindex == nullptr);
    }

    SECTION("Lookup multiple blocks") {
        CBlockHeader block1 = CreateChildHeader(genesis.GetHash());
        CBlockHeader block2 = CreateChildHeader(block1.GetHash());

        bm.AddToBlockIndex(block1);
        bm.AddToBlockIndex(block2);

        REQUIRE(bm.LookupBlockIndex(genesis.GetHash()) != nullptr);
        REQUIRE(bm.LookupBlockIndex(block1.GetHash()) != nullptr);
        REQUIRE(bm.LookupBlockIndex(block2.GetHash()) != nullptr);
    }

    SECTION("Const lookup") {
        const BlockManager& cbm = bm;
        const CBlockIndex* pindex = cbm.LookupBlockIndex(genesis.GetHash());

        REQUIRE(pindex != nullptr);
        REQUIRE(pindex->GetBlockHash() == genesis.GetHash());
    }
}

TEST_CASE("BlockManager - Active Chain", "[chain][block_manager][unit]") {
    BlockManager bm;
    CBlockHeader genesis = CreateTestHeader();
    bm.Initialize(genesis);

    SECTION("Genesis is initial tip") {
        REQUIRE(bm.GetTip() != nullptr);
        REQUIRE(bm.GetTip()->GetBlockHash() == genesis.GetHash());
        REQUIRE(bm.GetTip()->nHeight == 0);
    }

    SECTION("SetActiveTip updates tip") {
        CBlockHeader block1 = CreateChildHeader(genesis.GetHash());
        CBlockIndex* p1 = bm.AddToBlockIndex(block1);

        bm.SetActiveTip(*p1);

        REQUIRE(bm.GetTip() == p1);
        REQUIRE(bm.GetTip()->nHeight == 1);
        REQUIRE(bm.ActiveChain().Height() == 1);
    }

    SECTION("Active chain tracks full chain") {
        CBlockHeader block1 = CreateChildHeader(genesis.GetHash());
        CBlockHeader block2 = CreateChildHeader(block1.GetHash());
        CBlockHeader block3 = CreateChildHeader(block2.GetHash());

        CBlockIndex* p1 = bm.AddToBlockIndex(block1);
        CBlockIndex* p2 = bm.AddToBlockIndex(block2);
        CBlockIndex* p3 = bm.AddToBlockIndex(block3);

        bm.SetActiveTip(*p3);

        const CChain& chain = bm.ActiveChain();
        REQUIRE(chain.Height() == 3);
        REQUIRE(chain[0]->GetBlockHash() == genesis.GetHash());
        REQUIRE(chain[1] == p1);
        REQUIRE(chain[2] == p2);
        REQUIRE(chain[3] == p3);
    }
}

TEST_CASE("BlockManager - GetBlockCount", "[chain][block_manager][unit]") {
    BlockManager bm;

    SECTION("Empty manager") {
        REQUIRE(bm.GetBlockCount() == 0);
    }

    SECTION("After initialization") {
        CBlockHeader genesis = CreateTestHeader();
        bm.Initialize(genesis);
        REQUIRE(bm.GetBlockCount() == 1);
    }

    SECTION("After adding blocks") {
        CBlockHeader genesis = CreateTestHeader();
        bm.Initialize(genesis);

        CBlockHeader block1 = CreateChildHeader(genesis.GetHash());
        CBlockHeader block2 = CreateChildHeader(block1.GetHash());

        bm.AddToBlockIndex(block1);
        REQUIRE(bm.GetBlockCount() == 2);

        bm.AddToBlockIndex(block2);
        REQUIRE(bm.GetBlockCount() == 3);
    }

    SECTION("Adding same block doesn't increase count") {
        CBlockHeader genesis = CreateTestHeader();
        bm.Initialize(genesis);

        CBlockHeader block1 = CreateChildHeader(genesis.GetHash());
        bm.AddToBlockIndex(block1);
        REQUIRE(bm.GetBlockCount() == 2);

        bm.AddToBlockIndex(block1);  // Add again
        REQUIRE(bm.GetBlockCount() == 2);  // No change
    }
}

TEST_CASE("BlockManager - Save/Load", "[chain][block_manager][unit]") {
    BlockManagerTestFixture fixture;

    SECTION("Save and load genesis only") {
        CBlockHeader genesis = CreateTestHeader();

        // Save
        {
            BlockManager bm;
            bm.Initialize(genesis);
            REQUIRE(bm.Save(fixture.test_file));
        }

        // Load
        {
            BlockManager bm;
            REQUIRE(bm.Load(fixture.test_file, genesis.GetHash()));
            REQUIRE(bm.GetBlockCount() == 1);
            REQUIRE(bm.GetTip() != nullptr);
            REQUIRE(bm.GetTip()->GetBlockHash() == genesis.GetHash());
        }
    }

    SECTION("Save and load multiple blocks") {
        CBlockHeader genesis = CreateTestHeader();
        CBlockHeader block1 = CreateChildHeader(genesis.GetHash(), 1000);
        CBlockHeader block2 = CreateChildHeader(block1.GetHash(), 2000);

        // Save
        {
            BlockManager bm;
            bm.Initialize(genesis);
            CBlockIndex* p1 = bm.AddToBlockIndex(block1);
            bm.AddToBlockIndex(block2);
            bm.SetActiveTip(*bm.LookupBlockIndex(block2.GetHash()));

            REQUIRE(bm.Save(fixture.test_file));
        }

        // Load
        {
            BlockManager bm;
            REQUIRE(bm.Load(fixture.test_file, genesis.GetHash()));
            REQUIRE(bm.GetBlockCount() == 3);
            REQUIRE(bm.GetTip()->GetBlockHash() == block2.GetHash());
            REQUIRE(bm.GetTip()->nHeight == 2);
        }
    }

    SECTION("Load from non-existent file") {
        BlockManager bm;
        CBlockHeader genesis = CreateTestHeader();

        REQUIRE_FALSE(bm.Load("/tmp/does_not_exist_12345.json", genesis.GetHash()));
        REQUIRE(bm.GetBlockCount() == 0);
    }

    SECTION("Genesis mismatch on load") {
        CBlockHeader genesis = CreateTestHeader();
        CBlockHeader wrong_genesis = CreateTestHeader(9999999);

        // Save with one genesis
        {
            BlockManager bm;
            bm.Initialize(genesis);
            REQUIRE(bm.Save(fixture.test_file));
        }

        // Try to load with different genesis
        {
            BlockManager bm;
            REQUIRE_FALSE(bm.Load(fixture.test_file, wrong_genesis.GetHash()));
            REQUIRE(bm.GetBlockCount() == 0);  // Should be cleared on failure
        }
    }

    SECTION("Save to invalid path") {
        BlockManager bm;
        CBlockHeader genesis = CreateTestHeader();
        bm.Initialize(genesis);

        REQUIRE_FALSE(bm.Save("/invalid/path/that/does/not/exist/file.json"));
    }

    SECTION("Chain work preserved across save/load") {
        CBlockHeader genesis = CreateTestHeader();
        CBlockHeader block1 = CreateChildHeader(genesis.GetHash());

        arith_uint256 original_work;

        // Save
        {
            BlockManager bm;
            bm.Initialize(genesis);
            CBlockIndex* p1 = bm.AddToBlockIndex(block1);
            original_work = p1->nChainWork;

            REQUIRE(bm.Save(fixture.test_file));
        }

        // Load
        {
            BlockManager bm;
            REQUIRE(bm.Load(fixture.test_file, genesis.GetHash()));

            CBlockIndex* p1 = bm.LookupBlockIndex(block1.GetHash());
            REQUIRE(p1 != nullptr);
            REQUIRE(p1->nChainWork == original_work);
        }
    }

    SECTION("Parent pointers reconstructed on load") {
        CBlockHeader genesis = CreateTestHeader();
        CBlockHeader block1 = CreateChildHeader(genesis.GetHash());
        CBlockHeader block2 = CreateChildHeader(block1.GetHash());

        // Save
        {
            BlockManager bm;
            bm.Initialize(genesis);
            bm.AddToBlockIndex(block1);
            bm.AddToBlockIndex(block2);
            REQUIRE(bm.Save(fixture.test_file));
        }

        // Load and verify parent pointers
        {
            BlockManager bm;
            REQUIRE(bm.Load(fixture.test_file, genesis.GetHash()));

            CBlockIndex* genesis_idx = bm.LookupBlockIndex(genesis.GetHash());
            CBlockIndex* p1 = bm.LookupBlockIndex(block1.GetHash());
            CBlockIndex* p2 = bm.LookupBlockIndex(block2.GetHash());

            REQUIRE(genesis_idx->pprev == nullptr);
            REQUIRE(p1->pprev == genesis_idx);
            REQUIRE(p2->pprev == p1);
        }
    }

    SECTION("Block metadata preserved") {
        CBlockHeader genesis = CreateTestHeader(1000, 0x1d00ffff, 42);

        // Save
        {
            BlockManager bm;
            bm.Initialize(genesis);
            REQUIRE(bm.Save(fixture.test_file));
        }

        // Load and verify
        {
            BlockManager bm;
            REQUIRE(bm.Load(fixture.test_file, genesis.GetHash()));

            CBlockIndex* pindex = bm.LookupBlockIndex(genesis.GetHash());
            REQUIRE(pindex->nTime == 1000);
            REQUIRE(pindex->nBits == 0x1d00ffff);
            REQUIRE(pindex->nNonce == 42);
            REQUIRE(pindex->nVersion == 1);
        }
    }
}

TEST_CASE("BlockManager - Load Error Handling", "[chain][block_manager][unit]") {
    BlockManagerTestFixture fixture;

    SECTION("Corrupted JSON file") {
        // Create corrupted file
        std::ofstream file(fixture.test_file);
        file << "{ invalid json ][{";
        file.close();

        BlockManager bm;
        CBlockHeader genesis = CreateTestHeader();

        REQUIRE_FALSE(bm.Load(fixture.test_file, genesis.GetHash()));
        REQUIRE(bm.GetBlockCount() == 0);
    }

    SECTION("Wrong version number") {
        // Create file with wrong version
        std::ofstream file(fixture.test_file);
        file << R"({"version": 999, "block_count": 0, "blocks": []})";
        file.close();

        BlockManager bm;
        CBlockHeader genesis = CreateTestHeader();

        REQUIRE_FALSE(bm.Load(fixture.test_file, genesis.GetHash()));
    }
}

TEST_CASE("BlockManager - GetBlockIndex", "[chain][block_manager][unit]") {
    BlockManager bm;
    CBlockHeader genesis = CreateTestHeader();
    bm.Initialize(genesis);

    SECTION("Get block index map") {
        const auto& block_index = bm.GetBlockIndex();

        REQUIRE(block_index.size() == 1);
        REQUIRE(block_index.find(genesis.GetHash()) != block_index.end());
    }

    SECTION("Block index contains all blocks") {
        CBlockHeader block1 = CreateChildHeader(genesis.GetHash());
        CBlockHeader block2 = CreateChildHeader(block1.GetHash());

        bm.AddToBlockIndex(block1);
        bm.AddToBlockIndex(block2);

        const auto& block_index = bm.GetBlockIndex();
        REQUIRE(block_index.size() == 3);
    }
}

TEST_CASE("BlockManager - Edge Cases", "[chain][block_manager][unit]") {
    SECTION("Multiple forks from same parent") {
        BlockManager bm;
        CBlockHeader genesis = CreateTestHeader();
        bm.Initialize(genesis);

        // Create two competing chains from genesis
        CBlockHeader fork1_block1 = CreateChildHeader(genesis.GetHash(), 1000, 0x1d00ffff);
        CBlockHeader fork2_block1 = CreateChildHeader(genesis.GetHash(), 2000, 0x1d00ffff);

        CBlockIndex* p1 = bm.AddToBlockIndex(fork1_block1);
        CBlockIndex* p2 = bm.AddToBlockIndex(fork2_block1);

        REQUIRE(p1->pprev == p2->pprev);  // Same parent
        REQUIRE(p1 != p2);  // Different blocks
        REQUIRE(p1->nHeight == p2->nHeight);  // Same height
        REQUIRE(bm.GetBlockCount() == 3);  // Genesis + 2 forks
    }

    SECTION("Out of order block addition - orphans rejected") {
        // Test defensive behavior: orphans (blocks with missing parents) are rejected
        // This is correct - ChainstateManager handles orphans via AddOrphanHeader,
        // not BlockManager::AddToBlockIndex
        BlockManager bm;
        CBlockHeader genesis = CreateTestHeader();
        bm.Initialize(genesis);

        CBlockHeader block1 = CreateChildHeader(genesis.GetHash());
        CBlockHeader block2 = CreateChildHeader(block1.GetHash());
        CBlockHeader block3 = CreateChildHeader(block2.GetHash());

        // Try to add block 3 first (orphan) - should be rejected
        CBlockIndex* p3 = bm.AddToBlockIndex(block3);
        REQUIRE(p3 == nullptr);  // Orphan rejected
        REQUIRE(bm.GetBlockCount() == 1);  // Only genesis

        // Try to add block 2 (still orphan) - should also be rejected
        CBlockIndex* p2 = bm.AddToBlockIndex(block2);
        REQUIRE(p2 == nullptr);  // Orphan rejected
        REQUIRE(bm.GetBlockCount() == 1);  // Still only genesis

        // Add block 1 (connects to genesis) - should succeed
        CBlockIndex* p1 = bm.AddToBlockIndex(block1);
        REQUIRE(p1 != nullptr);
        REQUIRE(p1->pprev != nullptr);
        REQUIRE(p1->nHeight == 1);
        REQUIRE(bm.GetBlockCount() == 2);  // Genesis + block1

        // Now add block 2 (connects to block1) - should succeed
        p2 = bm.AddToBlockIndex(block2);
        REQUIRE(p2 != nullptr);
        REQUIRE(p2->pprev == p1);
        REQUIRE(p2->nHeight == 2);
        REQUIRE(bm.GetBlockCount() == 3);  // Genesis + block1 + block2

        // Finally add block 3 (connects to block2) - should succeed
        p3 = bm.AddToBlockIndex(block3);
        REQUIRE(p3 != nullptr);
        REQUIRE(p3->pprev == p2);
        REQUIRE(p3->nHeight == 3);
        REQUIRE(bm.GetBlockCount() == 4);  // Complete chain
    }
}
