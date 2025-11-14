#include "network/header_sync_manager.hpp"
#include "network/peer_lifecycle_manager.hpp"
#include "network/peer.hpp"
#include "network/peer_misbehavior.hpp"
#include "network/protocol.hpp"
#include "chain/chainstate_manager.hpp"
#include "chain/chainparams.hpp"
#include "chain/validation.hpp"
#include "chain/block.hpp"
#include "util/logging.hpp"
#include "util/time.hpp"
#include <cstring>
#include <chrono>
#include <cmath>

// File-scoped constants for HeaderSync behavior and time conversions
namespace {
  // Time conversion: microseconds per second. We store sync timestamps in microseconds.
  static constexpr int64_t kMicrosPerSecond = 1'000'000;

  // Headers sync stall timeout (microseconds). If no headers are received from the sync peer within
  // this window, we disconnect it to allow reselection.
  static constexpr int64_t kHeadersSyncTimeoutUs = 120 * kMicrosPerSecond; // 120 seconds

  // During IBD we accept small unsolicited HEADERS announcements (e.g., INV-triggered) from any peer,
  // but gate larger batches to the designated sync peer. This bounds wasted processing on multiple peers.
  static constexpr size_t kMaxUnsolicitedAnnouncement = 2;
}

namespace unicity {
namespace network {

HeaderSyncManager::HeaderSyncManager(validation::ChainstateManager& chainstate,
                                     PeerLifecycleManager& peer_mgr)
    : chainstate_manager_(chainstate),
      peer_manager_(peer_mgr) {
  // Subscribe to peer disconnect events
  peer_disconnect_subscription_ = NetworkEvents().SubscribePeerDisconnected(
      [this](int peer_id, const std::string&, uint16_t, const std::string&, bool) {
        OnPeerDisconnected(static_cast<uint64_t>(peer_id));
      });
}

uint64_t HeaderSyncManager::GetSyncPeerId() const {
  return sync_state_.sync_peer_id;
}

void HeaderSyncManager::SetSyncPeerUnlocked(uint64_t peer_id) {
  int64_t now_us = util::GetTime() * kMicrosPerSecond;
  // Invariant: at most one sync peer at a time (enforced by HasSyncPeer() check)
  sync_state_.sync_peer_id = peer_id;
  sync_state_.sync_start_time_us = now_us;
  sync_state_.last_headers_received_us = now_us;
}

void HeaderSyncManager::SetSyncPeer(uint64_t peer_id) {
  SetSyncPeerUnlocked(peer_id);
}

void HeaderSyncManager::ClearSyncPeerUnlocked() {
  // Clear current sync peer and allow re-selection on next maintenance.
  // NOTE: We do NOT clear peer->sync_started() here. That flag persists for the
  // lifetime of the connection (Bitcoin Core's fSyncStarted semantics) to indicate
  // "we've already attempted sync with this peer". This prevents re-selecting the
  // same peer that just gave us empty headers.
  // Bitcoin Core: stickiness comes from fSyncStarted persisting, NOT from time-based cooldown.
  sync_state_.sync_peer_id = NO_SYNC_PEER;
  sync_state_.sync_start_time_us = 0;
  // Preserve timestamp for debugging/future use
  // sync_state_.last_headers_received_us = 0;
}

void HeaderSyncManager::ClearSyncPeer() {
  ClearSyncPeerUnlocked();
}

void HeaderSyncManager::OnPeerDisconnected(uint64_t peer_id) {
  // Bitcoin Core cleanup (FinalizeNode): if (state->fSyncStarted) nSyncStarted--;
  // If this was our sync peer, reset sync state to allow retry with another peer
  if (sync_state_.sync_peer_id == peer_id) {
    LOG_NET_DEBUG("Sync peer {} disconnected, clearing sync state", peer_id);
    ClearSyncPeerUnlocked();

    // Reset sync_started on all remaining outbound peers to allow retry after stall.
    // This ensures that if the sync peer failed/stalled, we can select another peer
    // even if it was previously attempted. This is necessary when the peer set is small
    // (e.g. in tests) and we need to retry with the same peers.
    auto outbound_peers = peer_manager_.get_outbound_peers();
    for (const auto &peer : outbound_peers) {
      if (peer && peer->sync_started()) {
        LOG_NET_TRACE("Resetting sync_started for peer {} to allow retry", peer->id());
        peer->set_sync_started(false);
      }
    }
  }
}

void HeaderSyncManager::ProcessTimers() {
  // Basic headers sync stall detection 
  // If initial sync is running and we haven't received headers for a while,
  // disconnect the sync peer to allow retrying another peer.
  uint64_t sync_id = sync_state_.sync_peer_id;
  int64_t last_us = sync_state_.last_headers_received_us;

  if (sync_id == NO_SYNC_PEER) return;

  // Use mockable wall-clock time for determinism in tests
  const int64_t now_us = util::GetTime() * kMicrosPerSecond;


  // Stall handling: if our designated sync peer hasn't delivered HEADERS within the timeout,
  // disconnect it and allow reselection. This avoids getting stuck forever on a slow or unresponsive
  // peer. We don’t trigger reselection inline; the normal SendMessages/maintenance loop handles it,
  // keeping control flow simple and testable.
  if (last_us > 0 && (now_us - last_us) > kHeadersSyncTimeoutUs) {
    LOG_NET_INFO("Headers sync stalled for {:.1f}s with peer {}, disconnecting",
                static_cast<double>(now_us - last_us) / static_cast<double>(kMicrosPerSecond), sync_id);
    // Ask ConnectionManager to drop the peer. This triggers OnPeerDisconnected() via callback
    peer_manager_.remove_peer(static_cast<int>(sync_id));
    // Do NOT call CheckInitialSync() here; SendMessages/maintenance cadence will do reselection.
  }
}

void HeaderSyncManager::CheckInitialSync() {
  // Prefer to run initial sync when in IBD (Bitcoin Core pattern).
  // If there is no current sync peer, we may (re)select one even post-IBD;
  // the resulting GETHEADERS is harmless if fully synced.
  // Bitcoin Core uses nSyncStarted counter - only selects new sync peer when nSyncStarted==0.
  if (HasSyncPeer()) {
    LOG_NET_TRACE("CheckInitialSync: already have sync peer, returning");
    return;
  }

  LOG_NET_DEBUG("CheckInitialSync: selecting new sync peer");

  // Outbound-only sync peer selection 
  auto outbound_peers = peer_manager_.get_outbound_peers();
  LOG_NET_DEBUG("CheckInitialSync: found {} outbound peers", outbound_peers.size());
  for (const auto &peer : outbound_peers) {
    if (!peer) continue;
    LOG_NET_DEBUG("CheckInitialSync: checking peer {}, sync_started={}", peer->id(), peer->sync_started());
    if (peer->sync_started()) {
      continue; // Already started with this peer
    }
    if (peer->is_feeler()) continue;    // Skip feelers - they auto-disconnect on VERACK
    // Gate initial sync on completed handshake to avoid protocol violations
    if (!peer->successfully_connected()) continue; // Wait until VERACK

    SetSyncPeerUnlocked(peer->id());
    peer->set_sync_started(true);  // CNodeState::fSyncStarted

    // Send GETHEADERS to initiate sync
    RequestHeadersFromPeer(peer);
    return; // Only one sync peer
  }

}

void HeaderSyncManager::RequestHeadersFromPeer(PeerPtr peer) {
  if (!peer) {
    return;
  }

  // Get block locator from chainstate
  // For initial sync, use pprev trick
  // This ensures we get non-empty response even if peer is at same tip
  CBlockLocator locator = GetLocatorFromPrev();

  // Create GETHEADERS message
  auto msg = std::make_unique<message::GetHeadersMessage>();
  msg->version = protocol::PROTOCOL_VERSION;

  // Convert locator hashes from uint256 to std::array<uint8_t, 32>
  for (const auto &hash : locator.vHave) {
    std::array<uint8_t, 32> hash_array;
    std::memcpy(hash_array.data(), hash.data(), 32);
    msg->block_locator_hashes.push_back(hash_array);
  }

  // hash_stop is all zeros (get as many as possible)
  msg->hash_stop.fill(0);

  // Bitcoin Core distinguishes between "initial getheaders" and "more getheaders"
  // based on whether we've received headers from this peer before
  bool is_initial = !peer->sync_started();
  int start_height = peer->start_height();

  if (is_initial) {
    LOG_NET_DEBUG("initial getheaders ({}) to peer={} (startheight:{})",
                  msg->block_locator_hashes.size(), peer->id(), start_height);
  } else {
    LOG_NET_DEBUG("more getheaders ({}) to end to peer={} (startheight:{})",
                  msg->block_locator_hashes.size(), peer->id(), start_height);
  }

  LOG_NET_TRACE("requesting headers from peer={} (locator size: {})",
                peer->id(), msg->block_locator_hashes.size());

  peer->send_message(std::move(msg));
}

bool HeaderSyncManager::HandleHeadersMessage(PeerPtr peer,
                                              message::HeadersMessage *msg) {
  if (!peer || !msg) {
    return false;
  }

  const auto &headers = msg->headers;
  int peer_id = peer->id();

  // During IBD, only process large (batch) headers from the
  // designated sync peer. Allow small unsolicited announcements (1-2 headers)
  // from any peer. This avoids wasting bandwidth processing full batches from multiple peers.
  // Gate large batches to the designated sync peer ONLY during IBD (Bitcoin Core behavior).
  if (chainstate_manager_.IsInitialBlockDownload()) {
    // Accept small unsolicited announcements (see kMaxUnsolicitedAnnouncement at top of file)
    uint64_t sync_id = GetSyncPeerId();
    if (!headers.empty() && headers.size() > kMaxUnsolicitedAnnouncement &&
        (sync_id == NO_SYNC_PEER || static_cast<uint64_t>(peer_id) != sync_id)) {
      LOG_NET_TRACE(
          "Ignoring unsolicited large headers batch from non-sync peer during IBD: peer={} size={}",
          peer_id, headers.size());
      // Do not penalize; just ignore per Core behavior
      return true;
    }
  }

  // Heuristic: if the batch's LAST header is already on our ACTIVE chain,
  // we treat the batch as extending known-valid work and SKIP anti-DoS checks
  // (e.g., low-work gating) for this batch. This avoids penalizing peers after
  // local invalidations/reorgs.
  // Important: Only the ACTIVE chain qualifies (not side chains), so attackers
  // cannot piggyback on validated-but-inactive forks to bypass the checks.
  bool skip_dos_checks = false;
  if (!headers.empty()) {
    const chain::CBlockIndex* last_header_index =
        chainstate_manager_.LookupBlockIndex(headers.back().GetHash());
    if (last_header_index && chainstate_manager_.IsOnActiveChain(last_header_index)) {
      skip_dos_checks = true;
      LOG_NET_TRACE("Peer {} sent {} headers, last header on active chain (log2_work={:.6f}), skipping DoS checks",
                    peer_id, headers.size(), std::log(last_header_index->nChainWork.getdouble()) / std::log(2.0));
    }
  }

  LOG_NET_TRACE("Processing {} headers from peer {}, skip_dos_checks={}",
                headers.size(), peer_id, skip_dos_checks);

  // Update last headers received timestamp
  sync_state_.last_headers_received_us = util::GetTime() * kMicrosPerSecond;

  // Empty reply: peer has no more headers to offer from our locator.
  // Clear current sync peer so the next CheckInitialSync() can choose another peer
  // (or do nothing if we're already up-to-date). No penalty for empty replies.
  // Bitcoin Core: Does NOT clear sync peer on empty headers - sticks with initial sync peer.
  // This prevents cycling through peers and ensures IBD INV gating works correctly.
  if (headers.empty()) {
    LOG_NET_DEBUG("received headers (0) peer={} - keeping as sync peer", peer_id);
    // Do NOT clear sync peer here - Bitcoin Core keeps the sync peer even after empty headers
    return true;
  }

  // DoS Protection: Check headers message size limit
  if (headers.size() > protocol::MAX_HEADERS_SIZE) {
    LOG_NET_ERROR("Rejecting oversized headers message from peer {} (size={}, max={})",
              peer_id, headers.size(), protocol::MAX_HEADERS_SIZE);
    peer_manager_.ReportOversizedMessage(peer_id);
    // Check if peer should be disconnected
    if (peer_manager_.ShouldDisconnect(peer_id)) {
      peer_manager_.remove_peer(peer_id);
    }
    ClearSyncPeer();
    return false;
  }

  LOG_NET_DEBUG("received headers ({}) peer={}", headers.size(), peer_id);

  // DoS Protection: Check if first header connects to known chain
  const uint256 &first_prev = headers[0].hashPrevBlock;
  bool prev_exists = chainstate_manager_.LookupBlockIndex(first_prev) != nullptr;

  if (!prev_exists) {
    uint256 first_hash = headers[0].GetHash();
    int unconnecting_count = peer_manager_.GetUnconnectingHeadersCount(peer_id);

    LOG_NET_DEBUG("received header {}: missing prev block {}, sending getheaders ({}) to end (peer={}, m_num_unconnecting_headers_msgs={})",
                  first_hash.ToString().substr(0, 16),
                  first_prev.ToString().substr(0, 16),
                  headers.size(),
                  peer_id,
                  unconnecting_count + 1);  // +1 because we increment below

    LOG_NET_WARN("headers don't connect to known chain from peer={} (first prevhash: {})",
             peer_id, first_prev.ToString());
    peer_manager_.IncrementUnconnectingHeaders(peer_id);
    // Check if peer should be disconnected (but continue processing as orphan chain)
    if (peer_manager_.ShouldDisconnect(peer_id)) {
      peer_manager_.remove_peer(peer_id);
    }
    // Do NOT ClearSyncPeer() and do NOT return; proceed to treat batch as orphans
  }

  // DoS Protection: Cheap PoW commitment check
  bool pow_ok = chainstate_manager_.CheckHeadersPoW(headers);
  if (!pow_ok) {
    LOG_NET_ERROR("headers failed PoW commitment check from peer={}", peer_id);
    peer_manager_.ReportInvalidPoW(peer_id);
    if (peer_manager_.ShouldDisconnect(peer_id)) {
      peer_manager_.remove_peer(peer_id);
    }
    ClearSyncPeer();
    return false;
  }

  // DoS Protection: Check headers are continuous
  // IMPORTANT: Check continuity BEFORE resetting unconnecting counter to prevent
  // attackers from gaming the system by alternating between unconnecting batches
  // and batches that connect but have internal gaps.
  bool continuous_ok = validation::CheckHeadersAreContinuous(headers);
  if (!continuous_ok) {
    LOG_NET_ERROR("non-continuous headers from peer={}", peer_id);
    peer_manager_.ReportNonContinuousHeaders(peer_id);
    if (peer_manager_.ShouldDisconnect(peer_id)) {
      peer_manager_.remove_peer(peer_id);
    }
    ClearSyncPeer();
    return false;
  }

  // Headers connect AND are continuous - reset unconnecting counter
  if (prev_exists) {
    int old_count = peer_manager_.GetUnconnectingHeadersCount(peer_id);
    if (old_count > 0) {
      LOG_NET_DEBUG("peer={}: resetting m_num_unconnecting_headers_msgs ({} -> 0)",
                    peer_id, old_count);
    }
    peer_manager_.ResetUnconnectingHeaders(peer_id);
  }

  // DoS Protection: Check for low-work headers 
  // Only run anti-DoS check if we haven't already validated this work
  if (!skip_dos_checks) {
    const chain::CBlockIndex* chain_start = chainstate_manager_.LookupBlockIndex(headers[0].hashPrevBlock);
    if (chain_start) {
      arith_uint256 total_work = chain_start->nChainWork + validation::CalculateHeadersWork(headers);

      // Get our dynamic anti-DoS threshold
      arith_uint256 minimum_work = validation::GetAntiDoSWorkThreshold(
          chainstate_manager_.GetTip(),
          chainstate_manager_.GetParams());

      // Bitcoin Core logic (TryLowWorkHeadersSync): Avoid DoS via low-difficulty-headers
      // by only processing if the headers are part of a chain with sufficient work.
      if (total_work < minimum_work) {
        // Only give up on this peer if their headers message was NOT full;
        // otherwise they have more headers after this, and their full chain
        // might have sufficient work even if this batch doesn't.
        if (headers.size() != protocol::MAX_HEADERS_SIZE) {
          // Batch was not full - peer has no more headers to offer
          LOG_NET_DEBUG("ignoring low-work chain from peer={} (work={}, threshold={}, height={})",
                        peer_id,
                        total_work.ToString(),
                        minimum_work.ToString(),
                        chain_start->nHeight + headers.size());
          // Bitcoin Core: Does NOT clear sync peer here, just ignores the headers
          // We match this by not calling ClearSyncPeer()
          return true;  // Handled (by ignoring)
        }
        // Batch was full - peer likely has more headers with additional work coming.
        // Don't abandon peer, just skip processing this batch and request more.
        LOG_NET_DEBUG("low-work headers from peer={} but batch is full (size={}, work={}, threshold={}), continuing sync",
                      peer_id,
                      headers.size(),
                      total_work.ToString(),
                      minimum_work.ToString());

        // SIMPLIFICATION vs Bitcoin Core:
        // Bitcoin Core enters HeadersSyncState here, which performs a two-phase download:
        // 1. PRESYNC: Download headers, validate PoW, accumulate work, store commitments
        // 2. REDOWNLOAD: Once sufficient work proven, re-download and accept headers
        // This prevents low-work chain DoS while using minimal memory (~1 bit per N headers).
        //
        // We simplify by just requesting more headers from the same peer and relying on:
        // - Stall detection (120s timeout) disconnects unresponsive peers
        // - No permanent memory allocated for low-work chains (headers not accepted)
        // - Sync peer reselection if current peer fails/stalls
        //
        // Unicity context: With 1-hour blocks, chains < 2000 blocks old (~83 days) fit
        // in a single batch, making multi-batch low-work attacks impractical. Even for
        // longer chains, 120s timeout provides adequate protection. HeadersSyncState's
        // complexity is only needed for Bitcoin-scale chains (850k+ blocks, hours to sync).
        // Trade-off: May waste bandwidth on low-work chains, but DoS-safe and simpler.
        // See Bitcoin Core's headerssync.h/cpp for full HeadersSyncState implementation.
        RequestHeadersFromPeer(peer);
        return true;  // Handled (by requesting more)
      }
    }
  } else {
    LOG_NET_TRACE("Skipping low-work check for peer {} (headers already validated)", peer_id);
  }

  // Store batch size
  last_batch_size_ = headers.size();

  // Accept all headers into block index
  for (const auto &header : headers) {
    validation::ValidationState state;
    chain::CBlockIndex *pindex =
        chainstate_manager_.AcceptBlockHeader(header, state, /*min_pow_checked=*/true);

    if (!pindex) {
      const std::string &reason = state.GetRejectReason();

      // Missing parent: cache as orphan (network-layer decision)
      if (reason == "prev-blk-not-found") {
        if (chainstate_manager_.AddOrphanHeader(header, peer_id)) {
          LOG_NET_TRACE("header from peer={} cached as orphan: {}",
                        peer_id, header.GetHash().ToString().substr(0, 16));
          continue;
        } else {
          LOG_NET_TRACE("peer={} exceeded orphan limit while caching prev-missing header",
                        peer_id);
          peer_manager_.ReportTooManyOrphans(peer_id);
          if (peer_manager_.ShouldDisconnect(peer_id)) {
            peer_manager_.remove_peer(peer_id);
          }
          ClearSyncPeer();
          return false;
        }
      }

      // Duplicate header - Bitcoin Core approach: only penalize if it's a duplicate of an INVALID header
      if (reason == "duplicate") {
        const chain::CBlockIndex* existing = chainstate_manager_.LookupBlockIndex(header.GetHash());
        LOG_NET_TRACE("Duplicate header from peer {}: {} (existing={}, valid={}, skip_dos_checks={})",
                      peer_id, header.GetHash().ToString().substr(0, 16),
                      existing ? "yes" : "no", existing ? existing->IsValid() : false, skip_dos_checks);

        // Bitcoin Core parity:
        // - If we're skipping DoS checks (ancestor on active chain), do not penalize duplicates.
        if (skip_dos_checks) {
          LOG_NET_TRACE("Skipping DoS check for duplicate header (batch contains ancestors)");
          continue;
        }
        // - If the duplicate refers to a valid-known header, it's benign; ignore.
        if (existing && existing->IsValid()) {
          LOG_NET_TRACE("Duplicate header already known valid; ignoring without penalty");
          continue;
        }
        // - If the duplicate refers to a known-invalid header, penalize ONCE per unique header per peer.
        const uint256 h = header.GetHash();
        if (peer_manager_.HasInvalidHeaderHash(peer_id, h)) {
          LOG_NET_TRACE("Peer {} re-sent duplicate of known-invalid header {}, suppressing additional penalty",
                        peer_id, h.ToString().substr(0,16));
          continue;
        }
        LOG_NET_WARN("Peer {} sent duplicate of KNOWN-INVALID header: {}", peer_id, h.ToString().substr(0,16));
        peer_manager_.ReportInvalidHeader(peer_id, "duplicate-invalid");
        peer_manager_.NoteInvalidHeaderHash(peer_id, h);
        if (peer_manager_.ShouldDisconnect(peer_id)) {
          peer_manager_.remove_peer(peer_id);
        }
        ClearSyncPeer();
        return false;
      }

      // Invalid header - penalize once per unique header (avoid double-penalty on duplicates)
      if (reason == "high-hash" || reason == "bad-diffbits" ||
          reason == "time-too-old" || reason == "time-too-new" ||
          reason == "bad-version" ||
          reason == "bad-prevblk" || reason == "bad-genesis" ||
          reason == "genesis-via-accept") {
        const uint256 h = header.GetHash();
        if (peer_manager_.HasInvalidHeaderHash(peer_id, h)) {
          LOG_NET_TRACE("peer {} re-sent previously invalid header {}, ignoring duplicate penalty",
                        peer_id, h.ToString().substr(0,16));
          continue; // no additional penalty for same invalid header
        }
        LOG_NET_ERROR("peer={} sent invalid header: {}", peer_id, reason);
        peer_manager_.ReportInvalidHeader(peer_id, reason);
        peer_manager_.NoteInvalidHeaderHash(peer_id, h);
        if (peer_manager_.ShouldDisconnect(peer_id)) {
          peer_manager_.remove_peer(peer_id);
        }
        ClearSyncPeer();
        return false;
      }

      // Unknown rejection reason - log and fail
      LOG_NET_ERROR("failed to accept header from peer={} - hash: {} reason: {} debug: {}",
                peer_id, header.GetHash().ToString(), reason, state.GetDebugMessage());
      ClearSyncPeer();
      return false;
    }

    // Add to candidate set for batch activation
    chainstate_manager_.TryAddBlockIndexCandidate(pindex);
  }

  // Activate best chain ONCE for the entire batch
  LOG_NET_TRACE("calling ActivateBestChain for batch of {} headers", headers.size());
  bool activate_result = chainstate_manager_.ActivateBestChain(nullptr);
  LOG_NET_TRACE("ActivateBestChain returned {}", activate_result ? "true" : "FALSE");
  if (!activate_result) {
    LOG_NET_DEBUG("failed to activate chain (ActivateBestChain returned false)");
    ClearSyncPeer();
    return false;
  }

  // Show progress during IBD or new block notification
  if (chainstate_manager_.IsInitialBlockDownload()) {
    const chain::CBlockIndex *tip = chainstate_manager_.GetTip();
    if (tip) {
      LOG_NET_TRACE("synchronizing block headers, height: {}", tip->nHeight);
    }
  } else {
    const chain::CBlockIndex *tip = chainstate_manager_.GetTip();
    if (tip) {
      LOG_NET_TRACE("new block header: height={} hash={}...", tip->nHeight,
               tip->GetBlockHash().ToString().substr(0, 16));
    }
  }

  // Check if we should request more headers
  // Bitcoin Core: Never clears fSyncStarted after receiving headers successfully
  // Only timeout clears it. This prevents trying all peers sequentially.
  if (ShouldRequestMore()) {
    RequestHeadersFromPeer(peer);
  } else {
    // Do not clear sync peer here. Keep the current sync peer so that future
    // INV announcements from this peer can trigger additional GETHEADERS,
    // matching Bitcoin Core behavior where fSyncStarted remains until timeout.
  }

  return true;
}

bool HeaderSyncManager::HandleGetHeadersMessage(
    PeerPtr peer, message::GetHeadersMessage *msg) {
  if (!peer || !msg) {
    return false;
  }

  int peer_id = peer->id();

  // NOTE: Bitcoin Core does NOT rate limit GETHEADERS requests
  // DoS protection relies on bounded response size (MAX_HEADERS_SIZE = 2000)
  // Rate limiting would break legitimate IBD sync which sends many requests rapidly

  LOG_NET_TRACE("peer={} requested headers (locator size: {})", peer_id,
                msg->block_locator_hashes.size());

  // Bitcoin Core: Ignore getheaders if our active chain has too little work
  // This prevents serving headers from a potentially bogus chain during initial sync
  // Exception: peers with Download permission (including NoBan) can still request headers
  const chain::CBlockIndex *active_tip = chainstate_manager_.GetTip();
  NetPermissionFlags permissions = peer_manager_.GetPeerPermissions(peer_id);
  if ((!active_tip || (active_tip->nChainWork < UintToArith256(chainstate_manager_.GetParams().GetConsensus().nMinimumChainWork)))
      && !HasPermission(permissions, NetPermissionFlags::Download)) {
    LOG_NET_DEBUG("Ignoring getheaders from peer={} because active chain has too little work; sending empty response", peer_id);

    // Send empty HEADERS response to indicate we're aware but can't help
    auto response = std::make_unique<message::HeadersMessage>();
    peer->send_message(std::move(response));
    return true;
  }

  // Find the fork point using the block locator
  // IMPORTANT: Only consider blocks on the ACTIVE chain, not side chains
  // Otherwise we might find a fork point on a side chain we know about
  const chain::CBlockIndex *fork_point = nullptr;
  for (const auto &hash_array : msg->block_locator_hashes) {
    // Convert std::array<uint8_t, 32> to uint256
    uint256 hash;
    std::memcpy(hash.data(), hash_array.data(), 32);

    const chain::CBlockIndex *pindex =
        chainstate_manager_.LookupBlockIndex(hash);
    if (chainstate_manager_.IsOnActiveChain(pindex)) {
      // Found a block that exists AND is on our active chain
      fork_point = pindex;
      LOG_NET_TRACE("found fork point at height {} (hash={}) on active chain",
                   fork_point->nHeight, hash.ToString().substr(0, 16));
      break;
    }
  }

  // NOTE: In practice, fork_point should never be null because genesis is hardcoded
  // and should always be in the locator. If it is null (peer on different network),
  // we send an empty headers message (matches Bitcoin Core behavior).
  // Bitcoin Core: if FindFork returns nullptr, pindex stays nullptr and loop doesn't execute.
  if (!fork_point) {
    LOG_NET_TRACE("no common blocks in locator from peer={} - sending empty headers", peer->id());
    auto response = std::make_unique<message::HeadersMessage>();
    peer->send_message(std::move(response));
    return true;
  }

  const chain::CBlockIndex *tip = chainstate_manager_.GetTip();
  LOG_NET_TRACE("preparing headers: fork_point height={} tip height={}",
                fork_point->nHeight, tip ? tip->nHeight : -1);

  // Build HEADERS response
  auto response = std::make_unique<message::HeadersMessage>();

  // Start from the block after fork point and collect headers
  const chain::CBlockIndex *pindex =
      chainstate_manager_.GetBlockAtHeight(fork_point->nHeight + 1);

  // Respect hash_stop (0 = no limit)
  uint256 stop_hash;
  bool has_stop = false;
  {
    // Convert std::array<uint8_t, 32> to uint256
    std::memcpy(stop_hash.data(), msg->hash_stop.data(), 32);
    has_stop = !stop_hash.IsNull();
  }

  while (pindex && response->headers.size() < protocol::MAX_HEADERS_SIZE) {
    CBlockHeader hdr = pindex->GetBlockHeader();
    response->headers.push_back(hdr);

    // If caller requested a stop-hash, include it and then stop
    if (has_stop && pindex->GetBlockHash() == stop_hash) {
      break;
    }

    if (pindex == tip) {
      break;
    }

    // Get next block in active chain
    pindex = chainstate_manager_.GetBlockAtHeight(pindex->nHeight + 1);
  }

  LOG_NET_TRACE("sending headers ({}) peer={}",
                response->headers.size(), peer->id());

  peer->send_message(std::move(response));
  return true;
}

bool HeaderSyncManager::IsSynced(int64_t max_age_seconds) const {
  const chain::CBlockIndex *tip = chainstate_manager_.GetTip();
  if (!tip) {
    return false;
  }

  // Check if tip is recent (use util::GetTime() to support mock time in tests)
  int64_t now = util::GetTime();
  int64_t tip_age = now - tip->nTime;

  return tip_age < max_age_seconds;
}

bool HeaderSyncManager::ShouldRequestMore() const {
  // Bitcoin Core logic 
  // if (nCount == MAX_HEADERS_RESULTS && !have_headers_sync)
  //
  // Request more if batch was full (peer may have more headers).
  // We don't have Bitcoin's HeadersSyncState mechanism, so we always
  // behave like have_headers_sync=false.
  //
  // IMPORTANT: Do NOT check IsSynced() here! In regtest, blocks are mined
  // instantly so tip is always "recent", which would cause us to abandon
  // sync peers prematurely. Bitcoin Core doesn't check sync state here either.
  return last_batch_size_ == protocol::MAX_HEADERS_SIZE;
}

CBlockLocator HeaderSyncManager::GetLocatorFromPrev() const {
  // Matches Bitcoin's initial sync logic
  // Start from pprev of tip to ensure non-empty response
  const chain::CBlockIndex *tip = chainstate_manager_.GetTip();
  if (!tip) {
    // At genesis, just use tip
    return chainstate_manager_.GetLocator();
  }

  if (tip->pprev) {
    // Use pprev - ensures peer sends back at least 1 header (our current tip)
    return chainstate_manager_.GetLocator(tip->pprev);
  } else {
    // Tip is genesis (no pprev), use tip itself
    return chainstate_manager_.GetLocator();
  }
}

} // namespace network
} // namespace unicity
