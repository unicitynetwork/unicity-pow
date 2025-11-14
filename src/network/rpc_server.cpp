// Copyright (c) 2025 The Unicity Foundation
// Distributed under the MIT software license

/**
 * RPC Server Implementation - Unix Domain Sockets
 *
 * This RPC server uses Unix domain sockets (filesystem-based IPC) instead
 * of TCP/IP networking. This means:
 * - RPC is only accessible locally on the same machine
 * - No network port is opened (no rpcport configuration)
 * - Authentication is handled by filesystem permissions
 * - The socket file is created at: datadir/node.sock
 *
 * This design prioritizes security over remote accessibility.
 * For remote access, users must SSH to the server.
 */

#include "network/rpc_server.hpp"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <boost/asio/ip/address.hpp>
#include "chain/chainparams.hpp"
#include "chain/chainstate_manager.hpp"
#include "chain/block.hpp"
#include "chain/validation.hpp"
#include "chain/pow.hpp"
#include "util/logging.hpp"
#include "chain/miner.hpp"
#include "util/time.hpp"
#include "util/uint.hpp"
#include "util/netaddress.hpp"
#include "util/string_parsing.hpp"
#include "network/network_manager.hpp"
#include "network/peer_lifecycle_manager.hpp"
#include "network/peer_discovery_manager.hpp"
#include <sstream>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

namespace unicity {
namespace rpc {

RPCServer::RPCServer(const std::string &socket_path,
                     validation::ChainstateManager &chainstate_manager,
                     network::NetworkManager &network_manager,
                     mining::CPUMiner *miner, const chain::ChainParams &params,
                     std::function<void()> shutdown_callback)
    : socket_path_(socket_path), chainstate_manager_(chainstate_manager),
      network_manager_(network_manager), miner_(miner), params_(params),
      shutdown_callback_(shutdown_callback), server_fd_(-1), running_(false),
      shutting_down_(false) {
  RegisterHandlers();
}

RPCServer::~RPCServer() { Stop(); }

// Note: Safe parsing functions moved to util/string_parsing.hpp for reuse across codebase

void RPCServer::RegisterHandlers() {
  // Blockchain commands
  handlers_["getinfo"] = [this](const auto &p) { return HandleGetInfo(p); };
  handlers_["getblockchaininfo"] = [this](const auto &p) {
    return HandleGetBlockchainInfo(p);
  };
  handlers_["getblockcount"] = [this](const auto &p) {
    return HandleGetBlockCount(p);
  };
  handlers_["getblockhash"] = [this](const auto &p) {
    return HandleGetBlockHash(p);
  };
  handlers_["getblockheader"] = [this](const auto &p) {
    return HandleGetBlockHeader(p);
  };
  handlers_["getbestblockhash"] = [this](const auto &p) {
    return HandleGetBestBlockHash(p);
  };
  handlers_["getdifficulty"] = [this](const auto &p) {
    return HandleGetDifficulty(p);
  };

  // Mining commands
  handlers_["getmininginfo"] = [this](const auto &p) {
    return HandleGetMiningInfo(p);
  };
  handlers_["getnetworkhashps"] = [this](const auto &p) {
    return HandleGetNetworkHashPS(p);
  };
  handlers_["startmining"] = [this](const auto &p) {
    return HandleStartMining(p);
  };
  handlers_["stopmining"] = [this](const auto &p) {
    return HandleStopMining(p);
  };
  handlers_["generate"] = [this](const auto &p) { return HandleGenerate(p); };

  // Network commands
  handlers_["getconnectioncount"] = [this](const auto &p) {
    return HandleGetConnectionCount(p);
  };
  handlers_["getpeerinfo"] = [this](const auto &p) {
    return HandleGetPeerInfo(p);
  };
  handlers_["addnode"] = [this](const auto &p) { return HandleAddNode(p); };
  handlers_["setban"] = [this](const auto &p) { return HandleSetBan(p); };
  handlers_["listbanned"] = [this](const auto &p) {
    return HandleListBanned(p);
  };
  handlers_["getaddrmaninfo"] = [this](const auto &p) {
    return HandleGetAddrManInfo(p);
  };
  handlers_["disconnectnode"] = [this](const auto &p) {
    return HandleDisconnectNode(p);
  };
  handlers_["getnextworkrequired"] = [this](const auto &p) {
    return HandleGetNextWorkRequired(p);
  };
  handlers_["reportmisbehavior"] = [this](const auto &p) {
    return HandleReportMisbehavior(p);
  };
  handlers_["addorphanheader"] = [this](const auto &p) {
    return HandleAddOrphanHeader(p);
  };
  handlers_["getorphanstats"] = [this](const auto &p) {
    return HandleGetOrphanStats(p);
  };
  handlers_["evictorphans"] = [this](const auto &p) {
    return HandleEvictOrphans(p);
  };

  // Control commands
  handlers_["stop"] = [this](const auto &p) { return HandleStop(p); };

  // Testing commands
  handlers_["setmocktime"] = [this](const auto &p) {
    return HandleSetMockTime(p);
  };
  handlers_["invalidateblock"] = [this](const auto &p) {
    return HandleInvalidateBlock(p);
  };
  handlers_["clearbanned"] = [this](const auto &p) {
    return HandleClearBanned(p);
  };
  handlers_["getchaintips"] = [this](const auto &p) {
    return HandleGetChainTips(p);
  };
  handlers_["submitheader"] = [this](const auto &p) {
    return HandleSubmitHeader(p);
  };
}

bool RPCServer::Start() {
  if (running_) {
    return true;
  }

  // Remove old socket file if it exists
  unlink(socket_path_.c_str());

  // SECURITY FIX: Set restrictive umask before creating socket
  mode_t old_umask = umask(0077);  // rw------- for socket file

  // Create Unix domain socket
  server_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
  if (server_fd_ < 0) {
    umask(old_umask);  // Restore umask
    LOG_ERROR("Failed to create RPC socket");
    return false;
  }

  // Bind to socket
  struct sockaddr_un addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

  if (bind(server_fd_, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    LOG_ERROR("Failed to bind RPC socket to {}", socket_path_);
    close(server_fd_);
    server_fd_ = -1;
    umask(old_umask);  // Restore umask
    return false;
  }

  // Restore umask
  umask(old_umask);

  // SECURITY FIX: Explicitly set permissions (double-check)
  chmod(socket_path_.c_str(), 0600);  // Only owner can access

  // Listen for connections
  if (listen(server_fd_, 5) < 0) {
    LOG_ERROR("Failed to listen on RPC socket");
    close(server_fd_);
    server_fd_ = -1;
    return false;
  }

  running_ = true;
  server_thread_ = std::thread(&RPCServer::ServerThread, this);

  LOG_NET_INFO("RPC server started on {}", socket_path_);
  return true;
}

void RPCServer::Stop() {
  if (!running_) {
    return;
  }

  // SECURITY FIX: Set shutdown flag to reject new requests
  shutting_down_.store(true, std::memory_order_release);
  running_ = false;

  if (server_fd_ >= 0) {
    close(server_fd_);
    server_fd_ = -1;
  }

  if (server_thread_.joinable()) {
    server_thread_.join();
  }

  unlink(socket_path_.c_str());

  LOG_NET_INFO("RPC server stopped");
}

void RPCServer::ServerThread() {
  while (running_) {
    struct sockaddr_un client_addr;
    socklen_t client_len = sizeof(client_addr);

    int client_fd =
        accept(server_fd_, (struct sockaddr *)&client_addr, &client_len);
    if (client_fd < 0) {
      if (running_) {
        LOG_NET_WARN("failed to accept RPC connection");
      }
      continue;
    }

    HandleClient(client_fd);
    close(client_fd);
  }
}

void RPCServer::HandleClient(int client_fd) {
  // Check shutdown flag
  if (shutting_down_.load(std::memory_order_acquire)) {
    std::string error = util::JsonError("Server shutting down");
    send(client_fd, error.c_str(), error.size(), 0);
    return;
  }

  // SECURITY: Use vector to avoid buffer overflow
  std::vector<char> buffer(4096);
  ssize_t received = recv(client_fd, buffer.data(), buffer.size(), 0);

  if (received <= 0) {
    return;
  }

  // SECURITY: Bounds check before creating string
  if (received >= static_cast<ssize_t>(buffer.size())) {
    LOG_NET_ERROR("RPC request too large: {} bytes", received);
    std::string error = util::JsonError("Request too large");
    send(client_fd, error.c_str(), error.size(), 0);
    return;
  }

  std::string request(buffer.data(), received);

  // SECURITY: Use proper JSON parsing instead of hand-rolled parser
  std::string method;
  std::vector<std::string> params;

  try {
    nlohmann::json j = nlohmann::json::parse(request);

    // Extract method
    if (!j.contains("method") || !j["method"].is_string()) {
      std::string error = util::JsonError("Missing or invalid method field");
      send(client_fd, error.c_str(), error.size(), 0);
      return;
    }

    method = j["method"].get<std::string>();

    // Extract params (optional)
    if (j.contains("params")) {
      if (j["params"].is_array()) {
        for (const auto& param : j["params"]) {
          if (param.is_string()) {
            params.push_back(param.get<std::string>());
          } else {
            // Convert non-string params to string
            params.push_back(param.dump());
          }
        }
      } else if (j["params"].is_string()) {
        // Single string param
        params.push_back(j["params"].get<std::string>());
      }
    }
  } catch (const nlohmann::json::exception& e) {
    LOG_NET_WARN("RPC JSON parse error: {}", e.what());
    std::string error = util::JsonError("Invalid JSON");
    send(client_fd, error.c_str(), error.size(), 0);
    return;
  }

  // Execute command
  std::string response = ExecuteCommand(method, params);

  // Send response
  send(client_fd, response.c_str(), response.size(), 0);
}

std::string RPCServer::ExecuteCommand(const std::string &method,
                                      const std::vector<std::string> &params) {
  auto it = handlers_.find(method);
  if (it == handlers_.end()) {
    return util::JsonError("Unknown command");
  }

  try {
    return it->second(params);
  } catch (const std::exception &e) {
    // SECURITY : Log full error internally but return sanitized error to client
    LOG_NET_ERROR("RPC command '{}' failed: {}", method, e.what());

    // Return sanitized error message (escape special characters)
    return util::JsonError(e.what());
  }
}

std::string RPCServer::HandleGetInfo(const std::vector<std::string> &params) {
  auto *tip = chainstate_manager_.GetTip();
  int height = tip ? tip->nHeight : -1;

  // Get difficulty
  double difficulty = 1.0;
  if (tip && tip->nBits != 0) {
    int nShift = (tip->nBits >> 24) & 0xff;
    double dDiff = (double)0x000fffff / (double)(tip->nBits & 0x00ffffff);
    while (nShift < 29) {
      dDiff *= 256.0;
      nShift++;
    }
    while (nShift > 29) {
      dDiff /= 256.0;
      nShift--;
    }
    difficulty = dDiff;
  }

  std::ostringstream oss;
  oss << "{\n"
      << "  \"version\": \"0.1.0\",\n"
      << "  \"chain\": \"" << params_.GetChainTypeString() << "\",\n"
      << "  \"blocks\": " << height << ",\n"
      << "  \"headers\": " << height << ",\n"
      << "  \"bestblockhash\": \""
      << (tip ? tip->GetBlockHash().GetHex() : "null") << "\",\n"
      << "  \"difficulty\": " << difficulty << ",\n"
      << "  \"mediantime\": " << (tip ? tip->GetMedianTimePast() : 0) << ",\n"
      << "  \"connections\": " << network_manager_.active_peer_count() << "\n"
      << "}\n";
  return oss.str();
}

std::string
RPCServer::HandleGetBlockchainInfo(const std::vector<std::string> &params) {
  auto *tip = chainstate_manager_.GetTip();
  int height = tip ? tip->nHeight : -1;

  // Calculate difficulty
  double difficulty = 1.0;
  if (tip && tip->nBits != 0) {
    int nShift = (tip->nBits >> 24) & 0xff;
    double dDiff = (double)0x000fffff / (double)(tip->nBits & 0x00ffffff);
    while (nShift < 29) {
      dDiff *= 256.0;
      nShift++;
    }
    while (nShift > 29) {
      dDiff /= 256.0;
      nShift--;
    }
    difficulty = dDiff;
  }

  // Compute average inter-block times over recent windows
  // Note: We cap the window at (chain height - 1) to avoid including the
  // genesis→block1 transition, which may have an artificial timestamp and skew the average
  auto compute_avg = [](const chain::CBlockIndex *p, int window) -> double {
    if (!p || !p->pprev || window <= 0) return 0.0;

    // Cap window to avoid going back to genesis (stop at height 1)
    // This ensures we only measure block N → block N-1 transitions where N >= 2
    int actual_window = std::min(window, p->nHeight - 1);
    if (actual_window <= 0) return 0.0;

    const chain::CBlockIndex *cur = p;
    long long sum = 0;
    int count = 0;
    for (int i = 0; i < actual_window && cur && cur->pprev; ++i) {
      long long dt = static_cast<long long>(cur->nTime) - static_cast<long long>(cur->pprev->nTime);
      sum += dt;
      cur = cur->pprev;
      ++count;
    }
    if (count == 0) return 0.0;
    return static_cast<double>(sum) / static_cast<double>(count);
  };

  // Averages in seconds
  double avg10 = compute_avg(tip, 10);
  double avg20 = compute_avg(tip, 20);
  double avg40 = compute_avg(tip, 40);
  double avg100 = compute_avg(tip, 100);
  double avg500 = compute_avg(tip, 500);

  // Convert to minutes for reporting
  double avg10_min = avg10 / 60.0;
  double avg20_min = avg20 / 60.0;
  double avg40_min = avg40 / 60.0;
  double avg100_min = avg100 / 60.0;
  double avg500_min = avg500 / 60.0;

  const auto &consensus = params_.GetConsensus();

  // Calculate log2_chainwork for compact display (Bitcoin Core approach)
  double log2_chainwork = 0.0;
  if (tip) {
    log2_chainwork = std::log(tip->nChainWork.getdouble()) / std::log(2.0);
  }

  // Convert consensus parameters for reporting
  double target_spacing_min = static_cast<double>(consensus.nPowTargetSpacing) / 60.0; // minutes
  double half_life_hours = static_cast<double>(consensus.nASERTHalfLife) / 3600.0;     // hours

  std::ostringstream oss;
  oss << "{\n"
      << "  \"chain\": \"" << params_.GetChainTypeString() << "\",\n"
      << "  \"blocks\": " << height << ",\n"
      << "  \"headers\": " << height << ",\n"
      << "  \"bestblockhash\": \""
      << (tip ? tip->GetBlockHash().GetHex() : "null") << "\",\n"
      << "  \"difficulty\": " << difficulty << ",\n"
      << "  \"time\": " << (tip ? tip->nTime : 0) << ",\n"
      << "  \"time_str\": \"" << (tip ? util::FormatTime(tip->nTime) : "null") << "\",\n"
      << "  \"mediantime\": " << (tip ? tip->GetMedianTimePast() : 0) << ",\n"
      << "  \"mediantime_str\": \"" << (tip ? util::FormatTime(tip->GetMedianTimePast()) : "null") << "\",\n"
      << "  \"chainwork\": \"" << (tip ? tip->nChainWork.GetHex() : "0") << "\",\n"
      << "  \"log2_chainwork\": " << std::fixed << std::setprecision(1) << log2_chainwork << ",\n"
      << "  \"avg_block_time_10\": \"" << std::fixed << std::setprecision(1) << avg10_min << " mins\",\n"
      << "  \"avg_block_time_20\": \"" << std::fixed << std::setprecision(1) << avg20_min << " mins\",\n"
      << "  \"avg_block_time_40\": \"" << std::fixed << std::setprecision(1) << avg40_min << " mins\",\n"
      << "  \"avg_block_time_100\": \"" << std::fixed << std::setprecision(1) << avg100_min << " mins\",\n"
      << "  \"avg_block_time_500\": \"" << std::fixed << std::setprecision(1) << avg500_min << " mins\",\n"
      << "  \"asert\": {\n"
      << "    \"target_spacing\": \"" << std::fixed << std::setprecision(1) << target_spacing_min << " mins\",\n"
      << "    \"half_life\": \"" << std::fixed << std::setprecision(1) << half_life_hours << " hours\",\n"
      << "    \"anchor_height\": " << consensus.nASERTAnchorHeight << "\n"
      << "  },\n"
      << "  \"initialblockdownload\": " << (chainstate_manager_.IsInitialBlockDownload() ? "true" : "false") << "\n"
      << "}\n";
  return oss.str();
}

std::string
RPCServer::HandleGetBlockCount(const std::vector<std::string> &params) {
  auto *tip = chainstate_manager_.GetTip();
  int height = tip ? tip->nHeight : -1;

  std::ostringstream oss;
  oss << height << "\n";
  return oss.str();
}

std::string
RPCServer::HandleGetBlockHash(const std::vector<std::string> &params) {
  if (params.empty()) {
    return util::JsonError("Missing height parameter");
  }

  // SECURITY FIX: Safe integer parsing with bounds check
  auto height_opt = util::SafeParseInt(params[0], 0, 10000000);
  if (!height_opt) {
    return util::JsonError("Invalid height (must be 0-10000000)");
  }

  int height = *height_opt;
  auto *index = chainstate_manager_.GetBlockAtHeight(height);

  if (!index) {
    return util::JsonError("Block height out of range");
  }

  return index->GetBlockHash().GetHex() + "\n";
}

std::string
RPCServer::HandleGetBlockHeader(const std::vector<std::string> &params) {
  if (params.empty()) {
    return util::JsonError("Missing block hash parameter");
  }

  // SECURITY FIX: Safe hash parsing with validation
  auto hash_opt = util::SafeParseHash(params[0]);
  if (!hash_opt) {
    return util::JsonError("Invalid block hash (must be 64 hex characters)");
  }

  uint256 hash = *hash_opt;

  auto *index = chainstate_manager_.LookupBlockIndex(hash);
  if (!index) {
    return util::JsonError("Block not found");
  }

  // Calculate difficulty
  double difficulty = 1.0;
  if (index->nBits != 0) {
    int nShift = (index->nBits >> 24) & 0xff;
    double dDiff = (double)0x000fffff / (double)(index->nBits & 0x00ffffff);
    while (nShift < 29) {
      dDiff *= 256.0;
      nShift++;
    }
    while (nShift > 29) {
      dDiff /= 256.0;
      nShift--;
    }
    difficulty = dDiff;
  }

  // Calculate confirmations
  auto *tip = chainstate_manager_.GetTip();
  int confirmations = -1;
  if (chainstate_manager_.IsOnActiveChain(index)) {
    confirmations = tip->nHeight - index->nHeight + 1;
  }

  std::ostringstream oss;
  oss << "{\n"
      << "  \"hash\": \"" << index->GetBlockHash().GetHex() << "\",\n"
      << "  \"confirmations\": " << confirmations << ",\n"
      << "  \"height\": " << index->nHeight << ",\n"
      << "  \"version\": " << index->nVersion << ",\n"
      << "  \"versionHex\": \"" << std::hex << std::setw(8) << std::setfill('0')
      << index->nVersion << std::dec << "\",\n"
      << "  \"time\": " << index->nTime << ",\n"
      << "  \"mediantime\": " << index->GetMedianTimePast() << ",\n"
      << "  \"nonce\": " << index->nNonce << ",\n"
      << "  \"bits\": \"" << std::hex << std::setw(8) << std::setfill('0')
      << index->nBits << std::dec << "\",\n"
      << "  \"difficulty\": " << difficulty << ",\n"
      << "  \"chainwork\": \"" << index->nChainWork.GetHex() << "\",\n"
      << "  \"previousblockhash\": \""
      << (index->pprev ? index->pprev->GetBlockHash().GetHex() : "null")
      << "\",\n"
      << "  \"rx_hash\": \"" << index->hashRandomX.GetHex() << "\"\n"
      << "}\n";
  return oss.str();
}

std::string
RPCServer::HandleGetBestBlockHash(const std::vector<std::string> &params) {
  auto *tip = chainstate_manager_.GetTip();
  if (!tip) {
    return "null\n";
  }

  return tip->GetBlockHash().GetHex() + "\n";
}

std::string
RPCServer::HandleGetConnectionCount(const std::vector<std::string> &params) {
  size_t count = network_manager_.active_peer_count();

  std::ostringstream oss;
  oss << count << "\n";
  return oss.str();
}

std::string
RPCServer::HandleGetPeerInfo(const std::vector<std::string> &params) {
  // Get all peers from NetworkManager
  auto all_peers = network_manager_.peer_manager().get_all_peers();

  std::ostringstream oss;
  oss << "[\n";

  for (size_t i = 0; i < all_peers.size(); i++) {
    const auto &peer = all_peers[i];
    if (!peer)
      continue;

    const auto &stats = peer->stats();

    // Calculate connection duration in seconds
    auto now = std::chrono::steady_clock::now();
    auto connected_time = stats.connected_time.load(std::memory_order_relaxed);
    auto connected_tp = std::chrono::steady_clock::time_point(connected_time);
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(
        now - connected_tp);

    // Get misbehavior score from ConnectionManager
    int misbehavior_score = 0;
    bool should_disconnect = false;
    try {
      auto &peer_mgr = network_manager_.peer_manager();
      misbehavior_score = peer_mgr.GetMisbehaviorScore(peer->id());
      should_disconnect = peer_mgr.ShouldDisconnect(peer->id());
    } catch (...) {
      // Peer might not be in sync manager yet (handshake incomplete)
    }

    oss << "  {\n"
        << "    \"id\": " << peer->id() << ",\n"
        << "    \"addr\": \"" << peer->address() << ":" << peer->port()
        << "\",\n"
        << "    \"inbound\": " << (peer->is_inbound() ? "true" : "false")
        << ",\n"
        << "    \"connected\": " << (peer->is_connected() ? "true" : "false")
        << ",\n"
        << "    \"successfully_connected\": "
        << (peer->successfully_connected() ? "true" : "false") << ",\n"
        << "    \"version\": " << peer->version() << ",\n"
        << "    \"subver\": \"" << peer->user_agent() << "\",\n"
        << "    \"services\": \"" << std::hex << std::setfill('0')
        << std::setw(16) << peer->services() << std::dec << "\",\n"
        << "    \"startingheight\": " << peer->start_height() << ",\n"
        << "    \"pingtime\": " << (stats.ping_time_ms.load(std::memory_order_relaxed).count() / 1000.0) << ",\n"
        << "    \"bytessent\": " << stats.bytes_sent << ",\n"
        << "    \"bytesrecv\": " << stats.bytes_received << ",\n"
        << "    \"messagessent\": " << stats.messages_sent << ",\n"
        << "    \"messagesrecv\": " << stats.messages_received << ",\n"
        << "    \"conntime\": " << duration.count() << ",\n"
        << "    \"misbehavior_score\": " << misbehavior_score << ",\n"
        << "    \"should_disconnect\": "
        << (should_disconnect ? "true" : "false") << "\n";

    if (i < all_peers.size() - 1) {
      oss << "  },\n";
    } else {
      oss << "  }\n";
    }
  }

  oss << "]\n";
  return oss.str();
}

std::string RPCServer::HandleAddNode(const std::vector<std::string> &params) {
  LOG_INFO("RPC addnode called");

  if (params.empty()) {
    LOG_INFO("RPC addnode: missing address");
    return util::JsonError("Missing node address parameter");
  }

  std::string node_addr = params[0];
  std::string command = "add"; // Default command
  if (params.size() > 1) {
    command = params[1];
  }

  LOG_INFO("RPC addnode: address={}, command={}", node_addr, command);

  // Parse address:port using IPv6-safe parser
  // Supports both IPv4: "192.168.1.1:9590" and IPv6: "[2001:db8::1]:9590"
  std::string host;
  uint16_t port = 0;
  if (!util::ParseIPPort(node_addr, host, port)) {
    LOG_INFO("RPC addnode: invalid address format");
    return util::JsonError("Invalid address format (use IP:port or [IPv6]:port)");
  }

  if (command == "add") {
    // Validate IP address using centralized validation
    auto normalized_ip = util::ValidateAndNormalizeIP(host);
    if (!normalized_ip.has_value()) {
      return util::JsonError("Invalid IP address (hostnames not supported)");
    }

    // Use centralized NetworkAddress::from_string() for IP conversion
    protocol::NetworkAddress addr = protocol::NetworkAddress::from_string(
        *normalized_ip, port, protocol::ServiceFlags::NODE_NETWORK);

    // Check if conversion failed (from_string returns zeroed IP on error)
    bool is_zero = std::all_of(addr.ip.begin(), addr.ip.end(), [](uint8_t b) { return b == 0; });
    if (is_zero) {
      return util::JsonError("Failed to parse IP address: " + *normalized_ip);
    }

    // Connect to the node
    LOG_INFO("RPC addnode: calling network_manager_.connect_to()");
    auto result = network_manager_.connect_to(addr);
    LOG_INFO("RPC addnode: connect_to() returned result");
    if (result != network::ConnectionResult::Success) {
      LOG_INFO("RPC addnode: connect_to() failed");
      return util::JsonError("Failed to connect to node");
    }

    std::ostringstream oss;
    oss << "{\n"
        << "  \"success\": true,\n"
        << "  \"message\": \"Connection initiated to " << node_addr << "\"\n"
        << "}\n";
    return oss.str();
  } else if (command == "remove") {
    // Find peer by address:port and disconnect (thread-safe)
    int peer_id =
        network_manager_.peer_manager().find_peer_by_address(host, port);

    if (peer_id < 0) {
      LOG_WARN("addnode remove: Peer not found: {}", node_addr);
      std::ostringstream oss;
      oss << "{\n"
          << "  \"error\": \"Peer not found: " << node_addr << "\"\n"
          << "}\n";
      return oss.str();
    }

    LOG_INFO("addnode remove: Found peer {} at {}, disconnecting", peer_id,
             node_addr);
    network_manager_.disconnect_from(peer_id);

    std::ostringstream oss;
    oss << "{\n"
        << "  \"success\": true,\n"
        << "  \"message\": \"Disconnected from " << node_addr << "\"\n"
        << "}\n";
    return oss.str();
  } else {
    return util::JsonError("Unknown command (use 'add' or 'remove')");
  }
}

std::string RPCServer::HandleSetBan(const std::vector<std::string> &params) {
  if (params.empty()) {
    return util::JsonError("Missing subnet/IP parameter");
  }

  std::string address = params[0];
  std::string command = "add"; // Default command
  if (params.size() > 1) {
    command = params[1];
  }

  if (command == "add") {
    // Default bantime: 24 hours (matches Core's spirit; permanent requires explicit mode)
    static constexpr int64_t DEFAULT_BANTIME_SEC = 24 * 60 * 60;

    // Validate and normalize IP address using centralized utility
    auto canon_addr_opt = util::ValidateAndNormalizeIP(address);
    if (!canon_addr_opt.has_value()) {
      return util::JsonError("Invalid IP address");
    }
    std::string canon_addr = *canon_addr_opt;

    // Optional bantime parameter (seconds); if 0 or omitted => default
    int64_t bantime = 0;
    if (params.size() > 2) {
      auto bantime_opt = util::SafeParseInt64(params[2], 0, INT64_MAX);
      if (!bantime_opt) {
        return util::JsonError("Invalid bantime (must be >= 0)");
      }
      bantime = *bantime_opt;
    }

    // Optional mode parameter: "absolute" | "permanent" | "relative" (default)
    std::string mode = "relative";
    if (params.size() > 3) {
      mode = params[3];
      for (auto &c : mode) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    }

    int64_t now = util::GetTime();
    int64_t offset = 0;

    if (mode == "permanent") {
      offset = 0; // BanMan treats 0 as permanent
    } else if (mode == "absolute") {
      if (bantime == 0) {
        return util::JsonError("absolute mode requires a non-zero bantime (unix timestamp)");
      }
      if (bantime <= now) {
        return util::JsonError("absolute bantime must be in the future");
      }
      offset = bantime - now;
    } else { // relative (default)
      if (bantime == 0) {
        offset = DEFAULT_BANTIME_SEC;
      } else {
        offset = bantime;
      }
    }

    // Ban the canonical address
    network_manager_.peer_manager().Ban(canon_addr, offset);

    std::ostringstream oss;
    if (mode == "permanent") {
      oss << "{\n"
          << "  \"success\": true,\n"
          << "  \"message\": \"Permanently banned " << canon_addr << "\"\n"
          << "}\n";
    } else if (mode == "absolute") {
      oss << "{\n"
          << "  \"success\": true,\n"
          << "  \"message\": \"Banned " << canon_addr << " until " << (now + offset)
          << " (absolute)\"\n"
          << "}\n";
    } else {
      oss << "{\n"
          << "  \"success\": true,\n"
          << "  \"message\": \"Banned " << canon_addr << " for " << offset
          << " seconds\"\n"
          << "}\n";
    }
    return oss.str();

  } else if (command == "remove") {
    // Use centralized IP normalization (converts IPv4-mapped IPv6 → IPv4)
    auto normalized = util::ValidateAndNormalizeIP(address);
    if (!normalized.has_value()) {
      return util::JsonError("Invalid IP address: " + address);
    }

    // Unban the normalized address (bans are always stored normalized)
    network_manager_.peer_manager().Unban(*normalized);

    std::ostringstream oss;
    oss << "{\n"
        << "  \"success\": true,\n"
        << "  \"message\": \"Unbanned " << address << "\"\n"
        << "}\n";
    return oss.str();

  } else {
    return util::JsonError("Unknown command (use 'add' or 'remove')");
  }
}

std::string
RPCServer::HandleListBanned(const std::vector<std::string> &params) {
  auto banned = network_manager_.peer_manager().GetBanned();

  std::ostringstream oss;
  oss << "[\n";

  size_t i = 0;
  for (const auto &[address, entry] : banned) {
    oss << "  {\n"
        << "    \"address\": \"" << address << "\",\n"
        << "    \"banned_until\": " << entry.nBanUntil << ",\n"
        << "    \"ban_created\": " << entry.nCreateTime << ",\n"
        << "    \"ban_reason\": \"manually added\"\n"
        << "  }";

    if (i < banned.size() - 1) {
      oss << ",";
    }
    oss << "\n";
    i++;
  }

  oss << "]\n";
  return oss.str();
}

std::string
RPCServer::HandleGetAddrManInfo(const std::vector<std::string> &params) {
  auto &discovery_man = network_manager_.discovery_manager();

  size_t total = discovery_man.Size();
  size_t tried = discovery_man.TriedCount();
  size_t new_addrs = discovery_man.NewCount();

  std::ostringstream oss;
  oss << "{\n"
      << "  \"total\": " << total << ",\n"
      << "  \"tried\": " << tried << ",\n"
      << "  \"new\": " << new_addrs << "\n"
      << "}\n";
  return oss.str();
}

std::string
RPCServer::HandleGetDifficulty(const std::vector<std::string> &params) {
  auto *tip = chainstate_manager_.GetTip();

  double difficulty = 1.0;
  if (tip && tip->nBits != 0) {
    int nShift = (tip->nBits >> 24) & 0xff;
    double dDiff = (double)0x000fffff / (double)(tip->nBits & 0x00ffffff);
    while (nShift < 29) {
      dDiff *= 256.0;
      nShift++;
    }
    while (nShift > 29) {
      dDiff /= 256.0;
      nShift--;
    }
    difficulty = dDiff;
  }

  std::ostringstream oss;
  oss << difficulty << "\n";
return oss.str();
}

std::string RPCServer::HandleDisconnectNode(const std::vector<std::string> &params) {
  // Only available on regtest
  if (params_.GetChainType() != chain::ChainType::REGTEST) {
    return util::JsonError("disconnectnode only available on regtest");
  }

  if (params.empty()) {
    return util::JsonError("Missing parameter: peer id or address:port");
  }

  // Try peer id first
  auto id_opt = util::SafeParseInt(params[0], 0, 100000000);
  int peer_id = -1;
  if (id_opt) {
    peer_id = *id_opt;
  } else {
    // Parse address[:port] using IPv6-safe parser
    std::string node_addr = params[0];
    std::string host;
    uint16_t port = 0;

    // Try parsing with port first
    if (util::ParseIPPort(node_addr, host, port)) {
      // Successfully parsed IP:port
      peer_id = network_manager_.peer_manager().find_peer_by_address(host, port);
    } else {
      // Try as bare IP address (no port)
      auto validated = util::ValidateAndNormalizeIP(node_addr);
      if (validated.has_value()) {
        host = *validated;
        peer_id = network_manager_.peer_manager().find_peer_by_address(host, port);
      } else {
        return util::JsonError("Invalid IP address or IP:port format");
      }
    }
  }

  if (peer_id < 0) {
    return util::JsonError("Peer not found");
  }

  bool ok = network_manager_.disconnect_from(peer_id);
  if (!ok) {
    return util::JsonError("Failed to disconnect peer");
  }

  std::ostringstream oss;
  oss << "{\n"
      << "  \"success\": true,\n"
      << "  \"peer_id\": " << peer_id << "\n"
      << "}\n";
  return oss.str();
}

std::string RPCServer::HandleAddOrphanHeader(const std::vector<std::string> &params) {
  // Test-only: available on test networks
  if (params_.GetChainType() == chain::ChainType::MAIN) {
    return util::JsonError("addorphanheader not available on mainnet");
  }
  if (params.empty()) {
    return util::JsonError("Missing parameter: hex-encoded 100-byte header");
  }
  const std::string &hex = params[0];
  int peer_id = 1;
  if (params.size() >= 2) {
    auto id_opt = util::SafeParseInt(params[1], 0, 100000000);
    if (!id_opt) return util::JsonError("Invalid peer_id");
    peer_id = *id_opt;
  }
  if (hex.size() != 200) {
    return util::JsonError("Invalid header length (expect 200 hex chars)");
  }
  auto hex_to_nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
  };
  std::vector<uint8_t> bytes; bytes.reserve(100);
  for (size_t i=0;i<hex.size();i+=2) {
    int hi = hex_to_nibble(hex[i]);
    int lo = hex_to_nibble(hex[i+1]);
    if (hi < 0 || lo < 0) return util::JsonError("Invalid hex in header");
    bytes.push_back(static_cast<uint8_t>((hi<<4)|lo));
  }
  CBlockHeader header;
  if (!header.Deserialize(bytes.data(), bytes.size())) {
    return util::JsonError("Failed to deserialize header");
  }
  bool added = chainstate_manager_.AddOrphanHeader(header, peer_id);
  size_t count = chainstate_manager_.GetOrphanHeaderCount();
  std::ostringstream oss;
  oss << "{\n"
      << "  \"added\": " << (added ? "true" : "false") << ",\n"
      << "  \"count\": " << count << "\n"
      << "}\n";
  return oss.str();
}

std::string RPCServer::HandleGetOrphanStats(const std::vector<std::string> &params) {
  if (params_.GetChainType() == chain::ChainType::MAIN) {
    return util::JsonError("getorphanstats not available on mainnet");
  }
  size_t count = chainstate_manager_.GetOrphanHeaderCount();
  auto per = chainstate_manager_.GetPeerOrphanCounts();
  std::ostringstream oss;
  oss << "{\n"
      << "  \"count\": " << count << ",\n"
      << "  \"by_peer\": [\n";
  size_t i=0; for (const auto &kv : per) {
    oss << "    { \"peer_id\": " << kv.first << ", \"count\": " << kv.second << " }";
    if (i++ + 1 < per.size()) oss << ",";
    oss << "\n";
  }
  oss << "  ]\n";
  oss << "}\n";
  return oss.str();
}

std::string RPCServer::HandleEvictOrphans(const std::vector<std::string> &params) {
  if (params_.GetChainType() == chain::ChainType::MAIN) {
    return util::JsonError("evictorphans not available on mainnet");
  }
  size_t before = chainstate_manager_.GetOrphanHeaderCount();
  size_t evicted = chainstate_manager_.EvictOrphanHeaders();
  size_t after = chainstate_manager_.GetOrphanHeaderCount();
  std::ostringstream oss;
  oss << "{\n"
      << "  \"before\": " << before << ",\n"
      << "  \"evicted\": " << evicted << ",\n"
      << "  \"after\": " << after << "\n"
      << "}\n";
  return oss.str();
}

std::string RPCServer::HandleReportMisbehavior(const std::vector<std::string> &params) {
  // Test-only RPC: available on testnet/regtest only
  if (params_.GetChainType() == chain::ChainType::MAIN) {
    return util::JsonError("reportmisbehavior not available on mainnet");
  }
  if (params.size() < 2) {
    return util::JsonError("Usage: reportmisbehavior <peer_id> <type> [count]");
  }

  // peer_id
  auto id_opt = util::SafeParseInt(params[0], 0, 100000000);
  if (!id_opt) {
    return util::JsonError("Invalid peer_id");
  }
  int peer_id = *id_opt;

  std::string mtype = params[1];
  for (auto &c : mtype) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
  int count = 1;
  if (params.size() >= 3) {
    auto cnt_opt = util::SafeParseInt(params[2], 1, 1000000);
    if (!cnt_opt) {
      return util::JsonError("Invalid count (must be >=1)");
    }
    count = *cnt_opt;
  }

  auto &pm = network_manager_.peer_manager();
  int applied = 0;
  bool existed_before = pm.get_peer(peer_id) != nullptr;

  auto apply = [&](auto fn) {
    for (int i=0;i<count;i++) { fn(); applied++; }
  };

  if (mtype == "invalid_pow") {
    apply([&]{ pm.ReportInvalidPoW(peer_id); });
  } else if (mtype == "non_continuous") {
    apply([&]{ pm.ReportNonContinuousHeaders(peer_id); });
  } else if (mtype == "oversized") {
    apply([&]{ pm.ReportOversizedMessage(peer_id); });
  } else if (mtype == "low_work") {
    apply([&]{ pm.ReportLowWorkHeaders(peer_id); });
  } else if (mtype == "invalid_header") {
    apply([&]{ pm.ReportInvalidHeader(peer_id, "test"); });
  } else if (mtype == "too_many_orphans") {
    apply([&]{ pm.ReportTooManyOrphans(peer_id); });
  } else if (mtype == "increment_unconnecting") {
    apply([&]{ pm.IncrementUnconnectingHeaders(peer_id); });
  } else if (mtype == "reset_unconnecting") {
    pm.ResetUnconnectingHeaders(peer_id);
    applied = 0;
  } else if (mtype == "clear_discouraged") {
    pm.ClearDiscouraged();
    applied = 0;
  } else {
    return util::JsonError("Unknown type (valid: invalid_pow, non_continuous, oversized, low_work, invalid_header, too_many_orphans, increment_unconnecting, reset_unconnecting)");
  }

  // Capture state BEFORE periodic processing (peer may be removed during processing)
  int score_before = 0;
  bool should_disc_before = false;
  try {
    score_before = pm.GetMisbehaviorScore(peer_id);
    should_disc_before = pm.ShouldDisconnect(peer_id);
  } catch (...) {
    // Peer state may not be fully initialized yet
  }

  // Do NOT trigger periodic removal here; tests may want to explicitly disconnect via RPC
  bool exists_after = pm.get_peer(peer_id) != nullptr;

  // Prefer the pre-check decision; if peer was already removed externally, indicate disconnect
  int score = score_before;
  bool should_disc = should_disc_before || !exists_after;

  std::ostringstream oss;
  oss << "{\n"
      << "  \"peer_id\": " << peer_id << ",\n"
      << "  \"applied\": " << applied << ",\n"
      << "  \"score\": " << score << ",\n"
      << "  \"should_disconnect\": " << (should_disc ? "true" : "false") << ",\n"
      << "  \"peer_existed_before\": " << (existed_before ? "true" : "false") << ",\n"
      << "  \"peer_exists_after\": " << (exists_after ? "true" : "false") << "\n"
      << "}\n";
  return oss.str();
}

std::string RPCServer::HandleGetNextWorkRequired(const std::vector<std::string> &params) {
  // Allow on test networks only (testnet/regtest)
  if (params_.GetChainType() == chain::ChainType::MAIN) {
    return util::JsonError("getnextworkrequired not available on mainnet");
  }
  const chain::CBlockIndex* tip = chainstate_manager_.GetTip();
  uint32_t bits = consensus::GetNextWorkRequired(tip, params_);
  std::ostringstream oss;
  oss << "{\n"
      << "  \"bits\": \"" << std::hex << std::setw(8) << std::setfill('0') << bits << std::dec << "\",\n"
      << "  \"bits_u32\": " << bits << "\n"
      << "}\n";
  return oss.str();
}

std::string
RPCServer::HandleGetMiningInfo(const std::vector<std::string> &params) {
  auto *tip = chainstate_manager_.GetTip();
  int height = tip ? tip->nHeight : -1;

  // Calculate difficulty
  double difficulty = 1.0;
  if (tip && tip->nBits != 0) {
    int nShift = (tip->nBits >> 24) & 0xff;
    double dDiff = (double)0x000fffff / (double)(tip->nBits & 0x00ffffff);
    while (nShift < 29) {
      dDiff *= 256.0;
      nShift++;
    }
    while (nShift > 29) {
      dDiff /= 256.0;
      nShift--;
    }
    difficulty = dDiff;
  }

  // Calculate network hashrate (simplified - based on last DEFAULT_HASHRATE_CALCULATION_BLOCKS)
  double networkhashps = 0.0;
  if (tip && tip->nHeight > 0) {
    int nblocks = std::min(protocol::DEFAULT_HASHRATE_CALCULATION_BLOCKS, tip->nHeight);
    const chain::CBlockIndex *pb = tip;
    const chain::CBlockIndex *pb0 = pb;

    // Walk back nblocks
    for (int i = 0; i < nblocks && pb0->pprev; i++) {
      pb0 = pb0->pprev;
    }

    int64_t timeDiff = pb->nTime - pb0->nTime;
    if (timeDiff > 0) {
      arith_uint256 workDiff = pb->nChainWork - pb0->nChainWork;
      networkhashps = workDiff.getdouble() / timeDiff;
    }
  }

  std::ostringstream oss;
  oss << "{\n"
      << "  \"blocks\": " << height << ",\n"
      << "  \"difficulty\": " << difficulty << ",\n"
      << "  \"networkhashps\": " << networkhashps << ",\n"
      << "  \"chain\": \"" << params_.GetChainTypeString() << "\"\n"
      << "}\n";
  return oss.str();
}

std::string
RPCServer::HandleGetNetworkHashPS(const std::vector<std::string> &params) {
  auto *tip = chainstate_manager_.GetTip();

  // Default to DEFAULT_HASHRATE_CALCULATION_BLOCKS
  int nblocks = protocol::DEFAULT_HASHRATE_CALCULATION_BLOCKS;
  if (!params.empty()) {
    // Fix: Use SafeParseInt to prevent integer overflow/crash (was std::stoi)
    auto nblocks_opt = util::SafeParseInt(params[0], -1, 10000000);
    if (!nblocks_opt) {
      return util::JsonError("Invalid nblocks parameter (must be -1 to 10000000)");
    }
    nblocks = *nblocks_opt;
    if (nblocks == -1 || nblocks == 0) {
      nblocks = protocol::DEFAULT_HASHRATE_CALCULATION_BLOCKS;
    }
  }

  double networkhashps = 0.0;
  if (tip && tip->nHeight > 0) {
    nblocks = std::min(nblocks, tip->nHeight);
    const chain::CBlockIndex *pb = tip;
    const chain::CBlockIndex *pb0 = pb;

    // Walk back nblocks
    for (int i = 0; i < nblocks && pb0->pprev; i++) {
      pb0 = pb0->pprev;
    }

    int64_t timeDiff = pb->nTime - pb0->nTime;
    if (timeDiff > 0) {
      arith_uint256 workDiff = pb->nChainWork - pb0->nChainWork;
      networkhashps = workDiff.getdouble() / timeDiff;
    }
  }

  std::ostringstream oss;
  oss << networkhashps << "\n";
  return oss.str();
}

std::string
RPCServer::HandleStartMining(const std::vector<std::string> &params) {
  if (!miner_) {
    return util::JsonError("Mining not available");
  }

  if (miner_->IsMining()) {
    return util::JsonError("Already mining");
  }

  // Parse optional mining address parameter
  // Note: Address is "sticky" - if not provided, previous address is retained
  if (!params.empty()) {
    const std::string& address_str = params[0];

    // Validate address is 40 hex characters (160 bits / 4 bits per hex char)
    // Validate length and hex characters using centralized helper
    if (address_str.length() != 40 || !util::IsValidHex(address_str)) {
      return util::JsonError("Invalid mining address (must be 40 hex characters)");
    }

    // Parse and set mining address (persists across subsequent calls)
    uint160 mining_address;
    mining_address.SetHex(address_str);
    miner_->SetMiningAddress(mining_address);
  }

  bool started = miner_->Start();
  if (!started) {
    return util::JsonError("Failed to start mining");
  }

  std::ostringstream oss;
  oss << "{\n"
      << "  \"mining\": true,\n"
      << "  \"message\": \"Mining started\",\n"
      << "  \"address\": \"" << miner_->GetMiningAddress().GetHex() << "\"\n"
      << "}\n";
  return oss.str();
}

std::string
RPCServer::HandleStopMining(const std::vector<std::string> &params) {
  if (!miner_) {
    return util::JsonError("Mining not available");
  }

  if (!miner_->IsMining()) {
    return util::JsonError("Not currently mining");
  }

  miner_->Stop();

  std::ostringstream oss;
  oss << "{\n"
      << "  \"mining\": false,\n"
      << "  \"message\": \"Mining stopped\"\n"
      << "}\n";
  return oss.str();
}

std::string RPCServer::HandleGenerate(const std::vector<std::string> &params) {
  if (!miner_) {
    return util::JsonError("Mining not available");
  }

  // SECURITY FIX: Only allow generate on regtest
  if (params_.GetChainType() != chain::ChainType::REGTEST) {
    return util::JsonError("generate only available on regtest");
  }

  if (params.empty()) {
    return util::JsonError("Missing number of blocks parameter");
  }

  // SECURITY FIX: Safe integer parsing with reasonable limit for regtest
  auto num_blocks_opt = util::SafeParseInt(params[0], 1, 1000);
  if (!num_blocks_opt) {
    return util::JsonError("Invalid number of blocks (must be 1-1000)");
  }

  int num_blocks = *num_blocks_opt;

  // Parse optional mining address parameter (second parameter)
  // Note: Address is "sticky" - if not provided, previous address is retained
  if (params.size() >= 2) {
    const std::string& address_str = params[1];

    // Validate address is 40 hex characters (160 bits / 4 bits per hex char)
    // Validate length and hex characters using centralized helper
    if (address_str.length() != 40 || !util::IsValidHex(address_str)) {
      return util::JsonError("Invalid mining address (must be 40 hex characters)");
    }

    // Parse and set mining address (persists across subsequent calls)
    uint160 mining_address;
    mining_address.SetHex(address_str);
    miner_->SetMiningAddress(mining_address);
  }

  // Get starting height and calculate target
  const chain::CBlockIndex *start_tip = chainstate_manager_.GetTip();
  int start_height = start_tip ? start_tip->nHeight : -1;
  int target_height = start_height + num_blocks;

  // Ensure miner is stopped before starting
  miner_->Stop();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Start mining with target height (miner stops itself when reached)
  if (!miner_->Start(target_height)) {
    LOG_ERROR("RPC: Failed to start mining");
    return "[]\n";
  }

  // Wait for miner to stop (up to 10 minutes total)
  int wait_count = 0;
  while (miner_->IsMining() && wait_count < 6000) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    wait_count++;
  }

  // Ensure miner is fully stopped
  miner_->Stop();

  // Get final height
  const chain::CBlockIndex *current_tip = chainstate_manager_.GetTip();
  int actual_height = current_tip ? current_tip->nHeight : -1;
  int blocks_mined = actual_height - start_height;

  // Return simple success message with count
  // NOTE: We don't return block hashes to avoid RPC buffer overflow
  // (returning 100+ hashes can exceed the 4KB buffer and crash the node)
  std::ostringstream oss;
  oss << "{\n"
      << "  \"blocks\": " << blocks_mined << ",\n"
      << "  \"height\": " << actual_height << "\n"
      << "}\n";
return oss.str();
}

std::string RPCServer::HandleClearBanned(const std::vector<std::string> &params) {
  // Only available on regtest
  if (params_.GetChainType() != chain::ChainType::REGTEST) {
    return util::JsonError("clearbanned only available on regtest");
  }
  network_manager_.peer_manager().ClearBanned();
  std::ostringstream oss;
  oss << "{\n"
      << "  \"success\": true,\n"
      << "  \"message\": \"Cleared all banned peers\"\n"
      << "}\n";
  return oss.str();
}

std::string RPCServer::HandleGetChainTips(const std::vector<std::string> &params) {
  // Only available on regtest
  if (params_.GetChainType() != chain::ChainType::REGTEST) {
    return util::JsonError("getchaintips only available on regtest");
  }

  // Stub implementation: return only the active tip for now
  const auto *tip = chainstate_manager_.GetTip();
  std::ostringstream oss;
  oss << "[\n";
  if (tip) {
    oss << "  {\n"
        << "    \"height\": " << tip->nHeight << ",\n"
        << "    \"hash\": \"" << tip->GetBlockHash().GetHex() << "\",\n"
        << "    \"branchlen\": 0,\n"
        << "    \"status\": \"active\"\n"
        << "  }\n";
  }
  oss << "]\n";
  return oss.str();
}

std::string RPCServer::HandleSubmitHeader(const std::vector<std::string> &params) {
  // Only available on test networks (testnet/regtest)
  if (params_.GetChainType() == chain::ChainType::MAIN) {
    return util::JsonError("submitheader not available on mainnet");
  }

  if (params.empty()) {
    return util::JsonError("Missing parameter: hex-encoded 100-byte header");
  }

  const std::string &hex = params[0];
  // Optional skip_pow flag (second param): true/false or 1/0
  bool skip_pow = false;
  if (params.size() >= 2) {
    std::string v = params[1];
    for (auto &c : v) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    if (v == "true" || v == "1") skip_pow = true;
    else if (v == "false" || v == "0") skip_pow = false;
    else {
      return util::JsonError("Invalid skip_pow value (use true/false or 1/0)");
    }
  }

  // Expect exactly 200 hex chars (100 bytes)
  if (hex.size() != 200) {
    return util::JsonError("Invalid header length (expect 200 hex chars)");
  }

  // Decode hex
  std::vector<uint8_t> bytes;
  bytes.reserve(100);
  auto hex_to_nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
  };

  for (size_t i = 0; i < hex.size(); i += 2) {
    int hi = hex_to_nibble(hex[i]);
    int lo = hex_to_nibble(hex[i + 1]);
    if (hi < 0 || lo < 0) {
      return util::JsonError("Invalid hex in header");
    }
    bytes.push_back(static_cast<uint8_t>((hi << 4) | lo));
  }

  if (bytes.size() != CBlockHeader::HEADER_SIZE) {
    return util::JsonError("Decoded header size mismatch");
  }

  CBlockHeader header;
  if (!header.Deserialize(bytes.data(), bytes.size())) {
    return util::JsonError("Failed to deserialize header");
  }

  // Apply temporary PoW-skip hook if requested (regtest-only)
  bool prev = chainstate_manager_.TestGetSkipPoWChecks();
  chainstate_manager_.TestSetSkipPoWChecks(skip_pow);

  validation::ValidationState state;
  bool ok = false;
  try {
    ok = chainstate_manager_.ProcessNewBlockHeader(header, state, /*min_pow_checked=*/true);
  } catch (...) {
    chainstate_manager_.TestSetSkipPoWChecks(prev);
    throw;
  }
  // Always restore flag
  chainstate_manager_.TestSetSkipPoWChecks(prev);

  if (!ok) {
    return util::JsonError(state.GetRejectReason() + ": " + state.GetDebugMessage());
  }

  std::ostringstream oss;
  oss << "{\n"
      << "  \"success\": true,\n"
      << "  \"hash\": \"" << header.GetHash().GetHex() << "\"\n"
      << "}\n";
  return oss.str();
}

std::string RPCServer::HandleStop(const std::vector<std::string> &params) {
  LOG_INFO("Received stop command via RPC");

  // SECURITY FIX: Set shutdown flag immediately to reject new requests
  shutting_down_.store(true, std::memory_order_release);

  // Trigger graceful shutdown via callback
  if (shutdown_callback_) {
    shutdown_callback_();
  }

  return "\"Unicity stopping\"\n";
}

std::string
RPCServer::HandleSetMockTime(const std::vector<std::string> &params) {
  if (params.empty()) {
    return util::JsonError("Missing timestamp parameter");
  }

  // SECURITY: Only allow setmocktime on regtest/testnet
  if (params_.GetChainType() == chain::ChainType::MAIN) {
    return util::JsonError("setmocktime not allowed on mainnet");
  }

  // SECURITY: Validate reasonable range (year 1970 to 2106)
  // Allow 0 for disabling mock time
  auto mock_time_opt = util::SafeParseInt64(params[0], 0, 4294967295LL);
  if (!mock_time_opt) {
    return util::JsonError("Invalid timestamp (must be 0 or 1-4294967295)");
  }
  int64_t mock_time = *mock_time_opt;

  // Set mock time (0 to disable)
  util::SetMockTime(mock_time);

  std::ostringstream oss;
  if (mock_time == 0) {
    oss << "{\n"
        << "  \"success\": true,\n"
        << "  \"message\": \"Mock time disabled\"\n"
        << "}\n";
  } else {
    oss << "{\n"
        << "  \"success\": true,\n"
        << "  \"mocktime\": " << mock_time << ",\n"
        << "  \"message\": \"Mock time set to " << mock_time << "\"\n"
        << "}\n";
  }

  return oss.str();
}

std::string
RPCServer::HandleInvalidateBlock(const std::vector<std::string> &params) {
  if (params.empty()) {
    return util::JsonError("Missing block hash parameter");
  }

  // SECURITY FIX: Safe hash parsing with validation
  auto hash_opt = util::SafeParseHash(params[0]);
  if (!hash_opt) {
    return util::JsonError("Invalid block hash (must be 64 hex characters)");
  }

  uint256 hash = *hash_opt;

  // Check if block exists
  auto *index = chainstate_manager_.LookupBlockIndex(hash);
  if (!index) {
    return util::JsonError("Block not found");
  }

  // Invalidate the block
  bool success = chainstate_manager_.InvalidateBlock(hash);

  if (!success) {
    return util::JsonError("Failed to invalidate block");
  }

  std::ostringstream oss;
  oss << "{\n"
      << "  \"success\": true,\n"
      << "  \"hash\": \"" << hash.GetHex() << "\",\n"
      << "  \"message\": \"Block and all descendants invalidated\"\n"
      << "}\n";

  return oss.str();
}

} // namespace rpc
} // namespace unicity
