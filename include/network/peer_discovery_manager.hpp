#pragma once

/*
 DiscoveryManager — peer discovery coordinator for Unicity

 Purpose
 - Own and coordinate AddressManager (peer address database) and AnchorManager (eclipse resistance)
 - Handle peer discovery protocol messages (ADDR/GETADDR)
 - Provide unified interface for address management and anchor persistence
 - Consolidate discovery-related components under one manager

 Key responsibilities
 1. Own AddressManager and AnchorManager
 2. Handle ADDR/GETADDR protocol messages
 3. Maintain recent address cache for fast GETADDR responses
 4. Implement echo suppression (don't send addresses back to source)
 5. Provide forwarding methods for address operations
 6. Provide forwarding methods for anchor operations

 Architecture
 This is a top-level manager that owns the discovery subsystem components.
 It provides a clean facade for NetworkManager to interact with address/anchor logic
 without exposing internal AddressManager/AnchorManager implementation details.

 Design Pattern
 - Facade pattern: Provides simplified interface to complex subsystem
 - Ownership: Uses unique_ptr to own child managers
 - Delegation: Routes requests to appropriate internal manager
*/

#include "network/peer.hpp"
#include "network/message.hpp"
#include "network/peer_tracking.hpp"
#include "network/protocol.hpp"
#include "network/notifications.hpp"
#include <memory>
#include <deque>
#include <mutex>
#include <random>
#include <string>
#include <vector>
#include <functional>
#include <optional>
#include <atomic>

namespace unicity {

// Forward declarations for chain types
namespace chain {
class ChainParams;
}

namespace network {

// Forward declarations
class AddressManager;
class AnchorManager;
class PeerLifecycleManager;

/**
 * PeerDiscoveryManager - Peer discovery manager (owns AddressManager + AnchorManager)
 *
 * Responsibilities:
 * - Own AddressManager and AnchorManager
 * - Handle ADDR messages (receive peer addresses)
 * - Handle GETADDR messages (serve peer addresses)
 * - Provide forwarding API for address operations
 * - Provide forwarding API for anchor operations
 * - Maintain recent address cache for fast responses
 * - Implement echo suppression (don't send addresses back to source)
 * - Fingerprinting protection (only respond to inbound peers)
 */
class PeerDiscoveryManager {
public:
  // Constructor: Create managers
  // peer_manager is used for per-peer state queries and for AnchorManager initialization
  explicit PeerDiscoveryManager(PeerLifecycleManager* peer_manager, const std::string& datadir = "");
  ~PeerDiscoveryManager();

  // Non-copyable
  PeerDiscoveryManager(const PeerDiscoveryManager&) = delete;
  PeerDiscoveryManager& operator=(const PeerDiscoveryManager&) = delete;

  // Lifecycle
  using ConnectToAnchorsCallback = std::function<void(const std::vector<protocol::NetworkAddress>&)>;

  /**
   * Start discovery services - load anchors and bootstrap if needed
   *
   * @param connect_anchors Callback to connect to loaded anchor addresses
   */
  void Start(ConnectToAnchorsCallback connect_anchors);

  // === Protocol Message Handlers ===

  /**
   * Handle ADDR message - process received peer addresses
   *
   * @param peer Peer that sent the message
   * @param msg ADDR message containing addresses
   * @return true if handled successfully
   */
  bool HandleAddr(PeerPtr peer, message::AddrMessage* msg);

  /**
   * Handle GETADDR message - serve peer addresses to requester
   *
   * @param peer Peer requesting addresses
   * @return true if handled successfully
   */
  bool HandleGetAddr(PeerPtr peer);

  /**
   * Notify that we sent GETADDR to a peer - boost their ADDR rate limit bucket
   * Bitcoin Core compatibility: when we request addresses, allow peer to send
   * up to MAX_ADDR_TO_SEND additional addresses in response
   *
   * @param peer_id Peer ID we sent GETADDR to
   */
  void NotifyGetAddrSent(int peer_id);

  // === AddressManager Forwarding Methods ===

  /**
   * Add a new address from peer discovery
   * Forwards to AddressManager
   */
  bool Add(const protocol::NetworkAddress& addr, uint32_t timestamp = 0);

  /**
   * Add multiple addresses (e.g., from ADDR message)
   * Forwards to AddressManager
   */
  size_t AddMultiple(const std::vector<protocol::TimestampedAddress>& addresses);

  /**
   * Bootstrap AddressManager from hardcoded seed nodes
   * Parses ChainParams::FixedSeeds() and adds them to AddressManager
   * @param params Chain parameters containing fixed seed addresses
   */
  void BootstrapFromFixedSeeds(const chain::ChainParams& params);

  /**
   * Mark address as a connection attempt
   * Forwards to AddressManager
   */
  void Attempt(const protocol::NetworkAddress& addr);

  /**
   * Mark address as successfully connected
   * Forwards to AddressManager
   */
  void Good(const protocol::NetworkAddress& addr);

  /**
   * Mark address as connection failure
   * Forwards to AddressManager
   */
  void Failed(const protocol::NetworkAddress& addr);

  /**
   * Get a random address to connect to
   * Forwards to AddressManager
   */
  std::optional<protocol::NetworkAddress> Select();

  /**
   * Select address from "new" table for feeler connection
   * Forwards to AddressManager
   */
  std::optional<protocol::NetworkAddress> SelectNewForFeeler();

  /**
   * Get multiple addresses for ADDR message (limited to max_count)
   * Forwards to AddressManager
   */
  std::vector<protocol::TimestampedAddress> GetAddresses(size_t max_count = protocol::MAX_ADDR_SIZE);

  /**
   * Get address manager statistics
   * Forwards to AddressManager
   */
  size_t Size() const;
  size_t TriedCount() const;
  size_t NewCount() const;

  /**
   * Remove stale addresses
   * Forwards to AddressManager
   */
  void CleanupStale();

  /**
   * Save address manager state to disk
   * Forwards to AddressManager
   */
  bool SaveAddresses(const std::string& filepath);

  /**
   * Load address manager state from disk
   * Forwards to AddressManager
   */
  bool LoadAddresses(const std::string& filepath);

  // === AnchorManager Forwarding Methods ===

  /**
   * Get current anchor peers from connected outbound peers
   * Forwards to AnchorManager
   */
  std::vector<protocol::NetworkAddress> GetAnchors() const;

  /**
   * Save current anchors to file
   * Forwards to AnchorManager
   */
  bool SaveAnchors(const std::string& filepath);

  /**
   * Load anchor addresses from file (passive - returns addresses for caller to connect)
   * Forwards to AnchorManager
   * @return Vector of anchor addresses to connect to (empty if file not found or invalid)
   */
  std::vector<protocol::NetworkAddress> LoadAnchors(const std::string& filepath);

  // === Test/Diagnostic Methods ===
  // These methods are intentionally public but should only be used in tests

  // Debug stats snapshot for GETADDR handling (for tests/triage)
  struct GetAddrDebugStats {
    uint64_t total{0};
    uint64_t served{0};
    uint64_t ignored_outbound{0};
    uint64_t ignored_prehandshake{0};
    uint64_t ignored_repeat{0};
    size_t last_from_addrman{0};
    size_t last_from_recent{0};
    size_t last_from_learned{0};
    size_t last_suppressed{0};
  };
  GetAddrDebugStats GetGetAddrDebugStats() const;

  // Test-only: seed RNG for deterministic shuffles
  void TestSeedRng(uint64_t seed);

  // Test-only accessors for internal managers
  AddressManager& addr_manager_for_test() { return *addr_manager_; }
  AnchorManager& anchor_manager_for_test() { return *anchor_manager_; }

private:
  // Datadir for persistence
  std::string datadir_;

  // Peer manager reference (for per-peer state queries)
  PeerLifecycleManager* peer_manager_;

  // Owned managers
  std::unique_ptr<AddressManager> addr_manager_;
  std::unique_ptr<AnchorManager> anchor_manager_;

  // Echo suppression TTL (do not echo back addresses learned from the requester within TTL)
  static constexpr int64_t ECHO_SUPPRESS_TTL_SEC = 600; // 10 minutes

  // Cap per-peer learned cache to bound memory
  static constexpr size_t MAX_LEARNED_PER_PEER = 2000;

  // Recently learned addresses (global ring buffer) to improve GETADDR responsiveness
  // Single-threaded: accessed only from io_context thread (message handlers)
  std::deque<protocol::TimestampedAddress> recent_addrs_;
  static constexpr size_t RECENT_ADDRS_MAX = 5000;

  // Debug counters/state for GETADDR decisions (thread-safe: atomics)
  std::atomic<uint64_t> stats_getaddr_total_{0};
  std::atomic<uint64_t> stats_getaddr_served_{0};
  std::atomic<uint64_t> stats_getaddr_ignored_outbound_{0};
  std::atomic<uint64_t> stats_getaddr_ignored_prehandshake_{0};
  std::atomic<uint64_t> stats_getaddr_ignored_repeat_{0};
  std::atomic<size_t> last_resp_from_addrman_{0};
  std::atomic<size_t> last_resp_from_recent_{0};
  std::atomic<size_t> last_resp_from_learned_{0};
  std::atomic<size_t> last_resp_suppressed_{0};

  // RNG for GETADDR reply randomization
  std::mt19937 rng_;

  // ADDR rate limiting (DoS protection)
  // Per-peer token bucket state for ADDR message processing
  struct AddrRateLimitState {
    double token_bucket{1.0}; // Start with 1 token (allows self-announcement, Bitcoin Core pattern)
    std::chrono::microseconds last_update{0};
    uint64_t addr_processed{0};
    uint64_t addr_rate_limited{0};
  };
  std::unordered_map<int, AddrRateLimitState> addr_rate_limit_; // peer_id -> state
  static constexpr double MAX_ADDR_RATE_PER_SECOND = 0.1;  // 0.1 addresses/second
  static constexpr double MAX_ADDR_PROCESSING_TOKEN_BUCKET = protocol::MAX_ADDR_SIZE;  // 1000

  // NetworkNotifications subscriptions (RAII - auto-unsubscribe on destruction)
  NetworkNotifications::Subscription peer_connected_sub_;
  NetworkNotifications::Subscription peer_disconnected_sub_;

  // Helper to build binary key (uses shared AddressKey from peer_tracking.hpp)
  static network::AddressKey MakeKey(const protocol::NetworkAddress& a);
};

} // namespace network
} // namespace unicity
