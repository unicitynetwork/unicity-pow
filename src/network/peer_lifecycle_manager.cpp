#include "network/peer_lifecycle_manager.hpp"
#include "network/peer_discovery_manager.hpp"
#include "network/network_manager.hpp"  // For ConnectionResult enum
#include "network/notifications.hpp"
#include "util/logging.hpp"
#include "util/time.hpp"
#include "util/netaddress.hpp"
#include "network/protocol.hpp"
#include <algorithm>
#include <utility>  // for std::exchange
#include <limits>
#include <fstream>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <fcntl.h>
#include <unistd.h>

using json = nlohmann::json;

namespace unicity {
namespace network {

namespace {
// Max connection attempts per cycle (Bitcoin Core pattern)
static constexpr int MAX_CONNECTION_ATTEMPTS_PER_CYCLE = 100;

} // namespace

PeerLifecycleManager::PeerLifecycleManager(boost::asio::io_context &io_context, const Config &config, const std::string& datadir)
    : io_context_(io_context), config_(config), ban_manager_(std::make_unique<BanManager>(datadir)),
      misbehavior_manager_(std::make_unique<MisbehaviorManager>(peer_states_)) {
  // Load persistent bans from disk if datadir is provided
  if (!datadir.empty()) {
    ban_manager_->LoadBans(datadir);
  }
}

void PeerLifecycleManager::SetDiscoveryManager(PeerDiscoveryManager* disc_mgr) {
  discovery_manager_ = disc_mgr;
  if (discovery_manager_) {
    LOG_NET_DEBUG("PeerLifecycleManager: PeerDiscoveryManager injected for address lifecycle tracking");
  } else {
    LOG_NET_WARN("PeerLifecycleManager: SetDiscoveryManager called with nullptr - address tracking disabled");
  }
}

PeerLifecycleManager::~PeerLifecycleManager() {
  Shutdown();
  disconnect_all();
}

void PeerLifecycleManager::Shutdown() {
  shutting_down_.store(true, std::memory_order_release);
}

int PeerLifecycleManager::add_peer(PeerPtr peer, NetPermissionFlags permissions,
                          const std::string &address) {
  if (!peer) {
    return -1;
  }

  // Reject additions during bulk shutdown (atomic check)
  if (stopping_all_.load(std::memory_order_acquire)) {
    LOG_NET_TRACE("add_peer: rejected while disconnect_all in progress");
    return -1;
  }

  // Check bans BEFORE slot accounting (unless peer has NoBan permission)
  if (!HasPermission(permissions, NetPermissionFlags::NoBan)) {
    std::string peer_addr = address.empty() ? peer->address() : address;
    auto normalized_addr_opt = util::ValidateAndNormalizeIP(peer_addr);
    if (!normalized_addr_opt.has_value()) {
      LOG_NET_TRACE("add_peer: rejecting invalid IP address {}", peer_addr);
      return -1;
    }
    const std::string& normalized_addr = *normalized_addr_opt;

    if (IsBanned(normalized_addr)) {
      LOG_NET_TRACE("add_peer: rejecting banned address {}", normalized_addr);
      return -1;
    }

    if (IsDiscouraged(normalized_addr)) {
      LOG_NET_TRACE("add_peer: rejecting discouraged address {}", normalized_addr);
      return -1;
    }
  }

  // Check connection limits
  bool is_inbound = peer->is_inbound();
  bool is_feeler_new = peer->is_feeler();
  bool is_manual_new = peer->is_manual();
  size_t current_inbound = 0;
  size_t current_outbound_nonfeeler = 0;

  // Count current connections using peer_states_
  peer_states_.ForEach([&](int id, const PeerTrackingData& state) {
    if (state.peer->is_inbound()) {
      current_inbound++;
    } else {
      // Outbound: only count full-relay (exclude feelers and manual from slot consumption)
      if (!state.peer->is_feeler() && !state.peer->is_manual()) {
        current_outbound_nonfeeler++;
      }
    }
  });

  // Check outbound limit (no eviction for outbound)
  // Do not count feeler connections against outbound capacity, and do not gate them here
  if (!is_inbound && !is_feeler_new && !is_manual_new && current_outbound_nonfeeler >= config_.max_outbound_peers) {
    LOG_NET_TRACE("add_peer: reject outbound full (current_nonfeeler={} >= max={}) manual={} address={}",
                  current_outbound_nonfeeler, config_.max_outbound_peers, peer->is_manual(),
                  address.empty() ? peer->address() : address);
    return -1; // Too many outbound full-relay connections
  }

  // Check inbound limit - try eviction if at capacity
  if (is_inbound && current_inbound >= config_.max_inbound_peers) {
    bool evicted = evict_inbound_peer();

    if (!evicted) {
      LOG_NET_TRACE("add_peer: inbound at capacity and eviction failed (likely all peers protected by recent-connection window)");
      return -1; // Couldn't evict anyone, reject connection
    }
    // Recompute inbound counts after eviction to avoid TOCTOU
    size_t inbound_now = 0;
    peer_states_.ForEach([&](int id, const PeerTrackingData& state) {
      if (state.peer && state.peer->is_inbound()) inbound_now++;
    });
    if (inbound_now >= config_.max_inbound_peers) {
      LOG_NET_TRACE("add_peer: inbound still at capacity after eviction, rejecting");
      return -1;
    }
    // Successfully evicted and capacity confirmed; continue
  }

  // Enforce per-IP inbound limit before adding
  if (is_inbound) {
    auto new_addr_opt = util::ValidateAndNormalizeIP(peer->address());
    if (!new_addr_opt.has_value()) {
      LOG_NET_TRACE("add_peer: rejecting inbound with invalid IP address {}", peer->address());
      return -1;
    }
    const std::string& new_addr = *new_addr_opt;
    int same_ip_inbound = 0;
    peer_states_.ForEach([&](int id, const PeerTrackingData& state) {
      if (!state.peer->is_inbound()) return;
      auto peer_addr_opt = util::ValidateAndNormalizeIP(state.peer->address());
      if (peer_addr_opt.has_value() && *peer_addr_opt == new_addr) {
        same_ip_inbound++;
      }
    });
    if (same_ip_inbound >= MAX_INBOUND_PER_IP) {
      LOG_NET_TRACE("add_peer: reject inbound per-IP limit address={} count={} limit={}",
                    new_addr, same_ip_inbound, MAX_INBOUND_PER_IP);
      return -1; // Reject new inbound from same IP
    }
  }

  // Allocate peer ID (simple monotonic counter via member counter)
  int peer_id = next_peer_id_.fetch_add(1, std::memory_order_relaxed);
  peer->set_id(peer_id);  // Set the ID on the peer object

  // Create and insert PeerTrackingData
  std::string peer_address = address.empty() ? peer->address() : address;
  auto creation_time = util::GetSteadyTime();

  PeerTrackingData state(peer, creation_time);
  state.misbehavior.permissions = permissions;
  state.misbehavior.address = peer_address;
  peer_states_.Insert(peer_id, std::move(state));

LOG_NET_DEBUG("Added connection peer={}", peer_id);

  // Publish peer connected notification
  std::string connection_type = is_inbound ? "inbound" : (is_feeler_new ? "feeler" : "outbound");
  NetworkEvents().NotifyPeerConnected(peer_id, peer_address, peer->port(), connection_type);

  return peer_id;  // Return the assigned ID
}

void PeerLifecycleManager::remove_peer(int peer_id) {
  // Extract data from peer state before erasing
  PeerPtr peer;
  std::string peer_address;
  uint16_t peer_port = 0;
  std::string disconnect_reason = "disconnected";
  bool mark_addr_good = false;

  bool found = peer_states_.Read(peer_id, [&](const PeerTrackingData& state) {
    peer = state.peer;

    // Capture peer address and port for notification
    if (peer) {
      peer_address = peer->address();
      peer_port = peer->port();
    }

    // Before erasing, capture misbehavior score for addrman update logic
    int misbehavior_score = state.misbehavior.misbehavior_score;
    if (state.misbehavior.should_discourage) {
      disconnect_reason = "misbehavior (score: " + std::to_string(misbehavior_score) + ")";

      // Discourage the peer's address (internal call - NoBan already checked when setting should_discourage)
      if (peer) {
        auto normalized_addr_opt = util::ValidateAndNormalizeIP(state.misbehavior.address);
        if (normalized_addr_opt.has_value()) {
          const std::string& normalized_addr = *normalized_addr_opt;
          Discourage(normalized_addr);
          LOG_NET_TRACE("remove_peer: discouraged {} due to misbehavior (score={})",
                        normalized_addr, misbehavior_score);
        }
      }
    }

    // Decide whether to mark as good in addrman
    if (peer && peer->successfully_connected() && misbehavior_score == 0 &&
        !peer->is_inbound() && !peer->is_feeler()) {
      std::string addr_str_after = peer->target_address();
      uint16_t addr_port_after = peer->target_port();
      mark_addr_good = (addr_port_after != 0 && !addr_str_after.empty());
    }
  });

  if (!found) {
    // Peer already removed - this is OK, just return silently
    LOG_NET_TRACE("remove_peer({}): peer NOT FOUND in map", peer_id);
    return;
  }

  bool skip_notifications = shutting_down_.load(std::memory_order_acquire);

  // Erase from peer_states_ (thread-safe)
  peer_states_.Erase(peer_id);
  LOG_NET_TRACE("remove_peer: erased peer {} from map", peer_id);

  // Publish disconnect notification with mark_addr_good flag
  // PeerDiscoveryManager will handle marking the address as good via NetworkNotifications
  if (!skip_notifications) {
    NetworkEvents().NotifyPeerDisconnected(peer_id, peer_address, peer_port, disconnect_reason, mark_addr_good);
  }

  // Disconnect the peer
  if (peer) {
    LOG_NET_DEBUG("disconnecting peer={}", peer_id);
    peer->disconnect();
  }
}

PeerPtr PeerLifecycleManager::get_peer(int peer_id) {
  PeerPtr result;
  peer_states_.Read(peer_id, [&](const PeerTrackingData& state) {
    result = state.peer;
  });
  return result;
}

int PeerLifecycleManager::find_peer_by_address(const std::string &address,
                                      uint16_t port) {
  // Validate and normalize the search address
  auto needle_addr_opt = util::ValidateAndNormalizeIP(address);
  if (!needle_addr_opt.has_value()) {
    return -1; // Invalid address
  }
  const std::string& needle_addr = *needle_addr_opt;
  int result = -1;

  peer_states_.ForEach([&](int id, const PeerTrackingData& state) {
    if (result != -1) return; // Already found
    if (!state.peer) return;

    // Validate and normalize peer's address
    auto peer_addr_opt = util::ValidateAndNormalizeIP(state.peer->address());
    if (!peer_addr_opt.has_value() || *peer_addr_opt != needle_addr) return;
    if (port != 0) {
      if (state.peer->port() == port) result = id;
      return;
    }
    // No port specified: return the first matching address
    result = id;
  });

  return result;
}

std::vector<PeerPtr> PeerLifecycleManager::get_all_peers() {
  std::vector<PeerPtr> result;
  result.reserve(peer_states_.Size());  // Reserve capacity to avoid reallocations

  peer_states_.ForEach([&](int id, const PeerTrackingData& state) {
    result.push_back(state.peer);
  });

  // Sort by peer ID to ensure deterministic iteration order
  // (unordered_map iteration is non-deterministic)
  // Use stable_sort for consistent ordering when IDs are equal (shouldn't happen, but defensive)
  std::stable_sort(result.begin(), result.end(), [](const PeerPtr& a, const PeerPtr& b) {
    return a->id() < b->id();
  });

  return result;
}

std::vector<PeerPtr> PeerLifecycleManager::get_outbound_peers() {
  std::vector<PeerPtr> result;
  result.reserve(peer_states_.Size());  // Reserve capacity (worst case: all outbound)

  peer_states_.ForEach([&](int id, const PeerTrackingData& state) {
    if (!state.peer->is_inbound()) {
      result.push_back(state.peer);
    }
  });

  // Sort by peer ID to ensure deterministic iteration order
  // (unordered_map iteration is non-deterministic)
  std::stable_sort(result.begin(), result.end(), [](const PeerPtr& a, const PeerPtr& b) {
    return a->id() < b->id();
  });

  return result;
}

std::vector<PeerPtr> PeerLifecycleManager::get_inbound_peers() {
  std::vector<PeerPtr> result;
  result.reserve(peer_states_.Size());  // Reserve capacity (worst case: all inbound)

  peer_states_.ForEach([&](int id, const PeerTrackingData& state) {
    if (state.peer->is_inbound()) {
      result.push_back(state.peer);
    }
  });

  // Sort by peer ID to ensure deterministic iteration order
  // (unordered_map iteration is non-deterministic)
  std::stable_sort(result.begin(), result.end(), [](const PeerPtr& a, const PeerPtr& b) {
    return a->id() < b->id();
  });

  return result;
}

size_t PeerLifecycleManager::peer_count() const {
  size_t count = 0;
  peer_states_.ForEach([&](int id, const PeerTrackingData& state) {
    count++;
  });
  LOG_NET_TRACE("peer_count() called: returning {}", count);
  return count;
}

size_t PeerLifecycleManager::outbound_count() const {
  size_t count = 0;

  // Count outbound peers (excluding feelers and manual which don't consume outbound slots)
  // Bitcoin Core pattern: Feelers and manual connections don't count against MAX_OUTBOUND_FULL_RELAY_CONNECTIONS
  peer_states_.ForEach([&](int id, const PeerTrackingData& state) {
    if (!state.peer->is_inbound() && !state.peer->is_feeler() && !state.peer->is_manual()) {
      count++;
    }
  });

  return count;
}

size_t PeerLifecycleManager::inbound_count() const {
  size_t count = 0;

  peer_states_.ForEach([&](int id, const PeerTrackingData& state) {
    if (state.peer->is_inbound()) {
      count++;
    }
  });

  return count;
}

bool PeerLifecycleManager::needs_more_outbound() const {
  return outbound_count() < config_.target_outbound_peers;
}

bool PeerLifecycleManager::can_accept_inbound() const {
  return inbound_count() < config_.max_inbound_peers;
}

bool PeerLifecycleManager::can_accept_inbound_from(const std::string& address) const {
  // Check global inbound limit
  size_t inbound_now = 0;
  peer_states_.ForEach([&](int id, const PeerTrackingData& state) {
    if (state.peer && state.peer->is_inbound()) inbound_now++;
  });
  if (inbound_now >= config_.max_inbound_peers) return false;

  auto needle_opt = util::ValidateAndNormalizeIP(address);
  if (!needle_opt.has_value()) {
    return false; // Invalid address, reject
  }
  const std::string& needle = *needle_opt;
  int same_ip_inbound = 0;
  peer_states_.ForEach([&](int id, const PeerTrackingData& state) {
    if (!state.peer || !state.peer->is_inbound()) return;
    auto peer_addr_opt = util::ValidateAndNormalizeIP(state.peer->address());
    if (peer_addr_opt.has_value() && *peer_addr_opt == needle) {
      same_ip_inbound++;
    }
  });

  return same_ip_inbound < MAX_INBOUND_PER_IP;
}
bool PeerLifecycleManager::evict_inbound_peer() {
  // Collect inbound peers that can be evicted
  // Protection rules (inspired by Bitcoin's SelectNodeToEvict):
  // 1. Never evict outbound peers
  // 2. Protect recently connected peers (last 10 seconds)
  // 3. Prefer evicting peers with worst ping times

  struct EvictionCandidate {
    int peer_id;
    std::chrono::steady_clock::time_point connected_time;
    int64_t ping_time_ms;
  };

  std::vector<EvictionCandidate> candidates;
  auto now = util::GetSteadyTime();

  peer_states_.ForEach([&](int id, const PeerTrackingData& state) {
    // Only consider inbound peers
    if (!state.peer->is_inbound()) {
      return;
    }

    // Protect peers with NoBan permission from eviction
    if (HasPermission(state.misbehavior.permissions, NetPermissionFlags::NoBan)) {
      return;  // Protected from eviction
    }

    // Protect recently connected peers (within 10 seconds)
    auto connected_time = state.peer->stats().connected_time.load(std::memory_order_relaxed);
    auto now_duration = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch());
    auto connection_age = now_duration - connected_time;
    if (connection_age.count() < 10) {
      return;
    }

    auto ping_ms = state.peer->stats().ping_time_ms.load(std::memory_order_relaxed);
    // Convert connected_time back to time_point for sorting
    auto connected_tp = std::chrono::steady_clock::time_point(connected_time);
    candidates.push_back({id, connected_tp, ping_ms.count()});
  });

  // If no candidates, can't evict
  if (candidates.empty()) {
    return false;
  }

  // Simple eviction strategy for headers-only chain:
  // Evict the peer with the worst (highest) ping time, or oldest connection if
  // no ping data
  int worst_peer_id = -1;
  auto oldest_connected = std::chrono::steady_clock::time_point::max();
  // Map unknown ping (-1) to a large sentinel so we prefer evicting unknowns
  auto map_ping = [](int64_t p){ return p < 0 ? std::numeric_limits<int64_t>::max()/2 : p; };
  int64_t worst_score = std::numeric_limits<int64_t>::min();

  for (const auto &candidate : candidates) {
    int64_t cand = map_ping(candidate.ping_time_ms);
    if (cand > worst_score) {
      worst_score = cand;
      worst_peer_id = candidate.peer_id;
      oldest_connected = candidate.connected_time;
    } else if (cand == worst_score) {
      // Tie-breaker: prefer evicting older connection
      if (candidate.connected_time < oldest_connected) {
        worst_peer_id = candidate.peer_id;
        oldest_connected = candidate.connected_time;
      } else if (candidate.connected_time == oldest_connected) {
        // Final tie-breaker: lower peer_id (deterministic for tests with simultaneous connections)
        if (candidate.peer_id < worst_peer_id) {
          worst_peer_id = candidate.peer_id;
        }
      }
    }
  }

  if (worst_peer_id >= 0) {
    remove_peer(worst_peer_id);
    return true;
  }

  return false;
}

void PeerLifecycleManager::disconnect_all() {
  // Set stopping flag to prevent new peers from being added
  stopping_all_.store(true, std::memory_order_release);

  bool skip_notifications = shutting_down_.load(std::memory_order_acquire);

  // Get all peers to disconnect
  std::map<int, PeerPtr> peers_to_disconnect;
  peer_states_.ForEach([&](int id, const PeerTrackingData& state) {
    peers_to_disconnect[id] = state.peer;
  });

  // Publish disconnect notifications for all peers
  if (!skip_notifications) {
    for (const auto &[id, peer] : peers_to_disconnect) {
      if (peer) {
        NetworkEvents().NotifyPeerDisconnected(id, peer->address(), peer->port(), "shutdown");
      }
    }
  }

  // Clear all peer states
  peer_states_.Clear();

  // Disconnect all peers
  for (auto &[id, peer] : peers_to_disconnect) {
    if (peer) {
      peer->disconnect();
    }
  }

  // Allow add_peer() after bulk disconnect completes
  stopping_all_.store(false, std::memory_order_release);
}

#ifdef UNICITY_TESTS
void PeerLifecycleManager::TestOnlySetPeerCreatedAt(int peer_id, std::chrono::steady_clock::time_point tp) {
  peer_states_.Modify(peer_id, [&](PeerTrackingData& state) {
    state.created_at = tp;
  });
}
#endif

void PeerLifecycleManager::process_periodic() {
  std::vector<int> to_remove;

  // Count peers for logging
  size_t peer_count = 0;
  peer_states_.ForEach([&](int id, const PeerTrackingData& state) {
    peer_count++;
  });
  LOG_NET_TRACE("process_periodic() peers={}", peer_count);

  // Find disconnected peers and peers marked for disconnection
  peer_states_.ForEach([&](int id, const PeerTrackingData& state) {
    if (!state.peer->is_connected()) {
      LOG_NET_TRACE("process_periodic: peer={} not connected, marking for removal", id);
      to_remove.push_back(id);
      return;
    }

    // Enforce feeler max lifetime
    if (state.peer->is_feeler()) {
      auto age = std::chrono::duration_cast<std::chrono::seconds>(
          util::GetSteadyTime() - state.created_at);
      if (age.count() >= FEELER_MAX_LIFETIME_SEC) {
        LOG_NET_TRACE("process_periodic: feeler peer={} exceeded lifetime ({}s >= {}s), marking for removal",
                      id, age.count(), FEELER_MAX_LIFETIME_SEC);
        to_remove.push_back(id);
        return;
      }
    }

    // Check for peers marked for disconnection due to misbehavior
    LOG_NET_TRACE("process_periodic: checking peer={} score={} should_discourage={}",
                  id, state.misbehavior.misbehavior_score, state.misbehavior.should_discourage);

    if (state.misbehavior.should_discourage) {
      // Never disconnect peers with NoBan permission (matches Bitcoin)
      if (HasPermission(state.misbehavior.permissions, NetPermissionFlags::NoBan)) {
        LOG_NET_TRACE("process_periodic: skipping NoBan peer={} (score={} but protected)",
                      id, state.misbehavior.misbehavior_score);
        return;
      }

      // Add to removal list if not already there
      if (std::find(to_remove.begin(), to_remove.end(), id) == to_remove.end()) {
        to_remove.push_back(id);
        LOG_NET_INFO("Disconnecting misbehaving peer {} (score: {})",
                     id, state.misbehavior.misbehavior_score);
      }
    }
  });

  LOG_NET_TRACE("process_periodic: removing {} peers", to_remove.size());

  // Remove disconnected peers
  for (int peer_id : to_remove) {
    remove_peer(peer_id);
  }

  // Cleanup stale addresses in AddressManager
  if (discovery_manager_) {
    discovery_manager_->CleanupStale();
  }

  // DEBUG metrics snapshot (trace level to avoid noise in normal runs)
  LOG_NET_TRACE(
      "metrics: outbound attempts={} successes={} failures={} | feeler attempts={} successes={} failures={}",
      metrics_outbound_attempts_.load(std::memory_order_relaxed),
      metrics_outbound_successes_.load(std::memory_order_relaxed),
      metrics_outbound_failures_.load(std::memory_order_relaxed),
      metrics_feeler_attempts_.load(std::memory_order_relaxed),
      metrics_feeler_successes_.load(std::memory_order_relaxed),
      metrics_feeler_failures_.load(std::memory_order_relaxed));
}

// === Misbehavior Tracking Public API ===

// === Misbehavior Tracking (delegated to MisbehaviorManager) ===

void PeerLifecycleManager::ReportInvalidPoW(int peer_id) {
  misbehavior_manager_->ReportInvalidPoW(peer_id);
}

void PeerLifecycleManager::ReportOversizedMessage(int peer_id) {
  misbehavior_manager_->ReportOversizedMessage(peer_id);
}

void PeerLifecycleManager::ReportNonContinuousHeaders(int peer_id) {
  misbehavior_manager_->ReportNonContinuousHeaders(peer_id);
}

void PeerLifecycleManager::ReportLowWorkHeaders(int peer_id) {
  misbehavior_manager_->ReportLowWorkHeaders(peer_id);
}

void PeerLifecycleManager::ReportInvalidHeader(int peer_id, const std::string &reason) {
  misbehavior_manager_->ReportInvalidHeader(peer_id, reason);
}

void PeerLifecycleManager::ReportTooManyOrphans(int peer_id) {
  misbehavior_manager_->ReportTooManyOrphans(peer_id);
}

bool PeerLifecycleManager::ShouldDisconnect(int peer_id) const {
  return misbehavior_manager_->ShouldDisconnect(peer_id);
}

int PeerLifecycleManager::GetMisbehaviorScore(int peer_id) const {
  return misbehavior_manager_->GetMisbehaviorScore(peer_id);
}

NetPermissionFlags PeerLifecycleManager::GetPeerPermissions(int peer_id) const {
  NetPermissionFlags result = NetPermissionFlags::None;
  peer_states_.Read(peer_id, [&](const PeerTrackingData& state) {
    result = state.misbehavior.permissions;
  });
  return result;
}

void PeerLifecycleManager::NoteInvalidHeaderHash(int peer_id, const uint256& hash) {
  misbehavior_manager_->NoteInvalidHeaderHash(peer_id, hash);
}

bool PeerLifecycleManager::HasInvalidHeaderHash(int peer_id, const uint256& hash) const {
  return misbehavior_manager_->HasInvalidHeaderHash(peer_id, hash);
}

void PeerLifecycleManager::IncrementUnconnectingHeaders(int peer_id) {
  misbehavior_manager_->IncrementUnconnectingHeaders(peer_id);
}

void PeerLifecycleManager::ResetUnconnectingHeaders(int peer_id) {
  misbehavior_manager_->ResetUnconnectingHeaders(peer_id);
}

int PeerLifecycleManager::GetUnconnectingHeadersCount(int peer_id) const {
  return misbehavior_manager_->GetUnconnectingHeadersCount(peer_id);
}

// === PeerTrackingData Accessors ===

std::optional<std::pair<uint256, int64_t>> PeerLifecycleManager::GetLastAnnouncement(int peer_id) const {
  std::optional<std::pair<uint256, int64_t>> result;
  peer_states_.Read(peer_id, [&](const PeerTrackingData& state) {
    result = std::make_pair(state.last_announced_block, state.last_announce_time_s);
  });
  return result;
}

void PeerLifecycleManager::SetLastAnnouncedBlock(int peer_id, const uint256& hash, int64_t time_s) {
  peer_states_.Modify(peer_id, [&](PeerTrackingData& state) {
    state.last_announced_block = hash;
    state.last_announce_time_s = time_s;
  });
}

// Block announcement queue operations

std::vector<uint256> PeerLifecycleManager::GetBlocksForInvRelay(int peer_id) const {
  std::vector<uint256> result;
  peer_states_.Read(peer_id, [&](const PeerTrackingData& state) {
    result = state.blocks_for_inv_relay;
  });
  return result;
}

void PeerLifecycleManager::AddBlockForInvRelay(int peer_id, const uint256& hash) {
  peer_states_.Modify(peer_id, [&](PeerTrackingData& state) {
    // Simple dedup: only add if not already present
    if (std::find(state.blocks_for_inv_relay.begin(), state.blocks_for_inv_relay.end(), hash) ==
        state.blocks_for_inv_relay.end()) {
      state.blocks_for_inv_relay.push_back(hash);
    }
  });
}

void PeerLifecycleManager::RemoveBlockForInvRelay(int peer_id, const uint256& hash) {
  peer_states_.Modify(peer_id, [&](PeerTrackingData& state) {
    auto& queue = state.blocks_for_inv_relay;
    queue.erase(std::remove(queue.begin(), queue.end(), hash), queue.end());
  });
}

std::vector<uint256> PeerLifecycleManager::MoveBlocksForInvRelay(int peer_id) {
  std::vector<uint256> result;
  peer_states_.Modify(peer_id, [&](PeerTrackingData& state) {
    result = std::exchange(state.blocks_for_inv_relay, {});
  });
  return result;
}

void PeerLifecycleManager::ClearBlocksForInvRelay(int peer_id) {
  peer_states_.Modify(peer_id, [](PeerTrackingData& state) {
    state.blocks_for_inv_relay.clear();
  });
}

bool PeerLifecycleManager::HasRepliedToGetAddr(int peer_id) const {
  bool result = false;
  peer_states_.Read(peer_id, [&](const PeerTrackingData& state) {
    result = state.getaddr_replied;
  });
  return result;
}

void PeerLifecycleManager::MarkGetAddrReplied(int peer_id) {
  peer_states_.Modify(peer_id, [](PeerTrackingData& state) {
    state.getaddr_replied = true;
  });
}

void PeerLifecycleManager::AddLearnedAddress(int peer_id, const AddressKey& key, const LearnedEntry& entry) {
  peer_states_.Modify(peer_id, [&](PeerTrackingData& state) {
    state.learned_addresses[key] = entry;
  });
}

std::optional<LearnedMap> PeerLifecycleManager::GetLearnedAddresses(int peer_id) const {
  std::optional<LearnedMap> result;
  peer_states_.Read(peer_id, [&](const PeerTrackingData& state) {
    result = state.learned_addresses;
  });
  return result;
}

void PeerLifecycleManager::ClearLearnedAddresses(int peer_id) {
  peer_states_.Modify(peer_id, [](PeerTrackingData& state) {
    state.learned_addresses.clear();
  });
}

std::vector<std::pair<int, LearnedMap>> PeerLifecycleManager::GetAllLearnedAddresses() const {
  std::vector<std::pair<int, LearnedMap>> result;
  peer_states_.ForEach([&](int peer_id, const PeerTrackingData& state) {
    if (!state.learned_addresses.empty()) {
      result.emplace_back(peer_id, state.learned_addresses);
    }
  });

  // Sort by peer ID to ensure deterministic iteration order
  // (unordered_map iteration is non-deterministic)
  std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
    return a.first < b.first;
  });

  return result;
}

std::vector<protocol::TimestampedAddress>
PeerLifecycleManager::GetLearnedAddressesForGetAddr(int exclude_peer_id, size_t max_count) const {
  std::vector<protocol::TimestampedAddress> result;
  result.reserve(std::min<size_t>(max_count, 1000)); // Reserve reasonable amount

  // Collect peer IDs first for deterministic ordering
  std::vector<int> peer_ids;
  peer_states_.ForEach([&](int peer_id, const PeerTrackingData& state) {
    if (peer_id != exclude_peer_id && !state.learned_addresses.empty()) {
      peer_ids.push_back(peer_id);
    }
  });

  // Sort for deterministic iteration
  std::sort(peer_ids.begin(), peer_ids.end());

  // Collect addresses from peers until we reach max_count
  for (int peer_id : peer_ids) {
    if (result.size() >= max_count) break; // Early exit - avoid copying more data

    peer_states_.Read(peer_id, [&](const PeerTrackingData& state) {
      for (const auto& [addr_key, entry] : state.learned_addresses) {
        if (result.size() >= max_count) break; // Early exit within peer's addresses
        result.push_back(entry.ts_addr);
      }
    });
  }

  return result;
}

// === Ban Management (delegated to BanManager) ===

bool PeerLifecycleManager::LoadBans(const std::string& datadir) {
  return ban_manager_->LoadBans(datadir);
}

bool PeerLifecycleManager::SaveBans() {
  return ban_manager_->SaveBans();
}

void PeerLifecycleManager::Ban(const std::string &address, int64_t ban_time_offset) {
  ban_manager_->Ban(address, ban_time_offset);
}

void PeerLifecycleManager::Unban(const std::string &address) {
  ban_manager_->Unban(address);
}

bool PeerLifecycleManager::IsBanned(const std::string &address) const {
  return ban_manager_->IsBanned(address);
}

void PeerLifecycleManager::Discourage(const std::string &address) {
  ban_manager_->Discourage(address);
}

bool PeerLifecycleManager::IsDiscouraged(const std::string &address) const {
  return ban_manager_->IsDiscouraged(address);
}

void PeerLifecycleManager::ClearDiscouraged() {
  ban_manager_->ClearDiscouraged();
}

void PeerLifecycleManager::SweepDiscouraged() {
  ban_manager_->SweepDiscouraged();
}

std::map<std::string, BanManager::CBanEntry> PeerLifecycleManager::GetBanned() const {
  return ban_manager_->GetBanned();
}

void PeerLifecycleManager::ClearBanned() {
  ban_manager_->ClearBanned();
}

void PeerLifecycleManager::SweepBanned() {
  ban_manager_->SweepBanned();
}

void PeerLifecycleManager::AddToWhitelist(const std::string& address) {
  ban_manager_->AddToWhitelist(address);
}

void PeerLifecycleManager::RemoveFromWhitelist(const std::string& address) {
  ban_manager_->RemoveFromWhitelist(address);
}

bool PeerLifecycleManager::IsWhitelisted(const std::string& address) const {
  return ban_manager_->IsWhitelisted(address);
}

// === Protocol Message Handlers ===

// === Connection Management ===

void PeerLifecycleManager::AttemptOutboundConnections(IsRunningCallback is_running, ConnectCallback connect_fn) {
  if (!is_running()) {
    return;
  }

  if (!discovery_manager_) {
    LOG_NET_WARN("AttemptOutboundConnections called but discovery_manager not set");
    return;
  }

  // Track addresses selected in current cycle to avoid re-dialing the same addr repeatedly
  std::unordered_set<AddressKey, AddressKey::Hasher> selected_this_cycle;

  // Try multiple addresses per cycle to fill outbound connection slots quickly
  size_t skipped_duplicates = 0;
  std::string sample_dup_ip;
  uint16_t sample_dup_port = 0;
  for (int i = 0; i < MAX_CONNECTION_ATTEMPTS_PER_CYCLE && needs_more_outbound(); i++) {
    // Select an address from the address manager
    auto maybe_addr = discovery_manager_->Select();
    if (!maybe_addr) {
      break; // No addresses available
    }

    auto &addr = *maybe_addr;
    AddressKey key = MakeKey(addr);

    // Per-cycle dedup: skip the same address within this loop (summarize instead of logging each occurrence)
    if (selected_this_cycle.find(key) != selected_this_cycle.end()) {
      ++skipped_duplicates;
      if (sample_dup_ip.empty()) {
        if (auto _ip_opt = protocol::NetworkAddressToString(addr); _ip_opt) {
          sample_dup_ip = *_ip_opt;
          sample_dup_port = addr.port;
        }
      }
      continue;
    }

    // Convert NetworkAddress to IP string for logging
    auto maybe_ip_str = protocol::NetworkAddressToString(addr);
    if (!maybe_ip_str) {
      LOG_NET_WARN("Failed to convert address to string, marking as failed");
      discovery_manager_->Failed(addr);
      continue;
    }

    const std::string &ip_str = *maybe_ip_str;

    // Check if already connected to this address
    if (find_peer_by_address(ip_str, addr.port) != -1) {
      continue;
    }

    selected_this_cycle.insert(key);

    LOG_NET_TRACE("Attempting outbound connection to {}:{}", ip_str, addr.port);

    // Mark as attempt (connection may still fail)
    discovery_manager_->Attempt(addr);

    // Metrics: outbound attempt
    metrics_outbound_attempts_.fetch_add(1, std::memory_order_relaxed);

    // Try to connect via callback
    auto result = connect_fn(addr);
    if (result != ConnectionResult::Success) {
      // Mark as failed for persistent error conditions (prevents infinite retry)
      if (result == ConnectionResult::AddressBanned ||
          result == ConnectionResult::AddressDiscouraged) {
        LOG_NET_DEBUG("Connection to {}:{} failed ({}) - marking as failed to trigger backoff",
                      ip_str, addr.port,
                      result == ConnectionResult::AddressBanned ? "banned" : "discouraged");
        discovery_manager_->Failed(addr);
      } else if (result != ConnectionResult::NoSlotsAvailable &&
                 result != ConnectionResult::AlreadyConnected) {
        LOG_NET_DEBUG("Connection initiation failed to {}:{}", ip_str, addr.port);
      }
    }
  }

  // Summarize duplicate skips for this cycle at trace level
  if (skipped_duplicates > 0) {
    LOG_NET_TRACE("AttemptOutboundConnections: skipped {} duplicate selections in cycle{}",
                  skipped_duplicates,
                  sample_dup_ip.empty() ? std::string("")
                                        : (std::string(" (e.g., ") + sample_dup_ip + ":" + std::to_string(sample_dup_port) + ")"));
  }
}

void PeerLifecycleManager::AttemptFeelerConnection(IsRunningCallback is_running,
                                                     GetTransportCallback get_transport,
                                                     SetupMessageHandlerCallback setup_handler,
                                                     uint32_t network_magic,
                                                     int32_t current_height,
                                                     uint64_t local_nonce) {
  if (!is_running()) {
    return;
  }

  if (!discovery_manager_) {
    LOG_NET_WARN("AttemptFeelerConnection called but discovery_manager not set");
    return;
  }

  // Enforce single feeler (Core parity)
  bool have_feeler = false;
  peer_states_.ForEach([&](int, const PeerTrackingData& st){ if (st.peer && st.peer->is_feeler()) have_feeler = true; });
  if (have_feeler) {
    return; // one feeler at a time
  }

  // Get address from "new" table (addresses we've heard about but never connected to)
  auto addr = discovery_manager_->SelectNewForFeeler();
  if (!addr) {
    return;
  }

  // Convert NetworkAddress to IP string
  auto addr_str_opt = protocol::NetworkAddressToString(*addr);
  if (!addr_str_opt) {
    return;
  }

  std::string address = *addr_str_opt;
  uint16_t port = addr->port;

  // Get transport layer
  auto transport = get_transport();
  if (!transport) {
    LOG_NET_ERROR("Failed to get transport for feeler connection");
    return;
  }

  // Allocate peer ID AFTER connection succeeds (Core pattern)
  auto holder = std::make_shared<TransportConnectionPtr>();
  auto callback = [this, address, port, addr, network_magic, current_height,
                   local_nonce, setup_handler, holder](bool success) {
    // Post to io_context to decouple from transport callback and ensure holder is assigned
    boost::asio::post(io_context_, [this, address, port, addr, success, network_magic,
                                    current_height, local_nonce, setup_handler, holder]() {
      auto connection_cb = *holder;
      if (!success || !connection_cb) {
        // Connection failed - no peer created, no ID allocated
        metrics_feeler_failures_.fetch_add(1, std::memory_order_relaxed);
        discovery_manager_->Attempt(*addr);
        return;
      }

      // Connection succeeded - NOW create the feeler peer and allocate ID
      auto peer = Peer::create_outbound(io_context_, connection_cb, network_magic,
                                       current_height, address, port,
                                       ConnectionType::FEELER);
      if (!peer) {
        LOG_NET_ERROR("Failed to create feeler peer for {}:{}", address, port);
        connection_cb->close();
        discovery_manager_->Attempt(*addr);
        return;
      }

      // Set local nonce
      peer->set_local_nonce(local_nonce);

      // Setup message handler
      setup_handler(peer.get());

      // Add to peer manager (allocates ID here - Bitcoin Core pattern)
      int peer_id = add_peer(peer);
      if (peer_id < 0) {
        LOG_NET_ERROR("Failed to add feeler peer {} to manager", address);
        // Clean up transient peer to avoid destructor warning
        peer->disconnect();
        discovery_manager_->Attempt(*addr);
        return;
      }

      // Get peer and start it
      auto peer_ptr = get_peer(peer_id);
      if (peer_ptr) {
        LOG_NET_DEBUG("Feeler connection to {}:{} (peer_id={})", address, port, peer_id);
        metrics_feeler_successes_.fetch_add(1, std::memory_order_relaxed);
        discovery_manager_->Good(*addr);
        peer_ptr->start();
      }
    });
  };

  auto connection = transport->connect(address, port, callback);
  *holder = connection;

  if (!connection) {
    LOG_NET_TRACE("Failed to initiate feeler connection to {}:{}", address, port);
    discovery_manager_->Attempt(*addr);
    return;
  }
}

void PeerLifecycleManager::ConnectToAnchors(const std::vector<protocol::NetworkAddress>& anchors,
                                             ConnectCallback connect_fn) {
  if (anchors.empty()) {
    return;
  }

  LOG_NET_TRACE("Connecting to {} anchor peers (eclipse attack resistance)", anchors.size());

  for (const auto& addr : anchors) {
    // Convert NetworkAddress to IP string for whitelist
    auto ip_opt = protocol::NetworkAddressToString(addr);
    if (ip_opt) {
      // Whitelist anchor peers (they get NoBan permission in connect callback)
      AddToWhitelist(*ip_opt);
    }

    // Connect to anchor with NoBan permission
    // Note: NetworkManager will call connect_to_with_permissions with NetPermissionFlags::NoBan
    auto result = connect_fn(addr);
    if (result != ConnectionResult::Success) {
      LOG_NET_DEBUG("Failed to connect to anchor {}:{}",
                    ip_opt ? *ip_opt : "unknown", addr.port);
    }
  }
}

bool PeerLifecycleManager::CheckIncomingNonce(uint64_t nonce, uint64_t local_nonce) {
  // Bitcoin Core CheckIncomingNonce pattern 
  // Check 1: Against our own local nonce (self-connection)
  if (nonce == local_nonce) {
    LOG_NET_INFO("Self-connection detected: incoming nonce {} matches our local nonce", nonce);
    return false;
  }

  // Check 2: Against all existing peers' remote nonces (duplicate connection or collision)
  // This catches cases where two nodes behind NAT accidentally choose the same nonce,
  // or where a peer tries to connect twice
  auto peers = get_all_peers();
  for (const auto& peer : peers) {
    // Check against the peer's remote nonce (the nonce they sent in their VERSION)
    // Skip peers that haven't completed handshake (no remote nonce yet)
    if (!peer->successfully_connected()) {
      continue;
    }

    // Bitcoin Core checks remote nonce of ALL peers (both inbound and outbound)
    if (peer->peer_nonce() == nonce) {
      LOG_NET_INFO("Nonce collision detected: incoming nonce {} matches existing peer {} ({})",
                   nonce, peer->id(), peer->address());
      return false;
    }
  }

  return true;  // Unique nonce, OK to proceed
}

ConnectionResult PeerLifecycleManager::ConnectTo(
    const protocol::NetworkAddress& addr,
    NetPermissionFlags permissions,
    std::shared_ptr<Transport> transport,
    OnGoodCallback on_good,
    OnAttemptCallback on_attempt,
    SetupMessageHandlerCallback setup_message_handler,
    uint32_t network_magic,
    int32_t chain_height,
    uint64_t local_nonce
) {
  // Convert NetworkAddress to IP string for transport layer
  auto ip_opt = protocol::NetworkAddressToString(addr);
  if (!ip_opt) {
    LOG_NET_ERROR("Failed to convert NetworkAddress to IP string");
    return ConnectionResult::TransportFailed;
  }
  const std::string& address = *ip_opt;
  uint16_t port = addr.port;

  // Check if address is banned
  if (IsBanned(address)) {
    return ConnectionResult::AddressBanned;
  }

  // Check if address is discouraged
  if (IsDiscouraged(address)) {
    return ConnectionResult::AddressDiscouraged;
  }

  // SECURITY: Prevent duplicate outbound connections to same peer
  // Bitcoin Core: AlreadyConnectedToAddress() check 
  // This prevents wasting connection slots and eclipse attack vulnerabilities
  if (find_peer_by_address(address, port) != -1) {
    return ConnectionResult::AlreadyConnected;
  }

  // Check if we can add more outbound connections
  if (!needs_more_outbound()) {
    return ConnectionResult::NoSlotsAvailable;
  }

  // In-flight dedup at connect-time: insert pending, skip if already pending
  {
    std::lock_guard<std::mutex> g(pending_outbound_mutex_);
    auto key = MakeKey(addr);
    if (pending_outbound_.find(key) != pending_outbound_.end()) {
#ifdef UNICITY_TESTS
      // Allow replacing a stale pending attempt only in tests (io.poll() nests callbacks)
      LOG_NET_TRACE("ConnectTo: replacing pending outbound entry for addr:{}:{} (test mode)", address, port);
      pending_outbound_.erase(key);
#else
      return ConnectionResult::AlreadyConnected;
#endif
    }
    pending_outbound_.insert(key);
  }

  LOG_NET_DEBUG("trying connection {}:{}", address, port);

  // Create async transport connection with callback (deliver connection via holder)
  auto holder = std::make_shared<TransportConnectionPtr>();
  auto cb = [this, address, port, addr, on_good, on_attempt, permissions, network_magic,
             chain_height, local_nonce, setup_message_handler, holder](bool success) {
    // IMPORTANT: Clear pending flag immediately in transport callback context.
    // This ensures that even if tests use io.poll() (which may not drain nested posts),
    // subsequent ConnectTo calls are not blocked by in-flight dedup state.
    {
      std::lock_guard<std::mutex> g(pending_outbound_mutex_);
      pending_outbound_.erase(MakeKey(addr));
    }

    // Post to io_context to decouple from transport callback context
    boost::asio::post(io_context_, [this, address, port, addr, success, on_good, on_attempt, permissions,
                                   network_magic, chain_height, local_nonce, setup_message_handler, holder]() {
      auto connection_cb = *holder;

      if (!success || !connection_cb) {
        // Connection failed - no peer created, no ID allocated (Bitcoin Core pattern)
        if (on_attempt) {
          on_attempt(addr);
        }
        // Mark as failed to back off in addrman
        if (discovery_manager_) {
          discovery_manager_->Failed(addr);
        }
        return;
      }

      // Connection succeeded - NOW create the peer and allocate ID
      // Determine connection type from permissions (Manual permission → MANUAL type)
      ConnectionType conn_type = HasPermission(permissions, NetPermissionFlags::Manual)
                                   ? ConnectionType::MANUAL
                                   : ConnectionType::OUTBOUND;
      auto peer = Peer::create_outbound(io_context_, connection_cb, network_magic,
                                       chain_height, address, port, conn_type);
      if (!peer) {
        LOG_NET_ERROR("Failed to create peer for {}:{}", address, port);
        // No peer created; close the raw connection held by holder
        connection_cb->close();
        if (on_attempt) {
          on_attempt(addr);
        }
        return;
      }

      // Set local nonce
      peer->set_local_nonce(local_nonce);

      // Setup message handler
      if (setup_message_handler) {
        setup_message_handler(peer.get());
      }

      // Add to peer manager (allocates ID here - Bitcoin Core pattern)
      int peer_id = add_peer(peer, permissions, address);
      if (peer_id < 0) {
        LOG_NET_ERROR("Failed to add outbound peer {} to manager", address);
        // Clean up transient peer to avoid destructor warning
        peer->disconnect();
        if (on_attempt) {
          on_attempt(addr);
        }
        return;
      }

      // Get peer and start it
      auto peer_ptr = get_peer(peer_id);
      if (peer_ptr) {
        LOG_NET_DEBUG("Connected to {}:{} (peer_id={})", address, port, peer_id);
        if (on_good) {
          on_good(addr);
        }
        peer_ptr->start();
      }
    });
  };

  auto connection = transport->connect(address, port, cb);
  *holder = connection;

  if (!connection) {
    LOG_NET_ERROR("Failed to initiate connection to {}:{}", address, port);
    // Remove pending and backoff
    {
      std::lock_guard<std::mutex> g(pending_outbound_mutex_);
      pending_outbound_.erase(MakeKey(addr));
    }
    // Track attempt before marking as failed (addrman contract)
    if (on_attempt) {
      on_attempt(addr);
    }
    if (discovery_manager_) {
      discovery_manager_->Failed(addr);
    }
    return ConnectionResult::TransportFailed;
  }

  return ConnectionResult::Success;
}

void PeerLifecycleManager::HandleInboundConnection(TransportConnectionPtr connection,
                                                    IsRunningCallback is_running,
                                                    SetupMessageHandlerCallback setup_handler,
                                                    uint32_t network_magic,
                                                    int32_t current_height,
                                                    uint64_t local_nonce,
                                                    NetPermissionFlags permissions) {
  if (!is_running() || !connection) {
    return;
  }

  // Get remote address for ban checking
  std::string remote_address = connection->remote_address();

  // Check if address is banned
  if (IsBanned(remote_address)) {
    LOG_NET_INFO("Rejected banned address: {}", remote_address);
    connection->close();
    return;
  }

  // Check if address is discouraged
  if (IsDiscouraged(remote_address)) {
    LOG_NET_INFO("Rejected discouraged address: {}", remote_address);
    connection->close();
    return;
  }

  // Check if we can accept more inbound connections (global and per-IP)
  if (!can_accept_inbound_from(remote_address)) {
    LOG_NET_TRACE("Rejecting inbound connection from {} (inbound limit reached)",
                  remote_address);
    connection->close();
    return;
  }

  // Create inbound peer
  auto peer = Peer::create_inbound(io_context_, connection,
                                   network_magic, current_height);
  if (peer) {
    // Set local nonce (in test mode where process-wide nonce is not set)
    peer->set_local_nonce(local_nonce);

    // Setup message handler via callback
    setup_handler(peer.get());

    // Add to peer manager FIRST (sets peer ID)
    int peer_id = add_peer(peer, permissions);
    if (peer_id < 0) {
      LOG_NET_ERROR("Failed to add inbound peer to manager");
      // Clean up the transient inbound peer to avoid destructor warning
      peer->disconnect();
      return;
    }

    // Retrieve peer and start it (NOW id_ is set correctly)
    peer_states_.Read(peer_id, [](const PeerTrackingData& state) {
      if (state.peer) {
        state.peer->start();
      }
    });
  }
}

// === Protocol Message Handlers ===

bool PeerLifecycleManager::HandleVerack(PeerPtr peer) {
  // Verify peer is still connected
  if (!peer || !peer->is_connected()) {
    LOG_NET_TRACE("Ignoring VERACK from disconnected peer");
    return true;
  }

  // Sanity check: by this point, the peer must be successfully_connected()
  // (Peer::handle_verack() marks the peer as successfully connected before this is called)
  assert(peer->successfully_connected() && "VERACK routed before peer marked successfully connected");
  if (!peer->successfully_connected()) {
    return true; // Defensive in release builds
  }

  // Mark address as good after handshake: outbound full-relay peers only
  // Bitcoin Core excludes feelers from Connected() - they're only for testing liveness
  if (!peer->is_inbound() && !peer->is_feeler() && discovery_manager_) {
    try {
      auto na = protocol::NetworkAddress::from_string(peer->address(), peer->port());
      discovery_manager_->Good(na);
    } catch (const std::exception& e) {
      LOG_NET_WARN("HandleVerack: failed to mark addr good for {}:{}: {}", peer->address(), peer->port(), e.what());
    }
  }

  // GETADDR policy: send exactly once to outbound full-relay peers
  // Bitcoin Core does NOT send GETADDR to feelers (they disconnect immediately)
  if (!peer->is_inbound() && !peer->is_feeler() && !peer->has_sent_getaddr()) {
    auto getaddr = std::make_unique<message::GetAddrMessage>();
    peer->send_message(std::move(getaddr));
    peer->mark_getaddr_sent();
    LOG_NET_DEBUG("Sent GETADDR to {}:{} to populate address manager", peer->address(), peer->port());

    // Notify PeerDiscoveryManager to boost ADDR rate limit bucket (Bitcoin Core pattern)
    // This allows the peer to send up to 1000 addresses in response without being rate limited
    if (discovery_manager_) {
      discovery_manager_->NotifyGetAddrSent(peer->id());
    }
  }

  return true;
}

} // namespace network
} // namespace unicity
