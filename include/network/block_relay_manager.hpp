#pragma once

#include "network/peer.hpp"
#include "network/peer_lifecycle_manager.hpp"
#include "network/message.hpp"
#include "util/uint.hpp"
#include <memory>

namespace unicity {

// Forward declarations
namespace validation {
class ChainstateManager;
}

namespace test {
class SimulatedNode;
}

namespace network {

// Forward declaration
class HeaderSyncManager;

// BlockRelayManager - Handles block announcements and relay (inspired by Bitcoin's SendMessages)
// Manages per-peer announcement queues, periodic flushing, and block relay to all peers
// Policy notes:
// - Immediate relays: callers enforce policy (e.g., not in IBD, recent-only via MAX_BLOCK_RELAY_AGE).
// - Periodic tip re-announcements: intentionally do not apply age gating here to aid
//   partition healing; peers dedup via per-peer queues and TTL.
class BlockRelayManager {
  // Friend class for test-only configuration access
  friend class test::SimulatedNode;

public:
  BlockRelayManager(validation::ChainstateManager& chainstate,
                    PeerLifecycleManager& peer_mgr,
                    HeaderSyncManager* header_sync);

  // Announce current tip to all connected peers (adds to their queues)
  void AnnounceTipToAllPeers();

  // Announce current tip to a specific peer (called when peer becomes READY)
  void AnnounceTipToPeer(Peer* peer);

  // Flush pending block announcements from all peers' queues
  // (sends queued blocks as INV messages)
  void FlushBlockAnnouncements();

  // Immediately relay a block to all connected peers (bypass queue)
  void RelayBlock(const uint256& block_hash);

  // Handle incoming INV message from a peer
  bool HandleInvMessage(PeerPtr peer, message::InvMessage* msg);

private:
  // Test-only: Override INV chunk size (default: protocol::MAX_INV_SIZE = 50000)
  // Allows testing chunking logic with smaller values for performance
  // Accessible only by friend class SimulatedNode
  void SetInvChunkSize(size_t chunk_size) { inv_chunk_size_ = chunk_size; }

  validation::ChainstateManager& chainstate_manager_;
  PeerLifecycleManager& peer_manager_;
  HeaderSyncManager* header_sync_manager_; // Optional - for INV->GETHEADERS coordination

  size_t inv_chunk_size_; // INV message chunk size (default: protocol::MAX_INV_SIZE)

  // Note: Per-peer announcement tracking (last announced block/time) is now stored
  // in PeerLifecycleManager's consolidated PeerTrackingData. No separate maps or mutex needed.
  // Cleanup is automatic via PeerTrackingData removal - no subscription needed.
};

} // namespace network
} // namespace unicity


