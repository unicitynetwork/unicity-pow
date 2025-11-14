#include "util/netaddress.hpp"
#include "util/string_parsing.hpp"
#include "util/logging.hpp"
#include <boost/asio/ip/address.hpp>

namespace unicity {
namespace util {

std::optional<std::string> ValidateAndNormalizeIP(const std::string& address) {
  // Reject empty strings
  if (address.empty()) {
    return std::nullopt;
  }

  try {
    // Parse the address
    boost::system::error_code ec;
    auto ip = boost::asio::ip::make_address(address, ec);

    // Reject invalid formats
    if (ec) {
      return std::nullopt;
    }

    // Normalize IPv4-mapped IPv6 addresses to IPv4 format
    // Example: ::ffff:192.168.1.1 -> 192.168.1.1
    if (ip.is_v6() && ip.to_v6().is_v4_mapped()) {
      auto v4 = boost::asio::ip::make_address_v4(boost::asio::ip::v4_mapped, ip.to_v6());
      return v4.to_string();
    }

    // Return canonical string representation
    return ip.to_string();

  } catch (const std::exception& e) {
    // Catch any unexpected exceptions during parsing
    LOG_TRACE("ValidateAndNormalizeIP: exception parsing address '{}': {}", address, e.what());
    return std::nullopt;
  }
}

bool IsValidIPAddress(const std::string& address) {
  return ValidateAndNormalizeIP(address).has_value();
}

// Helper function to parse port string (used internally by ParseIPPort)
static bool ParsePortString(const std::string& port_str, uint16_t& out_port) {
  // Use SafeParsePort for proper validation (rejects trailing characters, whitespace, etc.)
  auto port_opt = SafeParsePort(port_str);
  if (!port_opt) {
    return false;
  }
  out_port = *port_opt;
  return true;
}

bool ParseIPPort(const std::string& address_port, std::string& out_ip, uint16_t& out_port) {
  if (address_port.empty()) {
    return false;
  }

  // Check for IPv6 format: "[IPv6]:port"
  if (address_port[0] == '[') {
    size_t bracket_end = address_port.find(']');
    if (bracket_end == std::string::npos) {
      return false; // Missing closing bracket
    }

    // Extract IPv6 address without brackets
    if (bracket_end < 2) {
      return false; // Empty brackets
    }
    out_ip = address_port.substr(1, bracket_end - 1);

    // Check for port after bracket
    if (bracket_end + 1 >= address_port.length() || address_port[bracket_end + 1] != ':') {
      return false; // Missing :port
    }

    std::string port_str = address_port.substr(bracket_end + 2);

    // Parse port using helper function
    if (!ParsePortString(port_str, out_port)) {
      return false;
    }

    // Validate and normalize IPv6 address
    auto normalized = ValidateAndNormalizeIP(out_ip);
    if (!normalized.has_value()) {
      return false;
    }
    out_ip = *normalized;
    return true;
  }

  // IPv4 format: "IP:port"
  // IMPORTANT: Check for multiple colons to detect unbracketed IPv6
  size_t first_colon = address_port.find(':');
  if (first_colon == std::string::npos) {
    return false; // Missing port
  }

  size_t second_colon = address_port.find(':', first_colon + 1);
  if (second_colon != std::string::npos) {
    // Multiple colons found - this is IPv6 without brackets, which is invalid
    return false;
  }

  out_ip = address_port.substr(0, first_colon);
  std::string port_str = address_port.substr(first_colon + 1);

  if (out_ip.empty()) {
    return false;
  }

  // Parse port using helper function
  if (!ParsePortString(port_str, out_port)) {
    return false;
  }

  // Validate and normalize IPv4 address
  auto normalized = ValidateAndNormalizeIP(out_ip);
  if (!normalized.has_value()) {
    return false;
  }
  out_ip = *normalized;
  return true;
}

} // namespace util
} // namespace unicity
