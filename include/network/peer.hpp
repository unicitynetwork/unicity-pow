#pragma once

#include "network/connection_types.hpp"
#include "network/message.hpp"
#include "network/protocol.hpp"
#include "network/transport.hpp"
#include <atomic>
#include <boost/asio.hpp>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>

namespace unicity {
namespace network {

// Forward declarations
class Peer;
using PeerPtr = std::shared_ptr<Peer>;

// Peer connection states
enum class PeerConnectionState {
  DISCONNECTED,    // Not connected
  CONNECTING,      // TCP connection in progress
  CONNECTED,       // TCP connected, handshake not started
  VERSION_SENT,    // Sent VERSION message
  READY,           // Received VERACK, fully connected and ready
  DISCONNECTING    // Shutting down
};

// Peer connection statistics
// All fields are atomic to prevent data races between timer callbacks
// and send/receive operations that may run on different threads
// Pattern matches Bitcoin Core: atomic duration types for timestamps
struct PeerStats {
  std::atomic<uint64_t> bytes_sent{0};
  std::atomic<uint64_t> bytes_received{0};
  std::atomic<uint64_t> messages_sent{0};
  std::atomic<uint64_t> messages_received{0};
  std::atomic<std::chrono::seconds> connected_time{std::chrono::seconds{0}};
  std::atomic<std::chrono::seconds> last_send{std::chrono::seconds{0}};
  std::atomic<std::chrono::seconds> last_recv{std::chrono::seconds{0}};
  std::atomic<std::chrono::milliseconds> ping_time_ms{std::chrono::milliseconds{-1}};  // -1 means not measured yet
};

// Message handler callback type (returns true if message handled successfully)
using MessageHandler =
    std::function<bool(PeerPtr peer, std::unique_ptr<message::Message> msg)>;

// Peer class - Represents a single peer connection
// Handles async TCP connection, protocol handshake (VERSION/VERACK),
// message framing/parsing, send/receive queuing, ping/pong keepalive, lifecycle
// management
//
// IMPORTANT: Peer is single-use. start() may be called exactly once for the
// lifetime of a Peer instance. After disconnect(), a Peer must NOT be restarted;
// higher layers should create a new Peer instance for any subsequent connection.
//
// NOTE: assumes networking reactor is single-threaded (no strand/locks inside Peer)
//       NetworkManager must run with Config::io_threads = 1.
class Peer : public std::enable_shared_from_this<Peer> {
private:
  // Passkey idiom: allows make_shared while preventing direct construction
  struct PrivateTag {};

public:
  // Create outbound peer (we initiate connection)
  static PeerPtr create_outbound(boost::asio::io_context &io_context,
                                 TransportConnectionPtr connection,
                                 uint32_t network_magic,
                                 int32_t start_height,
                                 const std::string &target_address = "",
                                 uint16_t target_port = 0,
                                 ConnectionType conn_type = ConnectionType::OUTBOUND);

  // Create inbound peer (they connected to us)
  static PeerPtr create_inbound(boost::asio::io_context &io_context,
                                TransportConnectionPtr connection,
                                uint32_t network_magic,
                                int32_t start_height);

  ~Peer();

  // Disable copying
  Peer(const Peer &) = delete;
  Peer &operator=(const Peer &) = delete;

  // Start peer connection (outbound: initiates connection, inbound: starts
  // receiving messages)
  void start();

  void disconnect();
  void send_message(std::unique_ptr<message::Message> msg);
  void set_message_handler(MessageHandler handler);

#ifdef UNICITY_TESTS
  // Test-only: override timeouts to keep tests fast
  // Pass 0ms to clear an override (use defaults).
  static void SetTimeoutsForTest(std::chrono::milliseconds handshake_ms,
                                 std::chrono::milliseconds inactivity_ms);
  static void ResetTimeoutsForTest();
#endif

  // Setters (called by ConnectionManager)
  void set_id(int id) { id_ = id; }
  // Override the node-local handshake nonce (used for self-connection detection)
  void set_local_nonce(uint64_t nonce) { local_nonce_ = nonce; }

  // SECURITY: Process-wide nonce for self-connection detection
  // Set once at startup, shared by all peers for reliable self-connect detection
  static void set_process_nonce(uint64_t nonce) { process_nonce_ = nonce; }
  static uint64_t get_process_nonce() { return process_nonce_; }

  // Getters
  PeerConnectionState state() const { return state_; }
  bool is_connected() const {
    return state_ != PeerConnectionState::DISCONNECTED &&
           state_ != PeerConnectionState::DISCONNECTING;
  }
  bool successfully_connected() const {
    return successfully_connected_;
  } // Handshake complete
  const PeerStats &stats() const { return stats_; }
  std::string address() const;
  uint16_t port() const;

  const std::string& target_address() const { return target_address_; }
  uint16_t target_port() const { return target_port_; }
  uint64_t get_local_nonce() const { return local_nonce_; }

  bool is_inbound() const { return is_inbound_; }
  ConnectionType connection_type() const { return connection_type_; }
  bool is_feeler() const { return connection_type_ == ConnectionType::FEELER; }
  bool is_manual() const { return connection_type_ == ConnectionType::MANUAL; }
  int id() const { return id_; }

  // Peer information from VERSION message
  int32_t version() const { return peer_version_; }
  uint64_t services() const { return peer_services_; }
  int32_t start_height() const { return peer_start_height_; }
  const std::string &user_agent() const { return peer_user_agent_; }
  uint64_t peer_nonce() const { return peer_nonce_; }

  // Header sync state (Bitcoin Core: CNodeState::fSyncStarted)
  bool sync_started() const { return sync_started_; }
  void set_sync_started(bool started) { sync_started_ = started; }

  // Discovery state
  bool has_sent_getaddr() const { return getaddr_sent_; }
  void mark_getaddr_sent() { getaddr_sent_ = true; }

  // Public constructor for make_shared, but requires PrivateTag (passkey idiom)
  // DO NOT call directly - use create_outbound() or create_inbound() factory functions
  Peer(PrivateTag, boost::asio::io_context &io_context, TransportConnectionPtr connection,
       uint32_t network_magic, bool is_inbound,
       int32_t start_height,
       const std::string &target_address = "", uint16_t target_port = 0,
       ConnectionType conn_type = ConnectionType::OUTBOUND);

private:

  // Connection management
  void do_disconnect();  // Internal implementation (runs on io_context thread)
  void on_connected();
  void on_disconnect();
  void on_transport_receive(const std::vector<uint8_t> &data);
  void on_transport_disconnect();


  // Handshake
  void send_version();
  void handle_version(const ::unicity::message::VersionMessage &msg);
  void handle_verack();

  // Message I/O
  void process_received_data();  // Uses recv_buffer_ with offset pattern
  void process_message(const protocol::MessageHeader &header,
                       const std::vector<uint8_t> &payload);

  // Ping/Pong
  void schedule_ping();
  void send_ping();
  void handle_pong(const ::unicity::message::PongMessage &msg);

  // Timeouts
  void start_handshake_timeout();
  void start_inactivity_timeout();
  void cancel_all_timers();


  // SECURITY: Post disconnect to io_context to prevent use-after-free
  // If caller holds last shared_ptr and calls disconnect() synchronously,
  // the destructor runs while still in the call stack (re-entrancy bug).
  // Posting disconnect defers it until after current call finishes.
  void post_disconnect();

  // Member variables
  boost::asio::io_context &io_context_;
  TransportConnectionPtr connection_;
  boost::asio::steady_timer handshake_timer_;
  boost::asio::steady_timer ping_timer_;
  boost::asio::steady_timer inactivity_timer_;

  uint32_t network_magic_;
  bool is_inbound_;
  ConnectionType connection_type_;  // Connection type (INBOUND, OUTBOUND_FULL_RELAY, FEELER, etc.)
  int id_;  // Set by ConnectionManager when peer is added

  // Self-connection prevention
  uint64_t local_nonce_; // Our node's nonce
  int32_t local_start_height_; // Our blockchain height at connection time

  // Stored peer address 
  // For outbound: target address we're connecting to (passed to create_outbound)
  // For inbound: runtime address from accepted socket (set in create_inbound)
  // Used for duplicate prevention and peer lookup (see ConnectionManager::find_peer_by_address)
  std::string target_address_;
  uint16_t target_port_{0};

  PeerConnectionState state_;
  PeerStats stats_;
  MessageHandler message_handler_;
  bool successfully_connected_{false}; // Set to true after VERACK received
#ifdef UNICITY_TESTS
  // Millisecond-precision last activity for test-only inactivity override path
  std::atomic<std::chrono::milliseconds> last_activity_ms_{std::chrono::milliseconds{0}};
#endif
  bool sync_started_{false};  // Bitcoin Core: CNodeState::fSyncStarted - whether we've started headers sync with this peer
  bool getaddr_sent_{false};  // Whether we've sent GETADDR to this peer (discovery)

  // Thread-safe guard for start(): ensures start() executes exactly once.
  // Peer objects are single-use; started_ is never reset after disconnect().
  // To reconnect, create a new Peer. This enforces lifecycle correctness.
  std::atomic<bool> started_{false};

  // Peer info from VERSION
  int32_t peer_version_ = 0;
  uint64_t peer_services_ = 0;
  int32_t peer_start_height_ = 0;
  std::string peer_user_agent_;
  uint64_t peer_nonce_ = 0; // Peer's nonce from their VERSION message

  // Receive buffer (accumulates data until complete message received)
  // Uses read offset pattern to avoid O(n²) erase-from-front
  std::vector<uint8_t> recv_buffer_;
  size_t recv_buffer_offset_ = 0;  // Read position in recv_buffer_

  // Ping tracking
  uint64_t last_ping_nonce_ = 0;
  std::chrono::steady_clock::time_point ping_sent_time_;

  // SECURITY: Rate limiting for unknown commands to prevent log spam DoS
  std::atomic<int> unknown_command_count_{0};
  std::chrono::steady_clock::time_point last_unknown_reset_{};

  // Process-wide nonce for self-connection detection (set once at startup)
  static std::atomic<uint64_t> process_nonce_;

#ifdef UNICITY_TESTS
  // Test-only timeout overrides (0ms = disabled)
  static std::atomic<std::chrono::milliseconds> handshake_timeout_override_ms_;
  static std::atomic<std::chrono::milliseconds> inactivity_timeout_override_ms_;
#endif

public:
};

} // namespace network
} // namespace unicity

