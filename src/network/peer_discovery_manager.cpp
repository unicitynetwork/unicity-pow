#include "network/peer_discovery_manager.hpp"
#include "network/addr_manager.hpp"
#include "network/anchor_manager.hpp"
#include "network/peer_lifecycle_manager.hpp"
#include "network/protocol.hpp"
#include "network/notifications.hpp"
#include "chain/chainparams.hpp"
#include "util/logging.hpp"
#include "util/time.hpp"
#include <algorithm>
#include <filesystem>
#include <unordered_set>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/address_v6.hpp>

namespace unicity {
namespace network {

PeerDiscoveryManager::PeerDiscoveryManager(PeerLifecycleManager* peer_manager, const std::string& datadir)
    : datadir_(datadir),
      peer_manager_(peer_manager),
      rng_(std::random_device{}()) {
  // Create and own AddressManager
  addr_manager_ = std::make_unique<AddressManager>();
  LOG_NET_INFO("PeerDiscoveryManager created AddressManager");

  // Create and own AnchorManager
  if (!peer_manager) {
    LOG_NET_ERROR("PeerDiscoveryManager: peer_manager is null, cannot create AnchorManager");
  } else {
    anchor_manager_ = std::make_unique<AnchorManager>(*peer_manager);
    LOG_NET_INFO("PeerDiscoveryManager created AnchorManager");

    // Inject self into PeerLifecycleManager for address lifecycle tracking
    peer_manager->SetDiscoveryManager(this);
  }

  // Subscribe to NetworkNotifications for address lifecycle management
  // Filter notifications to only process events from our associated PeerLifecycleManager
  peer_connected_sub_ = NetworkEvents().SubscribePeerConnected(
      [this](int peer_id, const std::string& address, uint16_t port, const std::string& connection_type) {
        // Only process if this peer belongs to our PeerLifecycleManager instance
        if (!peer_manager_ || !peer_manager_->get_peer(peer_id)) {
          return; // Not our peer, ignore
        }

        // Only process outbound connections (not feelers, not inbound)
        if (connection_type == "outbound") {
          try {
            protocol::NetworkAddress net_addr = protocol::NetworkAddress::from_string(address, port);
            // Ensure address exists; Good() will be called post-VERACK
            addr_manager_->add(net_addr);
            LOG_NET_DEBUG("Recorded outbound peer {}:{} to addrman",
                          address, port);
          } catch (const std::exception& e) {
            LOG_NET_WARN("PeerDiscoveryManager: failed to record connected peer {}:{}: {}",
                         address, port, e.what());
          }
        }
      });

  // Subscribe to PeerDisconnected to mark addresses as good when appropriate
  // PeerLifecycleManager calculates the mark_addr_good flag based on peer state
  peer_disconnected_sub_ = NetworkEvents().SubscribePeerDisconnected(
      [this](int peer_id, const std::string& address, uint16_t port,
             const std::string& reason, bool mark_addr_good) {
        // Clean up ADDR rate limiting state for this peer (always, regardless of peer_manager state)
        addr_rate_limit_.erase(peer_id);

        // Only mark address as good if this peer belongs to our PeerLifecycleManager instance
        if (!peer_manager_ || !peer_manager_->get_peer(peer_id)) {
          return; // Not our peer, ignore addr manager update
        }

        // Mark address as good if flagged by PeerLifecycleManager
        if (mark_addr_good && port != 0 && !address.empty()) {
          try {
            protocol::NetworkAddress net_addr = protocol::NetworkAddress::from_string(address, port);
            addr_manager_->good(net_addr);
            LOG_NET_TRACE("PeerDiscoveryManager: marked disconnected peer {}:{} as good in address manager",
                          address, port);
          } catch (const std::exception& e) {
            LOG_NET_WARN("PeerDiscoveryManager: failed to update address manager for disconnected peer {}:{}: {}",
                         address, port, e.what());
          }
        }
      });

  LOG_NET_INFO("PeerDiscoveryManager initialized with NetworkNotifications subscriptions");
}

PeerDiscoveryManager::~PeerDiscoveryManager() {
  // Don't log in destructor - logger may already be shut down
}

void PeerDiscoveryManager::Start(ConnectToAnchorsCallback connect_anchors) {
  // Load and connect to anchor peers (eclipse attack resistance)
  // Anchors are the last 2-3 outbound peers we connected to before shutdown
  if (!datadir_.empty()) {
    std::string anchors_path = datadir_ + "/anchors.json";
    if (std::filesystem::exists(anchors_path)) {
      auto anchor_addrs = LoadAnchors(anchors_path);
      if (!anchor_addrs.empty()) {
        LOG_NET_TRACE("Loaded {} anchors, connecting to them first", anchor_addrs.size());
        connect_anchors(anchor_addrs);
      } else {
        LOG_NET_DEBUG("No anchors loaded from {}", anchors_path);
      }
    }
  }

  // Bootstrap from fixed seeds if AddressManager is empty
  if (Size() == 0) {
    BootstrapFromFixedSeeds(chain::GlobalChainParams::Get());
  }
}

bool PeerDiscoveryManager::HandleAddr(PeerPtr peer, message::AddrMessage* msg) {
  if (!msg) {
    return false;
  }

  // Gate ADDR on post-VERACK (Bitcoin Core parity) - check before null manager
  if (!peer || !peer->successfully_connected()) {
    LOG_NET_TRACE("Ignoring ADDR from non-connected peer");
    return true; // Not an error, just gated
  }

  if (!addr_manager_) {
    return false;
  }

  // Validate size and apply misbehavior if available
  if (msg->addresses.size() > protocol::MAX_ADDR_SIZE) {
    LOG_NET_WARN("Peer {} sent oversized ADDR message ({} addrs, max {}), truncating",
                 peer->id(), msg->addresses.size(), protocol::MAX_ADDR_SIZE);
    if (peer_manager_) {
      peer_manager_->ReportOversizedMessage(peer->id());
      if (peer_manager_->ShouldDisconnect(peer->id())) {
        peer_manager_->remove_peer(peer->id());
        return false;
      }
    }
    msg->addresses.resize(protocol::MAX_ADDR_SIZE);
  }

  const int peer_id = peer->id();
  const int64_t now_s = util::GetTime();

  // Rate limit addresses using token bucket (DoS protection)
  // Bitcoin Core compatibility: check if peer has Addr permission to bypass rate limiting
  const bool rate_limited = peer_manager_ ?
      !HasPermission(peer_manager_->GetPeerPermissions(peer_id), NetPermissionFlags::Addr) : true;

  std::vector<protocol::TimestampedAddress> accepted_addrs;
  accepted_addrs.reserve(msg->addresses.size());

  uint64_t num_rate_limited = 0;

  // Update token bucket based on elapsed time
  auto& rate_state = addr_rate_limit_[peer_id];
  const auto current_time = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::system_clock::now().time_since_epoch());

  if (rate_state.token_bucket < MAX_ADDR_PROCESSING_TOKEN_BUCKET) {
    const auto time_diff = std::max(current_time - rate_state.last_update,
                                    std::chrono::microseconds{0});
    const double elapsed_seconds = time_diff.count() / 1e6;
    const double increment = elapsed_seconds * MAX_ADDR_RATE_PER_SECOND;
    rate_state.token_bucket = std::min(rate_state.token_bucket + increment,
                                      MAX_ADDR_PROCESSING_TOKEN_BUCKET);
  }
  rate_state.last_update = current_time;

  // Shuffle addresses for fairness (Bitcoin Core pattern)
  // This ensures that if rate limiting kicks in, it doesn't consistently favor
  // addresses at the beginning of the message
  std::vector<protocol::TimestampedAddress> shuffled_addrs = msg->addresses;
  std::shuffle(shuffled_addrs.begin(), shuffled_addrs.end(), rng_);

  // Process each address with rate limiting
  for (const auto& ta : shuffled_addrs) {
    // Apply rate limiting (consume token)
    if (rate_state.token_bucket < 1.0) {
      if (rate_limited) {
        ++num_rate_limited;
        ++rate_state.addr_rate_limited;
        continue;  // Skip this address
      }
      // Peer has Addr permission - bypass rate limiting
    } else {
      rate_state.token_bucket -= 1.0;
    }
    ++rate_state.addr_processed;
    accepted_addrs.push_back(ta);
  }

  // Log rate limiting stats
  if (num_rate_limited > 0) {
    LOG_NET_DEBUG("ADDR rate limiting: peer={} total={} accepted={} rate_limited={}",
                  peer_id, msg->addresses.size(), accepted_addrs.size(), num_rate_limited);
  }

  // Feed AddressManager with accepted addresses only
  if (!accepted_addrs.empty()) {
    addr_manager_->add_multiple(accepted_addrs);
  }

  // Update learned addresses via ConnectionManager (only accepted addresses)
  if (peer_manager_) {
    peer_manager_->ModifyLearnedAddresses(peer_id, [&](LearnedMap& learned) {
      // Prune old entries by TTL
      for (auto it = learned.begin(); it != learned.end(); ) {
        const int64_t age = now_s - it->second.last_seen_s;
        if (age > ECHO_SUPPRESS_TTL_SEC) {
          it = learned.erase(it);
        } else {
          ++it;
        }
      }

      // Insert/update learned entries (only accepted addresses)
      for (const auto& ta : accepted_addrs) {
        network::AddressKey k = MakeKey(ta.address);
        auto& e = learned[k];
        if (e.last_seen_s == 0 || ta.timestamp >= e.ts_addr.timestamp) {
          e.ts_addr = ta; // preserve services + latest timestamp
        }
        e.last_seen_s = now_s;
      }

      // Enforce per-peer cap with batched eviction (O(n) instead of O(k*n))
      // Allow 10% overage tolerance to avoid frequent evictions
      if (learned.size() > MAX_LEARNED_PER_PEER * 11 / 10) {
        // Collect all entries with timestamps
        std::vector<std::pair<int64_t, AddressKey>> by_time;
        by_time.reserve(learned.size());
        for (const auto& [key, entry] : learned) {
          by_time.emplace_back(entry.last_seen_s, key);
        }

        // Evict down to 90% capacity in one pass
        size_t target_size = MAX_LEARNED_PER_PEER * 9 / 10;  // 1800
        if (by_time.size() > target_size) {
          // Partial sort: find the target_size oldest elements
          std::nth_element(by_time.begin(),
                          by_time.begin() + target_size,
                          by_time.end(),
                          [](const auto& a, const auto& b) { return a.first < b.first; });

          // Remove the oldest entries (first target_size elements are now the oldest)
          for (size_t i = 0; i < target_size; ++i) {
            learned.erase(by_time[i].second);
          }

          LOG_NET_DEBUG("Evicted {} old learned addresses for peer={} (capacity management)",
                       target_size, peer_id);
        }
      }
    });
  }

  // Global recent ring (O(1) eviction) - only add accepted addresses
  for (const auto& ta : accepted_addrs) {
    recent_addrs_.push_back(ta);
    if (recent_addrs_.size() > RECENT_ADDRS_MAX) {
      recent_addrs_.pop_front();
    }
  }

  return true;
}

void PeerDiscoveryManager::NotifyGetAddrSent(int peer_id) {
  // Boost the ADDR rate limiting bucket to allow full response (Bitcoin Core pattern)
  // When we request addresses, the peer should be able to send up to MAX_ADDR_TO_SEND
  // without being rate limited
  auto& rate_state = addr_rate_limit_[peer_id];
  rate_state.token_bucket += MAX_ADDR_PROCESSING_TOKEN_BUCKET;
  LOG_NET_TRACE("GETADDR sent to peer={}, boosted token bucket by {} (now: {})",
                peer_id, MAX_ADDR_PROCESSING_TOKEN_BUCKET, rate_state.token_bucket);
}

bool PeerDiscoveryManager::HandleGetAddr(PeerPtr peer) {
  // Gate GETADDR on post-VERACK (Bitcoin Core parity) - check before other logic
  if (!peer || !peer->successfully_connected()) {
    LOG_NET_TRACE("Ignoring GETADDR from pre-VERACK peer");
    return true;
  }

  if (!addr_manager_) {
    return false;
  }

  stats_getaddr_total_.fetch_add(1, std::memory_order_relaxed);

  // Respond only to INBOUND peers (fingerprinting protection)
  // This asymmetric behavior prevents attackers from fingerprinting nodes by:
  // 1. Sending fake addresses to victim's AddrMan
  // 2. Later requesting GETADDR to check if those addresses are returned
  if (!peer->is_inbound()) {
    stats_getaddr_ignored_outbound_.fetch_add(1, std::memory_order_relaxed);
    LOG_NET_DEBUG("GETADDR ignored: outbound peer={} (inbound-only policy)", peer->id());
    return true; // Not an error; just ignore
  }

  const int peer_id = peer->id();
  const int64_t now_s = util::GetTime();

  // Once-per-connection gating (reply to GETADDR only once per connection)
  // Use ConnectionManager accessor
  if (peer_manager_ && peer_manager_->HasRepliedToGetAddr(peer_id)) {
    stats_getaddr_ignored_repeat_.fetch_add(1, std::memory_order_relaxed);
    LOG_NET_DEBUG("GETADDR ignored: repeat on same connection peer={}", peer_id);
    return true;
  }
  if (peer_manager_) {
    peer_manager_->MarkGetAddrReplied(peer_id);
  }

  // Copy suppression map for this peer while pruning old entries
  // Use ConnectionManager accessor
  LearnedMap suppression_map_copy;
  if (peer_manager_) {
    auto learned_opt = peer_manager_->GetLearnedAddresses(peer_id);
    if (learned_opt) {
      // Prune TTL before copying
      for (auto it = learned_opt->begin(); it != learned_opt->end(); ) {
        const int64_t age = now_s - it->second.last_seen_s;
        if (age > ECHO_SUPPRESS_TTL_SEC) {
          it = learned_opt->erase(it);
        } else {
          ++it;
        }
      }
      suppression_map_copy = *learned_opt; // copy after pruning
    }
  }

  // Build response outside lock
  auto response = std::make_unique<message::AddrMessage>();

  // Requester's own address key (avoid reflecting it)
  AddressKey peer_self_key{};
  bool have_self = false;
  try {
    protocol::NetworkAddress peer_na = protocol::NetworkAddress::from_string(peer->address(), peer->port());
    peer_self_key = MakeKey(peer_na);
    have_self = true;
  } catch (...) {
    have_self = false;
  }

  // Suppression predicate
  auto is_suppressed = [&](const AddressKey& key)->bool {
    auto it = suppression_map_copy.find(key);
    if (it != suppression_map_copy.end()) {
      const int64_t age = now_s - it->second.last_seen_s;
      if (age >= 0 && age <= ECHO_SUPPRESS_TTL_SEC) return true;
    }
    if (have_self && key == peer_self_key) return true;
    return false;
  };

  std::unordered_set<AddressKey, AddressKey::Hasher> included;

  size_t c_from_recent = 0;
  size_t c_from_addrman = 0;
  size_t c_from_learned = 0;
  size_t c_suppressed = 0;

  // 1) Prefer recently learned addresses (most recent first)
  for (auto it = recent_addrs_.rbegin(); it != recent_addrs_.rend(); ++it) {
    if (response->addresses.size() >= protocol::MAX_ADDR_SIZE) break;
    AddressKey k = MakeKey(it->address);
    if (is_suppressed(k)) { c_suppressed++; continue; }
    if (!included.insert(k).second) continue;
    response->addresses.push_back(*it);
    c_from_recent++;
  }

  // 2) Top-up from AddrMan sample
  if (response->addresses.size() < protocol::MAX_ADDR_SIZE) {
    auto addrs = addr_manager_->get_addresses(protocol::MAX_ADDR_SIZE);
    for (const auto& ta : addrs) {
      if (response->addresses.size() >= protocol::MAX_ADDR_SIZE) break;
      AddressKey k = MakeKey(ta.address);
      if (is_suppressed(k)) { c_suppressed++; continue; }
      if (!included.insert(k).second) continue;
      response->addresses.push_back(ta);
      c_from_addrman++;
    }
  }

  // 3) Fallback: if still empty, include learned addresses from other peers (excluding requester)
  // Use memory-efficient API that only copies what we need (capped at MAX_ADDR_SIZE)
  if (response->addresses.empty() && peer_manager_) {
    auto learned_addrs = peer_manager_->GetLearnedAddressesForGetAddr(peer_id, protocol::MAX_ADDR_SIZE);

    for (const auto& ts_addr : learned_addrs) {
      if (response->addresses.size() >= protocol::MAX_ADDR_SIZE) break;

      AddressKey akey = MakeKey(ts_addr.address);
      if (is_suppressed(akey)) { c_suppressed++; continue; }
      if (!included.insert(akey).second) continue; // Skip duplicates

      response->addresses.push_back(ts_addr);
      c_from_learned++;
    }
  }

  // Save composition stats and log
  last_resp_from_addrman_.store(c_from_addrman, std::memory_order_relaxed);
  last_resp_from_recent_.store(c_from_recent, std::memory_order_relaxed);
  last_resp_from_learned_.store(c_from_learned, std::memory_order_relaxed);
  last_resp_suppressed_.store(c_suppressed, std::memory_order_relaxed);
  LOG_NET_DEBUG("GETADDR served peer={} addrs_total={} from_recent={} from_addrman={} from_learned={} suppressed={}",
                peer_id, response->addresses.size(), c_from_recent, c_from_addrman, c_from_learned, c_suppressed);

  // Verify peer still connected before sending (TOCTOU protection)
  if (!peer->is_connected()) {
    LOG_NET_TRACE("Peer {} disconnected before GETADDR response could be sent", peer_id);
    return true; // Not an error, just too late
  }

  // Privacy: randomize order to avoid recency leaks
  if (!response->addresses.empty()) {
    std::shuffle(response->addresses.begin(), response->addresses.end(), rng_);
  }

  peer->send_message(std::move(response));
  stats_getaddr_served_.fetch_add(1, std::memory_order_relaxed);
  return true;
}

#ifdef UNICITY_TESTS
PeerDiscoveryManager::GetAddrDebugStats PeerDiscoveryManager::GetGetAddrDebugStats() const {
  GetAddrDebugStats s;
  s.total = stats_getaddr_total_.load(std::memory_order_relaxed);
  s.served = stats_getaddr_served_.load(std::memory_order_relaxed);
  s.ignored_outbound = stats_getaddr_ignored_outbound_.load(std::memory_order_relaxed);
  s.ignored_prehandshake = stats_getaddr_ignored_prehandshake_.load(std::memory_order_relaxed);
  s.ignored_repeat = stats_getaddr_ignored_repeat_.load(std::memory_order_relaxed);
  s.last_from_addrman = last_resp_from_addrman_.load(std::memory_order_relaxed);
  s.last_from_recent = last_resp_from_recent_.load(std::memory_order_relaxed);
  s.last_from_learned = last_resp_from_learned_.load(std::memory_order_relaxed);
  s.last_suppressed = last_resp_suppressed_.load(std::memory_order_relaxed);
  return s;
}

void PeerDiscoveryManager::TestSeedRng(uint64_t seed) {
  rng_.seed(seed);
}
#endif

// Helper to build binary key (uses shared AddressKey from peer_tracking.hpp)
network::AddressKey PeerDiscoveryManager::MakeKey(const protocol::NetworkAddress& a) {
  network::AddressKey k; k.ip = a.ip; k.port = a.port; return k;
}

// === Bootstrap and Discovery Methods ===

void PeerDiscoveryManager::BootstrapFromFixedSeeds(const chain::ChainParams& params) {
  // Bootstrap AddressManager from hardcoded seed nodes
  // (follows Bitcoin's ThreadDNSAddressSeed logic when addrman is empty)

  const auto& fixed_seeds = params.FixedSeeds();

  if (fixed_seeds.empty()) {
    LOG_NET_TRACE("no fixed seeds available for bootstrap");
    return;
  }

  LOG_NET_INFO("Bootstrapping from {} fixed seed nodes", fixed_seeds.size());

  // Use AddressManager's time format (seconds since epoch)
  // Use util::GetTime() for consistency and testability (supports mock time)
  uint32_t current_time = static_cast<uint32_t>(util::GetTime());
  size_t added_count = 0;

  // Parse each "IP:port" string and add to AddressManager
  for (const auto& seed_str : fixed_seeds) {
    // Parse IP:port format (e.g., "178.18.251.16:9590")
    size_t colon_pos = seed_str.find(':');
    if (colon_pos == std::string::npos) {
      LOG_NET_WARN("Invalid seed format (missing port): {}", seed_str);
      continue;
    }

    std::string ip_str = seed_str.substr(0, colon_pos);
    std::string port_str = seed_str.substr(colon_pos + 1);

    // Parse port
    uint16_t port = 0;
    try {
      int port_int = std::stoi(port_str);
      if (port_int <= 0 || port_int > 65535) {
        LOG_NET_WARN("Invalid port in seed: {}", seed_str);
        continue;
      }
      port = static_cast<uint16_t>(port_int);
    } catch (const std::exception& e) {
      LOG_NET_WARN("Failed to parse port in seed {}: {}", seed_str, e.what());
      continue;
    }

    // Use centralized NetworkAddress::from_string() for IP conversion
    try {
      protocol::NetworkAddress addr = protocol::NetworkAddress::from_string(
          ip_str, port, protocol::ServiceFlags::NODE_NETWORK);

      // Check if conversion failed (from_string returns zeroed IP on error)
      bool is_zero = std::all_of(addr.ip.begin(), addr.ip.end(), [](uint8_t b) { return b == 0; });
      if (is_zero) {
        LOG_NET_WARN("Failed to parse IP in seed {}", seed_str);
        continue;
      }

      // Add to AddressManager with current timestamp
      if (addr_manager_->add(addr, current_time)) {
        added_count++;
        LOG_NET_DEBUG("Added seed node: {}", seed_str);
      }

    } catch (const std::exception& e) {
      LOG_NET_WARN("Exception parsing seed {}: {}", seed_str, e.what());
      continue;
    }
  }

  LOG_NET_INFO("Successfully added {} seed nodes to AddressManager", added_count);
}

// === AddressManager Forwarding Methods ===

bool PeerDiscoveryManager::Add(const protocol::NetworkAddress& addr, uint32_t timestamp) {
  if (!addr_manager_) {
    LOG_NET_ERROR("PeerDiscoveryManager::Add: addr_manager_ is null");
    return false;
  }
  return addr_manager_->add(addr, timestamp);
}

size_t PeerDiscoveryManager::AddMultiple(const std::vector<protocol::TimestampedAddress>& addresses) {
  if (!addr_manager_) {
    LOG_NET_ERROR("PeerDiscoveryManager::AddMultiple: addr_manager_ is null");
    return 0;
  }
  return addr_manager_->add_multiple(addresses);
}

void PeerDiscoveryManager::Attempt(const protocol::NetworkAddress& addr) {
  if (!addr_manager_) {
    LOG_NET_ERROR("PeerDiscoveryManager::Attempt: addr_manager_ is null");
    return;
  }
  addr_manager_->attempt(addr);
}

void PeerDiscoveryManager::Good(const protocol::NetworkAddress& addr) {
  if (!addr_manager_) {
    LOG_NET_ERROR("PeerDiscoveryManager::Good: addr_manager_ is null");
    return;
  }
  addr_manager_->good(addr);
}

void PeerDiscoveryManager::Failed(const protocol::NetworkAddress& addr) {
  if (!addr_manager_) {
    LOG_NET_ERROR("PeerDiscoveryManager::Failed: addr_manager_ is null");
    return;
  }
  addr_manager_->failed(addr);
}

std::optional<protocol::NetworkAddress> PeerDiscoveryManager::Select() {
  if (!addr_manager_) {
    LOG_NET_ERROR("PeerDiscoveryManager::Select: addr_manager_ is null");
    return std::nullopt;
  }
  return addr_manager_->select();
}

std::optional<protocol::NetworkAddress> PeerDiscoveryManager::SelectNewForFeeler() {
  if (!addr_manager_) {
    LOG_NET_ERROR("PeerDiscoveryManager::SelectNewForFeeler: addr_manager_ is null");
    return std::nullopt;
  }
  return addr_manager_->select_new_for_feeler();
}

std::vector<protocol::TimestampedAddress> PeerDiscoveryManager::GetAddresses(size_t max_count) {
  if (!addr_manager_) {
    LOG_NET_ERROR("PeerDiscoveryManager::GetAddresses: addr_manager_ is null");
    return {};
  }
  return addr_manager_->get_addresses(max_count);
}

size_t PeerDiscoveryManager::Size() const {
  if (!addr_manager_) {
    return 0;
  }
  return addr_manager_->size();
}

size_t PeerDiscoveryManager::TriedCount() const {
  if (!addr_manager_) {
    return 0;
  }
  return addr_manager_->tried_count();
}

size_t PeerDiscoveryManager::NewCount() const {
  if (!addr_manager_) {
    return 0;
  }
  return addr_manager_->new_count();
}

void PeerDiscoveryManager::CleanupStale() {
  if (!addr_manager_) {
    LOG_NET_ERROR("PeerDiscoveryManager::CleanupStale: addr_manager_ is null");
    return;
  }
  addr_manager_->cleanup_stale();
}

bool PeerDiscoveryManager::SaveAddresses(const std::string& filepath) {
  if (!addr_manager_) {
    LOG_NET_ERROR("PeerDiscoveryManager::SaveAddresses: addr_manager_ is null");
    return false;
  }
  return addr_manager_->Save(filepath);
}

bool PeerDiscoveryManager::LoadAddresses(const std::string& filepath) {
  if (!addr_manager_) {
    LOG_NET_ERROR("PeerDiscoveryManager::LoadAddresses: addr_manager_ is null");
    return false;
  }
  return addr_manager_->Load(filepath);
}

// === AnchorManager Forwarding Methods ===

std::vector<protocol::NetworkAddress> PeerDiscoveryManager::GetAnchors() const {
  if (!anchor_manager_) {
    LOG_NET_WARN("PeerDiscoveryManager::GetAnchors: anchor_manager_ is null");
    return {};
  }
  return anchor_manager_->GetAnchors();
}

bool PeerDiscoveryManager::SaveAnchors(const std::string& filepath) {
  if (!anchor_manager_) {
    LOG_NET_ERROR("PeerDiscoveryManager::SaveAnchors: anchor_manager_ is null");
    return false;
  }
  return anchor_manager_->SaveAnchors(filepath);
}

std::vector<protocol::NetworkAddress> PeerDiscoveryManager::LoadAnchors(const std::string& filepath) {
  if (!anchor_manager_) {
    LOG_NET_ERROR("PeerDiscoveryManager::LoadAnchors: anchor_manager_ is null");
    return {};
  }
  return anchor_manager_->LoadAnchors(filepath);
}

} // namespace network
} // namespace unicity
