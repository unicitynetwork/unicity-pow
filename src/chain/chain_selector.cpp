// Copyright (c) 2025 The Unicity Foundation
// Distributed under the MIT software license

// DTC1
#include "chain/chain_selector.hpp"
#include "util/logging.hpp"
#include <cassert>

namespace unicity {
namespace validation {

// Comparator implementation
bool CBlockIndexWorkComparator::operator()(const chain::CBlockIndex *pa,
                                           const chain::CBlockIndex *pb) const {
  // First, sort by chain work (descending - most work first)
  if (pa->nChainWork != pb->nChainWork) {
    return pa->nChainWork > pb->nChainWork;
  }

  // If same work, sort by height (taller chain first)
  if (pa->nHeight != pb->nHeight) {
    return pa->nHeight > pb->nHeight;
  }

  // If same work and height, sort by hash (deterministic tie-breaker)
  return pa->GetBlockHash() < pb->GetBlockHash();
}

chain::CBlockIndex *ChainSelector::FindMostWorkChain() {
  // Remove invalid/top-skipped entries while scanning front-to-back
  while (!m_candidates.empty()) {
    auto it = m_candidates.begin();
    chain::CBlockIndex *pindexCandidate = *it;

    // Drop FAILED or insufficiently validated entries eagerly
    if (pindexCandidate->status.IsFailed() ||
        !pindexCandidate->IsValid(chain::BlockStatus::TREE)) {
      LOG_CHAIN_TRACE("Erasing invalid candidate: height={}, hash={}, status={}",
                      pindexCandidate->nHeight,
                      pindexCandidate->GetBlockHash().ToString().substr(0, 16),
                      pindexCandidate->status.ToString());
      m_candidates.erase(it);
      continue;
    }

    // Found valid candidate with most work
    return pindexCandidate;
  }

  LOG_CHAIN_TRACE("No valid candidates in set (size={})", m_candidates.size());
  return nullptr;
}

void ChainSelector::TryAddBlockIndexCandidate(
    chain::CBlockIndex *pindex, const chain::BlockManager &block_manager) {
  if (!pindex) {
    return;
  }

  // A block is a candidate if it could be a valid chain tip
  // Criteria:
  // 1. It has no known children (it's a leaf in the block tree)
  // 2. It has been validated to at least TREE level

  // CRITICAL INVARIANT CHECK: Verify nHeight and nChainWork are set
  // These fields are used by CBlockIndexWorkComparator for std::set ordering
  // and MUST be immutable after insertion to maintain set correctness.
  // This assertion catches violations early during development.
  assert(pindex->nHeight >= 0 &&
         "nHeight must be set before adding to candidates");
  assert(pindex->nChainWork > 0 &&
         "nChainWork must be set before adding to candidates");

  // Check validation status
  if (!pindex->IsValid(chain::BlockStatus::TREE)) {
    LOG_CHAIN_TRACE("Block {} not added to candidates: not validated to TREE level",
              pindex->GetBlockHash().ToString().substr(0, 16));
    return;
  }


  // CRITICAL SAFETY CHECK: Verify this block has no children
  // A block can only be a candidate if it's a leaf node (no descendants)
  // This prevents interior nodes from being activated, which would lose
  // parts of the active chain
  //
  // Note: This is an O(n) check over all blocks in the index, but it's
  // necessary for correctness. In practice, TryAddBlockIndexCandidate is only
  // called:
  // 1. When a new block is accepted (guaranteed to be a leaf)
  // 2. During Initialize (genesis has no children)
  // 3. During Load (tip is guaranteed to be a leaf)
  // So this check should rarely reject blocks in correct usage.
  bool has_children = false;
  for (const auto &[hash, block_index] : block_manager.GetBlockIndex()) {
    if (block_index.pprev == pindex) {
      has_children = true;
      LOG_CHAIN_TRACE("Block {} not added to candidates: has child {} (not a leaf)",
               pindex->GetBlockHash().ToString().substr(0, 16),
               hash.ToString().substr(0, 16));
      break;
    }
  }
  if (has_children) {
    return;
  }

  // CRITICAL: If this block extends a previous candidate (its parent was a
  // candidate), we must remove the parent from candidates since it's no longer
  // a tip. This prevents the candidate set from filling up with non-tip blocks.
  if (pindex->pprev) {
    auto it = m_candidates.find(pindex->pprev);
    if (it != m_candidates.end()) {
      LOG_CHAIN_TRACE("Removed parent from candidates (extended): height={}, hash={}",
                pindex->pprev->nHeight,
                pindex->pprev->GetBlockHash().ToString().substr(0, 16));
      m_candidates.erase(it);
    }
  }

  // Add the new block as a candidate
  m_candidates.insert(pindex);

  LOG_CHAIN_TRACE("Added candidate: height={}, hash={}, log2_work={:.6f}, candidates_count={}",
            pindex->nHeight, pindex->GetBlockHash().ToString().substr(0, 16),
            std::log(pindex->nChainWork.getdouble()) / std::log(2.0), m_candidates.size());
}

void ChainSelector::PruneBlockIndexCandidates(
    const chain::BlockManager &block_manager) {
  // Remove all candidates that should no longer be considered as tips:
  // 1. Blocks with less or equal chainwork than current tip (lost competition or tie)
  // 2. The current tip itself (no longer competing)
  // 3. Any ancestor of the tip (interior of active chain)
  // 4. Any block with children (not a leaf - defensive check)

  const chain::CBlockIndex *pindexTipConst = block_manager.GetTip();
  if (!pindexTipConst) {
    return;
  }
  chain::CBlockIndex *pindexTip =
      const_cast<chain::CBlockIndex *>(pindexTipConst);

  // Build set of all blocks that have children (for leaf check)
  const auto &block_index = block_manager.GetBlockIndex();
  std::set<const chain::CBlockIndex *> blocks_with_children;
  for (const auto &[hash, block] : block_index) {
    if (block.pprev) {
      blocks_with_children.insert(block.pprev);
    }
  }

  auto it = m_candidates.begin();
  size_t removed = 0;

  while (it != m_candidates.end()) {
    chain::CBlockIndex *pindex = *it;
    bool should_remove = false;

    // Rule 1: Remove if less work than tip (Core semantics keep equal-work ties)
    if (pindex->nChainWork < pindexTip->nChainWork) {
      LOG_CHAIN_TRACE("Pruning candidate (< tip work): height={}, hash={}, log2_work={:.6f} < tip_log2_work={:.6f}",
                pindex->nHeight,
                pindex->GetBlockHash().ToString().substr(0, 16),
                std::log(pindex->nChainWork.getdouble()) / std::log(2.0),
                std::log(pindexTip->nChainWork.getdouble()) / std::log(2.0));
      should_remove = true;
    }
    // Rule 2: Remove if it IS the current tip
    else if (pindex == pindexTip) {
      LOG_CHAIN_TRACE("Pruning candidate (is current tip): height={}, hash={}",
                pindex->nHeight,
                pindex->GetBlockHash().ToString().substr(0, 16));
      should_remove = true;
    }
    // Rule 3: Remove if it's an ancestor of the tip (interior of active chain)
    else if (block_manager.ActiveChain().Contains(pindex)) {
      LOG_CHAIN_TRACE("Pruning candidate (on active chain): height={}, hash={}",
                pindex->nHeight,
                pindex->GetBlockHash().ToString().substr(0, 16));
      should_remove = true;
    }
    // Rule 4: Remove if it has children (not a leaf - defensive check)
    else if (blocks_with_children.find(pindex) != blocks_with_children.end()) {
      LOG_CHAIN_TRACE(
          "Pruning candidate (has children, not a leaf!): height={}, hash={}",
          pindex->nHeight, pindex->GetBlockHash().ToString().substr(0, 16));
      should_remove = true;
    }
    // Rule 5: Remove if failed or insufficiently validated
    else if (pindex->status.IsFailed() ||
             !pindex->IsValid(chain::BlockStatus::TREE)) {
      LOG_CHAIN_TRACE(
          "Pruning candidate (failed/invalid): height={}, hash={}, status={}",
          pindex->nHeight, pindex->GetBlockHash().ToString().substr(0, 16),
          pindex->status.ToString());
      should_remove = true;
    }

    if (should_remove) {
      it = m_candidates.erase(it);
      removed++;
    } else {
      ++it;
    }
  }

  if (removed > 0) {
    LOG_CHAIN_TRACE("Pruned {} stale candidates (remaining: {})", removed,
              m_candidates.size());
  }
}

void ChainSelector::AddCandidateUnchecked(chain::CBlockIndex *pindex) {
  if (pindex) {
    m_candidates.insert(pindex);
  }
}

void ChainSelector::ClearCandidates() { m_candidates.clear(); }

void ChainSelector::UpdateBestHeader(chain::CBlockIndex *pindex) {
  if (!pindex) {
    return;
  }

  if (!m_best_header || pindex->nChainWork > m_best_header->nChainWork) {
    m_best_header = pindex;
  }
}

void ChainSelector::RemoveCandidate(chain::CBlockIndex *pindex) {
  if (pindex) {
    m_candidates.erase(pindex);
  }
}

} // namespace validation
} // namespace unicity
