#pragma once

#include "network/peer.hpp"
#include "network/peer_misbehavior.hpp"
#include "util/uint.hpp"
#include <array>
#include <chrono>
#include <deque>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace unicity {
namespace network {

// Forward declarations
struct LearnedEntry;

// AddressKey for binary IP:port keying (from MessageRouter)
struct AddressKey {
  std::array<uint8_t, 16> ip{};
  uint16_t port{0};

  struct Hasher {
    size_t operator()(const AddressKey& k) const noexcept {
      // FNV-1a 64-bit
      uint64_t h = 1469598103934665603ULL;
      auto mix = [&](uint8_t b) {
        h ^= b;
        h *= 1099511628211ULL;
      };
      for (auto b : k.ip)
        mix(b);
      mix(static_cast<uint8_t>(k.port >> 8));
      mix(static_cast<uint8_t>(k.port & 0xFF));
      return static_cast<size_t>(h);
    }
  };

  bool operator==(const AddressKey& o) const noexcept {
    return port == o.port && ip == o.ip;
  }
};

// Learned address entry (from MessageRouter)
struct LearnedEntry {
  protocol::TimestampedAddress ts_addr{};
  int64_t last_seen_s{0};
};

using LearnedMap = std::unordered_map<AddressKey, LearnedEntry, AddressKey::Hasher>;

/**
 * PeerTrackingData - Consolidated per-peer state
 *
 * - Single source of truth for all per-peer data across network managers
 * - Stored in ThreadSafeMap<int, PeerTrackingData> in ConnectionManager
 *
 * Note: Named "PeerTrackingData" to avoid conflict with PeerState enum (connection state)
 */
struct PeerTrackingData {
  // === Core Connection ===
  // The actual peer object (ownership)
  PeerPtr peer;

  // === Lifecycle Metadata ===
  // When this peer was created (for feeler lifetime enforcement)
  std::chrono::steady_clock::time_point created_at;

  // === DoS & Permissions ===
  // Misbehavior tracking (from ConnectionManager)
  PeerMisbehaviorData misbehavior;

  // === Block Relay  ===
  // Block announcement queue (migrated from Peer)
  // Simple vector is fine - queue size is typically 0-1 items (flushed every 1s)
  std::vector<uint256> blocks_for_inv_relay;

  // Last block announced to this peer (to avoid re-announcing same tip)
  uint256 last_announced_block;

  // Last announcement time (unix seconds)
  int64_t last_announce_time_s{0};

  // === Address Discovery  ===
  // Whether we've replied to GETADDR from this peer (once-per-connection policy)
  bool getaddr_replied{false};

  // Learned addresses from this peer (for echo suppression)
  LearnedMap learned_addresses;

  // Constructors
  PeerTrackingData() = default;

  explicit PeerTrackingData(PeerPtr p,
                        std::chrono::steady_clock::time_point created = std::chrono::steady_clock::now())
      : peer(std::move(p)), created_at(created) {}

  // Explicitly defaulted copy/move to ensure map compatibility
  PeerTrackingData(const PeerTrackingData&) = default;
  PeerTrackingData(PeerTrackingData&&) = default;
  PeerTrackingData& operator=(const PeerTrackingData&) = default;
  PeerTrackingData& operator=(PeerTrackingData&&) = default;
};

} // namespace network
} // namespace unicity
