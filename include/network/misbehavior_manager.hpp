#pragma once

/*
 MisbehaviorManager — manages peer misbehavior tracking and penalties

 Purpose
 - Track misbehavior scores for peers
 - Apply penalties for protocol violations
 - Determine when peers should be disconnected
 - Track duplicate invalid headers and unconnecting header sequences

 Key responsibilities
 1. Apply penalties for different types of violations
 2. Check misbehavior thresholds and mark peers for disconnection
 3. Respect NetPermissionFlags (NoBan peers are tracked but not disconnected)
 4. Track unconnecting headers with progressive penalty system
 5. Prevent duplicate penalties for the same invalid header

 Architecture
 Extracted from PeerLifecycleManager to separate DoS protection logic.
 Operates on per-peer state (PeerTrackingData) owned by PeerLifecycleManager.
*/

#include "network/peer_tracking.hpp"
#include "util/threadsafe_containers.hpp"
#include "util/uint.hpp"
#include <string>

namespace unicity {
namespace network {

class MisbehaviorManager {
public:
  /**
   * Constructor
   * @param peer_states Reference to the peer states map (owned by PeerLifecycleManager)
   */
  explicit MisbehaviorManager(util::ThreadSafeMap<int, PeerTrackingData>& peer_states);

  ~MisbehaviorManager() = default;

  // Non-copyable
  MisbehaviorManager(const MisbehaviorManager&) = delete;
  MisbehaviorManager& operator=(const MisbehaviorManager&) = delete;

  // === Public Violation Reporting API ===
  // These are the ONLY methods that external code (like HeaderSync) should call

  /**
   * Report invalid proof of work
   * @param peer_id Peer that sent invalid PoW
   */
  void ReportInvalidPoW(int peer_id);

  /**
   * Report oversized message
   * @param peer_id Peer that sent oversized message
   */
  void ReportOversizedMessage(int peer_id);

  /**
   * Report non-continuous headers sequence
   * @param peer_id Peer that sent non-continuous headers
   */
  void ReportNonContinuousHeaders(int peer_id);

  /**
   * Report low-work headers
   * @param peer_id Peer that sent low-work headers
   */
  void ReportLowWorkHeaders(int peer_id);

  /**
   * Report invalid header
   * @param peer_id Peer that sent invalid header
   * @param reason Description of why header is invalid
   */
  void ReportInvalidHeader(int peer_id, const std::string& reason);

  /**
   * Report too many orphan headers
   * @param peer_id Peer that exceeded orphan limit
   */
  void ReportTooManyOrphans(int peer_id);

  // === Unconnecting Headers Tracking ===

  /**
   * Increment unconnecting headers counter
   * Applies penalty if threshold is exceeded
   * @param peer_id Peer that sent unconnecting headers
   */
  void IncrementUnconnectingHeaders(int peer_id);

  /**
   * Reset unconnecting headers counter (when progress is made)
   * @param peer_id Peer to reset
   */
  void ResetUnconnectingHeaders(int peer_id);

  // === Duplicate Invalid Header Tracking ===

  /**
   * Record that a peer sent a specific invalid header
   * Used to prevent double-penalizing the same header
   * @param peer_id Peer that sent invalid header
   * @param hash Hash of invalid header
   */
  void NoteInvalidHeaderHash(int peer_id, const uint256& hash);

  /**
   * Check if peer has already been penalized for this invalid header
   * @param peer_id Peer to check
   * @param hash Hash of header
   * @return true if peer has already sent this invalid header
   */
  bool HasInvalidHeaderHash(int peer_id, const uint256& hash) const;

  // === Query Methods (for testing/debugging) ===

  /**
   * Get current misbehavior score for a peer
   * @param peer_id Peer to query
   * @return Misbehavior score (0 if peer not found)
   */
  int GetMisbehaviorScore(int peer_id) const;

  /**
   * Check if peer should be disconnected due to misbehavior
   * Respects NoBan permission (always returns false for NoBan peers)
   * @param peer_id Peer to check
   * @return true if peer should be disconnected
   */
  bool ShouldDisconnect(int peer_id) const;

  /**
   * Get unconnecting headers count for a peer (for logging)
   * @param peer_id Peer to query
   * @return Number of unconnecting headers messages (0 if peer not found)
   */
  int GetUnconnectingHeadersCount(int peer_id) const;

private:
  /**
   * Apply a misbehavior penalty to a peer
   * Internal method - not exposed to external code
   * @param peer_id Peer to penalize
   * @param penalty Penalty amount
   * @param reason Description of violation
   * @return true if peer should be disconnected
   */
  bool Misbehaving(int peer_id, int penalty, const std::string& reason);

  // Reference to peer states (owned by PeerLifecycleManager)
  util::ThreadSafeMap<int, PeerTrackingData>& peer_states_;
};

} // namespace network
} // namespace unicity
