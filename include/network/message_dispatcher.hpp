#ifndef UNICITY_NETWORK_MESSAGE_DISPATCHER_HPP
#define UNICITY_NETWORK_MESSAGE_DISPATCHER_HPP

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace unicity {

// Forward declaration
namespace message {
class Message;
} // namespace message

namespace network {

// Forward declarations
class Peer;
using PeerPtr = std::shared_ptr<Peer>;

/**
 * MessageDispatcher - Protocol message routing via handler registry
 *
 * Design:
 * - Managers register handlers for their message types
 * - Thread-safe registration and dispatch
 * - Extensible: new messages = new registration, no code changes
 *
 * Ownership Model:
 * - Handlers receive raw Message* pointer (borrowed, not owned)
 * - Message lifetime guaranteed only during handler execution
 * - Handlers MUST NOT store the pointer for async processing
 * - Handlers must complete synchronously
 *
 * Rationale:
 * - Current handlers use messages synchronously (no storage needed)
 * - Avoids shared_ptr overhead for common case
 * - Future: If async handlers needed, API will change to smart pointers
 *
 * Usage:
 *   MessageDispatcher dispatcher;
 *   dispatcher.RegisterHandler("verack",
 *     [this](PeerPtr p, message::Message* m) {
 *       return connection_mgr_->HandleVerack(p);
 *     });
 *   dispatcher.Dispatch(peer, "verack", msg);
 */
class MessageDispatcher {
public:
  // Handler signature: takes peer + message, returns success
  // WARNING: Message* is borrowed - do not store for async use
  using MessageHandler = std::function<bool(PeerPtr, ::unicity::message::Message*)>;

  MessageDispatcher() = default;
  ~MessageDispatcher() = default;

  // Non-copyable
  MessageDispatcher(const MessageDispatcher&) = delete;
  MessageDispatcher& operator=(const MessageDispatcher&) = delete;

  /**
   * Register handler for a message command
   * Thread-safe, can be called during initialization
   *
   * Example:
   *   dispatcher.RegisterHandler("verack",
   *     [this](PeerPtr p, message::Message* m) {
   *       return connection_mgr_->HandleVerack(p);
   *     });
   *
   * @param command Message command string (e.g., "verack", "inv")
   * @param handler Function to handle this message type (required, must not be empty)
   *
   * Note: Empty handlers are rejected to prevent std::bad_function_call
   */
  void RegisterHandler(const std::string& command, MessageHandler handler);

  /**
   * Unregister handler (for testing/cleanup)
   *
   * @param command Message command to unregister
   */
  void UnregisterHandler(const std::string& command);

  /**
   * Dispatch message to registered handler
   *
   * @param peer Peer that sent the message
   * @param command Message command string
   * @param msg Parsed message object (borrowed pointer, valid only during handler execution)
   * @return false if no handler found or handler returns false, true otherwise
   *
   * Note: msg pointer is only valid during synchronous handler execution.
   * Handlers must not store this pointer for later use.
   */
  bool Dispatch(PeerPtr peer, const std::string& command, ::unicity::message::Message* msg);

  /**
   * Check if handler exists for command
   *
   * @param command Message command to check
   * @return true if handler is registered
   */
  bool HasHandler(const std::string& command) const;

  /**
   * Get list of registered commands (for diagnostics)
   *
   * @return Sorted vector of registered command strings
   */
  std::vector<std::string> GetRegisteredCommands() const;

private:
  mutable std::mutex mutex_;
  std::unordered_map<std::string, MessageHandler> handlers_;
};

} // namespace network
} // namespace unicity

#endif // UNICITY_NETWORK_MESSAGE_DISPATCHER_HPP
