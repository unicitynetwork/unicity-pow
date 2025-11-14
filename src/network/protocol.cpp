#include "network/protocol.hpp"
#include <algorithm>
#include <cstring>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/address_v4.hpp>
#include <boost/asio/ip/address_v6.hpp>
#include <boost/asio/ip/v6_only.hpp>

namespace unicity {
namespace protocol {

// MessageHeader implementation
MessageHeader::MessageHeader() : magic(0), length(0) {
  command.fill(0);
  checksum.fill(0);
}

MessageHeader::MessageHeader(uint32_t magic, const std::string &cmd,
                             uint32_t len)
    : magic(magic), length(len) {
  set_command(cmd);
  checksum.fill(0); // Checksum set separately
}

std::string MessageHeader::get_command() const {
  // Find null terminator or end of array
  auto end = std::find(command.begin(), command.end(), '\0');
  return std::string(command.begin(), end);
}

void MessageHeader::set_command(const std::string &cmd) {
  command.fill(0); // Null-pad the entire array
  size_t copy_len = std::min(cmd.length(), COMMAND_SIZE);
  std::memcpy(command.data(), cmd.data(), copy_len);
}

// NetworkAddress implementation
NetworkAddress::NetworkAddress() : services(0), port(0) { ip.fill(0); }

NetworkAddress::NetworkAddress(uint64_t svcs,
                               const std::array<uint8_t, 16> &addr, uint16_t p)
    : services(svcs), ip(addr), port(p) {}

NetworkAddress NetworkAddress::from_ipv4(uint64_t services, uint32_t ipv4,
                                         uint16_t port) {
  NetworkAddress addr;
  addr.services = services;
  addr.port = port;

  // IPv4-mapped IPv6 address format: ::ffff:x.x.x.x
  addr.ip.fill(0);
  addr.ip[10] = 0xff;
  addr.ip[11] = 0xff;

  // Store IPv4 in big-endian (network byte order)
  addr.ip[12] = (ipv4 >> 24) & 0xff;
  addr.ip[13] = (ipv4 >> 16) & 0xff;
  addr.ip[14] = (ipv4 >> 8) & 0xff;
  addr.ip[15] = ipv4 & 0xff;

  return addr;
}

NetworkAddress NetworkAddress::from_string(const std::string &ip_str,
                                           uint16_t port,
                                           uint64_t services) {
  NetworkAddress addr;
  addr.services = services;
  addr.port = port;

  // Parse IP address
  boost::system::error_code ec;
  auto ip_addr = boost::asio::ip::make_address(ip_str, ec);

  if (ec) {
    // If parsing fails, return empty address
    addr.ip.fill(0);
    return addr;
  }

  // Convert to IPv6 format (IPv4 addresses are mapped to IPv6)
  if (ip_addr.is_v4()) {
    // Convert IPv4 to IPv4-mapped IPv6 (::ffff:x.x.x.x)
    auto v6_mapped = boost::asio::ip::make_address_v6(
        boost::asio::ip::v4_mapped, ip_addr.to_v4());
    auto bytes = v6_mapped.to_bytes();
    std::copy(bytes.begin(), bytes.end(), addr.ip.begin());
  } else {
    // Pure IPv6
    auto bytes = ip_addr.to_v6().to_bytes();
    std::copy(bytes.begin(), bytes.end(), addr.ip.begin());
  }

  return addr;
}

uint32_t NetworkAddress::get_ipv4() const {
  if (!is_ipv4()) {
    return 0;
  }

  // Extract IPv4 from the last 4 bytes (big-endian)
  return (static_cast<uint32_t>(ip[12]) << 24) |
         (static_cast<uint32_t>(ip[13]) << 16) |
         (static_cast<uint32_t>(ip[14]) << 8) | static_cast<uint32_t>(ip[15]);
}

bool NetworkAddress::is_ipv4() const {
  // Check for IPv4-mapped IPv6 prefix: ::ffff:x.x.x.x
  return ip[0] == 0 && ip[1] == 0 && ip[2] == 0 && ip[3] == 0 && ip[4] == 0 &&
         ip[5] == 0 && ip[6] == 0 && ip[7] == 0 && ip[8] == 0 && ip[9] == 0 &&
         ip[10] == 0xff && ip[11] == 0xff;
}

bool NetworkAddress::operator<(const NetworkAddress& other) const {
  // Lexicographic comparison for std::set
  // Compare IP first, then port, then services
  if (ip != other.ip) {
    return ip < other.ip;
  }
  if (port != other.port) {
    return port < other.port;
  }
  return services < other.services;
}

bool NetworkAddress::operator==(const NetworkAddress& other) const {
  return ip == other.ip && port == other.port && services == other.services;
}

// TimestampedAddress implementation
TimestampedAddress::TimestampedAddress() : timestamp(0) {}

TimestampedAddress::TimestampedAddress(uint32_t ts, const NetworkAddress &addr)
    : timestamp(ts), address(addr) {}

// InventoryVector implementation
InventoryVector::InventoryVector() : type(InventoryType::ERROR) {
  hash.fill(0);
}

InventoryVector::InventoryVector(InventoryType t,
                                 const std::array<uint8_t, 32> &h)
    : type(t), hash(h) {}

// Convert NetworkAddress to IP string (IPv4 or IPv6)
// Returns std::nullopt if conversion fails
std::optional<std::string> NetworkAddressToString(const NetworkAddress& addr) {
  try {
    // Convert 16-byte array to boost::asio IP address
    boost::asio::ip::address_v6::bytes_type bytes;
    std::copy(addr.ip.begin(), addr.ip.end(), bytes.begin());
    auto v6_addr = boost::asio::ip::make_address_v6(bytes);

    // Check if it's IPv4-mapped and convert if needed
    if (v6_addr.is_v4_mapped()) {
      return boost::asio::ip::make_address_v4(boost::asio::ip::v4_mapped, v6_addr)
                 .to_string();
    } else {
      return v6_addr.to_string();
    }
  } catch (const std::exception &) {
    return std::nullopt;
  }
}

} // namespace protocol
} // namespace unicity
