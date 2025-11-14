// Copyright (c) 2025 The Unicity Foundation
// Distributed under the MIT software license
// Based on Bitcoin Cash ASERT implementation

#include "chain/pow.hpp"
#include "chain/block_index.hpp"
#include "chain/chainparams.hpp"
#include "util/logging.hpp"
#include "chain/randomx_pow.hpp"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <randomx.h>

namespace unicity {
namespace consensus {

/**
 * ASERT (Absolutely Scheduled Exponentially weighted Rising Targets)
 *
 * Based on Bitcoin Cash's aserti3-2d algorithm
 * https://reference.cash/protocol/forks/2020-11-15-asert
 *
 * ASERT adjusts difficulty exponentially based on how far ahead or behind
 * schedule the blockchain is. For every nASERTHalfLife seconds ahead or behind,
 * difficulty doubles or halves.
 */

/**
 * Calculate next difficulty target using ASERT algorithm
 *
 * @param refTarget Reference (anchor) block difficulty target
 * @param nPowTargetSpacing Target time between blocks (in seconds)
 * @param nTimeDiff Actual time since anchor (in seconds)
 * @param nHeightDiff Number of blocks since anchor
 * @param powLimit Maximum allowed target (minimum difficulty)
 * @param nHalfLife Time for difficulty to double/halve (in seconds)
 * @return New difficulty target
 */
static arith_uint256 CalculateASERT(const arith_uint256 &refTarget,
                                    int64_t nPowTargetSpacing,
                                    int64_t nTimeDiff, int64_t nHeightDiff,
                                    const arith_uint256 &powLimit,
                                    int64_t nHalfLife) {
  // Input target must be sane; clamp if not.
  if (refTarget == 0 || refTarget > powLimit) {
    return ArithU512ToU256(arith_uint512(powLimit));
  }

  // Height diff must be non-negative; if violated, clamp by returning powLimit.
  if (nHeightDiff < 0) {
    return ArithU512ToU256(arith_uint512(powLimit));
  }

  // Compute exponent for exponential adjustment:
  //   exponent = (time_diff - ideal_time) / half_life
  // where ideal_time = target_spacing * (height_diff + 1)
  //
  // We use fixed-point arithmetic with 16 bits of fractional precision
  // (multiply by 65536) and perform the multiply in 128-bit to avoid overflow.
  if (nHalfLife <= 0) {
    // Invalid consensus parameter; clamp to powLimit
    return ArithU512ToU256(arith_uint512(powLimit));
  }
  const int64_t exponentBase = nTimeDiff - nPowTargetSpacing * (nHeightDiff + 1);
  __int128 prod = static_cast<__int128>(exponentBase) * static_cast<__int128>(65536);
  __int128 divres = prod / static_cast<__int128>(nHalfLife);
  int64_t exponent;
  if (divres > std::numeric_limits<int64_t>::max()) {
    exponent = std::numeric_limits<int64_t>::max();
  } else if (divres < std::numeric_limits<int64_t>::min()) {
    exponent = std::numeric_limits<int64_t>::min();
  } else {
    exponent = static_cast<int64_t>(divres);
  }

  // Decompose exponent into integer and fractional parts
  // Using arithmetic right shift for proper floored division
  static_assert(int64_t(-1) >> 1 == int64_t(-1), "ASERT needs arithmetic shift support");
  int64_t shifts = exponent >> 16;
  const auto frac = uint16_t(exponent);
  // In release builds, don't assert on rounding differences; just proceed.

  // Polynomial approximation of 2^x for 0 <= x < 1:
  //   2^x ≈ 1 + 0.695502049*x + 0.2262698*x² + 0.0782318*x³
  // Error is less than 0.013%
  const uint32_t factor =
      65536 + ((+195766423245049ull * frac + 971821376ull * frac * frac +
                5127ull * frac * frac * frac + (1ull << 47)) >>
               48);

  // Use 512-bit arithmetic to prevent overflow
  arith_uint512 nextTarget512(refTarget);
  nextTarget512 *= factor;
  arith_uint512 powLimit512(powLimit);

  // Apply the integer part of exponent: multiply by 2^(shifts) / 65536
  // which is equivalent to: multiply by 2^(shifts - 16)
  shifts -= 16;
  if (shifts <= 0) {
    nextTarget512 >>= -shifts;
  } else {
    // Detect overflow when left-shifting
    const auto nextTarget512Shifted = nextTarget512 << shifts;
    if ((nextTarget512Shifted >> shifts) != nextTarget512) {
      // Overflow detected - clamp to powLimit
      nextTarget512 = powLimit512;
    } else {
      nextTarget512 = nextTarget512Shifted;
    }
  }

  // Clamp to powLimit
  if (nextTarget512 > powLimit512) {
    nextTarget512 = powLimit512;
  }

  // Convert back to 256-bit
  arith_uint256 nextTarget = ArithU512ToU256(nextTarget512);

  // Target must be at least 1 (but 0 is not valid, use 1 instead)
  if (nextTarget == 0) {
    nextTarget = arith_uint256(1);
  } else if (nextTarget > powLimit) {
    nextTarget = powLimit;
  }

  return nextTarget;
}

/**
 * Get next required proof of work using ASERT
 *
 * @param pindexPrev Previous block index
 * @param params Chain parameters
 * @return Compact representation of next difficulty target
 */
uint32_t GetNextWorkRequired(const chain::CBlockIndex *pindexPrev,
                             const chain::ChainParams &params) {
  const auto &consensus = params.GetConsensus();

  LOG_CHAIN_TRACE("GetNextWorkRequired: prev_height={}",
                  pindexPrev ? pindexPrev->nHeight : -1);

  // Genesis block - use powLimit
  if (pindexPrev == nullptr) {
    uint32_t result = UintToArith256(consensus.powLimit).GetCompact();
    LOG_CHAIN_TRACE("GetNextWorkRequired: GENESIS, returning powLimit bits={:#x}", result);
    return result;
  }

  // Regtest: no difficulty adjustment, always use powLimit
  if (params.GetChainType() == chain::ChainType::REGTEST) {
    uint32_t result = UintToArith256(consensus.powLimit).GetCompact();
    LOG_CHAIN_TRACE("GetNextWorkRequired: REGTEST mode, returning powLimit bits={:#x}", result);
    return result;
  }

  // Before anchor height: use powLimit (allows genesis and block 1 to be mined
  // easily)
  if (pindexPrev->nHeight < consensus.nASERTAnchorHeight) {
    uint32_t result = UintToArith256(consensus.powLimit).GetCompact();
    LOG_CHAIN_TRACE("GetNextWorkRequired: Before anchor (height {} < anchor {}), returning powLimit bits={:#x}",
                    pindexPrev->nHeight, consensus.nASERTAnchorHeight, result);
    return result;
  }

  // Find the anchor block (block at nASERTAnchorHeight)
  // Uses skip list for O(log n) performance (Bitcoin Core pattern)
  const chain::CBlockIndex *pindexAnchor = pindexPrev->GetAncestor(consensus.nASERTAnchorHeight);

  // Should never happen if chain is valid
  assert(pindexAnchor != nullptr);
  assert(pindexAnchor->nHeight == consensus.nASERTAnchorHeight);

  // Get reference target from anchor block
  arith_uint256 refTarget;
  refTarget.SetCompact(pindexAnchor->nBits);
  const arith_uint256 powLimit = UintToArith256(consensus.powLimit);

  // Calculate time and height differences from anchor
  // nTimeDiff: time elapsed from anchor block to current block's parent
  // nHeightDiff: number of blocks from anchor to current block's parent
  const int64_t nTimeDiff = pindexPrev->nTime - pindexAnchor->nTime;
  const int nHeightDiff = pindexPrev->nHeight - consensus.nASERTAnchorHeight;

  LOG_CHAIN_TRACE("GetNextWorkRequired: ASERT calculation: anchor_height={} anchor_bits={:#x} "
                  "time_diff={}s height_diff={} target_spacing={}s half_life={}s",
                  pindexAnchor->nHeight, pindexAnchor->nBits, nTimeDiff, nHeightDiff,
                  consensus.nPowTargetSpacing, consensus.nASERTHalfLife);

  // Calculate next target using ASERT
  arith_uint256 nextTarget =
      CalculateASERT(refTarget, consensus.nPowTargetSpacing, nTimeDiff,
                     nHeightDiff, powLimit, consensus.nASERTHalfLife);

  uint32_t result = nextTarget.GetCompact();
  LOG_CHAIN_TRACE("GetNextWorkRequired: ASERT result bits={:#x} (difficulty={})",
                  result, GetDifficulty(result, params));
  return result;
}

/**
 * Get difficulty as a floating point number
 *
 * @param nBits Compact representation of target
 * @param params Chain parameters
 * @return Difficulty value
 */
double GetDifficulty(uint32_t nBits, const chain::ChainParams &params) {
  const auto &consensus = params.GetConsensus();
  arith_uint256 powLimit = UintToArith256(consensus.powLimit);

  // Check for invalid nBits
  bool fNegative, fOverflow;
  arith_uint256 target;
  target.SetCompact(nBits, &fNegative, &fOverflow);

  if (fNegative || fOverflow || target == 0 || target > powLimit) {
    return 0.0;
  }

  // Extract exponent and mantissa from compact nBits format
  // nBits format: 0xMMEEEEEE where MM is exponent, EEEEEE is mantissa
  int nShift = (nBits >> 24) & 0xff;
  double dDiff = (double)0x0000ffff / (double)(nBits & 0x00ffffff);

  // Adjust for exponent (shift)
  // Standard difficulty uses shift=29 as baseline
  while (nShift < 29) {
    dDiff *= 256.0;
    nShift++;
  }
  while (nShift > 29) {
    dDiff /= 256.0;
    nShift--;
  }

  return dDiff;
}

/**
 * Get target from compact bits representation
 *
 * @param nBits Compact bits
 * @return Target as arith_uint256
 */
arith_uint256 GetTargetFromBits(uint32_t nBits) {
  arith_uint256 target;
  bool fNegative;
  bool fOverflow;
  target.SetCompact(nBits, &fNegative, &fOverflow);

  if (fNegative || fOverflow || target == 0) {
    return arith_uint256(0);
  }

  return target;
}

/**
 * Check if proof-of-work is valid
 *
 * @param block Block header to verify
 * @param nBits Difficulty target
 * @param params Chain parameters
 * @param mode Verification mode (FULL, COMMITMENT_ONLY, or MINING)
 * @param outHash Output parameter for RandomX hash (required for MINING mode)
 * @return true if PoW is valid
 */
bool CheckProofOfWork(const CBlockHeader &block, uint32_t nBits,
                      const chain::ChainParams &params,
                      crypto::POWVerifyMode mode, uint256 *outHash) {
  const auto &consensus = params.GetConsensus();
  uint32_t nEpochDuration = consensus.nRandomXEpochDuration;

  LOG_CHAIN_TRACE("CheckProofOfWork: block_hash={} nBits={:#x} mode={}",
                  block.GetHash().ToString().substr(0, 16), nBits,
                  mode == crypto::POWVerifyMode::FULL ? "FULL" :
                  mode == crypto::POWVerifyMode::COMMITMENT_ONLY ? "COMMITMENT" : "MINING");

  // Convert nBits to target
  bool fNegative, fOverflow;
  arith_uint256 bnTarget;
  bnTarget.SetCompact(nBits, &fNegative, &fOverflow);

  // Check range
  if (fNegative || bnTarget == 0 || fOverflow) {
    LOG_CHAIN_TRACE("CheckProofOfWork: FAILED - invalid nBits (negative={} overflow={} zero={})",
                    fNegative, fOverflow, bnTarget == 0);
    return false;
  }

  uint256 hashRandomX;
  bool fHashVerified = false;
  bool fCommitmentVerified = false;

  if (outHash == nullptr && mode == crypto::POWVerifyMode::MINING) {
    throw std::runtime_error("MINING mode requires outHash parameter");
  }

  // Do cheaper commitment verification first
  if (mode != crypto::POWVerifyMode::MINING) {
    if (block.hashRandomX.IsNull()) {
      LOG_CHAIN_TRACE("CheckProofOfWork: FAILED - hashRandomX is null");
      return false;
    }
    arith_uint256 commitment = UintToArith256(crypto::GetRandomXCommitment(block));
    if (commitment > bnTarget) {
      LOG_CHAIN_TRACE("CheckProofOfWork: FAILED - commitment {} > target {}",
                      commitment.ToString().substr(0, 16), bnTarget.ToString().substr(0, 16));
      return false;
    }
    LOG_CHAIN_TRACE("CheckProofOfWork: Commitment check PASSED");
    hashRandomX = block.hashRandomX;
    fCommitmentVerified = true;
  }

  // Compute RandomX hash if necessary
  if (mode == crypto::POWVerifyMode::FULL ||
      mode == crypto::POWVerifyMode::MINING) {
    uint32_t nEpoch = crypto::GetEpoch(block.nTime, nEpochDuration);
    LOG_CHAIN_TRACE("CheckProofOfWork: Computing RandomX hash (epoch={})", nEpoch);

    std::shared_ptr<crypto::RandomXVMWrapper> vmRef =
        crypto::GetCachedVM(nEpoch);
    if (!vmRef) {
      throw std::runtime_error("Could not obtain VM for RandomX");
    }

    char rx_hash[RANDOMX_HASH_SIZE];

    // Create copy of header with hashRandomX set to null
    CBlockHeader tmp(block);
    tmp.hashRandomX.SetNull();

    // Calculate hash (thread-safe via thread-local VM)
    randomx_calculate_hash(vmRef->vm, &tmp, sizeof(tmp), rx_hash);

    // If not mining, compare hash in block header with our computed value
    if (mode != crypto::POWVerifyMode::MINING) {
      if (memcmp(rx_hash, block.hashRandomX.begin(), RANDOMX_HASH_SIZE) != 0) {
        LOG_CHAIN_TRACE("CheckProofOfWork: FAILED - RandomX hash mismatch");
        return false;
      }
      LOG_CHAIN_TRACE("CheckProofOfWork: RandomX hash verification PASSED");
    } else {
      // If mining, check if commitment meets target
      hashRandomX = uint256(
          std::vector<unsigned char>(rx_hash, rx_hash + RANDOMX_HASH_SIZE));
      arith_uint256 commitment = UintToArith256(crypto::GetRandomXCommitment(block, &hashRandomX));
      if (commitment > bnTarget) {
        LOG_CHAIN_TRACE("CheckProofOfWork: FAILED (mining) - commitment {} > target {}",
                        commitment.ToString().substr(0, 16), bnTarget.ToString().substr(0, 16));
        return false;
      }
      LOG_CHAIN_TRACE("CheckProofOfWork: Mining commitment check PASSED");
      fCommitmentVerified = true;
    }
    fHashVerified = true;
  }

  // Sanity check
  if (!(fHashVerified && fCommitmentVerified) &&
      !(mode == crypto::POWVerifyMode::COMMITMENT_ONLY && !fHashVerified &&
        fCommitmentVerified)) {
    return false;
  }

  if (outHash != nullptr) {
    *outHash = hashRandomX;
  }

  LOG_CHAIN_TRACE("CheckProofOfWork: SUCCESS");
  return true;
}

} // namespace consensus
} // namespace unicity
