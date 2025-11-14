// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2022 The Bitcoin Core developers
// Copyright (c) 2025 The Unicity Foundation
// Distributed under the MIT software license

#pragma once

#include "chain/block_index.hpp"
#include "chain/block.hpp"
#include <vector>

namespace unicity {
namespace chain {

// CChain - In-memory indexed chain of blocks
// Represents single linear chain as vector of CBlockIndex pointers
// Used for active chain (best known) and tracking competing forks
// Fast O(1) access by height, does NOT own CBlockIndex objects

class CChain {
private:
  std::vector<CBlockIndex *> vChain;

public:
  CChain() = default;

  // Prevent copying (chains should be owned, not copied)
  CChain(const CChain &) = delete;
  CChain &operator=(const CChain &) = delete;

  CBlockIndex *Genesis() const {
    return vChain.size() > 0 ? vChain[0] : nullptr;
  }

  CBlockIndex *Tip() const {
    return vChain.size() > 0 ? vChain[vChain.size() - 1] : nullptr;
  }

  CBlockIndex *operator[](int nHeight) const {
    if (nHeight < 0 || nHeight >= (int)vChain.size())
      return nullptr;
    return vChain[nHeight];
  }

  // Check whether block is present in this chain
  bool Contains(const CBlockIndex *pindex) const {
    if (!pindex)
      return false; 
    if (pindex->nHeight < 0 || pindex->nHeight >= (int)vChain.size()) {
      return false; 
    }
    return vChain[pindex->nHeight] == pindex;
  }

  // Find successor of block in this chain (nullptr if not found or is tip)
  CBlockIndex *Next(const CBlockIndex *pindex) const {
    if (Contains(pindex))
      return (*this)[pindex->nHeight + 1];
    else
      return nullptr;
  }

  // Return maximal height in chain (equal to chain.Tip() ? chain.Tip()->nHeight: -1)
  int Height() const { return int(vChain.size()) - 1; }

  // Set/initialize chain with given tip (walks backwards using pprev to
  // populate entire vector)
  void SetTip(CBlockIndex &block);

  void Clear() { vChain.clear(); }

  // Return CBlockLocator that refers to tip of this chain (used for GETHEADER messages)
  CBlockLocator GetLocator() const;

  // Find last common block between this chain and block index entry (fork point)
  const CBlockIndex *FindFork(const CBlockIndex *pindex) const;

  // Find earliest block with timestamp >= nTime and height >= height
  CBlockIndex *FindEarliestAtLeast(int64_t nTime, int height) const;
};

// Get locator for block index entry (returns exponentially spaced hashes for
// efficient sync)
CBlockLocator GetLocator(const CBlockIndex *index);

// Construct list of hash entries for locator (exponentially increasing
// intervals) Example for height 1000: [1000, 999, 998, 996, 992, 984, 968, 936,
// 872, 744, 488, 0]
std::vector<uint256> LocatorEntries(const CBlockIndex *index);

} // namespace chain
} // namespace unicity


