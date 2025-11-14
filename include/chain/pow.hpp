// Copyright (c) 2025 The Unicity Foundation
// Distributed under the MIT software license

#pragma once

#include "util/arith_uint256.hpp"
#include "chain/block.hpp"
#include <cstdint>

namespace unicity {

// Forward declarations
namespace chain {
class CBlockIndex;
class ChainParams;
} // namespace chain

namespace crypto {
enum class POWVerifyMode;
}

namespace consensus {

// ASERT (Absolutely Scheduled Exponentially Rising Targets):
// Per-block exponential difficulty adjustment based on Bitcoin Cash aserti3-2d
// Responsive to hashrate changes while maintaining predictable block times
// Difficulty doubles/halves every nASERTHalfLife seconds ahead/behind schedule

uint32_t GetNextWorkRequired(const chain::CBlockIndex *pindexLast,
                             const chain::ChainParams &params);

// Returns difficulty as floating point: max_target / current_target (1.0 =
// genesis)
double GetDifficulty(uint32_t nBits, const chain::ChainParams &params);

arith_uint256 GetTargetFromBits(uint32_t nBits);

// CONSENSUS-CRITICAL: Validates proof-of-work meets difficulty target
// Wrapper around crypto::CheckProofOfWorkRandomX with chain parameters
// In MINING mode, outHash must be non-null to receive computed RandomX hash
bool CheckProofOfWork(const CBlockHeader &block, uint32_t nBits,
                      const chain::ChainParams &params,
                      crypto::POWVerifyMode mode, uint256 *outHash = nullptr);

} // namespace consensus
} // namespace unicity


