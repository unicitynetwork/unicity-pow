#include "network/misbehavior_manager.hpp"
#include "network/peer_misbehavior.hpp"
#include "util/logging.hpp"

namespace unicity {
namespace network {

MisbehaviorManager::MisbehaviorManager(util::ThreadSafeMap<int, PeerTrackingData>& peer_states)
    : peer_states_(peer_states) {
}

void MisbehaviorManager::ReportInvalidPoW(int peer_id) {
  Misbehaving(peer_id, MisbehaviorPenalty::INVALID_POW,
              "header with invalid proof of work");
}

void MisbehaviorManager::ReportOversizedMessage(int peer_id) {
  Misbehaving(peer_id, MisbehaviorPenalty::OVERSIZED_MESSAGE,
              "oversized message");
}

void MisbehaviorManager::ReportNonContinuousHeaders(int peer_id) {
  Misbehaving(peer_id, MisbehaviorPenalty::NON_CONTINUOUS_HEADERS,
              "non-continuous headers sequence");
}

void MisbehaviorManager::ReportLowWorkHeaders(int peer_id) {
  Misbehaving(peer_id, MisbehaviorPenalty::LOW_WORK_HEADERS,
              "low-work headers");
}

void MisbehaviorManager::ReportInvalidHeader(int peer_id, const std::string &reason) {
  Misbehaving(peer_id, MisbehaviorPenalty::INVALID_HEADER,
              "invalid header: " + reason);
}

void MisbehaviorManager::ReportTooManyOrphans(int peer_id) {
  Misbehaving(peer_id, MisbehaviorPenalty::TOO_MANY_ORPHANS,
              "exceeded orphan header limit");
}

bool MisbehaviorManager::Misbehaving(int peer_id, int penalty,
                               const std::string &reason) {
  LOG_NET_TRACE("Misbehaving() peer={} penalty={} reason={}", peer_id, penalty, reason);

  bool should_disconnect = false;

  peer_states_.Modify(peer_id, [&](PeerTrackingData& state) {
    PeerMisbehaviorData &data = state.misbehavior;

    // Add penalty to score (always track, even for NoBan peers - matches Bitcoin)
    int old_score = data.misbehavior_score;
    data.misbehavior_score += penalty;

    // Bitcoin Core format: "Misbehaving: peer=%d (%d -> %d)"
    LOG_NET_INFO("Misbehaving: peer={} ({} -> {}) {}",
                 peer_id, old_score, data.misbehavior_score, reason);

    LOG_NET_TRACE("Misbehaving() peer={} score: {} -> {} (threshold={})",
                  peer_id, old_score, data.misbehavior_score, DISCOURAGEMENT_THRESHOLD);

    LOG_NET_TRACE("peer {} ({}) misbehavior +{}: {} (total score: {})", peer_id,
                 data.address, penalty, reason, data.misbehavior_score);

    // Check if threshold exceeded
    if (data.misbehavior_score >= DISCOURAGEMENT_THRESHOLD && old_score < DISCOURAGEMENT_THRESHOLD) {
      LOG_NET_TRACE("Misbehaving() peer={} THRESHOLD EXCEEDED", peer_id);

      // Check if peer has NoBan permission (matches Bitcoin: track score but don't disconnect)
      if (HasPermission(data.permissions, NetPermissionFlags::NoBan)) {
        LOG_NET_TRACE("noban peer {} not punished (score {} >= threshold {})",
                     peer_id, data.misbehavior_score, DISCOURAGEMENT_THRESHOLD);
        // DO NOT set should_discourage for NoBan peers
        return;
      }

      // Normal peer: mark for disconnection
      data.should_discourage = true;
      should_disconnect = true;
      LOG_NET_TRACE("peer {} ({}) marked for disconnect (score {} >= threshold {})",
                   peer_id, data.address, data.misbehavior_score,
                   DISCOURAGEMENT_THRESHOLD);
    }
  });

  if (!peer_states_.Contains(peer_id)) {
    // Peer not found - may have been disconnected already
    LOG_NET_TRACE("Misbehaving() peer={} not found in map (already disconnected?)", peer_id);
    return false;
  }

  LOG_NET_TRACE("Misbehaving() peer={} threshold not exceeded, continuing", peer_id);
  return should_disconnect;
}

bool MisbehaviorManager::ShouldDisconnect(int peer_id) const {
  bool result = false;
  peer_states_.Read(peer_id, [&](const PeerTrackingData& state) {
    // Never disconnect peers with NoBan permission (matches Bitcoin)
    if (HasPermission(state.misbehavior.permissions, NetPermissionFlags::NoBan)) {
      return;
    }
    result = state.misbehavior.should_discourage;
  });
  return result;
}

int MisbehaviorManager::GetMisbehaviorScore(int peer_id) const {
  int result = 0;
  peer_states_.Read(peer_id, [&](const PeerTrackingData& state) {
    result = state.misbehavior.misbehavior_score;
  });
  return result;
}

void MisbehaviorManager::NoteInvalidHeaderHash(int peer_id, const uint256& hash) {
  peer_states_.Modify(peer_id, [&](PeerTrackingData& state) {
    state.misbehavior.invalid_header_hashes.insert(hash.GetHex());
  });
}

bool MisbehaviorManager::HasInvalidHeaderHash(int peer_id, const uint256& hash) const {
  bool result = false;
  peer_states_.Read(peer_id, [&](const PeerTrackingData& state) {
    result = state.misbehavior.invalid_header_hashes.find(hash.GetHex()) != state.misbehavior.invalid_header_hashes.end();
  });
  return result;
}

void MisbehaviorManager::IncrementUnconnectingHeaders(int peer_id) {
  bool threshold_exceeded = false;

  peer_states_.Modify(peer_id, [&](PeerTrackingData& state) {
    PeerMisbehaviorData &data = state.misbehavior;
    if (data.unconnecting_penalized) {
      return; // already penalized; do nothing further
    }
    data.num_unconnecting_headers_msgs++;

    LOG_NET_TRACE("IncrementUnconnectingHeaders: peer {} now has {} unconnecting msgs (threshold={})",
                  peer_id, data.num_unconnecting_headers_msgs, MAX_UNCONNECTING_HEADERS);

    if (data.num_unconnecting_headers_msgs >= MAX_UNCONNECTING_HEADERS) {
      LOG_NET_TRACE("peer {} ({}) sent too many unconnecting headers ({} >= {})",
                   peer_id, data.address, data.num_unconnecting_headers_msgs,
                   MAX_UNCONNECTING_HEADERS);
      data.unconnecting_penalized = true; // latch to avoid repeated penalties
      threshold_exceeded = true;
    }
  });

  if (!peer_states_.Contains(peer_id)) {
    LOG_NET_TRACE("IncrementUnconnectingHeaders: peer {} not found in misbehavior map", peer_id);
    return;
  }

  if (threshold_exceeded) {
    Misbehaving(peer_id, MisbehaviorPenalty::TOO_MANY_UNCONNECTING,
                "too many unconnecting headers");
  }
}

void MisbehaviorManager::ResetUnconnectingHeaders(int peer_id) {
  peer_states_.Modify(peer_id, [](PeerTrackingData& state) {
    state.misbehavior.num_unconnecting_headers_msgs = 0;
  });
}

int MisbehaviorManager::GetUnconnectingHeadersCount(int peer_id) const {
  int result = 0;
  peer_states_.Read(peer_id, [&](const PeerTrackingData& state) {
    result = state.misbehavior.num_unconnecting_headers_msgs;
  });
  return result;
}

} // namespace network
} // namespace unicity
