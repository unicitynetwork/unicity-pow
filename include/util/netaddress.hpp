#pragma once

/*
 Network Address Utilities

 Purpose:
 - Validate and normalize IP address strings
 - Prevent invalid addresses from entering the system
 - Centralized address handling to avoid code duplication

 Key functions:
 - ValidateAndNormalizeIP: Validates address format and normalizes (IPv4-mapped -> IPv4)
 - IsValidIPAddress: Quick check if address string is valid
*/

#include <string>
#include <optional>

namespace unicity {
namespace util {

/**
 * Validate and normalize an IP address string
 *
 * This function wraps boost::asio::ip::make_address() with critical additions:
 * 1. Validates that the string is a valid IP address (IPv4 or IPv6)
 * 2. **Normalizes IPv4-mapped IPv6 addresses to IPv4 format (::ffff:1.2.3.4 -> 1.2.3.4)**
 * 3. Returns the canonical string representation
 *
 * Why not use boost directly?
 * - IPv4-mapped normalization is CRITICAL for security (prevents ban evasion, limit bypass)
 * - Without normalization: "192.168.1.1" and "::ffff:192.168.1.1" would be treated as different
 * - Used in 10+ locations: BanManager (ban storage/lookup), PeerRegistry (per-IP limits),
 *   RPC Server (API responses) - consistency is essential
 *
 * Security: Prevents malformed addresses from entering the system
 * - Rejects empty strings
 * - Rejects invalid IP formats
 * - Rejects hostnames (only numeric IPs accepted)
 * - Exception-safe wrapper
 *
 * @param address IP address string to validate and normalize
 * @return Normalized IP address string, or std::nullopt if invalid
 *
 * Examples:
 *   "192.168.1.1" -> "192.168.1.1"
 *   "::ffff:192.168.1.1" -> "192.168.1.1" (IPv4-mapped normalized - PREVENTS EVASION)
 *   "2001:db8::1" -> "2001:db8::1"
 *   "invalid" -> std::nullopt
 *   "" -> std::nullopt
 */
std::optional<std::string> ValidateAndNormalizeIP(const std::string& address);

/**
 * Check if a string is a valid IP address
 *
 * @param address String to validate
 * @return true if valid IP address, false otherwise
 */
bool IsValidIPAddress(const std::string& address);

/**
 * Parse "IP:port" string into separate IP and port components
 *
 * Supports both IPv4 and IPv6 formats:
 * - IPv4: "192.168.1.1:9590"
 * - IPv6: "[2001:db8::1]:9590"
 *
 * @param address_port String in "IP:port" or "[IPv6]:port" format
 * @param out_ip Output parameter for IP address string
 * @param out_port Output parameter for port number
 * @return true if successfully parsed, false otherwise
 */
bool ParseIPPort(const std::string& address_port, std::string& out_ip, uint16_t& out_port);

} // namespace util
} // namespace unicity
