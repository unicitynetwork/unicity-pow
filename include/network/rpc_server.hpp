// Copyright (c) 2025 The Unicity Foundation
// Distributed under the MIT software license

#pragma once

#include "util/uint.hpp"
#include <atomic>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace unicity {

// Forward declarations
namespace chain {
class ChainParams;
}
namespace network {
class NetworkManager;
}
namespace mining {
class CPUMiner;
}
namespace validation {
class ChainstateManager;
}

namespace rpc {

/**
 * RPC Server using Unix Domain Sockets (Local-Only Access)
 *
 * IMPORTANT DESIGN NOTE:
 * This RPC implementation deliberately uses Unix domain sockets instead of
 * TCP/IP networking. This is a security-focused design choice that differs
 * from Bitcoin Core's JSON-RPC over HTTP approach.
 *
 * Why Unix Sockets Instead of TCP:
 * - Security: No network exposure, eliminating remote attack vectors
 * - Simplicity: No need for RPC credentials, ports, or firewall rules
 * - Access Control: Managed via file system permissions on the socket
 *
 * Trade-offs:
 * - No remote access (must SSH to server to run commands)
 * - No direct integration with remote monitoring tools
 * - Docker containers need socket volume mount for access
 *
 * The socket is created at: datadir/node.sock
 *
 * If you need remote access:
 * 1. Use SSH to run commands on the server
 * 2. Set up a local monitoring agent that exports metrics
 * 3. Use a reverse proxy (not recommended for production)
 */
class RPCServer {
public:
  using CommandHandler =
      std::function<std::string(const std::vector<std::string> &)>;

  RPCServer(const std::string &socket_path,
            validation::ChainstateManager &chainstate_manager,
            network::NetworkManager &network_manager, mining::CPUMiner *miner,
            const chain::ChainParams &params,
            std::function<void()> shutdown_callback = nullptr);
  ~RPCServer();

  bool Start();
  void Stop();
  bool IsRunning() const { return running_; }

private:
  // Note: Safe parsing functions moved to util/string_parsing.hpp for reuse

  void ServerThread();
  void HandleClient(int client_fd);
  std::string ExecuteCommand(const std::string &method,
                             const std::vector<std::string> &params);
  void RegisterHandlers();

  // Command handlers - Blockchain
  std::string HandleGetInfo(const std::vector<std::string> &params);
  std::string HandleGetBlockchainInfo(const std::vector<std::string> &params);
  std::string HandleGetBlockCount(const std::vector<std::string> &params);
  std::string HandleGetBlockHash(const std::vector<std::string> &params);
  std::string HandleGetBlockHeader(const std::vector<std::string> &params);
  std::string HandleGetBestBlockHash(const std::vector<std::string> &params);
  std::string HandleGetDifficulty(const std::vector<std::string> &params);

  // Command handlers - Mining
  std::string HandleGetMiningInfo(const std::vector<std::string> &params);
  std::string HandleGetNetworkHashPS(const std::vector<std::string> &params);
  std::string HandleStartMining(const std::vector<std::string> &params);
  std::string HandleStopMining(const std::vector<std::string> &params);
  std::string HandleGenerate(const std::vector<std::string> &params);

  // Command handlers - Network
  std::string HandleGetConnectionCount(const std::vector<std::string> &params);
  std::string HandleGetPeerInfo(const std::vector<std::string> &params);
  std::string HandleAddNode(const std::vector<std::string> &params);
  std::string HandleSetBan(const std::vector<std::string> &params);
  std::string HandleListBanned(const std::vector<std::string> &params);
  std::string HandleGetAddrManInfo(const std::vector<std::string> &params);
  std::string HandleDisconnectNode(const std::vector<std::string> &params);
  std::string HandleGetNextWorkRequired(const std::vector<std::string> &params);
  std::string HandleReportMisbehavior(const std::vector<std::string> &params);
  std::string HandleAddOrphanHeader(const std::vector<std::string> &params);
  std::string HandleGetOrphanStats(const std::vector<std::string> &params);
  std::string HandleEvictOrphans(const std::vector<std::string> &params);

  // Command handlers - Control
  std::string HandleStop(const std::vector<std::string> &params);

  // Command handlers - Testing
  std::string HandleSetMockTime(const std::vector<std::string> &params);
  std::string HandleInvalidateBlock(const std::vector<std::string> &params);
  std::string HandleClearBanned(const std::vector<std::string> &params);
  std::string HandleGetChainTips(const std::vector<std::string> &params);
  std::string HandleSubmitHeader(const std::vector<std::string> &params);

private:
  std::string socket_path_;
  validation::ChainstateManager &chainstate_manager_;
  network::NetworkManager &network_manager_;
  mining::CPUMiner *miner_; // Optional, can be nullptr
  const chain::ChainParams &params_;
  std::function<void()> shutdown_callback_;

  int server_fd_;
  std::atomic<bool> running_;
  std::atomic<bool> shutting_down_;
  std::thread server_thread_;

  std::map<std::string, CommandHandler> handlers_;
};

} // namespace rpc
} // namespace unicity


