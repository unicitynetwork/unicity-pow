#ifndef UNICITY_TEST_SIMULATED_NODE_HPP
#define UNICITY_TEST_SIMULATED_NODE_HPP

#include "test_chainstate_manager.hpp"
#include "simulated_network.hpp"
#include "network_bridged_transport.hpp"
#include "network/network_manager.hpp"
#include "chain/chainparams.hpp"
#include <boost/asio.hpp>
#include <memory>
#include <string>
#include <thread>

namespace unicity {
namespace test {

/**
 * SimulatedNode - Network simulation node using REAL P2P components
 *
 * Architecture:
 * - TestChainstateManager: Real blockchain logic, bypasses PoW for speed
 * - NetworkManager: Real production P2P networking code
 * - Peer: Real protocol implementation (VERSION, VERACK, HEADERS, etc.)
 * - NetworkBridgedTransport: Routes messages through SimulatedNetwork
 *
 * This gives us authentic P2P behavior in a simulated, deterministic environment.
 */
class SimulatedNode : public SimulatedNetwork::ISimulatedNode {
public:
    SimulatedNode(int node_id,
                  SimulatedNetwork* network,
                  const chain::ChainParams* params = nullptr);
    ~SimulatedNode();

    // Delete copy/move (has unique_ptr and io_context)
    SimulatedNode(const SimulatedNode&) = delete;
    SimulatedNode& operator=(const SimulatedNode&) = delete;
    SimulatedNode(SimulatedNode&&) = delete;
    SimulatedNode& operator=(SimulatedNode&&) = delete;

    // Node identity
    int GetId() const { return node_id_; }
    std::string GetAddress() const;
    uint16_t GetPort() const { return port_; }

    // Connection management
    bool ConnectTo(int peer_node_id, const std::string& address = "", uint16_t port = protocol::ports::REGTEST);
    void DisconnectFrom(int peer_id);

    // Mining (instant, no PoW)
    uint256 MineBlock(const std::string& miner_address = "test_miner");

    // Blockchain state
    int GetTipHeight() const;
    uint256 GetTipHash() const;
    const chain::CBlockIndex* GetTip() const;
    bool GetIsIBD() const;

    // Network state
    size_t GetPeerCount() const;
    size_t GetOutboundPeerCount() const;
    size_t GetInboundPeerCount() const;

    // Ban management
    bool IsBanned(const std::string& address) const;
    void Ban(const std::string& address, int64_t ban_time_seconds = 86400);
    void Unban(const std::string& address);

    // Component access
    TestChainstateManager& GetChainstate() { return *chainstate_; }
    network::NetworkManager& GetNetworkManager() { return *network_manager_; }

    // Test configuration
    void SetBypassPOWValidation(bool bypass) {
        chainstate_->SetBypassPOWValidation(bypass);
    }

    // Set permissions for inbound connections (for testing NoBan, etc.)
    void SetInboundPermissions(network::NetPermissionFlags flags) {
        network_manager_->set_default_inbound_permissions(flags);
    }

    // Test configuration: Override block relay INV chunk size (friend access to BlockRelayManager)
    void SetBlockRelayChunkSize(size_t chunk_size);

    // Statistics
    struct NodeStats {
        size_t blocks_mined = 0;
        size_t connections_made = 0;
        size_t disconnections = 0;
    };
    NodeStats GetStats() const { return stats_; }

    // Process io_context events (called by SimulatedNetwork)
    void ProcessEvents() override;

    // Process periodic maintenance (called by SimulatedNetwork::AdvanceTime)
    void ProcessPeriodic() override;
    
    // Optional: override IO threads for NetworkManager timers (default 0 for deterministic tests)
    explicit SimulatedNode(int node_id,
                           SimulatedNetwork* network,
                           const chain::ChainParams* params,
                           size_t io_threads_override);

protected:
    // Network transport (bridges to SimulatedNetwork)
    SimulatedNetwork* sim_network_;

    // Chain parameters
    const chain::ChainParams* params_;

private:
    // Node identity
    int node_id_;
    std::string address_;
    uint16_t port_;

    // Async I/O
    boost::asio::io_context io_context_;
    std::unique_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> work_guard_;

    // Blockchain
    std::unique_ptr<TestChainstateManager> chainstate_;

    // Chain parameters (owned)
    std::unique_ptr<chain::ChainParams> params_owned_;

    // Transport
    std::shared_ptr<NetworkBridgedTransport> transport_;

    // Real P2P networking
    std::unique_ptr<network::NetworkManager> network_manager_;

    // Statistics
    NodeStats stats_;

    // Setup
    void InitializeNetworking();

    // Config
    size_t io_threads_override_ = 0;
};

} // namespace test
} // namespace unicity

#endif // UNICITY_TEST_SIMULATED_NODE_HPP
