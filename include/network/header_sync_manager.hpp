#pragma once

/*
 * HeaderSyncManager — headers-only synchronization coordinator
 *
 * Functionality:
 * - Headers-only network: HEADERS payloads contain only fixed-size 100-byte headers
 *   (no per-header txcount like Bitcoin Core). GETHEADERS/HEADERS is the only sync path.
 * - Single sync peer at a time; selection is outbound-only. Initial request
 *   uses a “pprev-of-tip” locator to guarantee a non-empty response when tips match.
 * - During IBD, accept large batches only from the designated sync peer; allow small
 *   unsolicited announcements (≤2 headers) from any peer. Post-IBD, unsolicited gating is
 *   relaxed but batch processing remains identical.
* - Low-work gating: uses CalculateHeadersWork() + GetAntiDoSWorkThreshold(). The threshold
*   applies during IBD as well (Core parity). If a full-sized batch has insufficient work,
*   we request more rather than penalize immediately (no Core-style
*   HeadersSyncState; simplified behavior).
 * - DoS-check skip heuristic: if the batch’s last header is already on the ACTIVE chain, we
 *   skip low-work checks for that batch to avoid false positives after local invalidations.
 *   Side chains do NOT qualify.
 * - Stall detection: a fixed 120s timeout disconnects an unresponsive sync peer; reselection
 *   occurs via the regular SendMessages/maintenance cadence (simpler than Core’s dynamic timers).
 */

#include "chain/block.hpp"
#include "network/peer.hpp"
#include "network/message.hpp"
#include "network/notifications.hpp"
#include <cstdint>
#include <memory>
#include <limits>

namespace unicity {

// Forward declarations
namespace validation {
class ChainstateManager;
}

namespace network {

// Forward declarations
class PeerLifecycleManager;

/**
 * HeaderSyncManager - Manages blockchain header synchronization
 *
 * Responsibilities:
 * - Handle incoming headers messages from peers
 * - Request headers from peers during sync
 * - Track sync state (synced, in progress, stalled)
 * - Generate block locators for header requests
 * - Coordinate initial blockchain download (IBD) for headers
 */
class HeaderSyncManager {
public:
  static constexpr uint64_t NO_SYNC_PEER = std::numeric_limits<uint64_t>::max();
  HeaderSyncManager(validation::ChainstateManager& chainstate,
                    PeerLifecycleManager& peer_mgr);

  // Non-copyable (holds NetworkNotifications subscription)
  HeaderSyncManager(const HeaderSyncManager&) = delete;
  HeaderSyncManager& operator=(const HeaderSyncManager&) = delete;

  // Non-movable (reference members prevent safe moving)
  HeaderSyncManager(HeaderSyncManager&&) = delete;
  HeaderSyncManager& operator=(HeaderSyncManager&&) = delete;

  // Message handlers
  // @param peer Peer that sent the message (required, must not be null)
  // @param msg Message payload (required, must not be null)
  // @return true if handled successfully, false on error
  bool HandleHeadersMessage(PeerPtr peer, message::HeadersMessage* msg);
  bool HandleGetHeadersMessage(PeerPtr peer, message::GetHeadersMessage* msg);

  // Sync coordination
  // @param peer Peer to request headers from (required, must not be null)
  void RequestHeadersFromPeer(PeerPtr peer);
  void CheckInitialSync();
  
  // Periodic maintenance (timeouts, retries)
  void ProcessTimers();

  // State queries
  bool IsSynced(int64_t max_age_seconds = 3600) const;
  bool ShouldRequestMore() const;

  // Block locator generation
  CBlockLocator GetLocatorFromPrev() const;

  // Sync tracking
  uint64_t GetSyncPeerId() const;
  bool HasSyncPeer() const { return GetSyncPeerId() != NO_SYNC_PEER; }
  void SetSyncPeer(uint64_t peer_id);
  void ClearSyncPeer();

private:
  // Internal helpers (io_context thread only)
  void SetSyncPeerUnlocked(uint64_t peer_id);
  void ClearSyncPeerUnlocked();

  // Peer lifecycle handler (private - called via NetworkNotifications)
  void OnPeerDisconnected(uint64_t peer_id);

  // Component references
  validation::ChainstateManager& chainstate_manager_;
  PeerLifecycleManager& peer_manager_;

  // Sync state (single-threaded: accessed only from io_context thread)
  // No mutex needed - all accesses serialized by io_context reactor
  struct SyncState {
    uint64_t sync_peer_id = NO_SYNC_PEER;     // NO_SYNC_PEER = no sync peer
    int64_t sync_start_time_us = 0;           // When sync started (microseconds since epoch)
    int64_t last_headers_received_us = 0;     // Last time we received headers (microseconds)
    // Note: Bitcoin Core maintains nSyncStarted counter, but we enforce exactly one
    // sync peer via HasSyncPeer() check in CheckInitialSync(), so no counter needed.
  };

  SyncState sync_state_{};

  // Header batch tracking (io_context thread only)
  size_t last_batch_size_{0};  // Size of last headers batch received

  // NetworkNotifications subscription (RAII cleanup on destruction)
  NetworkNotifications::Subscription peer_disconnect_subscription_;
};

} // namespace network
} // namespace unicity


