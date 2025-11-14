// Copyright (c) 2025 The Unicity Foundation
// Connection types implementation

#include "network/connection_types.hpp"

namespace unicity {
namespace network {

std::string ConnectionTypeAsString(ConnectionType conn_type) {
  switch (conn_type) {
  case ConnectionType::INBOUND:
    return "inbound";
  case ConnectionType::OUTBOUND:
    return "outbound";
  case ConnectionType::MANUAL:
    return "manual";
  case ConnectionType::FEELER:
    return "feeler";
  default:
    return "unknown";
  }
}

} // namespace network
} // namespace unicity
