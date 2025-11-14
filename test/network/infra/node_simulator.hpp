#ifndef UNICITY_TEST_ATTACK_SIMULATED_NODE_HPP
#define UNICITY_TEST_ATTACK_SIMULATED_NODE_HPP

#include "simulated_node.hpp"
#include "chain/block.hpp"

namespace unicity {
namespace test {

/**
 * NodeSimulator - Extends SimulatedNode to send malicious P2P messages
 *
 * This node can:
 * - Send headers with invalid PoW
 * - Send orphan headers (unknown parents)
 * - Send non-continuous headers
 * - Send oversized messages
 * - Stall responses to GETHEADERS
 *
 * Used for testing DoS protection and attack resilience.
 */
class NodeSimulator : public SimulatedNode {
public:
    NodeSimulator(int node_id,
                        SimulatedNetwork* network,
                        const chain::ChainParams* params = nullptr)
        : SimulatedNode(node_id, network, params)
    {
    }

    // Send orphan headers (headers with unknown parents)
    void SendOrphanHeaders(int peer_node_id, size_t count);

    // Send headers with invalid PoW
    void SendInvalidPoWHeaders(int peer_node_id, const uint256& prev_hash, size_t count);

    // Send non-continuous headers (don't connect properly)
    void SendNonContinuousHeaders(int peer_node_id, const uint256& prev_hash);

    // Send oversized HEADERS message (>2000 headers)
    void SendOversizedHeaders(int peer_node_id, size_t count);

    // Enable stalling mode - don't respond to GETHEADERS requests
    void EnableStalling(bool enabled) { stalling_enabled_ = enabled; }

    // Mine a block privately (don't broadcast) - for selfish mining attacks
    uint256 MineBlockPrivate(const std::string& miner_address = "selfish_miner");

    // Broadcast a previously mined private block to a specific peer
    void BroadcastBlock(const uint256& block_hash, int peer_node_id);

    // Send low-work headers to a peer (for DoS testing)
    // Sends headers from the attacker's chain (which has low total work)
    void SendLowWorkHeaders(int peer_node_id, const std::vector<uint256>& block_hashes);

private:
    bool stalling_enabled_ = false;

    // Helper: Create a dummy header
    CBlockHeader CreateDummyHeader(const uint256& prev_hash, uint32_t nBits);

    // Access protected members from SimulatedNode
    using SimulatedNode::sim_network_;
    using SimulatedNode::params_;
};

} // namespace test
} // namespace unicity

#endif // UNICITY_TEST_ATTACK_SIMULATED_NODE_HPP
