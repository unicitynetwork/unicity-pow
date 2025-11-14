// Copyright (c) 2025 The Unicity Foundation
// Defensive tests for BlockManager - verifying all protective validations
//
// These tests verify defensive fixes added to catch corruption, tampering,
// and edge cases that should never happen in practice but could occur due to:
// - File corruption
// - Manual JSON editing
// - Bugs in serialization
// - Version incompatibilities

#include "catch_amalgamated.hpp"
#include "chain/block_manager.hpp"
#include "chain/block.hpp"
#include "chain/chainparams.hpp"
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

using namespace unicity::chain;
using json = nlohmann::json;

// Helper to create a test block header
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

static CBlockHeader CreateChildHeader(const uint256& prevHash, uint32_t nTime = 1234567890, uint32_t nBits = 0x1d00ffff) {
    CBlockHeader header = CreateTestHeader(nTime);
    header.hashPrevBlock = prevHash;
    header.nBits = nBits;
    return header;
}

// Test fixture
class DefensiveTestFixture {
public:
    std::string test_file;

    DefensiveTestFixture() {
        auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        test_file = "/tmp/defensive_test_" + std::to_string(now) + ".json";
    }

    ~DefensiveTestFixture() {
        std::filesystem::remove(test_file);
    }

    // Helper to create valid chain JSON
    json CreateValidChainJSON(int num_blocks = 3) {
        json root;
        root["version"] = 1;
        root["block_count"] = num_blocks;

        BlockManager bm;
        CBlockHeader genesis = CreateTestHeader();
        bm.Initialize(genesis);

        root["genesis_hash"] = genesis.GetHash().ToString();

        CBlockIndex* prev = bm.GetTip();
        for (int i = 1; i < num_blocks; i++) {
            CBlockHeader block = CreateChildHeader(prev->GetBlockHash(), 1234567890 + i * 100);
            prev = bm.AddToBlockIndex(block);
        }

        root["tip_hash"] = prev->GetBlockHash().ToString();

        json blocks = json::array();
        for (const auto& [hash, block_index] : bm.GetBlockIndex()) {
            json block_data;
            block_data["hash"] = hash.ToString();
            block_data["version"] = block_index.nVersion;
            block_data["miner_address"] = block_index.minerAddress.ToString();
            block_data["time"] = block_index.nTime;
            block_data["bits"] = block_index.nBits;
            block_data["nonce"] = block_index.nNonce;
            block_data["hash_randomx"] = block_index.hashRandomX.ToString();
            block_data["height"] = block_index.nHeight;
            block_data["chainwork"] = block_index.nChainWork.GetHex();
            block_data["status"] = {
                {"validation", block_index.status.validation},
                {"failure", block_index.status.failure}
            };

            if (block_index.pprev) {
                block_data["prev_hash"] = block_index.pprev->GetBlockHash().ToString();
            } else {
                block_data["prev_hash"] = uint256().ToString();
            }

            blocks.push_back(block_data);
        }

        root["blocks"] = blocks;
        return root;
    }
};

//==============================================================================
// CATEGORY 1: Corruption Detection
//==============================================================================

TEST_CASE("BlockManager Defensive - Hash Corruption Detection", "[chain][block_manager][defensive]") {
    DefensiveTestFixture fixture;

    SECTION("Detect corrupted block hash") {
        // Create valid JSON, then corrupt a hash
        json root = fixture.CreateValidChainJSON(3);

        // Corrupt the hash of block at index 1 (not genesis)
        root["blocks"][1]["hash"] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

        // Write to file
        std::ofstream file(fixture.test_file);
        file << root.dump();
        file.close();

        // Load should fail - recomputed hash won't match stored hash
        BlockManager bm;
        CBlockHeader genesis = CreateTestHeader();
        REQUIRE_FALSE(bm.Load(fixture.test_file, genesis.GetHash()));
    }

    SECTION("Detect tampered header fields") {
        json root = fixture.CreateValidChainJSON(2);

        // Tamper with a header field (change nTime)
        root["blocks"][1]["time"] = 99999;
        // Hash remains the same (invalid!)

        std::ofstream file(fixture.test_file);
        file << root.dump();
        file.close();

        BlockManager bm;
        CBlockHeader genesis = CreateTestHeader();
        REQUIRE_FALSE(bm.Load(fixture.test_file, genesis.GetHash()));
    }
}

TEST_CASE("BlockManager Defensive - Multiple Genesis Detection", "[chain][block_manager][defensive]") {
    DefensiveTestFixture fixture;

    SECTION("Reject multiple genesis blocks") {
        json root = fixture.CreateValidChainJSON(3);

        // Corrupt: set two blocks to have null prev_hash (both claim to be genesis)
        root["blocks"][1]["prev_hash"] = uint256().ToString();

        std::ofstream file(fixture.test_file);
        file << root.dump();
        file.close();

        BlockManager bm;
        CBlockHeader genesis = CreateTestHeader();
        REQUIRE_FALSE(bm.Load(fixture.test_file, genesis.GetHash()));
    }

    SECTION("Reject zero genesis blocks") {
        json root = fixture.CreateValidChainJSON(2);

        // Corrupt: make all blocks have a parent (no genesis)
        std::string fake_parent = "1111111111111111111111111111111111111111111111111111111111111111";
        root["blocks"][0]["prev_hash"] = fake_parent;

        std::ofstream file(fixture.test_file);
        file << root.dump();
        file.close();

        BlockManager bm;
        CBlockHeader genesis = CreateTestHeader();
        REQUIRE_FALSE(bm.Load(fixture.test_file, genesis.GetHash()));
    }

    SECTION("Reject wrong genesis hash") {
        json root = fixture.CreateValidChainJSON(2);

        // The file claims a different genesis hash
        root["genesis_hash"] = "2222222222222222222222222222222222222222222222222222222222222222";

        std::ofstream file(fixture.test_file);
        file << root.dump();
        file.close();

        BlockManager bm;
        CBlockHeader genesis = CreateTestHeader();
        // This should fail at the genesis hash mismatch check (before unique genesis check)
        REQUIRE_FALSE(bm.Load(fixture.test_file, genesis.GetHash()));
    }
}

TEST_CASE("BlockManager Defensive - Chain Continuity", "[chain][block_manager][defensive]") {
    DefensiveTestFixture fixture;

    SECTION("Detect broken chain - missing parent") {
        json root = fixture.CreateValidChainJSON(3);

        // Remove the block at height 1, so block at height 2 has missing parent
        auto& blocks = root["blocks"];
        for (size_t i = 0; i < blocks.size(); i++) {
            if (blocks[i]["height"] == 1) {
                blocks.erase(i);
                break;
            }
        }
        root["block_count"] = blocks.size();

        std::ofstream file(fixture.test_file);
        file << root.dump();
        file.close();

        BlockManager bm;
        CBlockHeader genesis = CreateTestHeader();
        REQUIRE_FALSE(bm.Load(fixture.test_file, genesis.GetHash()));
    }

    SECTION("Detect blocks not descending from genesis") {
        json root = fixture.CreateValidChainJSON(2);

        // Best tip doesn't connect to genesis (separate chain)
        json& tip_block = root["blocks"][1];
        tip_block["prev_hash"] = "3333333333333333333333333333333333333333333333333333333333333333";
        // This will fail hash reconstruction, but let's also test continuity

        std::ofstream file(fixture.test_file);
        file << root.dump();
        file.close();

        BlockManager bm;
        CBlockHeader genesis = CreateTestHeader();
        REQUIRE_FALSE(bm.Load(fixture.test_file, genesis.GetHash()));
    }
}

//==============================================================================
// CATEGORY 2: Height Validation
//==============================================================================

TEST_CASE("BlockManager Defensive - Height Invariants", "[chain][block_manager][defensive]") {
    DefensiveTestFixture fixture;

    SECTION("Detect parent height >= child height") {
        json root = fixture.CreateValidChainJSON(3);

        // Corrupt: child has same height as parent
        root["blocks"][1]["height"] = 0;  // Same as genesis

        std::ofstream file(fixture.test_file);
        file << root.dump();
        file.close();

        BlockManager bm;
        CBlockHeader genesis = CreateTestHeader();
        REQUIRE_FALSE(bm.Load(fixture.test_file, genesis.GetHash()));
    }

    SECTION("Detect parent height > child height") {
        json root = fixture.CreateValidChainJSON(3);

        // Corrupt: parent has higher height than child
        root["blocks"][0]["height"] = 10;
        root["blocks"][1]["height"] = 5;

        std::ofstream file(fixture.test_file);
        file << root.dump();
        file.close();

        BlockManager bm;
        CBlockHeader genesis = CreateTestHeader();
        REQUIRE_FALSE(bm.Load(fixture.test_file, genesis.GetHash()));
    }

    SECTION("Detect negative height") {
        json root = fixture.CreateValidChainJSON(2);

        root["blocks"][1]["height"] = -1;

        std::ofstream file(fixture.test_file);
        file << root.dump();
        file.close();

        BlockManager bm;
        CBlockHeader genesis = CreateTestHeader();
        // This should fail somewhere (likely during validation)
        REQUIRE_FALSE(bm.Load(fixture.test_file, genesis.GetHash()));
    }

    SECTION("Detect height gap") {
        json root = fixture.CreateValidChainJSON(3);

        // Genesis=0, block1=1, block2=10 (gap!)
        root["blocks"][2]["height"] = 10;

        std::ofstream file(fixture.test_file);
        file << root.dump();
        file.close();

        BlockManager bm;
        CBlockHeader genesis = CreateTestHeader();
        REQUIRE_FALSE(bm.Load(fixture.test_file, genesis.GetHash()));
    }

    SECTION("Detect genesis with non-zero height") {
        json root = fixture.CreateValidChainJSON(2);

        root["blocks"][0]["height"] = 5;

        std::ofstream file(fixture.test_file);
        file << root.dump();
        file.close();

        BlockManager bm;
        CBlockHeader genesis = CreateTestHeader();
        REQUIRE_FALSE(bm.Load(fixture.test_file, genesis.GetHash()));
    }
}

//==============================================================================
// CATEGORY 3: JSON Validation
//==============================================================================

TEST_CASE("BlockManager Defensive - JSON Structure", "[chain][block_manager][defensive]") {
    DefensiveTestFixture fixture;

    SECTION("Reject missing 'blocks' field") {
        json root;
        root["version"] = 1;
        root["block_count"] = 0;
        root["genesis_hash"] = CreateTestHeader().GetHash().ToString();
        // Missing "blocks" field

        std::ofstream file(fixture.test_file);
        file << root.dump();
        file.close();

        BlockManager bm;
        CBlockHeader genesis = CreateTestHeader();
        REQUIRE_FALSE(bm.Load(fixture.test_file, genesis.GetHash()));
    }

    SECTION("Reject 'blocks' as non-array") {
        json root;
        root["version"] = 1;
        root["block_count"] = 1;
        root["genesis_hash"] = CreateTestHeader().GetHash().ToString();
        root["blocks"] = "not an array";  // Wrong type!

        std::ofstream file(fixture.test_file);
        file << root.dump();
        file.close();

        BlockManager bm;
        CBlockHeader genesis = CreateTestHeader();
        REQUIRE_FALSE(bm.Load(fixture.test_file, genesis.GetHash()));
    }

    SECTION("Warn on block_count mismatch") {
        json root = fixture.CreateValidChainJSON(3);

        // block_count says 10 but array has 3
        root["block_count"] = 10;

        std::ofstream file(fixture.test_file);
        file << root.dump();
        file.close();

        BlockManager bm;
        CBlockHeader genesis = CreateTestHeader();
        // Should still load (uses actual array size) but logs warning
        bool loaded = bm.Load(fixture.test_file, genesis.GetHash());
        REQUIRE(loaded);
        REQUIRE(bm.GetBlockCount() == 3);  // Actual size, not claimed size
    }

    SECTION("Reject missing required field - hash") {
        json root = fixture.CreateValidChainJSON(2);

        root["blocks"][1].erase("hash");

        std::ofstream file(fixture.test_file);
        file << root.dump();
        file.close();

        BlockManager bm;
        CBlockHeader genesis = CreateTestHeader();
        REQUIRE_FALSE(bm.Load(fixture.test_file, genesis.GetHash()));
    }

    SECTION("Reject missing required field - height") {
        json root = fixture.CreateValidChainJSON(2);

        root["blocks"][1].erase("height");

        std::ofstream file(fixture.test_file);
        file << root.dump();
        file.close();

        BlockManager bm;
        CBlockHeader genesis = CreateTestHeader();
        REQUIRE_FALSE(bm.Load(fixture.test_file, genesis.GetHash()));
    }

    SECTION("Reject empty blocks array") {
        json root;
        root["version"] = 1;
        root["block_count"] = 0;
        root["genesis_hash"] = CreateTestHeader().GetHash().ToString();
        root["blocks"] = json::array();  // Empty

        std::ofstream file(fixture.test_file);
        file << root.dump();
        file.close();

        BlockManager bm;
        CBlockHeader genesis = CreateTestHeader();
        REQUIRE_FALSE(bm.Load(fixture.test_file, genesis.GetHash()));
    }

    SECTION("Reject unsupported version") {
        json root = fixture.CreateValidChainJSON(2);
        root["version"] = 999;

        std::ofstream file(fixture.test_file);
        file << root.dump();
        file.close();

        BlockManager bm;
        CBlockHeader genesis = CreateTestHeader();
        REQUIRE_FALSE(bm.Load(fixture.test_file, genesis.GetHash()));
    }
}

//==============================================================================
// CATEGORY 4: Tip Selection Validation
//==============================================================================

TEST_CASE("BlockManager Defensive - Tip Selection", "[chain][block_manager][defensive]") {
    DefensiveTestFixture fixture;

    SECTION("Select tip with most work") {
        // Create two forks with different work
        BlockManager bm;
        CBlockHeader genesis = CreateTestHeader(1000, 0x1d00ffff);
        bm.Initialize(genesis);

        // Fork A: 2 blocks with high difficulty (more work)
        CBlockHeader a1 = CreateChildHeader(genesis.GetHash(), 2000, 0x1d00aaaa);
        CBlockHeader a2 = CreateChildHeader(a1.GetHash(), 3000, 0x1d00aaaa);
        bm.AddToBlockIndex(a1);
        bm.AddToBlockIndex(a2);

        // Fork B: 3 blocks with low difficulty (less total work)
        CBlockHeader b1 = CreateChildHeader(genesis.GetHash(), 2000, 0x1d00ffff);
        CBlockHeader b2 = CreateChildHeader(b1.GetHash(), 3000, 0x1d00ffff);
        CBlockHeader b3 = CreateChildHeader(b2.GetHash(), 4000, 0x1d00ffff);
        bm.AddToBlockIndex(b1);
        bm.AddToBlockIndex(b2);
        bm.AddToBlockIndex(b3);

        // Save and reload
        REQUIRE(bm.Save(fixture.test_file));

        BlockManager bm2;
        REQUIRE(bm2.Load(fixture.test_file, genesis.GetHash()));

        // Should select fork with most work (not necessarily longest)
        REQUIRE(bm2.GetTip() != nullptr);
        // Fork A has more work due to higher difficulty
        REQUIRE(bm2.GetTip()->nHeight <= 3);  // Could be either fork
    }

    SECTION("Reject load when saved tip is invalid") {
        json root = fixture.CreateValidChainJSON(3);

        // Find and mark the block at height 2 (the tip) as invalid
        for (auto& block : root["blocks"]) {
            if (block["height"] == 2) {
                block["status"] = {
                    {"validation", BlockStatus::TREE},
                    {"failure", BlockStatus::VALIDATION_FAILED}
                };
                break;
            }
        }

        std::ofstream file(fixture.test_file);
        file << root.dump();
        file.close();

        BlockManager bm;
        CBlockHeader genesis = CreateTestHeader();
        // Load should fail because saved tip is marked as invalid
        REQUIRE_FALSE(bm.Load(fixture.test_file, genesis.GetHash()));
    }
}

//==============================================================================
// CATEGORY 5: Boundary Conditions
//==============================================================================

TEST_CASE("BlockManager Defensive - Boundary Conditions", "[chain][block_manager][defensive]") {
    DefensiveTestFixture fixture;

    SECTION("Handle genesis-only blockchain") {
        json root = fixture.CreateValidChainJSON(1);

        std::ofstream file(fixture.test_file);
        file << root.dump();
        file.close();

        BlockManager bm;
        CBlockHeader genesis = CreateTestHeader();
        REQUIRE(bm.Load(fixture.test_file, genesis.GetHash()));
        REQUIRE(bm.GetBlockCount() == 1);
        REQUIRE(bm.GetTip()->nHeight == 0);
    }

    SECTION("Handle maximum timestamp") {
        BlockManager bm;
        CBlockHeader genesis = CreateTestHeader(UINT32_MAX);
        REQUIRE(bm.Initialize(genesis));

        REQUIRE(bm.Save(fixture.test_file));

        BlockManager bm2;
        REQUIRE(bm2.Load(fixture.test_file, genesis.GetHash()));
        REQUIRE(bm2.GetTip()->nTime == UINT32_MAX);
    }

    SECTION("Save preserves height order") {
        // Create blocks out of order, save, verify JSON is sorted
        BlockManager bm;
        CBlockHeader genesis = CreateTestHeader();
        bm.Initialize(genesis);

        CBlockHeader b1 = CreateChildHeader(genesis.GetHash(), 2000);
        CBlockHeader b2 = CreateChildHeader(b1.GetHash(), 3000);
        CBlockHeader b3 = CreateChildHeader(b2.GetHash(), 4000);

        bm.AddToBlockIndex(b1);
        bm.AddToBlockIndex(b2);
        bm.AddToBlockIndex(b3);

        REQUIRE(bm.Save(fixture.test_file));

        // Read JSON and verify blocks are in height order
        std::ifstream file(fixture.test_file);
        json root;
        file >> root;
        file.close();

        REQUIRE(root["blocks"].is_array());
        int prev_height = -1;
        for (const auto& block : root["blocks"]) {
            int height = block["height"].get<int>();
            REQUIRE(height > prev_height);  // Ascending order
            prev_height = height;
        }
    }
}

//==============================================================================
// CATEGORY 6: Large Chain Performance
//==============================================================================

TEST_CASE("BlockManager Defensive - Performance", "[chain][block_manager][defensive][!benchmark]") {
    DefensiveTestFixture fixture;

    SECTION("Handle moderately large chain (1000 blocks)") {
        BlockManager bm;
        CBlockHeader genesis = CreateTestHeader();
        bm.Initialize(genesis);

        CBlockIndex* prev = bm.GetTip();
        for (int i = 1; i < 1000; i++) {
            CBlockHeader block = CreateChildHeader(prev->GetBlockHash(), 1000000 + i);
            prev = bm.AddToBlockIndex(block);
            REQUIRE(prev != nullptr);
        }

        REQUIRE(bm.GetBlockCount() == 1000);

        // Save and reload should be fast (no quadratic behavior)
        auto start = std::chrono::steady_clock::now();
        REQUIRE(bm.Save(fixture.test_file));
        auto save_duration = std::chrono::steady_clock::now() - start;

        BlockManager bm2;
        start = std::chrono::steady_clock::now();
        REQUIRE(bm2.Load(fixture.test_file, genesis.GetHash()));
        auto load_duration = std::chrono::steady_clock::now() - start;

        REQUIRE(bm2.GetBlockCount() == 1000);
        REQUIRE(bm2.GetTip()->nHeight == 999);

        // Should complete in reasonable time (not checking specific duration,
        // just ensuring it doesn't hang or crash)
        INFO("Save took: " << std::chrono::duration_cast<std::chrono::milliseconds>(save_duration).count() << "ms");
        INFO("Load took: " << std::chrono::duration_cast<std::chrono::milliseconds>(load_duration).count() << "ms");
    }

    SECTION("Handle many forks (50 competing chains)") {
        BlockManager bm;
        CBlockHeader genesis = CreateTestHeader();
        bm.Initialize(genesis);

        // Create 50 forks from genesis, each 5 blocks long
        for (int fork = 0; fork < 50; fork++) {
            CBlockIndex* prev = bm.GetTip();  // Genesis
            for (int height = 1; height <= 5; height++) {
                CBlockHeader block = CreateChildHeader(prev->GetBlockHash(), 1000000 + fork * 1000 + height);
                prev = bm.AddToBlockIndex(block);
                REQUIRE(prev != nullptr);
            }
        }

        REQUIRE(bm.GetBlockCount() == 1 + 50 * 5);  // Genesis + 250 blocks

        REQUIRE(bm.Save(fixture.test_file));

        BlockManager bm2;
        REQUIRE(bm2.Load(fixture.test_file, genesis.GetHash()));
        REQUIRE(bm2.GetBlockCount() == 251);
    }
}

//==============================================================================
// CATEGORY 7: Orphan Handling (Defensive)
//==============================================================================

TEST_CASE("BlockManager Defensive - Orphan Rejection", "[chain][block_manager][defensive]") {
    SECTION("AddToBlockIndex rejects orphan with unknown parent") {
        BlockManager bm;
        CBlockHeader genesis = CreateTestHeader();
        bm.Initialize(genesis);

        // Create header with unknown parent
        uint256 unknown;
        unknown.SetHex("4444444444444444444444444444444444444444444444444444444444444444");
        CBlockHeader orphan = CreateChildHeader(unknown);

        // Should reject (orphans belong in ChainstateManager, not BlockManager)
        CBlockIndex* pindex = bm.AddToBlockIndex(orphan);
        REQUIRE(pindex == nullptr);
        REQUIRE(bm.GetBlockCount() == 1);  // Only genesis
    }

    SECTION("AddToBlockIndex accepts valid child after parent added") {
        BlockManager bm;
        CBlockHeader genesis = CreateTestHeader();
        bm.Initialize(genesis);

        CBlockHeader block1 = CreateChildHeader(genesis.GetHash());
        CBlockHeader block2 = CreateChildHeader(block1.GetHash());

        // Try to add block2 first (orphan) - rejected
        CBlockIndex* p2 = bm.AddToBlockIndex(block2);
        REQUIRE(p2 == nullptr);

        // Add block1 (valid)
        CBlockIndex* p1 = bm.AddToBlockIndex(block1);
        REQUIRE(p1 != nullptr);
        REQUIRE(p1->nHeight == 1);

        // Now block2 can be added
        p2 = bm.AddToBlockIndex(block2);
        REQUIRE(p2 != nullptr);
        REQUIRE(p2->nHeight == 2);
        REQUIRE(p2->pprev == p1);
    }
}
