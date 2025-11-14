#include "network/blockchain_sync_manager.hpp"
#include "network/header_sync_manager.hpp"
#include "network/block_relay_manager.hpp"
#include "network/peer_lifecycle_manager.hpp"
#include "network/message.hpp"
#include "chain/chainstate_manager.hpp"
#include "util/logging.hpp"

namespace unicity {
namespace network {

BlockchainSyncManager::BlockchainSyncManager(validation::ChainstateManager& chainstate,
                                             PeerLifecycleManager& peer_manager) {
  // Create and own HeaderSyncManager
  header_sync_manager_ = std::make_unique<HeaderSyncManager>(chainstate, peer_manager);

  // Create and own BlockRelayManager (needs HeaderSyncManager reference)
  // LIFETIME: BlockRelayManager stores a raw pointer to HeaderSyncManager.
  // This is safe ONLY because:
  // 1. Both are owned by BlockchainSyncManager as unique_ptr members
  // 2. Destruction order: block_relay_manager_ destroyed before header_sync_manager_
  //    (members destroyed in reverse declaration order)
  // 3. BlockRelayManager never accesses header_sync_manager_ after destruction
  block_relay_manager_ = std::make_unique<BlockRelayManager>(
      chainstate, peer_manager, header_sync_manager_.get());

  LOG_NET_INFO("BlockchainSyncManager initialized (created HeaderSyncManager and BlockRelayManager)");
}

// Destructor must be defined in .cpp where complete types are visible
// unique_ptr<T> requires complete type T for destruction
// See: https://en.cppreference.com/w/cpp/memory/unique_ptr (Deleter requirements)
BlockchainSyncManager::~BlockchainSyncManager() = default;

bool BlockchainSyncManager::HandleHeaders(PeerPtr peer, message::HeadersMessage* msg) {
  if (!header_sync_manager_) {
    LOG_NET_ERROR("BlockchainSyncManager::HandleHeaders: header_sync_manager_ is null");
    return false;
  }
  return header_sync_manager_->HandleHeadersMessage(peer, msg);
}

bool BlockchainSyncManager::HandleGetHeaders(PeerPtr peer, message::GetHeadersMessage* msg) {
  if (!header_sync_manager_) {
    LOG_NET_ERROR("BlockchainSyncManager::HandleGetHeaders: header_sync_manager_ is null");
    return false;
  }
  return header_sync_manager_->HandleGetHeadersMessage(peer, msg);
}

bool BlockchainSyncManager::HandleInv(PeerPtr peer, message::InvMessage* msg) {
  if (!block_relay_manager_) {
    LOG_NET_ERROR("BlockchainSyncManager::HandleInv: block_relay_manager_ is null");
    return false;
  }
  return block_relay_manager_->HandleInvMessage(peer, msg);
}

} // namespace network
} // namespace unicity
