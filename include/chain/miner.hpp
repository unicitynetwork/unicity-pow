// Copyright (c) 2025 The Unicity Foundation
// Distributed under the MIT software license

#pragma once

#include "chain/chainparams.hpp"
#include "chain/block.hpp"
#include "util/uint.hpp"
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace unicity {

// Forward declarations
namespace chain {
class BlockManager;
class CBlockIndex;
} // namespace chain

namespace validation {
class ChainstateManager;
}

namespace mining {

// Block template - header ready for mining
struct BlockTemplate {
  CBlockHeader header;   // Block header to mine
  uint32_t nBits;        // Difficulty target
  int nHeight;           // Block height
  uint256 hashPrevBlock; // Previous block hash
};

// CPU Miner - Single-threaded RandomX mining for regtest
// Atomics for safe RPC access, designed for regtest/testing only

class CPUMiner {
public:
  CPUMiner(const chain::ChainParams &params,
           validation::ChainstateManager &chainstate);
  ~CPUMiner();

  bool Start(int target_height = -1);  // -1 = mine forever
  void Stop();

  bool IsMining() const { return mining_.load(); }
  double GetHashrate() const;
  uint64_t GetTotalHashes() const { return total_hashes_.load(); }
  int GetBlocksFound() const { return blocks_found_.load(); }

  // Set mining address for block rewards
  // Address is "sticky" - persists across mining sessions until explicitly changed
  // Can be called before Start() or while mining is stopped
  // Thread-safe: uses mutex protection
  void SetMiningAddress(const uint160& address) {
    std::lock_guard<std::mutex> lock(address_mutex_);
    mining_address_ = address;
  }

  uint160 GetMiningAddress() const {
    std::lock_guard<std::mutex> lock(address_mutex_);
    return mining_address_;
  }

  // Invalidate current block template (called when chain tip changes)
  // Thread-safe: uses atomic flag checked by mining thread
  void InvalidateTemplate() { template_invalidated_.store(true); }

  // === Test/Diagnostic Methods ===
  // These methods are intentionally public but should only be used in tests

  // Test-only: expose selected internals for deterministic unit testing
  bool DebugShouldRegenerateTemplate(const uint256& prev_hash) { return ShouldRegenerateTemplate(prev_hash); }
  BlockTemplate DebugCreateBlockTemplate() { return CreateBlockTemplate(); }

private:
  void MiningWorker();
  BlockTemplate CreateBlockTemplate();
  bool ShouldRegenerateTemplate(const uint256& prev_hash); // e.g., chain tip changed
  // Chain params
  const chain::ChainParams &params_;
  validation::ChainstateManager &chainstate_;

  // Mining configuration
  uint160 mining_address_; // Address to receive block rewards
  mutable std::mutex address_mutex_; // Protects mining_address_ from concurrent access

  // Mining state (atomics for RPC thread safety)
  std::atomic<bool> mining_{false};
  std::atomic<uint64_t> total_hashes_{0};
  std::atomic<int> blocks_found_{0};
  std::atomic<bool> template_invalidated_{false};
  std::atomic<int> target_height_{-1};  // -1 = mine forever, else stop at height

  // Mining timing (for hashrate calculation)
  std::chrono::steady_clock::time_point start_time_;
  mutable std::mutex time_mutex_; // Protects start_time_ from concurrent access

  // Mining thread
  std::thread worker_;
  mutable std::mutex stop_mutex_; // Protects Stop() from concurrent calls
};

} // namespace mining
} // namespace unicity


