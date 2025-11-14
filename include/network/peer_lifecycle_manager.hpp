#pragma once

/*
 PeerLifecycleManager — unified peer lifecycle and misbehavior tracking for Unicity

 Purpose
 - Maintain a registry of active peer connections (both inbound and outbound)
 - Enforce connection limits (max_inbound, max_outbound, per-IP limits)
 - Track misbehavior scores and apply DoS protection policies
 - Coordinate with AddressManager for connection lifecycle updates (good/failed)
 - Provide peer discovery/eviction logic for connection management

 Key responsibilities
 1. Peer lifecycle: add, remove, lookup by ID or address
 2. Connection policy: limit enforcement, feeler connections, eviction
 3. Misbehavior tracking: score accumulation, thresholds, disconnect decisions
 4. Permission system: NoBan and Manual flags to protect certain connections
 5. Integration: publishes NetworkNotifications for peer events
 6. Address lifecycle: reports connection outcomes to DiscoveryManager 

 Misbehavior system
 - Each peer has a misbehavior score; penalties are applied for protocol violations
 - Threshold: 100 points → automatic disconnect (DISCOURAGEMENT_THRESHOLD)
 - Permission flags can prevent banning (NoBan) or mark manual connections
 - Duplicate-invalid tracking: avoid double-penalizing the same invalid header
 - Unconnecting headers: progressive tracking with max threshold before penalty

 Penalties (from MisbehaviorPenalty namespace)
   INVALID_POW = 100 (instant ban)
   INVALID_HEADER = 100 (instant ban, unless duplicate)
   TOO_MANY_UNCONNECTING = 100 (after MAX_UNCONNECTING_HEADERS threshold)
   TOO_MANY_ORPHANS = 100 (instant ban)
   OVERSIZED_MESSAGE = 20
   NON_CONTINUOUS_HEADERS = 20
   LOW_WORK_HEADERS = 10

 Connection limits
 - max_outbound_peers: default 8 (protocol::DEFAULT_MAX_OUTBOUND_CONNECTIONS)
 - max_inbound_peers: default 125 (protocol::DEFAULT_MAX_INBOUND_CONNECTIONS)
 - target_outbound_peers: attempt to maintain this many outbound connections
 - MAX_INBOUND_PER_IP = 2: per-IP inbound limit to prevent single-host flooding

 Feeler connections
 - Short-lived test connections to validate addresses in the "new" table
 - FEELER_MAX_LIFETIME_SEC = 120: forced removal after 2 minutes
 - Marked as feeler via PeerPtr flags, tracked for cleanup in process_periodic()

 Public API design
 - Report* methods: external code (HeaderSync, message handlers) reports violations
   • ReportInvalidPoW, ReportInvalidHeader, ReportLowWorkHeaders, etc.
   • Each applies the appropriate penalty internally via private Misbehaving()
 - Increment/Reset UnconnectingHeaders: track non-connectable header sequences
 - Query methods: GetMisbehaviorScore(), ShouldDisconnect() for testing/debugging
 - NO direct penalty manipulation from external code; all penalties are internal


 Threading
 - All public methods are thread-safe (protected by mutex_)
 - Shutdown() sets flag to prevent notifications during destruction
 - Uses NetworkNotifications to publish peer disconnect events

 Differences from Bitcoin Core
 - Simpler permission model: only NoBan and Manual flags (no BloomFilter, etc.)
 - No NetGroupManager: per-IP limits only, no ASN-based grouping yet
 - Misbehavior data stored separately from Peer objects for cleaner separation
 - Feeler connections explicitly tracked and aged out (no implicit heuristics)
 - Inbound eviction: simple heuristic (oldest non-protected peer), not Core's
   complex network-diversity preservation logic

 Notes
 - find_peer_by_address() requires exact IP:port match if port != 0
 - evict_inbound_peer() prefers to evict older, non-protected peers first
 - TestOnlySetPeerCreatedAt() is for unit tests to simulate feeler aging
 - process_periodic() should be called regularly (e.g., every 10 seconds) to
   handle feeler cleanup and connection maintenance
*/

#include "network/addr_manager.hpp"
#include "network/ban_manager.hpp"
#include "network/misbehavior_manager.hpp"
#include "network/peer.hpp"
#include "network/peer_misbehavior.hpp"  // For PeerMisbehaviorData, NetPermissionFlags, etc.
#include "network/peer_tracking.hpp"
#include "util/threadsafe_containers.hpp"
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_set>
#include <atomic>
#include <chrono>

namespace unicity {
namespace network {

// Forward declarations
class BanManager;
class MisbehaviorManager;
class PeerDiscoveryManager;
class Transport;
enum class ConnectionResult;  // From network_manager.hpp

class PeerLifecycleManager {
public:
  struct Config {
    size_t max_outbound_peers;    // Max outbound connections
    size_t max_inbound_peers;     // Max inbound connections
    size_t target_outbound_peers; // Try to maintain this many outbound

    Config()
        : max_outbound_peers(protocol::DEFAULT_MAX_OUTBOUND_CONNECTIONS),
          max_inbound_peers(protocol::DEFAULT_MAX_INBOUND_CONNECTIONS),
          target_outbound_peers(protocol::DEFAULT_MAX_OUTBOUND_CONNECTIONS) {}
  };

  explicit PeerLifecycleManager(boost::asio::io_context &io_context,
                       const Config &config = Config{},
                       const std::string& datadir = "");

  // Max lifetime for a feeler connection before forced removal 
  static constexpr int FEELER_MAX_LIFETIME_SEC = 120;

  ~PeerLifecycleManager();

  // Set PeerDiscoveryManager (must be called after construction to enable address tracking)
  void SetDiscoveryManager(PeerDiscoveryManager* disc_mgr);

  // Shutdown: disable callbacks and mark as shutting down to avoid UAF during destructor
  void Shutdown();

  // Add a peer (with optional permissions)
  // Allocates peer ID internally and adds to manager 
  // Returns the assigned peer_id on success, -1 on failure
  int add_peer(PeerPtr peer, NetPermissionFlags permissions = NetPermissionFlags::None,
               const std::string &address = "");

  // Remove a peer by ID (idempotent - safe to call multiple times with same ID)
  void remove_peer(int peer_id);

  // Get a peer by ID
  PeerPtr get_peer(int peer_id);

  // Find peer ID by address:port (thread-safe)
  // Contract: if port != 0, requires exact address:port match; returns -1 if no exact match even if IP matches on a different port.
  // Returns -1 if not found
  int find_peer_by_address(const std::string &address, uint16_t port);

  // Get all active peers
  std::vector<PeerPtr> get_all_peers();

  // Get outbound peers only
  std::vector<PeerPtr> get_outbound_peers();

  // Get inbound peers only
  std::vector<PeerPtr> get_inbound_peers();

  // Get count of active peers
  size_t peer_count() const;
  size_t outbound_count() const;
  size_t inbound_count() const;

  // Check if we need more outbound connections
  bool needs_more_outbound() const;

  // Check if we can accept more inbound connections
  bool can_accept_inbound() const;
  
  // Check if we can accept more inbound connections from a specific IP address
  bool can_accept_inbound_from(const std::string& address) const;
  
  // Per-IP inbound limit (policy)
  static constexpr int MAX_INBOUND_PER_IP = 2;

  // Try to evict a peer to make room for a new inbound connection
  // Returns true if a peer was evicted
  bool evict_inbound_peer();

  // Disconnect and remove all peers
  void disconnect_all();

  // Process periodic tasks (cleanup, connection maintenance)
  void process_periodic();

  // Test-only: set a peer's creation time (used to simulate feeler aging)
  // This method is intentionally public but should only be used in tests
  void TestOnlySetPeerCreatedAt(int peer_id, std::chrono::steady_clock::time_point tp);

  // === Misbehavior Tracking (delegated to MisbehaviorManager) ===
  // Public API for reporting protocol violations

  // Track unconnecting headers from a peer
  void IncrementUnconnectingHeaders(int peer_id);
  void ResetUnconnectingHeaders(int peer_id);
  int GetUnconnectingHeadersCount(int peer_id) const;

  // Report specific protocol violations (used by message handlers)
  void ReportInvalidPoW(int peer_id);
  void ReportOversizedMessage(int peer_id);
  void ReportNonContinuousHeaders(int peer_id);
  void ReportLowWorkHeaders(int peer_id);
  void ReportInvalidHeader(int peer_id, const std::string &reason);
  void ReportTooManyOrphans(int peer_id);

  // Duplicate-invalid tracking
  void NoteInvalidHeaderHash(int peer_id, const uint256& hash);
  bool HasInvalidHeaderHash(int peer_id, const uint256& hash) const;

  // Query misbehavior state (for testing/debugging)
  int GetMisbehaviorScore(int peer_id) const;
  bool ShouldDisconnect(int peer_id) const;

  // Get peer permissions (for protocol logic, e.g., Download flag)
  NetPermissionFlags GetPeerPermissions(int peer_id) const;

  // === Ban Management (delegated to BanManager) ===
  // Two-tier system:
  // 1. Manual bans: Persistent, stored on disk, permanent or timed
  // 2. Discouragement: Temporary, in-memory, for misbehavior

  // Persistent ban management
  void Ban(const std::string &address, int64_t ban_time_offset = 0);
  void Unban(const std::string &address);
  bool IsBanned(const std::string &address) const;
  std::map<std::string, BanManager::CBanEntry> GetBanned() const;
  void ClearBanned();
  void SweepBanned();

  // Temporary discouragement (misbehavior)
  void Discourage(const std::string &address);
  bool IsDiscouraged(const std::string &address) const;
  void ClearDiscouraged();
  void SweepDiscouraged();

  // Whitelist (NoBan) support
  void AddToWhitelist(const std::string& address);
  void RemoveFromWhitelist(const std::string& address);
  bool IsWhitelisted(const std::string& address) const;

  // Persistence
  bool LoadBans(const std::string& datadir);
  bool SaveBans();

  // === PeerTrackingData Accessors (for BlockRelayManager, MessageRouter) ===
  // Thread-safe accessors for consolidated per-peer state

  // Block relay state accessors
  // Atomic getter for last announcement (hash + timestamp)
  // Returns pair of (hash, timestamp) or nullopt if peer not found
  // This prevents race conditions when checking TTL in block relay
  std::optional<std::pair<uint256, int64_t>> GetLastAnnouncement(int peer_id) const;
  void SetLastAnnouncedBlock(int peer_id, const uint256& hash, int64_t time_s);

  // Block announcement queue operations
  // Note: Queue size is typically 0-1 items (flushed every 1s), so simple std::find is fine
  std::vector<uint256> GetBlocksForInvRelay(int peer_id) const;
  void AddBlockForInvRelay(int peer_id, const uint256& hash);  // Adds with dedup check
  void RemoveBlockForInvRelay(int peer_id, const uint256& hash);
  std::vector<uint256> MoveBlocksForInvRelay(int peer_id);  // Move and clear
  void ClearBlocksForInvRelay(int peer_id);

  // Address discovery state accessors
  bool HasRepliedToGetAddr(int peer_id) const;
  void MarkGetAddrReplied(int peer_id);
  void AddLearnedAddress(int peer_id, const AddressKey& key, const LearnedEntry& entry);
  std::optional<LearnedMap> GetLearnedAddresses(int peer_id) const;
  void ClearLearnedAddresses(int peer_id);
  // In-place modification of learned addresses (for efficient bulk updates)
  template <typename Func>
  void ModifyLearnedAddresses(int peer_id, Func&& modifier) {
    peer_states_.Modify(peer_id, [&](PeerTrackingData& state) {
      modifier(state.learned_addresses);
    });
  }
  // Get all peers' learned addresses (for iteration in GETADDR fallback)
  // DEPRECATED: Copies all learned address maps - use GetLearnedAddressesForGetAddr instead
  std::vector<std::pair<int, LearnedMap>> GetAllLearnedAddresses() const;

  /**
   * Get learned addresses for GETADDR response (memory-efficient)
   * @param exclude_peer_id Skip addresses learned from this peer 
   * @param max_count Maximum addresses to return (stops early to avoid copying unnecessary data)
   * @return Vector of timestamped addresses from peers' learned address maps
   */
  std::vector<protocol::TimestampedAddress>
  GetLearnedAddressesForGetAddr(int exclude_peer_id, size_t max_count) const;

  // === Connection Management ===

  // Callback types for AttemptOutboundConnections
  using ConnectCallback = std::function<ConnectionResult(const protocol::NetworkAddress&)>;
  using IsRunningCallback = std::function<bool()>;

  /**
   * Attempt to establish new outbound connections
   * Coordinates address selection, duplicate checking, and connection attempts
   *
   * @param is_running Callback to check if networking is still running
   * @param connect_fn Callback to initiate a connection to an address
   */
  void AttemptOutboundConnections(IsRunningCallback is_running, ConnectCallback connect_fn);

  // Callback types for AttemptFeelerConnection
  using SetupMessageHandlerCallback = std::function<void(Peer*)>;
  using GetTransportCallback = std::function<std::shared_ptr<Transport>()>;

  /**
   * Attempt a feeler connection to validate addresses in the "new" table
   * Feeler connections are short-lived test connections that disconnect after handshake
   *
   * @param is_running Callback to check if networking is still running
   * @param get_transport Callback to get the transport layer
   * @param setup_handler Callback to setup message handler for the peer
   * @param network_magic Network magic bytes for the connection
   * @param current_height Current blockchain height for VERSION message
   * @param local_nonce Unique nonce for self-connection detection
   */
  void AttemptFeelerConnection(IsRunningCallback is_running,
                                GetTransportCallback get_transport,
                                SetupMessageHandlerCallback setup_handler,
                                uint32_t network_magic,
                                int32_t current_height,
                                uint64_t local_nonce);

  /**
   * Connect to anchor peers 
   * Anchors are the last 2-3 outbound peers from the previous session
   *
   * @param anchors List of anchor addresses to connect to
   * @param connect_fn Callback to initiate a connection
   */
  void ConnectToAnchors(const std::vector<protocol::NetworkAddress>& anchors,
                        ConnectCallback connect_fn);

  /**
   * Check if incoming nonce collides with our local nonce or any existing peer's remote nonce
   * Detect self-connection and duplicate connections
   *
   * Checks:
   * 1. Against local_nonce (self-connection: we connected to ourselves)
   * 2. Against all existing peers' remote nonces (duplicate connection or nonce collision)
   *
   * @param nonce Nonce from incoming VERSION message
   * @param local_nonce Our node's local nonce
   * @return true if OK (unique nonce), false if collision detected
   */
  bool CheckIncomingNonce(uint64_t nonce, uint64_t local_nonce);

  // Callbacks for ConnectTo method
  using OnGoodCallback = std::function<void(const protocol::NetworkAddress&)>;
  using OnAttemptCallback = std::function<void(const protocol::NetworkAddress&)>;

  /**
   * Connect to a peer address (main outbound connection logic)
   * Performs all checks (banned, discouraged, already connected, slot availability)
   * and initiates async transport connection.
   * 
   * Peer ID is allocated only after connection succeeds (Bitcoin Core pattern).
   * No wasted IDs on failed connection attempts.
   *
   * @param addr Address to connect to
   * @param permissions Permission flags for this connection
   * @param transport Transport layer for connection
   * @param on_good Callback when connection succeeds (for discovery tracking)
   * @param on_attempt Callback when connection is attempted (for discovery tracking)
   * @param setup_message_handler Callback to setup peer message handler
   * @param network_magic Network magic for VERSION message
   * @param chain_height Current chain height for VERSION message
   * @param local_nonce Local nonce for self-connection detection
   * @return ConnectionResult indicating success or failure reason
   */
  ConnectionResult ConnectTo(
      const protocol::NetworkAddress& addr,
      NetPermissionFlags permissions,
      std::shared_ptr<Transport> transport,
      OnGoodCallback on_good,
      OnAttemptCallback on_attempt,
      SetupMessageHandlerCallback setup_message_handler,
      uint32_t network_magic,
      int32_t chain_height,
      uint64_t local_nonce
  );

  /**
   * Handle an inbound connection
   * Processes incoming connections, validates against bans/limits, creates peer
   *
   * @param connection Transport connection from remote peer
   * @param is_running Callback to check if networking is still running
   * @param setup_handler Callback to setup message handler for the peer
   * @param network_magic Network magic bytes for the connection
   * @param current_height Current blockchain height for VERSION message
   * @param local_nonce Unique nonce for self-connection detection
   * @param permissions Permission flags for the inbound peer
   */
  void HandleInboundConnection(TransportConnectionPtr connection,
                               IsRunningCallback is_running,
                               SetupMessageHandlerCallback setup_handler,
                               uint32_t network_magic,
                               int32_t current_height,
                               uint64_t local_nonce,
                               NetPermissionFlags permissions = NetPermissionFlags::None);

  // === Protocol Message Handlers ===

  /**
   * Handle VERACK message - mark outbound peers as successful in address manager
   *
   * @param peer Peer that successfully completed handshake
   * @return true if handled successfully
   */
  bool HandleVerack(PeerPtr peer);

private:
  // Helper: build AddressKey from NetworkAddress (16-byte IP + port)
  static AddressKey MakeKey(const protocol::NetworkAddress& a) {
    AddressKey k; k.ip = a.ip; k.port = a.port; return k;
  }

  boost::asio::io_context &io_context_;
  PeerDiscoveryManager* discovery_manager_{nullptr};  // Phase 2: Injected after construction to break circular dependency
  Config config_;

  // === State Consolidation ===
  // Unified per-peer state (replaces old peers_, peer_misbehavior_, peer_created_at_ maps)
  // Thread-safe via ThreadSafeMap - no separate mutex needed
  util::ThreadSafeMap<int, PeerTrackingData> peer_states_;

  // Get next available peer ID (Bitcoin Core pattern)
  // Monotonic 32-bit counter; IDs are only allocated after connection succeeds
  // (we do not recycle IDs within a process lifetime)
  std::atomic<int> next_peer_id_{1};  // Start at 1 (Bitcoin reserves 0)

  // Track in-flight outbound connection attempts to avoid duplicate concurrent dials
  std::mutex pending_outbound_mutex_;
  std::unordered_set<AddressKey, AddressKey::Hasher> pending_outbound_;

  // === Lightweight connection metrics (for observability) ===
  std::atomic<uint64_t> metrics_outbound_attempts_{0};
  std::atomic<uint64_t> metrics_outbound_successes_{0};
  std::atomic<uint64_t> metrics_outbound_failures_{0};
  std::atomic<uint64_t> metrics_feeler_attempts_{0};
  std::atomic<uint64_t> metrics_feeler_successes_{0};
  std::atomic<uint64_t> metrics_feeler_failures_{0};

  // Shutdown flag to guard callbacks during destruction (atomic for thread-safety)
  std::atomic<bool> shutting_down_{false};
  // In-progress bulk shutdown (disconnect_all); reject add_peer while true (atomic for thread-safety)
  std::atomic<bool> stopping_all_{false};
  // === Ban Management (delegated to BanManager) ===
  std::unique_ptr<BanManager> ban_manager_;

  // === Misbehavior Management (delegated to MisbehaviorManager) ===
  std::unique_ptr<MisbehaviorManager> misbehavior_manager_;
};

} // namespace network
} // namespace unicity


