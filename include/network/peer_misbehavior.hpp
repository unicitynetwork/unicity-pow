#pragma once

#include <cstdint>
#include <string>
#include <unordered_set>

namespace unicity {
namespace network {

// Permission flags for peer connections
enum class NetPermissionFlags : uint32_t {
  None = 0,
  // Allow getheaders during IBD and block-download after maxuploadtarget limit
  Download = (1U << 6),
  // Can't be banned/disconnected/discouraged for misbehavior
  // Note: NoBan includes Download permission
  NoBan = (1U << 4) | Download,
  // Manual connection (not subject to connection limits)
  Manual = (1U << 1),
  // Can send us unlimited amounts of addrs (bypasses ADDR rate limiting)
  // allow whitelisted peers to bypass addr rate limits
  Addr = (1U << 7),
};

inline NetPermissionFlags operator|(NetPermissionFlags a, NetPermissionFlags b) {
  return static_cast<NetPermissionFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline NetPermissionFlags operator&(NetPermissionFlags a, NetPermissionFlags b) {
  return static_cast<NetPermissionFlags>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

inline bool HasPermission(NetPermissionFlags flags, NetPermissionFlags check) {
  return (flags & check) == check && static_cast<uint32_t>(check) != 0;
}

// Peer misbehavior tracking data
struct PeerMisbehaviorData {
  int misbehavior_score{0};
  bool should_discourage{false};
  int num_unconnecting_headers_msgs{0};
  bool unconnecting_penalized{false};
  NetPermissionFlags permissions{NetPermissionFlags::None};
  std::string address;
  // Track duplicates of invalid headers reported by this peer to avoid double-penalty
  std::unordered_set<std::string> invalid_header_hashes;
};

// DoS Protection Constants 
static constexpr int DISCOURAGEMENT_THRESHOLD = 100;

// Misbehavior penalties
namespace MisbehaviorPenalty {
static constexpr int INVALID_POW = 100;
static constexpr int OVERSIZED_MESSAGE = 20;
static constexpr int NON_CONTINUOUS_HEADERS = 20;
static constexpr int LOW_WORK_HEADERS = 10;
static constexpr int INVALID_HEADER = 100;
static constexpr int TOO_MANY_UNCONNECTING = 100;  // Instant disconnect after threshold
static constexpr int TOO_MANY_ORPHANS = 100;       // Instant disconnect
static constexpr int PRE_VERACK_MESSAGE = 100;     // Protocol messages before handshake complete (instant disconnect)
}

// Maximum unconnecting headers messages before penalty
static constexpr int MAX_UNCONNECTING_HEADERS = 10;

} // namespace network
} // namespace unicity
