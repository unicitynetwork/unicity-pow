// Copyright (c) 2025 The Unicity Foundation
// Distributed under the MIT software license

#pragma once

#include <stddef.h>
#include "chain/block_index.hpp"
#include "chain/chain.hpp"
#include "util/uint.hpp"
#include <map>
#include <string>

// Forward declaration
class CBlockHeader;

namespace unicity {
namespace chain {

// BlockManager - Manages all known block headers and the active chain
// Simplified from Bitcoin Core for headers-only chain
//
// THREAD SAFETY: NO internal synchronization - caller MUST serialize all access
// BlockManager is a PRIVATE member of ChainstateManager
// ChainstateManager::validation_mutex_ protects ALL BlockManager methods
// ALL public methods (Initialize, AddToBlockIndex, LookupBlockIndex, Save, Load, etc.)
// MUST be called while holding ChainstateManager::validation_mutex_
// m_block_index, m_active_chain, and m_initialized are NOT thread-safe
// Concurrent access without external locking will cause data races and undefined behavior

class BlockManager {
public:
  BlockManager();
  ~BlockManager();

  bool Initialize(const CBlockHeader &genesis);

  // Look up block by hash (returns nullptr if not found)
  CBlockIndex *LookupBlockIndex(const uint256 &hash);
  const CBlockIndex *LookupBlockIndex(const uint256 &hash) const;

  // Add new block header to index (returns pointer to CBlockIndex, existing or
  // new) Creates CBlockIndex, sets parent pointer, calculates height and chain
  // work
  CBlockIndex *AddToBlockIndex(const CBlockHeader &header);

  CChain &ActiveChain() { return m_active_chain; }
  const CChain &ActiveChain() const { return m_active_chain; }

  CBlockIndex *GetTip() { return m_active_chain.Tip(); }
  const CBlockIndex *GetTip() const { return m_active_chain.Tip(); }

  // Set new tip for active chain (populates entire vChain vector by walking backwards)
  void SetActiveTip(CBlockIndex &block) { m_active_chain.SetTip(block); }

  size_t GetBlockCount() const { return m_block_index.size(); }

  // Read-only access to block index (for checking if block has children)
  const std::map<uint256, CBlockIndex> &GetBlockIndex() const {
    return m_block_index;
  }

  bool Save(const std::string &filepath) const;

  // Load headers from disk (reconstructs block index and active chain)
  // Returns true if loaded successfully and passes validation checks
  bool Load(const std::string &filepath, const uint256 &expected_genesis_hash);

private:
  // Map of all known blocks: hash -> CBlockIndex (map owns CBlockIndex objects,
  // keys are what phashBlock points to)
  std::map<uint256, CBlockIndex> m_block_index;

  // Active (best) chain (points to CBlockIndex objects owned by m_block_index)
  CChain m_active_chain;

  uint256 m_genesis_hash; // Genesis block hash (for validation)
  bool m_initialized{false};
};

} // namespace chain
} // namespace unicity


