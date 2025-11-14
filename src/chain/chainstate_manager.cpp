// Copyright (c) 2025 The Unicity Foundation
// Distributed under the MIT software license

#include "chain/chainstate_manager.hpp"
#include <cassert>
#include <cmath>
#include <compare>
#include <ctime>
#include <utility>
#include "util/arith_uint256.hpp"
#include "chain/block_index.hpp"
#include "chain/block_manager.hpp"
#include "chain/chain.hpp"
#include "chain/chainparams.hpp"
#include "util/logging.hpp"
#include "chain/notifications.hpp"
#include "chain/pow.hpp"
#include "chain/randomx_pow.hpp"
#include "util/time.hpp"
#include "chain/validation.hpp"
#include "network/protocol.hpp"

namespace unicity {
namespace validation {

// Format UNIX timestamp as "YYYY-MM-DD HH:MM:SS UTC" (Bitcoin Core style)
static std::string FormatDateTimeUTC(int64_t t) {
  std::time_t tt = static_cast<std::time_t>(t);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &tt);
#else
  gmtime_r(&tt, &tm);
#endif
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
  return std::string(buf);
}

ChainstateManager::ChainstateManager(const chain::ChainParams &params)
    : block_manager_(), params_(params),
      suspicious_reorg_depth_(params.GetConsensus().nSuspiciousReorgDepth) {}

chain::CBlockIndex *
ChainstateManager::AcceptBlockHeader(const CBlockHeader &header,
                                     ValidationState &state, bool min_pow_checked) {
  std::lock_guard<std::recursive_mutex> lock(validation_mutex_);

  uint256 hash = header.GetHash();
  LOG_CHAIN_TRACE("AcceptBlockHeader: hash={} prev={}",
                  hash.ToString().substr(0, 16),
                  header.hashPrevBlock.ToString().substr(0, 16));

  // Step 1: Check for duplicate
  chain::CBlockIndex *pindex = block_manager_.LookupBlockIndex(hash);
  if (pindex) {
    // Block header is already known
    if (pindex->status.IsFailed()) {
      LOG_CHAIN_DEBUG("AcceptBlockHeader: block {} is marked invalid",
                      hash.ToString());
      state.Invalid("duplicate", "known invalid header re-announced");
      return nullptr;
    }
    LOG_CHAIN_TRACE("Block header {} already exists and is valid, returning existing",
                    hash.ToString().substr(0, 16));
    return pindex;
  }

  // Step 2: Cheap POW commitment check (anti-DoS prefilter)
  if (!CheckProofOfWork(header, crypto::POWVerifyMode::COMMITMENT_ONLY)) {
    state.Invalid("high-hash", "proof of work commitment failed");
    LOG_CHAIN_ERROR("Block header {} failed POW commitment check",
                    hash.ToString().substr(0, 16));
    return nullptr;
  }

  // Step 3: Check if this is a genesis block (must be initialized separately)
  if (header.hashPrevBlock.IsNull()) {
    if (hash != params_.GetConsensus().hashGenesisBlock) {
      state.Invalid("bad-genesis", "genesis block hash mismatch");
      LOG_CHAIN_ERROR("Rejected fake genesis block: {} (expected: {})",
                      hash.ToString(),
                      params_.GetConsensus().hashGenesisBlock.ToString());
      return nullptr;
    }
    state.Invalid("genesis-via-accept", "genesis block must be added via Initialize()");
    return nullptr;
  }

  // Step 4: Parent must exist in index
  chain::CBlockIndex *pindexPrev = block_manager_.LookupBlockIndex(header.hashPrevBlock);
  if (!pindexPrev) {
    LOG_CHAIN_DEBUG("AcceptBlockHeader: header {} has prev block not found: {}",
                    hash.ToString(), header.hashPrevBlock.ToString());
    state.Invalid("prev-blk-not-found", "parent block not found");
    return nullptr;
  }

  // Step 5: Parent must not be invalid
  if (pindexPrev->status.IsFailed()) {
    LOG_CHAIN_DEBUG("AcceptBlockHeader: header {} has prev block invalid: {}",
                    hash.ToString(), header.hashPrevBlock.ToString());
    state.Invalid("bad-prevblk", "previous block is invalid");
    return nullptr;
  }

  // Step 6: Descends from any known invalid block? mark BLOCK_FAILED_CHILD along path and fail
  if (!pindexPrev->IsValid(chain::BlockStatus::TREE)) {
    for (chain::CBlockIndex *failedit : m_failed_blocks) {
      if (pindexPrev->GetAncestor(failedit->nHeight) == failedit) {
        chain::CBlockIndex *invalid_walk = pindexPrev;
        while (invalid_walk != failedit) {
          invalid_walk->status.MarkAncestorFailed();
          invalid_walk = invalid_walk->pprev;
        }
        LOG_CHAIN_TRACE("Header {} descends from invalid block", hash.ToString().substr(0, 16));
        state.Invalid("bad-prevblk", "previous block descends from invalid block");
        return nullptr;
      }
    }
  }

  // Step 7: Contextual checks (timestamp, difficulty) using parent
  int64_t adjusted_time = GetAdjustedTime();
  if (!ContextualCheckBlockHeaderWrapper(header, pindexPrev, adjusted_time, state)) {
    LOG_CHAIN_ERROR("Contextual check failed for {}: {} - {}",
                    hash.ToString().substr(0, 16), state.GetRejectReason(), state.GetDebugMessage());
    return nullptr;
  }

  // Step 8: Full PoW (RandomX) after context to ensure correct epoch
  if (!CheckBlockHeaderWrapper(header, state)) {
    LOG_CHAIN_ERROR("Full PoW check failed for {}: {} - {}",
                    hash.ToString().substr(0, 16), state.GetRejectReason(), state.GetDebugMessage());
    return nullptr;
  }

  // Step 9: Anti-DoS gate – require caller to have validated sufficient work
  // SECURITY: Follows Bitcoin Core pattern (TryLowWorkHeadersSync). Caller MUST perform
  // batch-level work validation using GetAntiDoSWorkThreshold() BEFORE calling this function.
  // See HeaderSyncManager::ProcessHeaders() for reference implementation.
  // This parameter is a defense-in-depth gate to prevent programming errors.
  if (!min_pow_checked) {
    LOG_CHAIN_DEBUG("AcceptBlockHeader: not adding new block header {}, missing anti-dos proof-of-work validation",
                    hash.ToString());
    state.Invalid("too-little-chainwork", "missing anti-DoS work validation");
    return nullptr;
  }

  // Step 10: Insert into block index
  pindex = block_manager_.AddToBlockIndex(header);
  if (!pindex) {
    state.Error("failed to add block to index");
    return nullptr;
  }
  pindex->nTimeReceived = util::GetTime();

  // Mark validity and update best header
  [[maybe_unused]] bool raised = pindex->RaiseValidity(chain::BlockStatus::TREE);
  chain_selector_.UpdateBestHeader(pindex);

  // Log new header acceptance (matches Bitcoin Core for selfish mining detection)
  // These messages are valuable for detecting potential selfish mining behavior;
  // if multiple displacing headers are seen near simultaneously across many
  // nodes in the network, this might be an indication of selfish mining.
  if (IsInitialBlockDownload()) {
    LOG_CHAIN_DEBUG("Saw new header hash={} height={}",
                    hash.ToString(), pindex->nHeight);
  } else {
    LOG_CHAIN_INFO("Saw new header hash={} height={}",
                   hash.ToString(), pindex->nHeight);
  }

  LOG_CHAIN_TRACE("Accepted new block header: hash={}, height={}, log2_work={:.6f}",
                  hash.ToString().substr(0, 16), pindex->nHeight,
                  std::log(pindex->nChainWork.getdouble()) / std::log(2.0));

  // Process orphan children now that parent exists
  ProcessOrphanHeaders(hash);

  return pindex;
}

bool ChainstateManager::ProcessNewBlockHeader(const CBlockHeader &header,
                                              ValidationState &state,
                                              bool min_pow_checked) {
  // THREAD SAFETY: Uses std::recursive_mutex to make the entire operation
  // atomic All three steps (anti-DoS gate, accept, activate) hold
  // validation_mutex_ without releasing it, preventing race conditions on
  // m_candidates.
  //
  // Without recursive mutex, there would be a race between:
  // 1. AcceptBlockHeader() releasing the lock
  // 2. TryAddBlockIndexCandidate() acquiring it
  // 3. ActivateBestChain() acquiring it
  //
  // During these windows, another thread could modify m_candidates, causing
  // undefined behavior (iterator invalidation, broken std::set ordering, etc.)

  // Accept header (validates + adds to index)
  chain::CBlockIndex *pindex = AcceptBlockHeader(header, state, /*min_pow_checked=*/min_pow_checked);
  if (!pindex) {
    return false;
  }
  // Add to candidate set (if it's a viable tip)
  TryAddBlockIndexCandidate(pindex);
  // Block accepted - now try to activate best chain
  return ActivateBestChain(nullptr);
}

bool ChainstateManager::ActivateBestChain(chain::CBlockIndex *pindexMostWork) {
  std::unique_lock<std::recursive_mutex> lock(validation_mutex_);
  std::vector<PendingNotification> pending_events;

  LOG_CHAIN_TRACE("ActivateBestChain: called with pindexMostWork={}",
                  pindexMostWork ? pindexMostWork->GetBlockHash().ToString().substr(0, 16) : "null");

  // Loop like Core: keep trying next-best candidates until activation succeeds
  bool success = false;
  for (;;) {
    // Resolve candidate if not provided or if previous was consumed
    if (!pindexMostWork) {
      pindexMostWork = chain_selector_.FindMostWorkChain();
      LOG_CHAIN_TRACE("ActivateBestChain: Found most work chain: {}",
                      pindexMostWork ? pindexMostWork->GetBlockHash().ToString().substr(0, 16) : "null");
    }

    if (!pindexMostWork) {
      LOG_CHAIN_TRACE("ActivateBestChain: No candidates found (no competing forks)");
      success = true;
      break;
    }

    // If candidate equals current tip, nothing to do
    if (block_manager_.GetTip() == pindexMostWork) {
      success = true;
      break;
    }

    // Network expiration check - refuse to activate blocks BEYOND expiration height
    // Block AT expiration is accepted (fires notification and triggers shutdown)
    if (params_.GetConsensus().nNetworkExpirationInterval > 0) {
      if (pindexMostWork->nHeight > params_.GetConsensus().nNetworkExpirationInterval) {
        LOG_CHAIN_ERROR("Network expired at block {} (received block at height {}). "
                        "This version is outdated. Please update to the latest version.",
                        params_.GetConsensus().nNetworkExpirationInterval, pindexMostWork->nHeight);
        // Fire expiration notification to trigger graceful shutdown
        Notifications().NotifyNetworkExpired(pindexMostWork->nHeight, params_.GetConsensus().nNetworkExpirationInterval);
        return false;
      }
    }

    // Run one activation attempt
    ActivateResult res = ActivateBestChainStep(pindexMostWork, pending_events);

    if (res == ActivateResult::OK) {
      success = true;
      break;
    }

    if (res == ActivateResult::POLICY_REFUSED) {
      // Demote this candidate and try the next
      chain_selector_.RemoveCandidate(pindexMostWork);
      pindexMostWork = nullptr;
      continue;
    }

    if (res == ActivateResult::CONSENSUS_INVALID) {
      // Mark the candidate invalid and try the next
      pindexMostWork->status.MarkFailed();
      m_failed_blocks.insert(pindexMostWork);
      chain_selector_.RemoveCandidate(pindexMostWork);
      pindexMostWork = nullptr;
      continue;
    }

    // SYSTEM_ERROR - give up
    success = false;
    break;
  }

  // Release lock and dispatch notifications
  lock.unlock();
  for (const auto &ev : pending_events) {
    switch (ev.type) {
    case NotifyType::BlockConnected:
      Notifications().NotifyBlockConnected(ev.header, ev.pindex);
      break;
    case NotifyType::BlockDisconnected:
      Notifications().NotifyBlockDisconnected(ev.header, ev.pindex);
      break;
    case NotifyType::ChainTip:
      Notifications().NotifyChainTip(ev.pindex, ev.height);
      break;
    }
  }
  return success;
}

ChainstateManager::ActivateResult ChainstateManager::ActivateBestChainStep(chain::CBlockIndex *pindexMostWork,
                                                                          std::vector<PendingNotification> &events) {
  // PRE: validation_mutex_ is held by caller
  if (!pindexMostWork) {
    return ActivateResult::OK;
  }

  // Get current tip
  chain::CBlockIndex *pindexOldTip = block_manager_.GetTip();

  LOG_CHAIN_TRACE("ActivateBestChainStep: pindexOldTip={} (height={}), pindexMostWork={} (height={})",
                  pindexOldTip ? pindexOldTip->GetBlockHash().ToString().substr(0, 16) : "null",
                  pindexOldTip ? pindexOldTip->nHeight : -1,
                  pindexMostWork->GetBlockHash().ToString().substr(0, 16),
                  pindexMostWork->nHeight);

  // Already at best tip?
  if (pindexOldTip == pindexMostWork) {
    return ActivateResult::OK;
  }

  // Require strictly more work to switch
  if (pindexOldTip && pindexMostWork->nChainWork <= pindexOldTip->nChainWork) {
    LOG_CHAIN_TRACE("Candidate has insufficient work; keeping current tip. Height: {}, Hash: {}",
                    pindexMostWork->nHeight,
                    pindexMostWork->GetBlockHash().ToString().substr(0, 16));
    return ActivateResult::OK;
  }

  // Find LCA
  const chain::CBlockIndex *pindexFork = chain::LastCommonAncestor(pindexOldTip, pindexMostWork);
  LOG_CHAIN_TRACE("ActivateBestChainStep: fork_point={} (height={})",
                  pindexFork ? pindexFork->GetBlockHash().ToString().substr(0, 16) : "null",
                  pindexFork ? pindexFork->nHeight : -1);

  if (!pindexFork) {
    if (!pindexOldTip) {
      LOG_CHAIN_TRACE("ActivateBestChainStep: No fork point and no old tip (initial activation). Proceeding.");
    } else {
      LOG_CHAIN_ERROR("ActivateBestChainStep: No common ancestor between old tip and candidate");
      return ActivateResult::CONSENSUS_INVALID;
    }
  }

  // Policy guard: suspicious reorg depth
  int reorg_depth = 0;
  if (pindexOldTip && pindexFork) {
    reorg_depth = pindexOldTip->nHeight - pindexFork->nHeight;
    LOG_CHAIN_TRACE("ActivateBestChainStep: reorg_depth={}, suspicious_reorg_depth_={}",
                    reorg_depth, suspicious_reorg_depth_);
    if (suspicious_reorg_depth_ > 0 && reorg_depth >= suspicious_reorg_depth_) {
      LOG_CHAIN_ERROR("CRITICAL: Suspicious reorg of {} blocks (policy max {}). Refusing.",
                      reorg_depth, suspicious_reorg_depth_ - 1);
      LOG_CHAIN_ERROR("* current tip @ height {} ({})", pindexOldTip->nHeight,
                      pindexOldTip->GetBlockHash().ToString());
      LOG_CHAIN_ERROR("*   reorg tip @ height {} ({})", pindexMostWork->nHeight,
                      pindexMostWork->GetBlockHash().ToString());
      LOG_CHAIN_ERROR("*  fork point @ height {} ({})", pindexFork ? pindexFork->nHeight : -1,
                      pindexFork ? pindexFork->GetBlockHash().ToString() : std::string("null"));
      Notifications().NotifySuspiciousReorg(reorg_depth, suspicious_reorg_depth_ - 1);
      return ActivateResult::POLICY_REFUSED;
    }
  }

  // Disconnect back to fork
  std::vector<chain::CBlockIndex *> disconnected_blocks;
  // Local event buffer; only appended to 'events' on success
  std::vector<PendingNotification> local_events;
  if (pindexOldTip && pindexFork) {
    LOG_CHAIN_TRACE("ActivateBestChainStep: Disconnecting {} blocks back to fork",
                    pindexOldTip->nHeight - pindexFork->nHeight);
    chain::CBlockIndex *pindexWalk = pindexOldTip;
    while (pindexWalk && pindexWalk != pindexFork) {
      disconnected_blocks.push_back(pindexWalk);
      LOG_CHAIN_TRACE("ActivateBestChainStep: Disconnecting block height={} hash={}",
                      pindexWalk->nHeight, pindexWalk->GetBlockHash().ToString().substr(0, 16));
      if (!DisconnectTip(local_events)) {
        LOG_CHAIN_ERROR("Failed to disconnect block during reorg");
        return ActivateResult::SYSTEM_ERROR;
      }
      pindexWalk = block_manager_.GetTip();
    }
  }

  // Build forward connect list
  std::vector<chain::CBlockIndex *> connect_blocks;
  chain::CBlockIndex *pindexWalk = pindexMostWork;
  while (pindexWalk && pindexWalk != pindexFork) {
    connect_blocks.push_back(pindexWalk);
    pindexWalk = pindexWalk->pprev;
  }

  LOG_CHAIN_TRACE("ActivateBestChainStep: Connecting {} blocks from fork to new tip",
                  connect_blocks.size());

  for (auto it = connect_blocks.rbegin(); it != connect_blocks.rend(); ++it) {
    LOG_CHAIN_TRACE("ActivateBestChainStep: Connecting block height={} hash={}",
                    (*it)->nHeight, (*it)->GetBlockHash().ToString().substr(0, 16));
    if (!ConnectTip(*it, local_events)) {
      LOG_CHAIN_ERROR("Failed to connect block during reorg at height {}", (*it)->nHeight);

      // Roll back to fork
      LOG_CHAIN_TRACE("Attempting rollback to fork...");
      while (block_manager_.GetTip() != pindexFork) {
        if (!DisconnectTip(local_events)) {
          LOG_CHAIN_ERROR("CRITICAL: Rollback failed! Chain state may be inconsistent!");
          return ActivateResult::SYSTEM_ERROR;
        }
      }

      // Reconnect old chain
      for (auto rit = disconnected_blocks.rbegin(); rit != disconnected_blocks.rend(); ++rit) {
        if (!ConnectTip(*rit, local_events)) {
          LOG_CHAIN_ERROR("CRITICAL: Failed to restore old chain! Chain state may be inconsistent!");
          return ActivateResult::SYSTEM_ERROR;
        }
      }

      const chain::CBlockIndex *restored_tip = block_manager_.GetTip();
      if (!restored_tip) {
        LOG_CHAIN_ERROR("CRITICAL: Rollback completed but tip is null! Inconsistent state!");
        return ActivateResult::SYSTEM_ERROR;
      }

      LOG_CHAIN_TRACE("Rollback successful - restored old tip at height {}", restored_tip->nHeight);

      // Treat this branch as invalid (consensus failure path)
      return ActivateResult::CONSENSUS_INVALID;
    }
  }

  if (!disconnected_blocks.empty()) {
    assert(pindexOldTip && "pindexOldTip must be non-null if blocks were disconnected");
    LOG_CHAIN_INFO("REORGANIZE: Disconnect {} blocks; Connect {} blocks",
                   disconnected_blocks.size(), connect_blocks.size());
    LOG_CHAIN_DEBUG("REORGANIZE: Old tip: height={}, hash={}", pindexOldTip->nHeight,
                    pindexOldTip->GetBlockHash().ToString().substr(0, 16));
    LOG_CHAIN_DEBUG("REORGANIZE: New tip: height={}, hash={}", pindexMostWork->nHeight,
                    pindexMostWork->GetBlockHash().ToString().substr(0, 16));
    LOG_CHAIN_DEBUG("REORGANIZE: Fork point: height={}, hash={}",
                    pindexFork ? pindexFork->nHeight : -1,
                    pindexFork ? pindexFork->GetBlockHash().ToString().substr(0, 16) : "null");
  }

  // Final tip notification (headers-only: emit explicit notification)
  local_events.push_back(PendingNotification{NotifyType::ChainTip, CBlockHeader{}, pindexMostWork, pindexMostWork->nHeight});

  // Prune stale candidates now that tip advanced
  chain_selector_.PruneBlockIndexCandidates(block_manager_);

  if (disconnected_blocks.empty()) {
    LOG_CHAIN_INFO("New best chain activated! Height: {}, Hash: {}, log2_work: {:.6f}",
                   pindexMostWork->nHeight,
                   pindexMostWork->GetBlockHash().ToString().substr(0, 16),
                   std::log(pindexMostWork->nChainWork.getdouble()) / std::log(2.0));
  }

  // Success: append local events to output buffer
  events.insert(events.end(), local_events.begin(), local_events.end());

  // Check if we just connected block AT expiration height - fire notification
  // (Block BEYOND expiration is already refused in ActivateBestChain before calling this function)
  if (params_.GetConsensus().nNetworkExpirationInterval > 0) {
    if (pindexMostWork->nHeight == params_.GetConsensus().nNetworkExpirationInterval) {
      LOG_CHAIN_ERROR("Network reached expiration block {} - this version will stop accepting new blocks. "
                      "Please update to the latest version.",
                      params_.GetConsensus().nNetworkExpirationInterval);
      // Fire expiration notification immediately (not through pending events)
      // This triggers shutdown in application.cpp
      Notifications().NotifyNetworkExpired(pindexMostWork->nHeight, params_.GetConsensus().nNetworkExpirationInterval);
    }
  }

  return ActivateResult::OK;
}

const chain::CBlockIndex *ChainstateManager::GetTip() const {
  std::lock_guard<std::recursive_mutex> lock(validation_mutex_);
  return block_manager_.GetTip();
}

chain::CBlockIndex *ChainstateManager::LookupBlockIndex(const uint256 &hash) {
  std::lock_guard<std::recursive_mutex> lock(validation_mutex_);
  return block_manager_.LookupBlockIndex(hash);
}

const chain::CBlockIndex *
ChainstateManager::LookupBlockIndex(const uint256 &hash) const {
  std::lock_guard<std::recursive_mutex> lock(validation_mutex_);
  return block_manager_.LookupBlockIndex(hash);
}

CBlockLocator
ChainstateManager::GetLocator(const chain::CBlockIndex *pindex) const {
  std::lock_guard<std::recursive_mutex> lock(validation_mutex_);
  if (pindex) {
    return chain::GetLocator(pindex);
  }
  return block_manager_.ActiveChain().GetLocator();
}

bool ChainstateManager::IsOnActiveChain(
    const chain::CBlockIndex *pindex) const {
  std::lock_guard<std::recursive_mutex> lock(validation_mutex_);
  return pindex && block_manager_.ActiveChain().Contains(pindex);
}

const chain::CBlockIndex *
ChainstateManager::GetBlockAtHeight(int height) const {
  std::lock_guard<std::recursive_mutex> lock(validation_mutex_);
  if (height < 0 || height > block_manager_.ActiveChain().Height()) {
    return nullptr;
  }
  return block_manager_.ActiveChain()[height];
}

bool ChainstateManager::ConnectTip(chain::CBlockIndex *pindexNew,
                                   std::vector<PendingNotification> &events) {
  if (!pindexNew) {
    LOG_CHAIN_ERROR("ConnectTip: null block index");
    return false;
  }

  LOG_CHAIN_TRACE("ConnectTip: connecting block height={} hash={}",
                  pindexNew->nHeight, pindexNew->GetBlockHash().ToString().substr(0, 16));

  // NOTIFICATION SEMANTICS :
  // ConnectTip: Update state BEFORE notifying
  //   - SetActiveTip() is called BEFORE NotifyBlockConnected()
  //   - Subscribers see the NEW tip when they call GetTip()
  //   - This allows subscribers to query the updated chain state
  //
  // DisconnectTip: Notify BEFORE updating state
  //   - NotifyBlockDisconnected() is called BEFORE SetActiveTip()
  //   - Subscribers see the block BEING disconnected when they call GetTip()
  //   - This allows subscribers to query the old state before it's removed
  //
  // CRITICAL: This asymmetry is intentional and matches Bitcoin Core's
  // semantics. Subscribers must be aware:
  //   - On BlockConnected: GetTip() returns the newly connected block
  //   - On BlockDisconnected: GetTip() returns the block being disconnected

  // For headers-only chain, "connecting" just means:
  // 1. Setting this as the active tip
  // 2. Emitting notifications

  block_manager_.SetActiveTip(*pindexNew);

  // Headers-only chain: concise UpdateTip log (no tx/progress/cache)
  const std::string best_hash = pindexNew->GetBlockHash().ToString().substr(0, 16);
  const double log2_work = std::log(pindexNew->nChainWork.getdouble()) / std::log(2.0);
  const std::string date_str = FormatDateTimeUTC(pindexNew->GetBlockTime());
  const uint32_t version = static_cast<uint32_t>(pindexNew->nVersion);
  LOG_CHAIN_INFO(
      "UpdateTip: new best={} height={} version=0x{:08x} log2_work={:.6f} date='{}'",
      best_hash, pindexNew->nHeight, version, log2_work, date_str);

  // Queue block connected notification AFTER updating tip
  CBlockHeader header = pindexNew->GetBlockHeader();
  events.push_back(PendingNotification{NotifyType::BlockConnected, header, pindexNew, pindexNew->nHeight});

  return true;
}

bool ChainstateManager::AddOrphanHeader(const CBlockHeader &header, int peer_id) {
  std::lock_guard<std::recursive_mutex> lock(validation_mutex_);
  return TryAddOrphanHeader(header, peer_id);
}

bool ChainstateManager::DisconnectTip(std::vector<PendingNotification> &events) {
  chain::CBlockIndex *pindexDelete = block_manager_.GetTip();
  if (!pindexDelete) {
    LOG_CHAIN_ERROR("DisconnectTip: no tip to disconnect");
    return false;
  }

  if (!pindexDelete->pprev) {
    LOG_CHAIN_ERROR("DisconnectTip: cannot disconnect genesis block");
    return false;
  }

  LOG_CHAIN_TRACE("DisconnectTip: disconnecting block height={} hash={}",
                  pindexDelete->nHeight, pindexDelete->GetBlockHash().ToString().substr(0, 16));

  // NOTIFICATION SEMANTICS:
  // Notify BEFORE updating state
  //   - Subscribers receive NotifyBlockDisconnected() while GetTip() still
  //   returns pindexDelete
  //   - This allows subscribers to query the old chain state before the block
  //   is removed
  //   - After notification, SetActiveTip() moves tip back to pprev
  //
  // See ConnectTip() for the complementary (but asymmetric) behavior on block
  // connection.
  CBlockHeader header = pindexDelete->GetBlockHeader();
  events.push_back(PendingNotification{NotifyType::BlockDisconnected, header, pindexDelete, pindexDelete->nHeight});

  // For headers-only chain, "disconnecting" just means:
  // Moving the active tip pointer back to parent
  block_manager_.SetActiveTip(*pindexDelete->pprev);

  return true;
}

void ChainstateManager::TryAddBlockIndexCandidate(chain::CBlockIndex *pindex) {
  std::lock_guard<std::recursive_mutex> lock(validation_mutex_);
  chain_selector_.TryAddBlockIndexCandidate(pindex, block_manager_);
}

bool ChainstateManager::IsInitialBlockDownload() const {
  // Fast path: check latch first (lock-free)
  if (m_cached_finished_ibd.load(std::memory_order_relaxed)) {
    return false;
  }

  // No tip yet - definitely in IBD
  const chain::CBlockIndex *tip = GetTip();
  if (!tip) {
    return true;
  }

  // Genesis (height 0) is considered IBD regardless of time skew or min chain work.
  // This helps fresh networks and simulated environments with mocked time.
  if (tip->nHeight == 0) {
    return true;
  }

  // Tip too old - still syncing (12 hours = 12 blocks for 1-hour block times)
  int64_t now = util::GetTime();
  if (tip->nTime < now - 12 * 3600) {
    return true;
  }

  // MinimumChainWork check (eclipse attack protection)
  // Prevents accepting fake low-work chains during IBD
  if (tip->nChainWork <
      UintToArith256(params_.GetConsensus().nMinimumChainWork)) {
    return true;
  }

  // All checks passed - we're synced!
  // Latch to false permanently
  LOG_CHAIN_INFO("Leaving InitialBlockDownload (latching to false)");
  m_cached_finished_ibd.store(true, std::memory_order_relaxed);

  return false;
}

bool ChainstateManager::Initialize(const CBlockHeader &genesis_header) {
  std::lock_guard<std::recursive_mutex> lock(validation_mutex_);

  if (!block_manager_.Initialize(genesis_header)) {
    return false;
  }

  // Initialize the candidate set with genesis block
  chain::CBlockIndex *genesis = block_manager_.GetTip();
  if (genesis) {
    // Mark genesis as valid to TREE level (it's pre-validated)
    [[maybe_unused]] bool raised =
        genesis->RaiseValidity(chain::BlockStatus::TREE);

    chain_selector_.AddCandidateUnchecked(genesis);
    chain_selector_.SetBestHeader(genesis);
    LOG_CHAIN_TRACE("Initialized with genesis as candidate: height={}, hash={}",
              genesis->nHeight,
              genesis->GetBlockHash().ToString().substr(0, 16));
  }

  return true;
}

bool ChainstateManager::Load(const std::string &filepath) {
  std::lock_guard<std::recursive_mutex> lock(validation_mutex_);

  if (!block_manager_.Load(filepath, params_.GetConsensus().hashGenesisBlock)) {
    return false;
  }

  // Defense-in-depth: Recompute chainwork and re-validate loaded headers instead of trusting on-disk fields.
  // 1) Recompute nChainWork deterministically from nBits and ancestry.
  // 2) Re-derive validity to TREE level using commitment-only PoW and contextual checks.
  {
    // Build height-ordered list
    std::vector<chain::CBlockIndex*> by_height;
    by_height.reserve(block_manager_.GetBlockIndex().size());
    for (auto &kv : block_manager_.GetBlockIndex()) {
      by_height.push_back(const_cast<chain::CBlockIndex*>(&kv.second));
    }
    std::sort(by_height.begin(), by_height.end(), [](const chain::CBlockIndex* a, const chain::CBlockIndex* b){
      return a->nHeight < b->nHeight;
    });

    for (chain::CBlockIndex* pindex : by_height) {
      // Recompute chainwork deterministically
      if (pindex->pprev) {
        pindex->nChainWork = pindex->pprev->nChainWork + chain::GetBlockProof(*pindex);
      } else {
        pindex->nChainWork = chain::GetBlockProof(*pindex); // genesis
      }

      // Recompute nTimeMax
      if (pindex->pprev) {
        pindex->nTimeMax = std::max<int64_t>(pindex->pprev->nTimeMax, pindex->GetBlockTime());
      } else {
        pindex->nTimeMax = pindex->GetBlockTime();
      }

      // Rebuild skip list pointer (safe in height order)
      pindex->BuildSkip();

      // Re-validate to TREE level
      bool valid = true;
      if (pindex->pprev) {
        const CBlockHeader hdr = pindex->GetBlockHeader();
        // Cheap PoW commitment check
        if (!consensus::CheckProofOfWork(hdr, hdr.nBits, params_, crypto::POWVerifyMode::COMMITMENT_ONLY)) {
          valid = false;
        } else {
          // Contextual checks
          ValidationState st;
          if (!ContextualCheckBlockHeaderWrapper(hdr, pindex->pprev, GetAdjustedTime(), st)) {
            valid = false;
          }
        }
      }

      if (!pindex->pprev) {
        // Genesis considered valid to TREE level
        [[maybe_unused]] bool _ = pindex->RaiseValidity(chain::BlockStatus::TREE);
        LOG_CHAIN_TRACE("Load: Genesis validation={} failure={}",
                       (int)pindex->status.validation, (int)pindex->status.failure);
      } else if (valid) {
        [[maybe_unused]] bool _ = pindex->RaiseValidity(chain::BlockStatus::TREE);
        LOG_CHAIN_TRACE("Load: Block {} validation={} failure={} (valid)",
                       pindex->GetBlockHash().ToString().substr(0,16),
                       (int)pindex->status.validation, (int)pindex->status.failure);
      } else {
        // Mark as failed-VALID; clear any ancestor failure and set to validation failed
        pindex->status.failure = chain::BlockStatus::VALIDATION_FAILED;
        m_failed_blocks.insert(pindex);
        LOG_CHAIN_TRACE("Load: Block {} validation={} failure={} (FAILED)",
                       pindex->GetBlockHash().ToString().substr(0,16),
                       (int)pindex->status.validation, (int)pindex->status.failure);
      }
    }
  }

  // Rebuild the candidate set after loading from disk
  // We need to find all leaf nodes (tips) in the block tree
  chain_selector_.ClearCandidates();
  chain_selector_.SetBestHeader(nullptr);

  // Walk through all blocks and find tips (blocks with no known children)
  // Algorithm:
  // 1. Build a set of all blocks that have children (by scanning pprev
  // pointers)
  // 2. Any block NOT in that set is a leaf (potential candidate)
  // 3. Only add valid leaves (BLOCK_VALID_TREE) to candidates

  const auto &block_index = block_manager_.GetBlockIndex();

  // Step 1: Build set of blocks with children
  std::set<const chain::CBlockIndex *> blocks_with_children;
  for (const auto &[hash, block] : block_index) {
    if (block.pprev) {
      blocks_with_children.insert(block.pprev);
    }
  }

  // Step 2: Find all leaf nodes and add valid ones as candidates
  size_t leaf_count = 0;
  size_t candidate_count = 0;
  for (const auto &[hash, block] : block_index) {
    // Check if this block is a leaf (has no children)
    if (blocks_with_children.find(&block) == blocks_with_children.end()) {
      leaf_count++;

      // Only add as candidate if validated to TREE level
      if (block.IsValid(chain::BlockStatus::TREE)) {
        // Need mutable pointer for candidate set
        chain::CBlockIndex *mutable_block =
            const_cast<chain::CBlockIndex *>(&block);
        chain_selector_.AddCandidateUnchecked(mutable_block);
        candidate_count++;

    LOG_CHAIN_TRACE("Added leaf as candidate: height={}, hash={}, log2_work={:.6f}",
                  block.nHeight, hash.ToString().substr(0, 16),
                  std::log(block.nChainWork.getdouble()) / std::log(2.0));

        // Track best header (most chainwork)
        chain_selector_.UpdateBestHeader(mutable_block);
      } else {
        LOG_CHAIN_TRACE("Found invalid leaf (not added to candidates): height={}, "
                  "hash={}, status={}",
                  block.nHeight, hash.ToString().substr(0, 16), block.status.ToString());
      }
    }
  }

  chain::CBlockIndex *tip = block_manager_.GetTip();
  LOG_CHAIN_TRACE(
      "Loaded chain state: {} total blocks, {} leaf nodes, {} valid candidates",
      block_index.size(), leaf_count, candidate_count);

  if (tip) {
    LOG_CHAIN_TRACE("Active chain tip: height={}, hash={}", tip->nHeight,
             tip->GetBlockHash().ToString().substr(0, 16));
  }

  if (chain_selector_.GetBestHeader()) {
    LOG_CHAIN_TRACE(
        "Best header: height={}, hash={}, log2_work={:.6f}",
        chain_selector_.GetBestHeader()->nHeight,
        chain_selector_.GetBestHeader()->GetBlockHash().ToString().substr(0,
                                                                          16),
        std::log(chain_selector_.GetBestHeader()->nChainWork.getdouble()) / std::log(2.0));
  }

  // Defense-in-depth: After recomputing chainwork, select the best candidate as active tip
  // The initial tip from BlockManager was the saved tip, but chainwork may have been corrupted
  // Now that we've recomputed the true chainwork, switch to the actual best chain
  chain::CBlockIndex *best_candidate = chain_selector_.FindMostWorkChain();
  if (best_candidate && best_candidate != tip) {
    block_manager_.SetActiveTip(*best_candidate);
    LOG_CHAIN_INFO("Updated active tip to best candidate after chainwork recomputation: height={}, hash={}",
                   best_candidate->nHeight, best_candidate->GetBlockHash().ToString().substr(0, 16));
  }

  return true;
}

bool ChainstateManager::Save(const std::string &filepath) const {
  std::lock_guard<std::recursive_mutex> lock(validation_mutex_);
  return block_manager_.Save(filepath);
}


size_t ChainstateManager::GetBlockCount() const {
  std::lock_guard<std::recursive_mutex> lock(validation_mutex_);
  return block_manager_.GetBlockCount();
}

int ChainstateManager::GetChainHeight() const {
  std::lock_guard<std::recursive_mutex> lock(validation_mutex_);
  return block_manager_.ActiveChain().Height();
}

void ChainstateManager::ProcessOrphanHeaders(const uint256 &parentHash) {
  // NOTE: Assumes validation_mutex_ is already held by caller

  LOG_CHAIN_TRACE("ProcessOrphanHeaders: parent={} orphan_pool_size={}",
                  parentHash.ToString().substr(0, 16), m_orphan_headers.size());

  std::vector<uint256> orphansToProcess;

  // Find all orphans that have this as parent
  for (const auto &[hash, orphan] : m_orphan_headers) {
    if (orphan.header.hashPrevBlock == parentHash) {
      orphansToProcess.push_back(hash);
      LOG_CHAIN_TRACE("ProcessOrphanHeaders: Found orphan {} waiting for parent",
                      hash.ToString().substr(0, 16));
    }
  }

  if (orphansToProcess.empty()) {
    LOG_CHAIN_TRACE("ProcessOrphanHeaders: No orphans found for parent");
    return;
  }

  LOG_CHAIN_TRACE("Processing {} orphan headers that were waiting for parent {}",
           orphansToProcess.size(), parentHash.ToString().substr(0, 16));

  // Process each orphan (this is recursive - orphan may have orphan children)
  for (const uint256 &hash : orphansToProcess) {
    auto it = m_orphan_headers.find(hash);
    if (it == m_orphan_headers.end()) {
      continue; // Already processed by earlier iteration
    }

    // IMPORTANT: Copy header BEFORE erasing from map to avoid dangling
    // reference
    CBlockHeader orphan_header = it->second.header; // Copy, not reference!
    int orphan_peer_id = it->second.peer_id;

    // Remove from orphan pool BEFORE processing
    // (prevents infinite recursion if orphan is invalid and re-added)
    m_orphan_headers.erase(it);

    // Decrement peer orphan count
    auto peer_it = m_peer_orphan_count.find(orphan_peer_id);
    if (peer_it != m_peer_orphan_count.end()) {
      peer_it->second--;
      if (peer_it->second == 0) {
        m_peer_orphan_count.erase(peer_it);
      }
    }

    // Recursively process the orphan
    LOG_CHAIN_TRACE("Processing orphan header: hash={}, parent={}",
              hash.ToString().substr(0, 16),
              orphan_header.hashPrevBlock.ToString().substr(0, 16));

    ValidationState orphan_state;
    chain::CBlockIndex *pindex =
        AcceptBlockHeader(orphan_header, orphan_state, /*min_pow_checked=*/true);

    if (!pindex) {
      LOG_CHAIN_TRACE("Orphan header {} failed validation: {}",
                hash.ToString().substr(0, 16), orphan_state.GetRejectReason());
      // If it's orphaned again (missing grandparent), it will be re-added to
      // pool If it's invalid, it won't be re-added
    } else {
      LOG_CHAIN_TRACE("Successfully processed orphan header: hash={}, height={}",
               hash.ToString().substr(0, 16), pindex->nHeight);
    }
  }
}

bool ChainstateManager::TryAddOrphanHeader(const CBlockHeader &header,
                                           int peer_id) {
  // NOTE: Assumes validation_mutex_ is already held by caller

  uint256 hash = header.GetHash();

  LOG_CHAIN_TRACE("TryAddOrphanHeader: hash={} prev={} peer={} pool_size={}",
                  hash.ToString().substr(0, 16),
                  header.hashPrevBlock.ToString().substr(0, 16),
                  peer_id, m_orphan_headers.size());

  // Check if already in orphan pool
  if (m_orphan_headers.find(hash) != m_orphan_headers.end()) {
    LOG_CHAIN_TRACE("Orphan header {} already in pool", hash.ToString().substr(0, 16));
    return true;
  }

  // DoS Protection 1: Check per-peer limit
  int peer_orphan_count = m_peer_orphan_count[peer_id];
  LOG_CHAIN_TRACE("TryAddOrphanHeader: peer={} has {}/{} orphans",
                  peer_id, peer_orphan_count, protocol::MAX_ORPHAN_HEADERS_PER_PEER);
  if (peer_orphan_count >= static_cast<int>(protocol::MAX_ORPHAN_HEADERS_PER_PEER)) {
    LOG_CHAIN_TRACE("Peer {} exceeded orphan limit ({}/{}), rejecting orphan {}",
             peer_id, peer_orphan_count, protocol::MAX_ORPHAN_HEADERS_PER_PEER,
             hash.ToString().substr(0, 16));
    return false;
  }

  // DoS Protection 2: Check total limit
  if (m_orphan_headers.size() >= protocol::MAX_ORPHAN_HEADERS) {
    // Evict oldest orphan to make room
    LOG_CHAIN_TRACE("Orphan pool full ({}/{}), evicting oldest",
              m_orphan_headers.size(), protocol::MAX_ORPHAN_HEADERS);

    size_t evicted = EvictOrphanHeaders();
    if (evicted == 0) {
      LOG_CHAIN_ERROR("Failed to evict any orphans, pool stuck at max size");
      return false;
    }
  }

  // Add to orphan pool
  m_orphan_headers[hash] = OrphanHeader{header, util::GetTime(), peer_id};

  // Update peer count
  m_peer_orphan_count[peer_id]++;

  LOG_CHAIN_TRACE("Added orphan header to pool: hash={}, peer={}, pool_size={}, "
            "peer_orphans={}",
            hash.ToString().substr(0, 16), peer_id, m_orphan_headers.size(),
            m_peer_orphan_count[peer_id]);

  return true;
}

size_t ChainstateManager::EvictOrphanHeaders() {
  std::lock_guard<std::recursive_mutex> lock(validation_mutex_);

  if (m_orphan_headers.empty()) {
    return 0;
  }

  int64_t now = util::GetTime();
  size_t evicted = 0;

  // Strategy 1: Evict expired orphans (older than chain-specific timeout)
  auto it = m_orphan_headers.begin();
  while (it != m_orphan_headers.end()) {
    if (now - it->second.nTimeReceived > params_.GetConsensus().nOrphanHeaderExpireTime) {
    LOG_CHAIN_TRACE("Evicting expired orphan header: hash={}, age={}s",
                it->first.ToString().substr(0, 16),
                now - it->second.nTimeReceived);

      // Decrement peer count
      int peer_id = it->second.peer_id;
      auto peer_it = m_peer_orphan_count.find(peer_id);
      if (peer_it != m_peer_orphan_count.end()) {
        peer_it->second--;
        if (peer_it->second == 0) {
          m_peer_orphan_count.erase(peer_it);
        }
      }

      it = m_orphan_headers.erase(it);
      evicted++;
    } else {
      ++it;
    }
  }

  // Strategy 2: If still at limit, evict oldest
  if (evicted == 0 && m_orphan_headers.size() >= protocol::MAX_ORPHAN_HEADERS) {
    // Find oldest orphan
    auto oldest = m_orphan_headers.begin();
    for (auto it = m_orphan_headers.begin(); it != m_orphan_headers.end();
         ++it) {
      if (it->second.nTimeReceived < oldest->second.nTimeReceived) {
        oldest = it;
      }
    }

    LOG_CHAIN_TRACE("Evicting oldest orphan header: hash={}, age={}s",
              oldest->first.ToString().substr(0, 16),
              now - oldest->second.nTimeReceived);

    // Decrement peer count
    int peer_id = oldest->second.peer_id;
    auto peer_it = m_peer_orphan_count.find(peer_id);
    if (peer_it != m_peer_orphan_count.end()) {
      peer_it->second--;
      if (peer_it->second == 0) {
        m_peer_orphan_count.erase(peer_it);
      }
    }

    m_orphan_headers.erase(oldest);
    evicted++;
  }

  if (evicted > 0) {
    LOG_CHAIN_TRACE("Evicted {} orphan headers (pool size now: {})", evicted,
             m_orphan_headers.size());
  }

  return evicted;
}

size_t ChainstateManager::GetOrphanHeaderCount() const {
  std::lock_guard<std::recursive_mutex> lock(validation_mutex_);
  return m_orphan_headers.size();
}

std::map<int,int> ChainstateManager::GetPeerOrphanCounts() const {
  std::lock_guard<std::recursive_mutex> lock(validation_mutex_);
  return m_peer_orphan_count;
}

bool ChainstateManager::CheckHeadersPoW(
    const std::vector<CBlockHeader> &headers) const {
  // Check all headers have valid proof-of-work
  // Uses virtual CheckProofOfWork so tests can override
  LOG_CHAIN_TRACE("CheckHeadersPoW: checking {} headers", headers.size());
  for (const auto &header : headers) {
    if (!CheckProofOfWork(header, crypto::POWVerifyMode::COMMITMENT_ONLY)) {
      LOG_CHAIN_TRACE("Header failed PoW commitment check: {}",
                header.GetHash().ToString().substr(0, 16));
      LOG_CHAIN_TRACE("CheckHeadersPoW: FAILED on header {}",
                      header.GetHash().ToString().substr(0, 16));
      return false;
    }
  }
  LOG_CHAIN_TRACE("CheckHeadersPoW: PASSED (all {} headers valid)", headers.size());
  return true;
}

bool ChainstateManager::CheckProofOfWork(const CBlockHeader &header,
                                         crypto::POWVerifyMode mode) const {
  // Test hook: allow PoW checks to be skipped (regtest-only RPCs)
  if (test_skip_pow_checks_.load(std::memory_order_acquire)) {
    return true;
  }
  // Default implementation: use real RandomX PoW validation
  return consensus::CheckProofOfWork(header, header.nBits, params_, mode);
}

bool ChainstateManager::CheckBlockHeaderWrapper(const CBlockHeader &header,
                                                ValidationState &state) const {
  // Test hook: allow PoW checks to be skipped (regtest-only RPCs)
  if (test_skip_pow_checks_.load(std::memory_order_acquire)) {
    return true;
  }
  // Default implementation: call real validation
  return CheckBlockHeader(header, params_, state);
}

bool ChainstateManager::ContextualCheckBlockHeaderWrapper(
    const CBlockHeader &header, const chain::CBlockIndex *pindexPrev,
    int64_t adjusted_time, ValidationState &state) const {
  // Call real contextual validation (never skipped)
  bool result = ContextualCheckBlockHeader(header, pindexPrev, params_, adjusted_time,
                                           state);

  // Check for network expiration and emit notification
  if (!result && state.GetRejectReason() == "network-expired") {
    int32_t current_height = pindexPrev ? pindexPrev->nHeight + 1 : 0;
    int32_t expiration_height = params_.GetConsensus().nNetworkExpirationInterval;

    // Emit notification to trigger shutdown
    Notifications().NotifyNetworkExpired(current_height, expiration_height);
  }

  return result;
}

bool ChainstateManager::InvalidateBlock(const uint256 &hash) {
  std::unique_lock<std::recursive_mutex> lock(validation_mutex_);
  std::vector<PendingNotification> pending_events;

  LOG_CHAIN_TRACE("InvalidateBlock: hash={}", hash.ToString().substr(0, 16));

  // Look up the block
  chain::CBlockIndex *pindex = block_manager_.LookupBlockIndex(hash);
  if (!pindex) {
    LOG_CHAIN_ERROR("InvalidateBlock: block {} not found", hash.ToString());
    return false;
  }

  // Can't invalidate genesis
  if (pindex->nHeight == 0) {
    LOG_CHAIN_ERROR("InvalidateBlock: cannot invalidate genesis block");
    return false;
  }

  LOG_CHAIN_TRACE("InvalidateBlock: Found block height={} on_active_chain={}",
                  pindex->nHeight, IsOnActiveChain(pindex));
  LOG_CHAIN_INFO("InvalidateBlock: {}", hash.ToString());

  chain::CBlockIndex *to_mark_failed = pindex;
  bool pindex_was_in_chain = false;

  // Step 1: Pre-build candidate map 
  // Find all competing fork blocks that have at least as much work as where
  // we'll end up (i.e., pindex->pprev)
  const auto &block_index = block_manager_.GetBlockIndex();
  std::multimap<arith_uint256, chain::CBlockIndex *> candidate_blocks_by_work;

  for (const auto &[block_hash, block] : block_index) {
    chain::CBlockIndex *candidate = const_cast<chain::CBlockIndex *>(&block);

    // Include blocks that:
    // 1. Are NOT in the active chain
    // 2. Have at least as much work as pindex->pprev (the new tip after
    // disconnection)
    // 3. Are validated to at least TREE level
    if (!block_manager_.ActiveChain().Contains(candidate) && pindex->pprev &&
        candidate->nChainWork >= pindex->pprev->nChainWork &&
        candidate->IsValid(chain::BlockStatus::TREE)) {
      candidate_blocks_by_work.insert(
          std::make_pair(candidate->nChainWork, candidate));
    LOG_CHAIN_TRACE("Pre-built candidate: height={}, hash={}, log2_work={:.6f}",
                candidate->nHeight, block_hash.ToString().substr(0, 16),
                std::log(candidate->nChainWork.getdouble()) / std::log(2.0));
    }
  }

  LOG_CHAIN_TRACE("Pre-built {} candidate blocks for invalidation",
            candidate_blocks_by_work.size());

  // Step 2: Disconnect loop with incremental candidate addition
  // We disconnect blocks from tip down to pindex, but do NOT mark their status yet.
  // Status marking is done in a single pass after disconnection is complete.
  while (true) {
    chain::CBlockIndex *current_tip = block_manager_.GetTip();

    // Check if pindex is in the active chain
    if (!current_tip || !block_manager_.ActiveChain().Contains(pindex)) {
      break;
    }

    pindex_was_in_chain = true;
    chain::CBlockIndex *invalid_walk_tip = current_tip;

    // Disconnect the current tip
    if (!DisconnectTip(pending_events)) {
      LOG_CHAIN_ERROR("Failed to disconnect tip during invalidation");
      return false;
    }

    // Remove from candidates (but don't mark status yet)
    chain_selector_.RemoveCandidate(invalid_walk_tip);

    // Add parent as candidate (so we have a valid tip to fall back to)
    if (invalid_walk_tip->pprev) {
      chain_selector_.AddCandidateUnchecked(invalid_walk_tip->pprev);
    }

    // ADD COMPETING FORKS as they become viable candidates
    // As we disconnect, some fork blocks now have enough work to be candidates
    if (invalid_walk_tip->pprev) {
      auto candidate_it = candidate_blocks_by_work.lower_bound(
          invalid_walk_tip->pprev->nChainWork);
      while (candidate_it != candidate_blocks_by_work.end()) {
        chain::CBlockIndex *fork_candidate = candidate_it->second;

        // Check if this fork has at least as much work as the new tip
        if (fork_candidate->nChainWork >= invalid_walk_tip->pprev->nChainWork) {
          chain_selector_.AddCandidateUnchecked(fork_candidate);
          LOG_CHAIN_TRACE(
              "Added competing fork as candidate: height={}, hash={}, log2_work={:.6f}",
              fork_candidate->nHeight,
              fork_candidate->GetBlockHash().ToString().substr(0, 16),
              std::log(fork_candidate->nChainWork.getdouble()) / std::log(2.0));
          candidate_it = candidate_blocks_by_work.erase(candidate_it);
        } else {
          ++candidate_it;
        }
      }
    }

    to_mark_failed = invalid_walk_tip;

    LOG_CHAIN_TRACE("Disconnected block at height {}: {}",
              invalid_walk_tip->nHeight,
              invalid_walk_tip->GetBlockHash().ToString().substr(0, 16));
  }

  // Safety check: If block is still in chain, something went wrong
  if (block_manager_.ActiveChain().Contains(pindex)) {
    LOG_CHAIN_ERROR(
        "InvalidateBlock: block still in active chain after disconnect loop");
    return false;
  }

  // Step 3: Mark status flags in a single pass (clearer semantics)
  // Only the originally requested block gets FAILED_VALID
  pindex->status.MarkFailed();
  m_failed_blocks.insert(pindex);
  chain_selector_.RemoveCandidate(pindex);

  // All descendants (including those we just disconnected) get FAILED_CHILD
  for (const auto &[block_hash, block] : block_index) {
    if (&block == pindex) {
      continue;
    }

    const chain::CBlockIndex *ancestor = block.GetAncestor(pindex->nHeight);
    if (ancestor == pindex) {
      chain::CBlockIndex *mutable_block = const_cast<chain::CBlockIndex *>(&block);

      // Mark as ancestor failed (descendants of invalidated block)
      mutable_block->status.MarkAncestorFailed();
      m_failed_blocks.insert(mutable_block);
      chain_selector_.RemoveCandidate(mutable_block);

      LOG_CHAIN_TRACE("Marked descendant {} at height {} as ANCESTOR_FAILED",
                block_hash.ToString().substr(0, 16), block.nHeight);
    }
  }

  // Final cleanup - catch any blocks that arrived during invalidation
  // Bitcoin Core does this to handle race conditions where blocks arrive during
  // invalidation
  chain::CBlockIndex *current_tip = block_manager_.GetTip();
  if (current_tip) {
    for (const auto &[block_hash, block] : block_index) {
      chain::CBlockIndex *mutable_block =
          const_cast<chain::CBlockIndex *>(&block);

      // Bitcoin Core's criteria: block is valid and has at least as much work
      // as tip
      if (block.IsValid(chain::BlockStatus::TREE) &&
          block.nChainWork >= current_tip->nChainWork) {

        // Check if it's a leaf (no children)
        bool is_leaf = true;
        for (const auto &[other_hash, other_block] : block_index) {
          if (other_block.pprev == mutable_block) {
            is_leaf = false;
            break;
          }
        }

        if (is_leaf) {
          chain_selector_.AddCandidateUnchecked(mutable_block);
        }
      }
    }
  }

  LOG_CHAIN_TRACE("Successfully invalidated block {} and {} descendants",
           hash.ToString(), pindex_was_in_chain ? "disconnected" : "marked");

  // NOTE: Bitcoin Core does NOT call ActivateBestChain() here
  // The candidates are set up correctly, and the next block arrival
  // or external ActivateBestChain() call will activate the best chain

  // Release lock and dispatch any queued notifications
  lock.unlock();
  for (const auto &ev : pending_events) {
    switch (ev.type) {
    case NotifyType::BlockConnected:
      Notifications().NotifyBlockConnected(ev.header, ev.pindex);
      break;
    case NotifyType::BlockDisconnected:
      Notifications().NotifyBlockDisconnected(ev.header, ev.pindex);
      break;
    case NotifyType::ChainTip:
      Notifications().NotifyChainTip(ev.pindex, ev.height);
      break;
    }
  }

  return true;
}

void ChainstateManager::TestSetSkipPoWChecks(bool enabled) {
  test_skip_pow_checks_.store(enabled, std::memory_order_release);
}

bool ChainstateManager::TestGetSkipPoWChecks() const {
  return test_skip_pow_checks_.load(std::memory_order_acquire);
}

} // namespace validation
} // namespace unicity
