// Copyright (c) 2025 The Unicity Foundation
// Distributed under the MIT software license

#pragma once

#include "chain/block.hpp"
#include <concepts>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace unicity {

// Forward declarations
namespace chain {
class CBlockIndex;
}
namespace sync {
class HeaderSync;
}

/**
 * Notification system for blockchain events
 *
 * Design philosophy:
 * - Simple observer pattern with std::function
 * - Thread-safe using std::mutex
 * - No background queue (synchronous callbacks)
 * - Type-safe with concepts
 * - RAII-based subscription management
 *
 * Unlike Bitcoin's ValidationInterface:
 * - No mempool/transaction events (we don't have mempool)
 * - No wallet events (we don't have wallets)
 * - No async queue 
 */

/**
 * Chain event notifications
 *
 * Events:
 * - BlockConnected: New block added to active chain
 * - UpdatedChainTip: Chain tip changed (may skip intermediate blocks)
 * - SyncStateChanged: IBD/sync state changed
 */
class ChainNotifications {
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
    friend class ChainNotifications;
    Subscription(ChainNotifications *owner, size_t id);

    ChainNotifications *owner_{nullptr};
    size_t id_{0};
    bool active_{false};
  };

  // Callback types
  using BlockConnectedCallback = std::function<void(
      const CBlockHeader &block, const chain::CBlockIndex *pindex)>;
  using BlockDisconnectedCallback = std::function<void(
      const CBlockHeader &block, const chain::CBlockIndex *pindex)>;
  using ChainTipCallback =
      std::function<void(const chain::CBlockIndex *pindexNew, int height)>;
  using SyncStateCallback = std::function<void(bool syncing, double progress)>;
  using SuspiciousReorgCallback = std::function<void(int reorg_depth, int max_allowed)>;
  using NetworkExpiredCallback = std::function<void(int current_height, int expiration_height)>;

  /**
   * Subscribe to block connected events
   * Returns RAII subscription handle
   */
  [[nodiscard]] Subscription
  SubscribeBlockConnected(BlockConnectedCallback callback);

  /**
   * Subscribe to block disconnected events (reorgs)
   * Returns RAII subscription handle
   */
  [[nodiscard]] Subscription
  SubscribeBlockDisconnected(BlockDisconnectedCallback callback);

  /**
   * Subscribe to chain tip updates
   * Returns RAII subscription handle
   */
  [[nodiscard]] Subscription SubscribeChainTip(ChainTipCallback callback);

  /**
   * Subscribe to sync state changes
   * Returns RAII subscription handle
   */
  [[nodiscard]] Subscription SubscribeSyncState(SyncStateCallback callback);

  /**
   * Subscribe to suspicious reorg detection
   * Returns RAII subscription handle
   */
  [[nodiscard]] Subscription
  SubscribeSuspiciousReorg(SuspiciousReorgCallback callback);

  /**
   * Subscribe to network expiration detection
   * Returns RAII subscription handle
   */
  [[nodiscard]] Subscription
  SubscribeNetworkExpired(NetworkExpiredCallback callback);

  /**
   * Notify all subscribers of block connected
   * Called by ChainstateManager::ConnectTip() when adding block to active chain
   *
   * - Called AFTER SetActiveTip() updates the chain state
   * - GetTip() returns the NEWLY CONNECTED block (pindex)
   * - Subscribers see the updated chain state with the new block already active
   * - This allows querying the new state immediately
   *
   * Example:
   *   void OnBlockConnected(const CBlockHeader& block, const CBlockIndex*
   * pindex) {
   *       // GetTip() now returns pindex (the newly connected block)
   *       assert(chainstate.GetTip() == pindex);
   *   }
   */
  void NotifyBlockConnected(const CBlockHeader &block,
                            const chain::CBlockIndex *pindex);

  /**
   * Notify all subscribers of block disconnected
   * Called during reorgs when removing block from active chain
   *
   * - Called BEFORE SetActiveTip() updates the chain state
   * - GetTip() returns the block BEING DISCONNECTED (pindex)
   * - Subscribers see the old chain state before the block is removed
   * - After all callbacks complete, SetActiveTip() moves tip to pindex->pprev
   *
   *   - On BlockConnected: GetTip() == newly connected block
   *   - On BlockDisconnected: GetTip() == block being disconnected
   *
   * Example:
   *   void OnBlockDisconnected(const CBlockHeader& block, const CBlockIndex*
   * pindex) {
   *       // GetTip() still returns pindex (the block being removed)
   *       assert(chainstate.GetTip() == pindex);
   *       // After this callback, tip will move to pindex->pprev
   *   }
   */
  void NotifyBlockDisconnected(const CBlockHeader &block,
                               const chain::CBlockIndex *pindex);

  /**
   * Notify all subscribers of chain tip update
   * Called by BlockManager when active tip changes
   */
  void NotifyChainTip(const chain::CBlockIndex *pindexNew, int height);

  /**
   * Notify all subscribers of sync state change
   * Called by HeaderSync when sync state changes
   */
  void NotifySyncState(bool syncing, double progress);

  /**
   * Notify all subscribers of suspicious reorg detection
   * Called by ChainstateManager when reorg exceeds safety threshold
   */
  void NotifySuspiciousReorg(int reorg_depth, int max_allowed);

  /**
   * Notify all subscribers of network expiration
   * Called by ChainstateManager when network expiration timebomb is triggered
   */
  void NotifyNetworkExpired(int current_height, int expiration_height);

  /**
   * Get singleton instance
   */
  static ChainNotifications &Get();

private:
  ChainNotifications() = default;

  // Unsubscribe by ID (called by Subscription destructor)
  void Unsubscribe(size_t id);

  struct CallbackEntry {
    size_t id;
    BlockConnectedCallback block_connected;
    BlockDisconnectedCallback block_disconnected;
    ChainTipCallback chain_tip;
    SyncStateCallback sync_state;
    SuspiciousReorgCallback suspicious_reorg;
    NetworkExpiredCallback network_expired;
  };

  std::mutex mutex_;
  std::vector<CallbackEntry> callbacks_;
  size_t next_id_{1}; // 0 reserved for invalid
};

/**
 * Global accessor for notifications
 * Simpler alternative to Bitcoin's GetMainSignals()
 */
inline ChainNotifications &Notifications() { return ChainNotifications::Get(); }

} // namespace unicity


