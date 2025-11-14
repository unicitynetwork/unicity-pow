#pragma once

#include "network/transport.hpp"
#include <atomic>
#include <boost/asio.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/strand.hpp>
#include <chrono>
#include <memory>
#include <queue>
#include <thread>
#include <vector>

namespace unicity {
namespace network {

/**
 * RealTransportConnection - TCP socket implementation of TransportConnection
 *
 * Wraps boost::asio::ip::tcp::socket and provides the abstract interface.
 */
class RealTransportConnection
    : public TransportConnection,
      public std::enable_shared_from_this<RealTransportConnection> {
public:
  /**
   * Create outbound connection (will connect to remote)
   */
  static TransportConnectionPtr
  create_outbound(boost::asio::io_context &io_context,
                  const std::string &address, uint16_t port,
                  ConnectCallback callback);

  /**
   * Create inbound connection (already connected socket)
   */
  static TransportConnectionPtr
  create_inbound(boost::asio::io_context &io_context,
                 boost::asio::ip::tcp::socket socket);

  ~RealTransportConnection() override;

  // Non-copyable, non-movable (connections are not reusable)
  RealTransportConnection(const RealTransportConnection&) = delete;
  RealTransportConnection& operator=(const RealTransportConnection&) = delete;
  RealTransportConnection(RealTransportConnection&&) = delete;
  RealTransportConnection& operator=(RealTransportConnection&&) = delete;

  // TransportConnection interface
  void start() override;
  bool send(const std::vector<uint8_t> &data) override;
  void close() override;
  bool is_open() const override;
  std::string remote_address() const override;
  uint16_t remote_port() const override;
  bool is_inbound() const override { return is_inbound_; }
  uint64_t connection_id() const override { return id_; }
  void set_receive_callback(ReceiveCallback callback) override;
  void set_disconnect_callback(DisconnectCallback callback) override;

#ifdef UNICITY_TESTS
  // Test-only: override connect timeout (0ms disables override)
  static void SetConnectTimeoutForTest(std::chrono::milliseconds timeout_ms);
  static void ResetConnectTimeoutForTest();

  // Test-only: override send queue byte limit (0 disables override)
  static void SetSendQueueLimitForTest(size_t bytes);
  static void ResetSendQueueLimitForTest();
#endif

private:
  RealTransportConnection(boost::asio::io_context &io_context, bool is_inbound);

  void do_connect(const std::string &address, uint16_t port,
                  ConnectCallback callback);

  // Strand-serialized internals (must be called on strand_)
  void start_read_impl();
  void do_write_impl();
  void close_impl();

  // Helper to deliver disconnect callback exactly once (must be called on strand)
  void deliver_disconnect_once();

  // Compute connect timeout (override if set, else default)
  std::chrono::milliseconds connect_timeout_ms() const;

  boost::asio::io_context &io_context_;
  boost::asio::ip::tcp::socket socket_;
  boost::asio::strand<boost::asio::any_io_executor> strand_;
  bool is_inbound_;
  uint64_t id_;
  static std::atomic<uint64_t> next_id_;

  // Callbacks (accessed only on strand_)
  ReceiveCallback receive_callback_;
  DisconnectCallback disconnect_callback_;
  // Flag to ensure disconnect callback is only delivered once
  bool disconnect_delivered_{false};

  // Send queue (accessed only on strand_)
  std::queue<std::shared_ptr<std::vector<uint8_t>>> send_queue_;
  size_t send_queue_bytes_ = 0;  // Total bytes in send queue (for DoS protection)
  // NOTE: writing_ is atomic because send() may check it from a non-strand thread
  // before dispatching the lambda onto the strand. By the time the lambda runs,
  // writing_ could have changed. Using atomic prevents data races.
  std::atomic<bool> writing_{false};

  // Receive buffer size for per-read allocations.
  static constexpr size_t RECV_BUFFER_SIZE = 256 * 1024; // 256 KB

  // Connect timeout and state
  // NOTE: connect_timer_ is a unique_ptr so that its destructor can be explicitly
  // controlled. When the object is destroyed inside the io_context thread (common
  // during shutdown), the timer destructor must not try to access scheduler state
  // that may have been destroyed. We move it to a local variable in close_impl()
  // so it gets destroyed after the io_context has been stopped.
  std::unique_ptr<boost::asio::steady_timer> connect_timer_;
  // CRITICAL: connect_done_ must be atomic because it's checked from multiple
  // async handlers (timeout, resolve, connect) which may execute concurrently
  // on different io_context threads.
  std::atomic<bool> connect_done_{false};
  std::shared_ptr<boost::asio::ip::tcp::resolver> resolver_;
  static constexpr std::chrono::milliseconds DEFAULT_CONNECT_TIMEOUT{std::chrono::seconds(10)};

#ifdef UNICITY_TESTS
  static std::atomic<std::chrono::milliseconds> connect_timeout_override_ms_;
  // Test-only override for send queue limit (0 = disabled)
  static std::atomic<size_t> send_queue_limit_override_bytes_;
#endif

  // Connection state
  std::atomic<bool> open_{false};
  std::string remote_addr_;
  uint16_t remote_port_ = 0;
};

/**
 * RealTransport - boost::asio implementation of Transport
 *
 * Manages io_context and provides connection factory methods.
 * Networking reactor is expected to run single-threaded by default.
 */
class RealTransport : public Transport, public std::enable_shared_from_this<RealTransport> {
public:
  /**
   * Create transport with specified number of IO threads
   */
  explicit RealTransport(size_t io_threads = 1);
  ~RealTransport() override;

  // Transport interface
  TransportConnectionPtr connect(const std::string &address, uint16_t port,
                                 ConnectCallback callback) override;

  bool
  listen(uint16_t port,
         std::function<void(TransportConnectionPtr)> accept_callback) override;

  void stop_listening() override;
  void run() override;
  void stop() override;
  bool is_running() const override { return running_; }

  // Access to io_context (for timers, etc.)
  boost::asio::io_context &io_context() { return *io_context_; }

  // Test/diagnostic: return bound listening port (0 if not listening)
  uint16_t listening_port() const;

private:
  void start_accept();
  void handle_accept(const boost::system::error_code &ec,
                     boost::asio::ip::tcp::socket socket);

  // NOTE: io_context_ is a unique_ptr so its destructor can be explicitly
  // controlled in stop(). When io_context_.stop() is called, pending handlers
  // are dequeued and executed. We must not destroy the io_context object
  // while handlers are still in the queue, or they may access destroyed state.
  // By using unique_ptr and resetting it after joining threads, we guarantee
  // all handlers complete before io_context is destroyed.
  std::unique_ptr<boost::asio::io_context> io_context_;
  std::unique_ptr<
      boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>
      work_guard_;
  std::vector<std::thread> io_threads_;
  std::atomic<bool> running_{false};
  size_t desired_io_threads_{1};

  // Acceptor for inbound connections
  std::unique_ptr<boost::asio::ip::tcp::acceptor> acceptor_;
  std::function<void(TransportConnectionPtr)> accept_callback_;
  uint16_t last_listen_port_{0};
};

} // namespace network
} // namespace unicity

