// Copyright (c) 2025 The Unicity Foundation
// Distributed under the MIT software license

#include "chain/block_manager.hpp"
#include <algorithm>
#include <cmath>
#include <compare>
#include <cstdint>
#include <exception>
#include <fstream>
#include <nlohmann/json.hpp>
#include <type_traits>
#include <utility>
#include <vector>
#include "util/arith_uint256.hpp"
#include "chain/block.hpp"
#include "util/logging.hpp"

//DTC1

namespace unicity {
namespace chain {

BlockManager::BlockManager() = default;
BlockManager::~BlockManager() = default;

// Helper: Verify that pindex forms a continuous chain back to genesis
// Returns true if chain is valid, false otherwise
// Used for defensive verification during Initialize and Load
static bool VerifyChainContinuity(const CBlockIndex *pindex,
                                   const uint256 &expected_genesis_hash,
                                   std::string &error_msg) {
  if (!pindex) {
    error_msg = "null pointer";
    return false;
  }

  const CBlockIndex *walk = pindex;
  int blocks_walked = 0;

  // Walk backwards to genesis
  while (walk->pprev) {
    walk = walk->pprev;
    blocks_walked++;

    // Sanity: prevent infinite loop in case of circular reference bug
    if (blocks_walked > 1000000) {
      error_msg = "chain walk exceeded 1M blocks (circular reference?)";
      return false;
    }
  }

  // Reached a block with no parent - must be genesis
  if (walk->GetBlockHash() != expected_genesis_hash) {
    error_msg = "chain does not descend from expected genesis (found " +
                walk->GetBlockHash().ToString().substr(0, 16) + ", expected " +
                expected_genesis_hash.ToString().substr(0, 16) + ")";
    return false;
  }

  // Verify height consistency
  if (walk->nHeight != 0) {
    error_msg = "genesis block has non-zero height " + std::to_string(walk->nHeight);
    return false;
  }

  if (pindex->nHeight != blocks_walked) {
    error_msg = "height mismatch: pindex->nHeight=" + std::to_string(pindex->nHeight) +
                " but walked " + std::to_string(blocks_walked) + " blocks";
    return false;
  }

  return true;
}

bool BlockManager::Initialize(const CBlockHeader &genesis) {
  LOG_CHAIN_TRACE("Initialize: called with genesis hash={}",
                  genesis.GetHash().ToString().substr(0, 16));

  if (m_initialized) {
    LOG_CHAIN_ERROR("BlockManager already initialized");
    return false;
  }

  // Add genesis block
  CBlockIndex *pindex = AddToBlockIndex(genesis);
  if (!pindex) {
    LOG_CHAIN_ERROR("Failed to add genesis block");
    return false;
  }

  // Set as active tip
  m_active_chain.SetTip(*pindex);

  // Remember genesis hash
  m_genesis_hash = genesis.GetHash();

  // Defensive: Verify chain continuity (trivial for genesis, but ensures invariant)
  std::string error_msg;
  if (!VerifyChainContinuity(pindex, m_genesis_hash, error_msg)) {
    LOG_CHAIN_ERROR("Chain continuity verification failed during Initialize: {}", error_msg);
    return false;
  }

  m_initialized = true;
  
  LOG_CHAIN_TRACE("BlockManager initialized with genesis: {}",m_genesis_hash.ToString().substr(0, 16));

  return true;
}

CBlockIndex *BlockManager::LookupBlockIndex(const uint256 &hash) {
  auto it = m_block_index.find(hash);
  if (it == m_block_index.end())
    return nullptr;
  return &it->second;
}

const CBlockIndex *BlockManager::LookupBlockIndex(const uint256 &hash) const {
  auto it = m_block_index.find(hash);
  if (it == m_block_index.end())
    return nullptr;
  return &it->second;
}

CBlockIndex *BlockManager::AddToBlockIndex(const CBlockHeader &header) {
  uint256 hash = header.GetHash();

  LOG_CHAIN_TRACE("AddToBlockIndex: hash={} prev={}",
                  hash.ToString().substr(0, 16),
                  header.hashPrevBlock.ToString().substr(0, 16));

  // Already have it?
  auto it = m_block_index.find(hash);
  if (it != m_block_index.end()) {
    LOG_CHAIN_TRACE("AddToBlockIndex: Block already exists, returning existing index");
    return &it->second;
  }

  // Create new entry (use try_emplace to construct CBlockIndex in-place)
  auto [iter, inserted] = m_block_index.try_emplace(hash, header);
  if (!inserted) {
    LOG_CHAIN_ERROR("Failed to insert block {}", hash.ToString().substr(0, 16));
    return nullptr;
  }

  CBlockIndex *pindex = &iter->second;

  // Set hash pointer (points to the map's key)
  // CRITICAL: This requires pointer stability from the container
  // std::map guarantees this, std::unordered_map does NOT
  static_assert(std::is_same<decltype(m_block_index), std::map<uint256, CBlockIndex>>::value,
                "m_block_index must be std::map for pointer stability (phashBlock = &iter->first). "
                "std::unordered_map will cause use-after-free on rehash.");
  pindex->phashBlock = &iter->first;

  // Connect to parent
  pindex->pprev = LookupBlockIndex(header.hashPrevBlock);

  // CRITICAL: Set nHeight and nChainWork ONCE during block creation
  // These fields are used by ChainSelector's CBlockIndexWorkComparator for
  // std::set ordering and MUST remain immutable after the block is added
  // to any sorted container (e.g., candidate set).
  //
  // NEVER modify these fields after this point. Doing so would violate
  // std::set invariants and cause undefined behavior (broken ordering,
  // failed lookups, crashes).
  //
  // If you need to modify these fields for any reason you MUST:
  // 1. Remove the block from all ChainSelector candidate sets
  // 2. Modify the fields
  // 3. Re-add the block to candidate sets if applicable
  if (pindex->pprev) {
    // Not genesis - calculate height and chain work
    pindex->nHeight = pindex->pprev->nHeight + 1;
    pindex->nChainWork = pindex->pprev->nChainWork + GetBlockProof(*pindex);
    // Maintain monotonic time for searches
    pindex->nTimeMax = std::max<int64_t>(pindex->pprev->nTimeMax, pindex->GetBlockTime());
    LOG_CHAIN_TRACE("AddToBlockIndex: Created new block index height={} log2_work={:.6f}",
                    pindex->nHeight, std::log(pindex->nChainWork.getdouble()) / std::log(2.0));
  } else {
    // Genesis block - verify invariant
    // CRITICAL DEFENSE: pprev is null but we must verify this is actually genesis
    // If hashPrevBlock is non-null, this is an orphan (missing parent), which
    // should NEVER reach AddToBlockIndex. ChainstateManager must reject orphans
    // and stash them in the orphan pool. If we allowed this, we'd permanently
    // corrupt the block with height=0 and wrong chainwork (std::set invariants
    // forbid changing these fields after insertion).
    if (!header.hashPrevBlock.IsNull()) {
      LOG_CHAIN_ERROR("CRITICAL BUG: AddToBlockIndex called with orphan header {}. "
                      "Parent {} not found in index. Orphans must be handled at "
                      "ChainstateManager level (AddOrphanHeader), not passed to "
                      "AddToBlockIndex. This indicates a caller bug.",
                      hash.ToString().substr(0, 16),
                      header.hashPrevBlock.ToString().substr(0, 16));
      // Remove the corrupted entry we just created
      m_block_index.erase(hash);
      return nullptr;
    }

    pindex->nHeight = 0;
    pindex->nChainWork = GetBlockProof(*pindex);
    pindex->nTimeMax = pindex->GetBlockTime();
    LOG_CHAIN_TRACE("AddToBlockIndex: Created GENESIS block index log2_work={:.6f}",
                    std::log(pindex->nChainWork.getdouble()) / std::log(2.0));
  }

  // Build skip list pointer for O(log n) ancestor lookup (Bitcoin Core pattern)
  // Must be called after pprev and nHeight are set
  pindex->BuildSkip();

  return pindex;
}

bool BlockManager::Save(const std::string &filepath) const {
  using json = nlohmann::json;

  try {
    LOG_CHAIN_TRACE("Saving {} headers to {}", m_block_index.size(), filepath);

    json root;
    root["version"] = 1; // Format version for future compatibility
    root["block_count"] = m_block_index.size();

    // Save tip hash
    if (m_active_chain.Tip()) {
      root["tip_hash"] = m_active_chain.Tip()->GetBlockHash().ToString();
    } else {
      root["tip_hash"] = "";
    }

    // Save genesis hash
    root["genesis_hash"] = m_genesis_hash.ToString();

    // Save all blocks in height order (topological order)
    // This makes the JSON file easier to read and diff for debugging
    std::vector<const CBlockIndex*> sorted_blocks;
    sorted_blocks.reserve(m_block_index.size());
    for (const auto &[hash, block_index] : m_block_index) {
      sorted_blocks.push_back(&block_index);
    }
    std::sort(sorted_blocks.begin(), sorted_blocks.end(),
              [](const CBlockIndex* a, const CBlockIndex* b) {
                return a->nHeight < b->nHeight;
              });

    json blocks = json::array();
    for (const CBlockIndex* block_index : sorted_blocks) {
      json block_data;

      // Block hash
      block_data["hash"] = block_index->GetBlockHash().ToString();

      // Header fields
      block_data["version"] = block_index->nVersion;
      block_data["miner_address"] = block_index->minerAddress.ToString();
      block_data["time"] = block_index->nTime;
      block_data["bits"] = block_index->nBits;
      block_data["nonce"] = block_index->nNonce;
      block_data["hash_randomx"] = block_index->hashRandomX.ToString();

      // Chain metadata
      block_data["height"] = block_index->nHeight;
      block_data["chainwork"] = block_index->nChainWork.GetHex();

      // Canonical status representation
      {
        nlohmann::json status_obj;
        status_obj["validation"] = block_index->status.validation;
        status_obj["failure"] = block_index->status.failure;
        block_data["status"] = status_obj;
      }

      // Previous block hash (for reconstruction)
      if (block_index->pprev) {
        block_data["prev_hash"] = block_index->pprev->GetBlockHash().ToString();
      } else {
        block_data["prev_hash"] = uint256().ToString(); // Genesis has null prev
      }

      blocks.push_back(block_data);
    }

    root["blocks"] = blocks;

    // Write to file
    std::ofstream file(filepath);
    if (!file.is_open()) {
      LOG_CHAIN_ERROR("Failed to open file for writing: {}", filepath);
      return false;
    }

    file << root.dump(2); // Pretty print with 2-space indent
    file.close();

    LOG_CHAIN_TRACE("Successfully saved {} headers", m_block_index.size());
    return true;

  } catch (const std::exception &e) {
    LOG_CHAIN_ERROR("Exception during Save: {}", e.what());
    return false;
  }
}

bool BlockManager::Load(const std::string &filepath, const uint256 &expected_genesis_hash) {
  using json = nlohmann::json;

  try {
    LOG_CHAIN_TRACE("Loading headers from {}", filepath);

    // Open file
    std::ifstream file(filepath);
    if (!file.is_open()) {
      LOG_CHAIN_TRACE("Header file not found: {} (starting fresh)", filepath);
      return false;
    }

    // Parse JSON
    json root;
    file >> root;
    file.close();

    // Validate format version
    int version = root.value("version", 0);
    if (version != 1) {
      LOG_CHAIN_ERROR("Unsupported header file version: {}", version);
      return false;
    }

    size_t block_count = root.value("block_count", 0);
    std::string genesis_hash_str = root.value("genesis_hash", "");
    std::string tip_hash_str = root.value("tip_hash", "");

    LOG_CHAIN_TRACE("Loading {} headers, genesis: {}, tip: {}", block_count,
                   genesis_hash_str, tip_hash_str);

    // Validate genesis block hash matches expected network
    uint256 loaded_genesis_hash;
    loaded_genesis_hash.SetHex(genesis_hash_str);
    if (loaded_genesis_hash != expected_genesis_hash) {
      LOG_CHAIN_ERROR("GENESIS MISMATCH: Loaded genesis {} does not match "
                      "expected genesis {}",
                      genesis_hash_str.substr(0, 16), expected_genesis_hash.ToString().substr(0, 16));
      LOG_CHAIN_ERROR(
          "This datadir contains headers from a different network!");
      LOG_CHAIN_ERROR(
          "Please delete the headers file or use a different datadir.");
      return false;
    }

    LOG_CHAIN_TRACE("Genesis block validation passed: {}", genesis_hash_str);

    // Clear existing state
    m_block_index.clear();
    m_active_chain.Clear();

    // Validate blocks field exists and is an array
    if (!root.contains("blocks")) {
      LOG_CHAIN_ERROR("Header file missing 'blocks' field");
      return false;
    }
    if (!root["blocks"].is_array()) {
      LOG_CHAIN_ERROR("'blocks' field is not an array");
      return false;
    }

    const json &blocks = root["blocks"];

    // Verify block_count matches actual array size (detect corruption/truncation)
    if (blocks.size() != block_count) {
      LOG_CHAIN_WARN("Block count mismatch: header says {}, array has {}. "
                    "File may be corrupted or truncated. Using actual array size.",
                    block_count, blocks.size());
      block_count = blocks.size();
    }

    // First pass: Create all CBlockIndex objects (without connecting pprev)
    std::map<uint256, std::pair<CBlockIndex *, uint256>>
        block_map; // hash -> (pindex, prev_hash), expected size: blocks.size()

    for (const auto &block_data : blocks) {
    // Validate required fields are present (canonical set)
    static const std::vector<std::string> required_fields_core = {
        "hash", "prev_hash", "version", "miner_address", "time",
        "bits", "nonce", "hash_randomx", "height", "chainwork", "status"
      };
    for (const auto &field : required_fields_core) {
      if (!block_data.contains(field)) {
        LOG_CHAIN_ERROR("Block entry missing required field '{}'. File corrupted.", field);
        return false;
      }
    }
    // Validate status object
    if (!block_data["status"].is_object() ||
        !block_data["status"].contains("validation") ||
        !block_data["status"].contains("failure")) {
      LOG_CHAIN_ERROR("Block entry 'status' missing 'validation' or 'failure' fields.");
      return false;
    }

      // Parse block data
      uint256 hash;
      hash.SetHex(block_data["hash"].get<std::string>());

      uint256 prev_hash;
      prev_hash.SetHex(block_data["prev_hash"].get<std::string>());

      // Create header
      CBlockHeader header;
      header.nVersion = block_data["version"].get<int32_t>();
      header.minerAddress.SetHex(block_data["miner_address"].get<std::string>());
      header.nTime = block_data["time"].get<uint32_t>();
      header.nBits = block_data["bits"].get<uint32_t>();
      header.nNonce = block_data["nonce"].get<uint32_t>();
      header.hashRandomX.SetHex(block_data["hash_randomx"].get<std::string>());
      header.hashPrevBlock = prev_hash;

      // Verify reconstructed header hash matches stored hash
      // This detects corruption, tampering, or missing fields in the JSON
      uint256 recomputed_hash = header.GetHash();
      if (recomputed_hash != hash) {
        LOG_CHAIN_ERROR("CORRUPTION DETECTED: Stored hash {} does not match "
                       "recomputed hash {} for block at height {}. "
                       "The header file may be corrupted or tampered. "
                       "Please delete {} and resync.",
                       hash.ToString().substr(0, 16), recomputed_hash.ToString().substr(0, 16),
                       block_data.value("height", -1), filepath);
        return false;
      }

      // Add to block index (use try_emplace to construct CBlockIndex in-place)
      auto [iter, inserted] = m_block_index.try_emplace(hash, header);

      // Restore status from canonical composite object
      const auto &st = block_data["status"];
      iter->second.status.validation = static_cast<BlockStatus::ValidationLevel>(st["validation"].get<int>());
      iter->second.status.failure = static_cast<BlockStatus::FailureState>(st["failure"].get<int>());
      if (!inserted) {
        LOG_CHAIN_ERROR("Duplicate block in header file: {}", hash.ToString().substr(0, 16));
        return false;
      }

      CBlockIndex *pindex = &iter->second;
      // Same pointer stability requirement as AddToBlockIndex
      static_assert(std::is_same<decltype(m_block_index), std::map<uint256, CBlockIndex>>::value,
                    "m_block_index must be std::map for pointer stability");
      pindex->phashBlock = &iter->first;

      // Restore metadata
      pindex->nHeight = block_data["height"].get<int>();
      pindex->nChainWork.SetHex(block_data["chainwork"].get<std::string>());

      // Store for second pass
      block_map[hash] = {pindex, prev_hash};
    }

    // Second pass: Connect pprev pointers
    for (auto &[hash, data] : block_map) {
      CBlockIndex *pindex = data.first;
      const uint256 &prev_hash = data.second;

      if (!prev_hash.IsNull()) {
        pindex->pprev = LookupBlockIndex(prev_hash);
        if (!pindex->pprev) {
          LOG_CHAIN_ERROR("Parent block not found for {}: {}", hash.ToString().substr(0, 16),
                          prev_hash.ToString().substr(0, 16));
          return false;
        }
      } else {
        pindex->pprev = nullptr; // Genesis
      }
    }

    // Validate genesis uniqueness: exactly one block with pprev == nullptr
    CBlockIndex *found_genesis = nullptr;
    int genesis_count = 0;
    for (auto &kv : m_block_index) {
      CBlockIndex *pindex = &kv.second;
      if (pindex->pprev == nullptr) {
        genesis_count++;
        found_genesis = pindex;
      }
    }

    if (genesis_count == 0) {
      LOG_CHAIN_ERROR("No genesis block found (no block with pprev == nullptr)");
      return false;
    }
    if (genesis_count > 1) {
      LOG_CHAIN_ERROR("Multiple genesis blocks found ({} blocks with pprev == nullptr). "
                     "File corrupted - should have exactly one genesis.",
                     genesis_count);
      return false;
    }

    // Verify the genesis block hash matches expected
    if (found_genesis->GetBlockHash() != expected_genesis_hash) {
      LOG_CHAIN_ERROR("Genesis block hash mismatch: found {} but expected {}",
                     found_genesis->GetBlockHash().ToString().substr(0, 16),
                     expected_genesis_hash.ToString().substr(0, 16));
      return false;
    }

    // Third pass: Compute nTimeMax in height order to ensure monotonicity
    std::vector<CBlockIndex*> by_height;
    by_height.reserve(m_block_index.size());
    for (auto &kv : m_block_index) {
      by_height.push_back(&kv.second);
    }
    std::sort(by_height.begin(), by_height.end(), [](const CBlockIndex* a, const CBlockIndex* b){
      return a->nHeight < b->nHeight;
    });
    for (CBlockIndex* pindex : by_height) {
      // Height must be non-negative
      if (pindex->nHeight < 0) {
        LOG_CHAIN_ERROR("INVARIANT VIOLATION: Block {} has negative height {}",
                        pindex->GetBlockHash().ToString().substr(0,16), pindex->nHeight);
        return false;
      }

      if (pindex->pprev) {
        // Defensive: Verify parent-child height invariant (exactly +1)
        // Because we're iterating in height order, parent must have lower height and be exactly one less
        if (pindex->pprev->nHeight + 1 != pindex->nHeight) {
          LOG_CHAIN_ERROR("INVARIANT VIOLATION: Block {} (height {}) has parent {} (height {}). "
                          "Expected parent height to be child-1.",
                          pindex->GetBlockHash().ToString().substr(0, 16), pindex->nHeight,
                          pindex->pprev->GetBlockHash().ToString().substr(0, 16), pindex->pprev->nHeight);
          return false;
        }
        pindex->nTimeMax = std::max<int64_t>(pindex->pprev->nTimeMax, pindex->GetBlockTime());
      } else {
        // Genesis must have height 0
        if (pindex->nHeight != 0) {
          LOG_CHAIN_ERROR("INVARIANT VIOLATION: Genesis block {} has non-zero height {}",
                          pindex->GetBlockHash().ToString().substr(0,16), pindex->nHeight);
          return false;
        }
        pindex->nTimeMax = pindex->GetBlockTime();
      }

      // Build skip list pointer for O(log n) ancestor lookup (Bitcoin Core pattern)
      // Must be called after pprev and nHeight are set
      // Safe to do here because we iterate in height order, so ancestors already have pskip set
      pindex->BuildSkip();
    }

    // Restore genesis hash
    m_genesis_hash.SetHex(genesis_hash_str);

    // Use saved tip as initial tip (chainwork will be recomputed by ChainstateManager)
    // Defense-in-depth: Don't trust chainwork from disk, let ChainstateManager recompute
    // and select the true best tip after validation
    CBlockIndex *initial_tip = nullptr;

    if (!tip_hash_str.empty()) {
      uint256 saved_tip_hash;
      saved_tip_hash.SetHex(tip_hash_str);
      initial_tip = LookupBlockIndex(saved_tip_hash);

      if (!initial_tip) {
        LOG_CHAIN_ERROR("Saved tip {} not found in loaded headers! Data may be corrupted.",
                       tip_hash_str.substr(0, 16));
        return false;
      }

      if (initial_tip->status.IsFailed()) {
        LOG_CHAIN_ERROR("Saved tip {} is marked as failed! Data may be corrupted or you need to resync after invalidateblock.",
                       tip_hash_str.substr(0, 16));
        return false;
      }
    } else {
      LOG_CHAIN_ERROR("No saved tip found in headers file!");
      return false;
    }

    m_active_chain.SetTip(*initial_tip);
    LOG_CHAIN_TRACE("Set active chain to saved tip: height={} hash={}",
                   initial_tip->nHeight, initial_tip->GetBlockHash().ToString().substr(0, 16));

    // Defensive: Verify chain continuity from initial_tip to genesis
    // This catches corrupted pprev pointers, broken chains, or height inconsistencies
    std::string error_msg;
    if (!VerifyChainContinuity(initial_tip, m_genesis_hash, error_msg)) {
      LOG_CHAIN_ERROR("Chain continuity verification failed during Load: {}", error_msg);
      LOG_CHAIN_ERROR("Initial tip does not form valid chain to genesis!");
      return false;
    }

    m_initialized = true;
    LOG_CHAIN_TRACE("Successfully loaded {} headers", m_block_index.size());
    return true;

  } catch (const std::exception &e) {
    LOG_CHAIN_ERROR("Exception during Load: {}", e.what());
    m_block_index.clear();
    m_active_chain.Clear();
    m_initialized = false;
    return false;
  }
}

} // namespace chain
} // namespace unicity
