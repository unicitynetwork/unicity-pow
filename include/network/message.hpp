#pragma once

#include "network/protocol.hpp"
#include "chain/block.hpp"
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace unicity {
namespace message {

// Forward declarations
class MessageSerializer;
class MessageDeserializer;

/**
 * VarInt - Variable length integer encoding
 */
class VarInt {
public:
  uint64_t value;

  VarInt() : value(0) {}
  explicit VarInt(uint64_t v) : value(v) {}

  // Get encoded size in bytes
  size_t encoded_size() const;

  // Encode to buffer
  size_t encode(uint8_t *buffer) const;

  // Decode from buffer, returns bytes consumed
  size_t decode(const uint8_t *buffer, size_t available);
};

/**
 * Serialization buffer for building wire-format messages
 */
class MessageSerializer {
public:
  MessageSerializer();

  // Write primitives
  void write_uint8(uint8_t value);
  void write_uint16(uint16_t value);
  void write_uint32(uint32_t value);
  void write_uint64(uint64_t value);
  void write_int32(int32_t value);
  void write_int64(int64_t value);
  void write_bool(bool value);

  // Write variable-length
  void write_varint(uint64_t value);
  void write_string(const std::string &str);
  void write_bytes(const uint8_t *data, size_t len);
  void write_bytes(const std::vector<uint8_t> &data);

  // Write protocol structures
  void write_network_address(const protocol::NetworkAddress &addr, bool include_timestamp = false);
  void write_inventory_vector(const protocol::InventoryVector &inv);

  // Get serialized data
  const std::vector<uint8_t> &data() const { return buffer_; }
  size_t size() const { return buffer_.size(); }

  // Clear buffer
  void clear() { buffer_.clear(); }

private:
  std::vector<uint8_t> buffer_;
};

/**
 * Deserialization buffer for parsing wire-format messages
 */
class MessageDeserializer {
public:
  MessageDeserializer(const uint8_t *data, size_t size);
  explicit MessageDeserializer(const std::vector<uint8_t> &data);

  // Read primitives
  uint8_t read_uint8();
  uint16_t read_uint16();
  uint32_t read_uint32();
  uint64_t read_uint64();
  int32_t read_int32();
  int64_t read_int64();
  bool read_bool();

  // Read variable-length
  uint64_t read_varint();
  std::string read_string(size_t max_length = SIZE_MAX);
  std::vector<uint8_t> read_bytes(size_t count);

  // Read protocol structures
  protocol::NetworkAddress read_network_address(bool has_timestamp = false);
  protocol::TimestampedAddress read_timestamped_address();
  protocol::InventoryVector read_inventory_vector();

  // State
  size_t bytes_remaining() const { return size_ - position_; }
  size_t position() const { return position_; }
  bool has_error() const { return error_; }

private:
  const uint8_t *data_;
  size_t size_;
  size_t position_;
  bool error_;

  void check_available(size_t bytes);
};

/**
 * Base class for all message payloads
 */
class Message {
public:
  virtual ~Message() = default;

  // Get command name for this message type
  virtual std::string command() const = 0;

  // Serialize message payload
  virtual std::vector<uint8_t> serialize() const = 0;

  // Deserialize message payload (returns true on success)
  virtual bool deserialize(const uint8_t *data, size_t size) = 0;
};

/**
 * VERSION message - First message sent to establish connection
 */


class VersionMessage : public Message {
public:
  int32_t version;
  uint64_t services;
  int64_t timestamp;
  protocol::NetworkAddress addr_recv;
  protocol::NetworkAddress addr_from;
  uint64_t nonce;
  std::string user_agent;
  int32_t start_height;

  VersionMessage();

  std::string command() const override { return protocol::commands::VERSION; }
  std::vector<uint8_t> serialize() const override;
  bool deserialize(const uint8_t *data, size_t size) override;
};

/**
 * VERACK message - Acknowledge version
 */
class VerackMessage : public Message {
public:
  VerackMessage() = default;

  std::string command() const override { return protocol::commands::VERACK; }
  std::vector<uint8_t> serialize() const override;
  bool deserialize(const uint8_t *data, size_t size) override;
};

/**
 * PING message - Keep-alive check
 */
class PingMessage : public Message {
public:
  uint64_t nonce;

  PingMessage() : nonce(0) {}
  explicit PingMessage(uint64_t n) : nonce(n) {}

  std::string command() const override { return protocol::commands::PING; }
  std::vector<uint8_t> serialize() const override;
  bool deserialize(const uint8_t *data, size_t size) override;
};

/**
 * PONG message - Response to ping
 */
class PongMessage : public Message {
public:
  uint64_t nonce;

  PongMessage() : nonce(0) {}
  explicit PongMessage(uint64_t n) : nonce(n) {}

  std::string command() const override { return protocol::commands::PONG; }
  std::vector<uint8_t> serialize() const override;
  bool deserialize(const uint8_t *data, size_t size) override;
};

/**
 * ADDR message - Share peer addresses
 */
class AddrMessage : public Message {
public:
  std::vector<protocol::TimestampedAddress> addresses;

  AddrMessage() = default;

  std::string command() const override { return protocol::commands::ADDR; }
  std::vector<uint8_t> serialize() const override;
  bool deserialize(const uint8_t *data, size_t size) override;
};

/**
 * GETADDR message - Request peer addresses
 */
class GetAddrMessage : public Message {
public:
  GetAddrMessage() = default;

  std::string command() const override { return protocol::commands::GETADDR; }
  std::vector<uint8_t> serialize() const override;
  bool deserialize(const uint8_t *data, size_t size) override;
};

/**
 * INV message - Announce inventory (blocks)
 */
class InvMessage : public Message {
public:
  std::vector<protocol::InventoryVector> inventory;

  InvMessage() = default;

  std::string command() const override { return protocol::commands::INV; }
  std::vector<uint8_t> serialize() const override;
  bool deserialize(const uint8_t *data, size_t size) override;
};


/**
 * GETHEADERS message - Request block headers
 */
class GetHeadersMessage : public Message {
public:
  uint32_t version;
  std::vector<std::array<uint8_t, 32>> block_locator_hashes;
  std::array<uint8_t, 32> hash_stop;

  GetHeadersMessage();

  std::string command() const override {
    return protocol::commands::GETHEADERS;
  }
  std::vector<uint8_t> serialize() const override;
  bool deserialize(const uint8_t *data, size_t size) override;
};

/**
 * HEADERS message - Receive block headers
 * This is the primary sync message for a headers-only blockchain
 */
class HeadersMessage : public Message {
public:
  std::vector<::CBlockHeader> headers;

  HeadersMessage() = default;

  std::string command() const override { return protocol::commands::HEADERS; }
  std::vector<uint8_t> serialize() const override;
  bool deserialize(const uint8_t *data, size_t size) override;
};

/**
 * Utility functions for message handling
 */

// Compute SHA-256 hash
std::array<uint8_t, 32> sha256(const uint8_t *data, size_t size);
std::array<uint8_t, 32> sha256(const std::vector<uint8_t> &data);

// Compute double SHA-256 (hash of hash)
std::array<uint8_t, 32> double_sha256(const uint8_t *data, size_t size);
std::array<uint8_t, 32> double_sha256(const std::vector<uint8_t> &data);

// Compute message checksum (first 4 bytes of double SHA-256)
std::array<uint8_t, 4> compute_checksum(const std::vector<uint8_t> &payload);

// Create message header with checksum
protocol::MessageHeader create_header(uint32_t magic,
                                      const std::string &command,
                                      const std::vector<uint8_t> &payload);

// Serialize header to bytes
std::vector<uint8_t> serialize_header(const protocol::MessageHeader &header);

// Deserialize header from bytes
bool deserialize_header(const uint8_t *data, size_t size,
                        protocol::MessageHeader &header);

// Factory function to create message from command name
std::unique_ptr<Message> create_message(const std::string &command);

} // namespace message
} // namespace unicity


