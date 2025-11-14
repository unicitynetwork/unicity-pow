// Copyright (c) 2025 The Unicity Foundation
// Distributed under the MIT software license

#include "network/rpc_client.hpp"
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace unicity {
namespace rpc {

RPCClient::RPCClient(const std::string &socket_path)
    : socket_path_(socket_path), socket_fd_(-1) {}

RPCClient::~RPCClient() { Disconnect(); }

bool RPCClient::Connect() {
  if (socket_fd_ >= 0) {
    return true; // Already connected
  }

  // Create Unix domain socket
  socket_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
  if (socket_fd_ < 0) {
    return false;
  }

  // Set up socket address
  struct sockaddr_un addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

  // Connect to node
  if (connect(socket_fd_, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    close(socket_fd_);
    socket_fd_ = -1;
    return false;
  }

  return true;
}

std::string RPCClient::ExecuteCommand(const std::string &method,
                                      const std::vector<std::string> &params) {
  if (!IsConnected()) {
    throw std::runtime_error("Not connected to node");
  }

  // Build simple JSON-RPC request
  std::ostringstream request;
  request << "{\"method\":\"" << method << "\"";

  if (!params.empty()) {
    request << ",\"params\":[";
    for (size_t i = 0; i < params.size(); ++i) {
      if (i > 0)
        request << ",";
      request << "\"" << params[i] << "\"";
    }
    request << "]";
  }

  request << "}\n";

  std::string request_str = request.str();

  // Send request
  ssize_t sent = send(socket_fd_, request_str.c_str(), request_str.size(), 0);
  if (sent < 0) {
    throw std::runtime_error("Failed to send request");
  }

  // Receive response
  char buffer[4096];
  ssize_t received = recv(socket_fd_, buffer, sizeof(buffer) - 1, 0);
  if (received < 0) {
    throw std::runtime_error("Failed to receive response");
  }

  buffer[received] = '\0';
  return std::string(buffer, received);
}

void RPCClient::Disconnect() {
  if (socket_fd_ >= 0) {
    close(socket_fd_);
    socket_fd_ = -1;
  }
}

} // namespace rpc
} // namespace unicity
