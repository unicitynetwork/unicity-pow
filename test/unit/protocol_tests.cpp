// Unit tests for protocol structures and serialization
#include "catch_amalgamated.hpp"
#include "network/protocol.hpp"
#include "chain/validation.hpp"
#include <cstring>

using namespace unicity::protocol;

TEST_CASE("MessageHeader - Command parsing", "[network][protocol]") {
    SECTION("Empty command") {
        MessageHeader header(magic::MAINNET, "", 100);
        CHECK(header.magic == magic::MAINNET);
        CHECK(header.length == 100);
        CHECK(header.get_command() == "");
    }

    SECTION("Short command") {
        MessageHeader header(magic::MAINNET, "ping", 0);
        CHECK(header.get_command() == "ping");
    }

    SECTION("Maximum length command (12 bytes)") {
        std::string long_cmd = "123456789012"; // Exactly 12 bytes
        MessageHeader header(magic::MAINNET, long_cmd, 0);
        CHECK(header.get_command() == long_cmd);
    }

    SECTION("Command too long (truncated to 12 bytes)") {
        std::string too_long = "1234567890123456"; // 16 bytes
        MessageHeader header(magic::MAINNET, too_long, 0);
        std::string result = header.get_command();
        CHECK(result.length() == COMMAND_SIZE);
        CHECK(result == "123456789012");
    }

    SECTION("Command with null padding") {
        MessageHeader header;
        header.set_command("verack");
        CHECK(header.get_command() == "verack");
        // Verify null padding
        CHECK(header.command[6] == '\0');
        CHECK(header.command[11] == '\0');
    }

    SECTION("set_command replaces previous value") {
        MessageHeader header;
        header.set_command("getaddr");
        CHECK(header.get_command() == "getaddr");

        header.set_command("ping");
        CHECK(header.get_command() == "ping");
        CHECK(header.command[4] == '\0'); // Null padded after "ping"
    }

    SECTION("Default constructor initializes to zero") {
        MessageHeader header;
        CHECK(header.magic == 0);
        CHECK(header.length == 0);
        CHECK(header.get_command() == "");
        CHECK(header.checksum[0] == 0);
        CHECK(header.checksum[3] == 0);
    }
}

TEST_CASE("MessageHeader - Protocol constants", "[network][protocol]") {
    SECTION("Magic bytes are distinct") {
        CHECK(magic::MAINNET != magic::TESTNET);
        CHECK(magic::MAINNET != magic::REGTEST);
        CHECK(magic::TESTNET != magic::REGTEST);
    }

    SECTION("Ports are distinct and follow convention") {
        CHECK(ports::MAINNET == 9590);
        CHECK(ports::TESTNET == ports::MAINNET + 10000);
        CHECK(ports::REGTEST == ports::MAINNET + 20000);
    }

    SECTION("Message header size constants") {
        CHECK(MESSAGE_HEADER_SIZE == 24);
        CHECK(COMMAND_SIZE == 12);
        CHECK(CHECKSUM_SIZE == 4);
    }
}

TEST_CASE("NetworkAddress - IPv4 mapping", "[network][protocol]") {
    SECTION("Create from IPv4 address") {
        // 192.168.1.1 = 0xC0A80101
        uint32_t ipv4 = (192 << 24) | (168 << 16) | (1 << 8) | 1;
        NetworkAddress addr = NetworkAddress::from_ipv4(NODE_NETWORK, ipv4, 8333);

        CHECK(addr.services == NODE_NETWORK);
        CHECK(addr.port == 8333);
        CHECK(addr.is_ipv4());
        CHECK(addr.get_ipv4() == ipv4);

        // Verify IPv4-mapped IPv6 format: ::ffff:192.168.1.1
        CHECK(addr.ip[10] == 0xff);
        CHECK(addr.ip[11] == 0xff);
        CHECK(addr.ip[12] == 192);
        CHECK(addr.ip[13] == 168);
        CHECK(addr.ip[14] == 1);
        CHECK(addr.ip[15] == 1);
    }

    SECTION("Loopback 127.0.0.1") {
        uint32_t loopback = (127 << 24) | 1; // 127.0.0.1
        NetworkAddress addr = NetworkAddress::from_ipv4(0, loopback, 9590);

        CHECK(addr.is_ipv4());
        CHECK(addr.get_ipv4() == loopback);
        CHECK(addr.ip[12] == 127);
        CHECK(addr.ip[13] == 0);
        CHECK(addr.ip[14] == 0);
        CHECK(addr.ip[15] == 1);
    }

    SECTION("Broadcast 255.255.255.255") {
        uint32_t broadcast = 0xFFFFFFFF;
        NetworkAddress addr = NetworkAddress::from_ipv4(0, broadcast, 8333);

        CHECK(addr.is_ipv4());
        CHECK(addr.get_ipv4() == broadcast);
        CHECK(addr.ip[12] == 255);
        CHECK(addr.ip[13] == 255);
        CHECK(addr.ip[14] == 255);
        CHECK(addr.ip[15] == 255);
    }

    SECTION("Zero address 0.0.0.0") {
        uint32_t zero = 0;
        NetworkAddress addr = NetworkAddress::from_ipv4(0, zero, 0);

        CHECK(addr.is_ipv4());
        CHECK(addr.get_ipv4() == 0);
    }
}

TEST_CASE("NetworkAddress - IPv6 detection", "[network][protocol]") {
    SECTION("Pure IPv6 is not IPv4-mapped") {
        NetworkAddress addr;
        addr.services = NODE_NETWORK;
        addr.port = 8333;

        // Set to 2001:db8::1 (documentation IPv6)
        addr.ip[0] = 0x20;
        addr.ip[1] = 0x01;
        addr.ip[2] = 0x0d;
        addr.ip[3] = 0xb8;
        // Rest zeros
        for (int i = 4; i < 15; i++) addr.ip[i] = 0;
        addr.ip[15] = 1;

        CHECK_FALSE(addr.is_ipv4());
        CHECK(addr.get_ipv4() == 0); // Returns 0 for non-IPv4
    }

    SECTION("Invalid IPv4-mapped (wrong prefix)") {
        NetworkAddress addr;
        addr.ip.fill(0);
        addr.ip[10] = 0xfe; // Should be 0xff
        addr.ip[11] = 0xff;
        addr.ip[12] = 192;
        addr.ip[13] = 168;
        addr.ip[14] = 1;
        addr.ip[15] = 1;

        CHECK_FALSE(addr.is_ipv4());
    }
}

TEST_CASE("NetworkAddress - Default constructor", "[network][protocol]") {
    NetworkAddress addr;
    CHECK(addr.services == 0);
    CHECK(addr.port == 0);
    CHECK(addr.ip[0] == 0);
    CHECK(addr.ip[15] == 0);
}

TEST_CASE("NetworkAddress - Parameterized constructor", "[network][protocol]") {
    std::array<uint8_t, 16> test_ip;
    test_ip.fill(0);
    test_ip[0] = 0x20;
    test_ip[1] = 0x01;

    NetworkAddress addr(NODE_NETWORK, test_ip, 9590);
    CHECK(addr.services == NODE_NETWORK);
    CHECK(addr.port == 9590);
    CHECK(addr.ip[0] == 0x20);
    CHECK(addr.ip[1] == 0x01);
}

TEST_CASE("TimestampedAddress - Construction", "[network][protocol]") {
    SECTION("Default constructor") {
        TimestampedAddress taddr;
        CHECK(taddr.timestamp == 0);
        CHECK(taddr.address.services == 0);
        CHECK(taddr.address.port == 0);
    }

    SECTION("Parameterized constructor") {
        NetworkAddress addr = NetworkAddress::from_ipv4(NODE_NETWORK, 0xC0A80101, 8333);
        TimestampedAddress taddr(1234567890, addr);

        CHECK(taddr.timestamp == 1234567890);
        CHECK(taddr.address.services == NODE_NETWORK);
        CHECK(taddr.address.port == 8333);
        CHECK(taddr.address.is_ipv4());
    }
}

TEST_CASE("InventoryVector - Construction", "[network][protocol]") {
    SECTION("Default constructor") {
        InventoryVector inv;
        CHECK(inv.type == InventoryType::ERROR);
        CHECK(inv.hash[0] == 0);
        CHECK(inv.hash[31] == 0);
    }

    SECTION("Parameterized constructor") {
        std::array<uint8_t, 32> test_hash;
        for (int i = 0; i < 32; i++) {
            test_hash[i] = static_cast<uint8_t>(i);
        }

        InventoryVector inv(InventoryType::MSG_BLOCK, test_hash);
        CHECK(inv.type == InventoryType::MSG_BLOCK);
        CHECK(inv.hash[0] == 0);
        CHECK(inv.hash[31] == 31);
    }
}

TEST_CASE("InventoryType - Enum values", "[network][protocol]") {
    SECTION("InventoryType values") {
        CHECK(static_cast<uint32_t>(InventoryType::ERROR) == 0);
        CHECK(static_cast<uint32_t>(InventoryType::MSG_BLOCK) == 2);
    }

    SECTION("InventoryType comparison") {
        InventoryType t1 = InventoryType::ERROR;
        InventoryType t2 = InventoryType::MSG_BLOCK;

        CHECK(t1 != t2);
        CHECK(t1 == InventoryType::ERROR);
        CHECK(t2 == InventoryType::MSG_BLOCK);
    }
}

TEST_CASE("ServiceFlags - Values", "[network][protocol]") {
    SECTION("Service flag values") {
        CHECK(NODE_NONE == 0);
        CHECK(NODE_NETWORK == 1);
    }

    SECTION("Service flags can be combined") {
        uint64_t combined = NODE_NETWORK | NODE_NONE;
        CHECK(combined == NODE_NETWORK);

        uint64_t flags = NODE_NETWORK;
        CHECK((flags & NODE_NETWORK) != 0);
        CHECK((flags & NODE_NONE) == 0);
    }
}

TEST_CASE("Protocol limits - Security constants", "[network][protocol]") {
    SECTION("Message size limits") {
        CHECK(MAX_SIZE == 0x02000000); // 32 MB
        CHECK(MAX_PROTOCOL_MESSAGE_LENGTH == 4 * 1000 * 1000); // 4 MB
        CHECK(DEFAULT_RECV_FLOOD_SIZE == 5 * 1000 * 1000); // 5 MB
    }

    SECTION("Protocol-specific limits") {
        CHECK(MAX_LOCATOR_SZ == 101);
        CHECK(MAX_INV_SIZE == 50000);
        CHECK(MAX_HEADERS_SIZE == 2000);
        CHECK(MAX_ADDR_SIZE == 1000);
    }

    SECTION("Timeouts are reasonable") {
        CHECK(VERSION_HANDSHAKE_TIMEOUT_SEC == 60);
        CHECK(PING_INTERVAL_SEC == 120);
        CHECK(PING_TIMEOUT_SEC == 20 * 60);
        CHECK(INACTIVITY_TIMEOUT_SEC == 20 * 60);
    }

    SECTION("Time validation") {
        CHECK(unicity::validation::MAX_FUTURE_BLOCK_TIME == 2 * 60 * 60); // 2 hours
    }
}

TEST_CASE("Protocol commands - String constants", "[network][protocol]") {
    SECTION("Command strings are valid") {
        CHECK(std::string(commands::VERSION) == "version");
        CHECK(std::string(commands::VERACK) == "verack");
        CHECK(std::string(commands::INV) == "inv");
        CHECK(std::string(commands::GETHEADERS) == "getheaders");
        CHECK(std::string(commands::HEADERS) == "headers");
// SENDHEADERS not supported in this implementation
        CHECK(std::string(commands::PING) == "ping");
        CHECK(std::string(commands::PONG) == "pong");
    }

    SECTION("Command strings fit in COMMAND_SIZE") {
        CHECK(std::strlen(commands::VERSION) <= COMMAND_SIZE);
        CHECK(std::strlen(commands::VERACK) <= COMMAND_SIZE);
        CHECK(std::strlen(commands::GETHEADERS) <= COMMAND_SIZE);
// SENDHEADERS not supported; no length check
    }
}
