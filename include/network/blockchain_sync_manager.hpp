#pragma once

/*
 BlockchainSyncManager — unified blockchain synchronization coordinator for Unicity

 Purpose
 - Own and coordinate HeaderSyncManager and BlockRelayManager
 - Provide clean interface for sync-related protocol messages
 - Route sync messages to the appropriate manager

 Key responsibilities
 1. Own HeaderSyncManager and BlockRelayManager
 2. Handle sync-related protocol messages (HEADERS, GETHEADERS, INV)
 3. Provide accessor methods for owned managers

 Message Handling
 - HEADERS: Delegate to HeaderSyncManager
 - GETHEADERS: Delegate to HeaderSyncManager
 - INV: Delegate to BlockRelayManager

 Architecture
 This is a top-level manager that owns the sync subsystem components.
 It provides ownership and delegation, allowing NetworkManager to interact
 with sync logic through a single interface.

 Design Pattern
 - Ownership: Uses unique_ptr to own child managers
 - Delegation: Routes protocol messages to appropriate internal manager
 - Accessor pattern: Provides direct access to owned managers when needed

 Thread Safety
 - All methods must be called from the Network thread
 - Internal managers (HeaderSyncManager, BlockRelayManager) are NOT thread-safe
 - Thread safety is enforced by NetworkManager's single-threaded event loop

 Note: IBD (Initial Block Download) state is managed by ChainstateManager
 in the chain layer, not by network-layer managers.
*/

#include <memory>
#include "network/peer.hpp"  // Defines PeerPtr = std::shared_ptr<Peer>

// Forward declarations
namespace unicity {

namespace message {
class HeadersMessage;
class GetHeadersMessage;
class InvMessage;
}

namespace validation {
class ChainstateManager;
}

namespace network {

class HeaderSyncManager;
class BlockRelayManager;
class PeerLifecycleManager;

class BlockchainSyncManager {
public:
  // Constructor: Creates owned sync managers internally
  BlockchainSyncManager(validation::ChainstateManager& chainstate, PeerLifecycleManager& peer_manager);

  ~BlockchainSyncManager();

  // Non-copyable (owns unique resources)
  BlockchainSyncManager(const BlockchainSyncManager&) = delete;
  BlockchainSyncManager& operator=(const BlockchainSyncManager&) = delete;

  // Non-movable (destruction order dependency - see member declaration comments)
  BlockchainSyncManager(BlockchainSyncManager&&) = delete;
  BlockchainSyncManager& operator=(BlockchainSyncManager&&) = delete;

  // === Protocol Message Handlers ===
  // These delegate to the appropriate internal manager

  /**
   * Handle HEADERS message - processes block headers from peer
   * Delegates to HeaderSyncManager
   *
   * @param peer Peer that sent the headers (required, must not be null)
   * @param msg Headers message payload (required, must not be null)
   * @return true if handled successfully, false on error
   */
  bool HandleHeaders(PeerPtr peer, message::HeadersMessage* msg);

  /**
   * Handle GETHEADERS message - peer requesting headers from us
   * Delegates to HeaderSyncManager
   *
   * @param peer Peer requesting headers (required, must not be null)
   * @param msg GetHeaders message with locator and stop hash (required, must not be null)
   * @return true if handled successfully, false on error
   */
  bool HandleGetHeaders(PeerPtr peer, message::GetHeadersMessage* msg);

  /**
   * Handle INV message - inventory announcement (blocks/txs)
   * Delegates to BlockRelayManager
   *
   * @param peer Peer announcing inventory (required, must not be null)
   * @param msg Inventory message payload (required, must not be null)
   * @return true if handled successfully, false on error
   */
  bool HandleInv(PeerPtr peer, message::InvMessage* msg);

  // === Component Accessors ===
  // Provide access to owned managers for NetworkManager to call their methods

  HeaderSyncManager& header_sync() { return *header_sync_manager_; }
  const HeaderSyncManager& header_sync() const { return *header_sync_manager_; }

  BlockRelayManager& block_relay() { return *block_relay_manager_; }
  const BlockRelayManager& block_relay() const { return *block_relay_manager_; }

private:
  // Owned managers via unique_ptr
  // BlockchainSyncManager now owns HeaderSyncManager and BlockRelayManager
  //
  // IMPORTANT: Declaration order matters for destruction safety!
  // - Members are destroyed in REVERSE declaration order
  // - block_relay_manager_ must be destroyed BEFORE header_sync_manager_
  // - BlockRelayManager holds a raw pointer to HeaderSyncManager
  std::unique_ptr<HeaderSyncManager> header_sync_manager_;  // Destroyed LAST
  std::unique_ptr<BlockRelayManager> block_relay_manager_;  // Destroyed FIRST
};

} // namespace network
} // namespace unicity
