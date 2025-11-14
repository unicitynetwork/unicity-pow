#pragma once

#include "version.hpp"
#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace unicity {
namespace protocol {

// Protocol version - increment when P2P protocol changes
constexpr uint32_t PROTOCOL_VERSION = 1;

// Minimum supported protocol version
// Peers with version < MIN_PROTOCOL_VERSION will be rejected
constexpr uint32_t MIN_PROTOCOL_VERSION = 1;

// Network magic bytes - unique identifier for the network
// ASCII encoding: "UNIC" (Unicity) for mainnet
namespace magic {
constexpr uint32_t MAINNET = 0x554E4943; // "UNIC" - Unicity mainnet
constexpr uint32_t TESTNET = 0xA3F8D412; // High bit separation from mainnet
constexpr uint32_t REGTEST = 0x4B7C2E91; // High bit separation from mainnet/testnet
} // namespace magic

 
namespace ports {
constexpr uint16_t MAINNET = 9590;
constexpr uint16_t TESTNET = 19590; // MAINNET + 10000
constexpr uint16_t REGTEST = 29590; // MAINNET + 20000
} // namespace ports

// Service flags - what services this node provides
// NODE_NETWORK means we can serve headers which are the full blocks in a headers-only chain
enum ServiceFlags : uint64_t {
  NODE_NONE = 0,
  NODE_NETWORK = (1 << 0), // Can serve block headers (headers-only network)
};

// Message types - 12 bytes, null-padded
// Headers-only chain: no transactions, compact blocks, bloom filters, or mempool
namespace commands {
// Handshake
constexpr const char *VERSION = "version";
constexpr const char *VERACK = "verack";

// Peer discovery
constexpr const char *ADDR = "addr";
constexpr const char *GETADDR = "getaddr";

// Block announcements and requests
constexpr const char *INV = "inv";
constexpr const char *GETHEADERS = "getheaders";
constexpr const char *HEADERS = "headers";

// Keep-alive
constexpr const char *PING = "ping";
constexpr const char *PONG = "pong";
} // namespace commands

// Inventory types for INV/GETDATA messages
// Headers-only chain: only MSG_BLOCK is needed (for block announcements)
enum class InventoryType : uint32_t {
  ERROR = 0,
  MSG_BLOCK = 2, // Used for block hash announcements (triggers GETHEADERS)
};

// Message header constants
constexpr size_t MESSAGE_HEADER_SIZE = 24;
constexpr size_t COMMAND_SIZE = 12;
constexpr size_t CHECKSUM_SIZE = 4;

// ============================================================================
// SECURITY LIMITS (from Bitcoin Core )
// ============================================================================

// Serialization limits (Bitcoin Core src/serialize.h)
constexpr uint64_t MAX_SIZE =  0x02000000; // 32 MB - Maximum serialized object size
constexpr size_t MAX_VECTOR_ALLOCATE = 5 * 1000 * 1000; // 5 MB - Incremental allocation limit

// Network message limits (Bitcoin Core src/net.h)
constexpr size_t MAX_PROTOCOL_MESSAGE_LENGTH = 4 * 1000 * 1000; // 4 MB - Single message limit
constexpr size_t DEFAULT_MAX_RECEIVE_BUFFER = 5 * 1000; // 5 KB per peer (unused - we use RECV_FLOOD_SIZE)
constexpr size_t DEFAULT_SEND_QUEUE_SIZE = 5 * 1000 * 1000; // 5 MB - Send queue limit per peer (enforced)
constexpr size_t DEFAULT_RECV_FLOOD_SIZE = 5 * 1000 * 1000; // 5 MB - Flood protection (enforced)

// Protocol-specific limits
constexpr unsigned int MAX_LOCATOR_SZ = 101;  // GETHEADERS/GETBLOCKS locator limit
constexpr uint32_t MAX_INV_SIZE = 50000; // Inventory items
constexpr uint32_t MAX_HEADERS_SIZE = 2000; // Headers per response
constexpr uint32_t MAX_ADDR_SIZE = 1000;    // Addresses per ADDR message

// Orphan header management limits (DoS protection)
constexpr size_t MAX_ORPHAN_HEADERS = 1000;         // Total orphans across all peers
constexpr size_t MAX_ORPHAN_HEADERS_PER_PEER = 50;  // Max orphans per peer
constexpr int64_t ORPHAN_HEADER_EXPIRE_TIME = 600;  // 10 minutes in seconds

// Connection limits
constexpr unsigned int DEFAULT_MAX_OUTBOUND_CONNECTIONS = 8;
constexpr unsigned int DEFAULT_MAX_INBOUND_CONNECTIONS = 125;

// Timeouts and intervals (in seconds)
constexpr int VERSION_HANDSHAKE_TIMEOUT_SEC = 60; // 1 minute for handshake
constexpr int PING_INTERVAL_SEC = 120;            // 2 minutes between pings
constexpr int PING_TIMEOUT_SEC = 20 * 60;         // 20 minutes - peer must respond to ping
constexpr int INACTIVITY_TIMEOUT_SEC = 20 * 60;   // 20 minutes

// Block relay timing (Bitcoin Core behavior)
constexpr int64_t MAX_BLOCK_RELAY_AGE = 10; // Only relay blocks received in last 10 seconds

// RPC/Mining statistics constants
constexpr int DEFAULT_HASHRATE_CALCULATION_BLOCKS = 4; // ~4 hours at 1-hour blocks

// Network address constants
constexpr size_t MAX_SUBVERSION_LENGTH = 256;

// User agent string (from version.hpp)
inline std::string GetUserAgent() { return unicity::GetUserAgent(); }

// Message header structure (24 bytes):
// magic (4 bytes), command (12 bytes null-padded), length (4 bytes), checksum
// (4 bytes)
struct MessageHeader {
  uint32_t magic;
  std::array<char, COMMAND_SIZE> command;
  uint32_t length;
  std::array<uint8_t, CHECKSUM_SIZE> checksum;

  MessageHeader();
  MessageHeader(uint32_t magic, const std::string &cmd, uint32_t len);

  // Get command as string (strips null padding)
  std::string get_command() const;

  // Set command from string (adds null padding)
  void set_command(const std::string &cmd);
};

// Network address structure (30 bytes without timestamp, 34 with)
struct NetworkAddress {
  uint64_t services;
  std::array<uint8_t, 16> ip; // IPv6 format (IPv4 mapped)
  uint16_t port;                // Host byte order (native endianness)

  NetworkAddress();
  NetworkAddress(uint64_t svcs, const std::array<uint8_t, 16> &addr,
                 uint16_t p);

  // Helper to create from IPv4
  static NetworkAddress from_ipv4(uint64_t services, uint32_t ipv4,
                                  uint16_t port);

  // Helper to create from IP string (supports both IPv4 and IPv6)
  static NetworkAddress from_string(const std::string &ip_str, uint16_t port,
                                    uint64_t services = NODE_NETWORK);

  // Helper to get IPv4 (returns 0 if not IPv4-mapped)
  uint32_t get_ipv4() const;

  // Check if this is IPv4-mapped
  bool is_ipv4() const;

  // Comparison operators (needed for std::set in timedata)
  bool operator<(const NetworkAddress& other) const;
  bool operator==(const NetworkAddress& other) const;
};

// Convert NetworkAddress to IP string (IPv4 or IPv6)
// Returns std::nullopt if conversion fails
std::optional<std::string> NetworkAddressToString(const NetworkAddress& addr);

// Timestamped network address (34 bytes)
struct TimestampedAddress {
  uint32_t timestamp;
  NetworkAddress address;

  TimestampedAddress();
  TimestampedAddress(uint32_t ts, const NetworkAddress &addr);
};

// Inventory vector - identifies a transaction or block
struct InventoryVector {
  InventoryType type;
  std::array<uint8_t, 32> hash; // SHA256 hash

  InventoryVector();
  InventoryVector(InventoryType t, const std::array<uint8_t, 32> &h);
};

} // namespace protocol
} // namespace unicity


