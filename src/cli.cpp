// Copyright (c) 2025 The Unicity Foundation
// Distributed under the MIT software license

#include "network/rpc_client.hpp"
#include "version.hpp"
#include <cstdlib>
#include <iostream>
#include <pwd.h>
#include <string>
#include <unistd.h>
#include <vector>

std::string GetDefaultDataDir() {
  const char *home = getenv("HOME");
  if (!home) {
    struct passwd *pw = getpwuid(getuid());
    if (pw) {
      home = pw->pw_dir;
    }
  }

  if (home) {
    return std::string(home) + "/.unicity";
  }

  return ".unicity";
}

void PrintUsage(const char *program_name) {
  std::cout
      << "Unicity CLI - Query blockchain node\n\n"
      << "Usage: " << program_name << " [options] <command> [params]\n\n"
      << "Options:\n"
      << "  --datadir=<path>     Data directory (default: ~/.unicity)\n"
      << "  --version            Show version information\n"
      << "  --help               Show this help message\n\n"
      << "Commands:\n"
      << "\n"
      << "Blockchain:\n"
      << "  getinfo              Get general node information\n"
      << "  getblockchaininfo    Get blockchain state information\n"
      << "  getblockcount        Get current block height\n"
      << "  getblockhash <height>    Get block hash at height\n"
      << "  getblockheader <hash>    Get block header by hash\n"
      << "  getbestblockhash     Get hash of best (tip) block\n"
      << "  getdifficulty        Get proof-of-work difficulty\n"
      << "\n"
      << "Mining:\n"
      << "  getmininginfo        Get mining-related information\n"
      << "  getnetworkhashps [nblocks]  Get network hashes per second\n"
      << "\n"
      << "Network:\n"
      << "  getpeerinfo          Get connected peer information\n"
      << "\n"
      << "Control:\n"
      << "  stop                 Stop the node\n"
      << std::endl;
}

int main(int argc, char *argv[]) {
  try {
    if (argc < 2) {
      PrintUsage(argv[0]);
      return 1;
    }

    // Parse options
    std::string datadir = GetDefaultDataDir();
    std::string command;
    std::vector<std::string> params;

    for (int i = 1; i < argc; ++i) {
      std::string arg = argv[i];

      if (arg == "--help" || arg == "-h") {
        PrintUsage(argv[0]);
        return 0;
      } else if (arg == "--version" || arg == "-v") {
        std::cout << unicity::GetFullVersionString() << std::endl;
        std::cout << unicity::GetCopyrightString() << std::endl;
        return 0;
      } else if (arg.find("--datadir=") == 0) {
        datadir = arg.substr(10);
      } else if (command.empty()) {
        command = arg;
      } else {
        params.push_back(arg);
      }
    }

    if (command.empty()) {
      std::cerr << "Error: No command specified\n";
      PrintUsage(argv[0]);
      return 1;
    }

    // Connect to node via Unix socket
    // NOTE: RPC uses Unix domain sockets for security (local-only access)
    // There is no network RPC port - all commands must be run locally
    std::string socket_path = datadir + "/node.sock";
    unicity::rpc::RPCClient client(socket_path);

    if (!client.Connect()) {
      std::cerr << "Error: Cannot connect to node at " << socket_path << "\n"
                << "Make sure the node is running.\n";
      return 1;
    }

    // Execute command
    std::string response = client.ExecuteCommand(command, params);

    // Print response
    std::cout << response;

    return 0;

  } catch (const std::exception &e) {
    std::cerr << "Error: " << e.what() << std::endl;
    return 1;
  }
}
