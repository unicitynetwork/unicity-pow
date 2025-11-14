// Copyright (c) 2025 The Unicity Foundation
// Distributed under the MIT software license

#include "chain/notifications.hpp"
#include <algorithm>

namespace unicity {

// ============================================================================
// ChainNotifications::Subscription
// ============================================================================

ChainNotifications::Subscription::Subscription(ChainNotifications *owner,
                                               size_t id)
    : owner_(owner), id_(id), active_(true) {}

ChainNotifications::Subscription::~Subscription() { Unsubscribe(); }

ChainNotifications::Subscription::Subscription(Subscription &&other) noexcept
    : owner_(other.owner_), id_(other.id_), active_(other.active_) {
  other.owner_ = nullptr;
  other.active_ = false;
}

ChainNotifications::Subscription &
ChainNotifications::Subscription::operator=(Subscription &&other) noexcept {
  if (this != &other) {
    Unsubscribe();
    owner_ = other.owner_;
    id_ = other.id_;
    active_ = other.active_;
    other.owner_ = nullptr;
    other.active_ = false;
  }
  return *this;
}

void ChainNotifications::Subscription::Unsubscribe() {
  if (active_ && owner_) {
    owner_->Unsubscribe(id_);
    active_ = false;
  }
}

// ============================================================================
// ChainNotifications
// ============================================================================

ChainNotifications::Subscription
ChainNotifications::SubscribeBlockConnected(BlockConnectedCallback callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  size_t id = next_id_++;

  CallbackEntry entry;
  entry.id = id;
  entry.block_connected = std::move(callback);
  callbacks_.push_back(std::move(entry));

  return Subscription(this, id);
}

ChainNotifications::Subscription ChainNotifications::SubscribeBlockDisconnected(
    BlockDisconnectedCallback callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  size_t id = next_id_++;

  CallbackEntry entry;
  entry.id = id;
  entry.block_disconnected = std::move(callback);
  callbacks_.push_back(std::move(entry));

  return Subscription(this, id);
}

ChainNotifications::Subscription
ChainNotifications::SubscribeChainTip(ChainTipCallback callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  size_t id = next_id_++;

  CallbackEntry entry;
  entry.id = id;
  entry.chain_tip = std::move(callback);
  callbacks_.push_back(std::move(entry));

  return Subscription(this, id);
}

ChainNotifications::Subscription
ChainNotifications::SubscribeSyncState(SyncStateCallback callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  size_t id = next_id_++;

  CallbackEntry entry;
  entry.id = id;
  entry.sync_state = std::move(callback);
  callbacks_.push_back(std::move(entry));

  return Subscription(this, id);
}

ChainNotifications::Subscription
ChainNotifications::SubscribeSuspiciousReorg(SuspiciousReorgCallback callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  size_t id = next_id_++;

  CallbackEntry entry;
  entry.id = id;
  entry.suspicious_reorg = std::move(callback);
  callbacks_.push_back(std::move(entry));

  return Subscription(this, id);
}

ChainNotifications::Subscription
ChainNotifications::SubscribeNetworkExpired(NetworkExpiredCallback callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  size_t id = next_id_++;

  CallbackEntry entry;
  entry.id = id;
  entry.network_expired = std::move(callback);
  callbacks_.push_back(std::move(entry));

  return Subscription(this, id);
}

void ChainNotifications::NotifyBlockConnected(
    const CBlockHeader &block, const chain::CBlockIndex *pindex) {
  std::lock_guard<std::mutex> lock(mutex_);

  for (const auto &entry : callbacks_) {
    if (entry.block_connected) {
      entry.block_connected(block, pindex);
    }
  }
}

void ChainNotifications::NotifyChainTip(const chain::CBlockIndex *pindexNew,
                                        int height) {
  std::lock_guard<std::mutex> lock(mutex_);

  for (const auto &entry : callbacks_) {
    if (entry.chain_tip) {
      entry.chain_tip(pindexNew, height);
    }
  }
}

void ChainNotifications::NotifyBlockDisconnected(
    const CBlockHeader &block, const chain::CBlockIndex *pindex) {
  std::lock_guard<std::mutex> lock(mutex_);

  for (const auto &entry : callbacks_) {
    if (entry.block_disconnected) {
      entry.block_disconnected(block, pindex);
    }
  }
}

void ChainNotifications::NotifySyncState(bool syncing, double progress) {
  std::lock_guard<std::mutex> lock(mutex_);

  for (const auto &entry : callbacks_) {
    if (entry.sync_state) {
      entry.sync_state(syncing, progress);
    }
  }
}

void ChainNotifications::NotifySuspiciousReorg(int reorg_depth,
                                                int max_allowed) {
  std::lock_guard<std::mutex> lock(mutex_);

  for (const auto &entry : callbacks_) {
    if (entry.suspicious_reorg) {
      entry.suspicious_reorg(reorg_depth, max_allowed);
    }
  }
}

void ChainNotifications::NotifyNetworkExpired(int current_height,
                                               int expiration_height) {
  std::lock_guard<std::mutex> lock(mutex_);

  for (const auto &entry : callbacks_) {
    if (entry.network_expired) {
      entry.network_expired(current_height, expiration_height);
    }
  }
}

void ChainNotifications::Unsubscribe(size_t id) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it =
      std::find_if(callbacks_.begin(), callbacks_.end(),
                   [id](const CallbackEntry &entry) { return entry.id == id; });

  if (it != callbacks_.end()) {
    callbacks_.erase(it);
  }
}

ChainNotifications &ChainNotifications::Get() {
  static ChainNotifications instance;
  return instance;
}

} // namespace unicity
