// Copyright (c) 2025 The Unicity Foundation
// Distributed under the MIT software license

#pragma once

#include "util/uint.hpp"
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <mutex>

namespace unicity {

/**
 * Notification system for network events
 *
 * Design philosophy (mirrors ChainNotifications):
 * - Simple observer pattern with std::function
 * - Single-threaded (io_context thread only, no mutex needed)
 * - No background queue (synchronous callbacks)
 * - RAII-based subscription management
 * - Singleton pattern (no wiring needed)
 *
 * Purpose:
 * - Enables decoupled inter-component communication
 * - Same pattern as ChainNotifications for consistency
 */

/**
 * Network event notifications
 *
 * Events:
 * - PeerConnected: New peer connection established
 * - PeerDisconnected: Peer disconnected (normal or kicked)
 * - InvalidHeader: Peer sent invalid header
 * - LowWorkHeaders: Peer sent headers with insufficient work
 * - InvalidBlock: Peer sent invalid block
 * - Misbehavior: Peer misbehaved (general)
 */
class NetworkNotifications {
public:
  /**
   * Subscription handle - RAII wrapper
   * Automatically unsubscribes when destroyed
   */
  class Subscription {
  public:
    Subscription() = default;
    ~Subscription();

    // Movable but not copyable
    Subscription(Subscription &&other) noexcept;
    Subscription &operator=(Subscription &&other) noexcept;
    Subscription(const Subscription &) = delete;
    Subscription &operator=(const Subscription &) = delete;

    // Unsubscribe explicitly
    void Unsubscribe();

  private:
    friend class NetworkNotifications;
    Subscription(NetworkNotifications *owner, size_t id);

    NetworkNotifications *owner_{nullptr};
    size_t id_{0};
    bool active_{false};
  };

  // Callback types
  using PeerConnectedCallback =
      std::function<void(int peer_id, const std::string &address, uint16_t port,
                         const std::string &connection_type)>;
  using PeerDisconnectedCallback =
      std::function<void(int peer_id, const std::string &address, uint16_t port,
                         const std::string &reason, bool mark_addr_good)>;
  using InvalidHeaderCallback =
      std::function<void(int peer_id, const uint256 &hash,
                         const std::string &reason)>;
  using LowWorkHeadersCallback =
      std::function<void(int peer_id, size_t count, const std::string &reason)>;
  using InvalidBlockCallback =
      std::function<void(int peer_id, const uint256 &hash,
                         const std::string &reason)>;
  using MisbehaviorCallback =
      std::function<void(int peer_id, int penalty, const std::string &reason)>;

  /**
   * Subscribe to peer connected events
   * Returns RAII subscription handle
   */
  [[nodiscard]] Subscription
  SubscribePeerConnected(PeerConnectedCallback callback);

  /**
   * Subscribe to peer disconnected events
   * Returns RAII subscription handle
   */
  [[nodiscard]] Subscription
  SubscribePeerDisconnected(PeerDisconnectedCallback callback);

  /**
   * Subscribe to invalid header events
   * Returns RAII subscription handle
   * NOTE: Currently not used - reserved for future DoS protection
   */
  [[nodiscard]] Subscription
  SubscribeInvalidHeader(InvalidHeaderCallback callback);

  /**
   * Subscribe to low work headers events
   * Returns RAII subscription handle
   * NOTE: Currently not used - reserved for future DoS protection
   */
  [[nodiscard]] Subscription
  SubscribeLowWorkHeaders(LowWorkHeadersCallback callback);

  /**
   * Subscribe to invalid block events
   * Returns RAII subscription handle
   * NOTE: Currently not used - reserved for future DoS protection
   */
  [[nodiscard]] Subscription
  SubscribeInvalidBlock(InvalidBlockCallback callback);

  /**
   * Subscribe to misbehavior events
   * Returns RAII subscription handle
   * NOTE: Currently not used - reserved for future DoS protection
   */
  [[nodiscard]] Subscription
  SubscribeMisbehavior(MisbehaviorCallback callback);

  /**
   * Notify all subscribers of peer connected
   * Called by NetworkManager/PeerLifecycleManager when new connection established
   */
  void NotifyPeerConnected(int peer_id, const std::string &address, uint16_t port,
                           const std::string &connection_type);

  /**
   * Notify all subscribers of peer disconnected
   * Called by PeerLifecycleManager when peer disconnects or is kicked
   * @param mark_addr_good If true, indicates this was a clean disconnect from a good peer
   *                       and the address should be marked as good in the address manager
   */
  void NotifyPeerDisconnected(int peer_id, const std::string &address, uint16_t port,
                              const std::string &reason, bool mark_addr_good = false);

  /**
   * Notify all subscribers of invalid header
   * Called by HeaderSyncManager when peer sends invalid header
   * NOTE: Currently not called - reserved for future DoS protection
   */
  void NotifyInvalidHeader(int peer_id, const uint256 &hash,
                           const std::string &reason);

  /**
   * Notify all subscribers of low work headers
   * Called by HeaderSyncManager when peer sends headers with insufficient work
   * NOTE: Currently not called - reserved for future DoS protection
   */
  void NotifyLowWorkHeaders(int peer_id, size_t count,
                            const std::string &reason);

  /**
   * Notify all subscribers of invalid block
   * Called by block validation when peer sends invalid block
   * NOTE: Currently not called - reserved for future DoS protection
   */
  void NotifyInvalidBlock(int peer_id, const uint256 &hash,
                          const std::string &reason);

  /**
   * Notify all subscribers of misbehavior
   * Called by any component when peer misbehaves
   * NOTE: Currently not called - reserved for future DoS protection
   */
  void NotifyMisbehavior(int peer_id, int penalty, const std::string &reason);

  /**
   * Get singleton instance
   */
  static NetworkNotifications &Get();

private:
  NetworkNotifications() = default;

  // Unsubscribe by ID (called by Subscription destructor)
  void Unsubscribe(size_t id);

  struct CallbackEntry {
    size_t id;
    PeerConnectedCallback peer_connected;
    PeerDisconnectedCallback peer_disconnected;
    InvalidHeaderCallback invalid_header;
    LowWorkHeadersCallback low_work_headers;
    InvalidBlockCallback invalid_block;
    MisbehaviorCallback misbehavior;
  };

  // Thread-safety: protect callbacks_ and next_id_ across threads
  mutable std::mutex mutex_;
  std::vector<CallbackEntry> callbacks_;
  size_t next_id_{1}; // 0 reserved for invalid
};

/**
 * Global accessor for network notifications
 * Matches ChainNotifications pattern: Notifications() vs NetworkEvents()
 */
inline NetworkNotifications &NetworkEvents() {
  return NetworkNotifications::Get();
}

} // namespace unicity
