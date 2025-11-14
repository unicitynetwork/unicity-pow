// Copyright (c) 2025 The Unicity Foundation
// Connection types for peer-to-peer network connections

#pragma once

#include <string>

namespace unicity {
namespace network {

/**
 * Different types of connections to a peer.
 * This enum encapsulates the information we have available at the time of
 * opening or accepting the connection. Aside from INBOUND, all types are
 * initiated by us.
 *
 * Based on Bitcoin Core's connection_types.h
 */
enum class ConnectionType {
  /**
   * Inbound connections are those initiated by a peer. This is the only
   * property we know at the time of connection, until P2P messages are
   * exchanged.
   */
  INBOUND,

  /**
   * These are the default connections that we use to connect with the
   * network. We relay headers and addresses. We automatically attempt to open
   * MAX_OUTBOUND_CONNECTIONS using addresses from our AddrMan.
   */
  OUTBOUND,

  /**
   * We open manual connections to addresses that users explicitly requested
   * via RPC or configuration options. Even if a manual connection is
   * misbehaving, we do not automatically disconnect or add it to our
   * discouragement filter.
   */
  MANUAL,

  /**
   * Feeler connections are short-lived connections made to check that a node
   * is alive. They can be useful for:
   * - test-before-evict: if one of the peers is considered for eviction from
   *   our AddrMan because another peer is mapped to the same slot in the
   *   tried table, evict only if this longer-known peer is offline.
   * - move node addresses from New to Tried table, so that we have more
   *   connectable addresses in our AddrMan.
   *
   * We make these connections approximately every FEELER_INTERVAL (2 minutes).
   * After the VERSION/VERACK handshake completes, we immediately disconnect.
   */
  FEELER,
};

/**
 * Convert ConnectionType enum to a string value
 */
std::string ConnectionTypeAsString(ConnectionType conn_type);

} // namespace network
} // namespace unicity


