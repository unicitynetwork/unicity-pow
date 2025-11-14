// Copyright (c) 2025 The Unicity Foundation
// Unit tests for network/message.cpp - Message serialization/deserialization
//
// These tests verify:
// - VarInt encoding/decoding (all size ranges)
// - MessageSerializer primitive types
// - MessageDeserializer primitive types
// - Round-trip serialization
// - Error handling (buffer underflow, malformed data)
// - Network protocol structures

#include "catch_amalgamated.hpp"
#include "network/message.hpp"
#include "network/protocol.hpp"
#include <cstring>

using namespace unicity::message;
using namespace unicity::protocol;

TEST_CASE("VarInt - Encoding Size", "[network][message][varint][unit]") {
    SECTION("1-byte encoding (< 0xfd)") {
        REQUIRE(VarInt(0).encoded_size() == 1);
        REQUIRE(VarInt(0xfc).encoded_size() == 1);
    }

    SECTION("3-byte encoding (0xfd to 0xffff)") {
        REQUIRE(VarInt(0xfd).encoded_size() == 3);
        REQUIRE(VarInt(0xffff).encoded_size() == 3);
    }

    SECTION("5-byte encoding (0x10000 to 0xffffffff)") {
        REQUIRE(VarInt(0x10000).encoded_size() == 5);
        REQUIRE(VarInt(0xffffffff).encoded_size() == 5);
    }

    SECTION("9-byte encoding (> 0xffffffff)") {
        REQUIRE(VarInt(0x100000000ULL).encoded_size() == 9);
        REQUIRE(VarInt(0xffffffffffffffffULL).encoded_size() == 9);
    }
}

TEST_CASE("VarInt - Encode/Decode Round Trip", "[network][message][varint][unit]") {
    auto test_roundtrip = [](uint64_t value) {
        VarInt original(value);
        uint8_t buffer[9];
        size_t encoded_bytes = original.encode(buffer);

        REQUIRE(encoded_bytes == original.encoded_size());

        VarInt decoded;
        size_t decoded_bytes = decoded.decode(buffer, encoded_bytes);

        REQUIRE(decoded_bytes == encoded_bytes);
        REQUIRE(decoded.value == value);
    };

    SECTION("1-byte values") {
        test_roundtrip(0);
        test_roundtrip(1);
        test_roundtrip(0x7f);
        test_roundtrip(0xfc);
    }

    SECTION("3-byte values") {
        test_roundtrip(0xfd);
        test_roundtrip(0x100);
        test_roundtrip(0xffff);
    }

    SECTION("5-byte values") {
        test_roundtrip(0x10000);
        test_roundtrip(0x12345678);
        test_roundtrip(0xffffffff);
    }

    SECTION("9-byte values") {
        test_roundtrip(0x100000000ULL);
        test_roundtrip(0x123456789abcdefULL);
        test_roundtrip(0xffffffffffffffffULL);
    }
}

TEST_CASE("VarInt - Decode Error Handling", "[network][message][varint][unit]") {
    SECTION("Insufficient buffer for 1-byte") {
        uint8_t buffer[] = {0x42};
        VarInt vi;

        REQUIRE(vi.decode(buffer, 0) == 0);  // No data available
    }

    SECTION("Insufficient buffer for 3-byte") {
        uint8_t buffer[] = {0xfd, 0x00};  // Needs 3 bytes but only 2
        VarInt vi;

        REQUIRE(vi.decode(buffer, 2) == 0);
    }

    SECTION("Insufficient buffer for 5-byte") {
        uint8_t buffer[] = {0xfe, 0x00, 0x00, 0x00};  // Needs 5 bytes but only 4
        VarInt vi;

        REQUIRE(vi.decode(buffer, 4) == 0);
    }

    SECTION("Insufficient buffer for 9-byte") {
        uint8_t buffer[] = {0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};  // Needs 9 but only 8
        VarInt vi;

        REQUIRE(vi.decode(buffer, 8) == 0);
    }
}

TEST_CASE("MessageSerializer - Primitives", "[network][message][serializer][unit]") {
    MessageSerializer ser;

    SECTION("uint8") {
        ser.write_uint8(0x42);
        ser.write_uint8(0xff);

        auto data = ser.data();
        REQUIRE(data.size() == 2);
        REQUIRE(data[0] == 0x42);
        REQUIRE(data[1] == 0xff);
    }

    SECTION("uint16 (little-endian)") {
        ser.write_uint16(0x1234);

        auto data = ser.data();
        REQUIRE(data.size() == 2);
        REQUIRE(data[0] == 0x34);  // Little-endian
        REQUIRE(data[1] == 0x12);
    }

    SECTION("uint32 (little-endian)") {
        ser.write_uint32(0x12345678);

        auto data = ser.data();
        REQUIRE(data.size() == 4);
        REQUIRE(data[0] == 0x78);
        REQUIRE(data[1] == 0x56);
        REQUIRE(data[2] == 0x34);
        REQUIRE(data[3] == 0x12);
    }

    SECTION("uint64 (little-endian)") {
        ser.write_uint64(0x123456789abcdef0ULL);

        auto data = ser.data();
        REQUIRE(data.size() == 8);
        REQUIRE(data[0] == 0xf0);
        REQUIRE(data[1] == 0xde);
        REQUIRE(data[2] == 0xbc);
        REQUIRE(data[3] == 0x9a);
        REQUIRE(data[4] == 0x78);
        REQUIRE(data[5] == 0x56);
        REQUIRE(data[6] == 0x34);
        REQUIRE(data[7] == 0x12);
    }

    SECTION("int32") {
        ser.write_int32(-1);

        auto data = ser.data();
        REQUIRE(data.size() == 4);
        REQUIRE(data[0] == 0xff);
        REQUIRE(data[1] == 0xff);
        REQUIRE(data[2] == 0xff);
        REQUIRE(data[3] == 0xff);
    }

    SECTION("int64") {
        ser.write_int64(-1);

        auto data = ser.data();
        REQUIRE(data.size() == 8);
        for (int i = 0; i < 8; i++) {
            REQUIRE(data[i] == 0xff);
        }
    }

    SECTION("bool") {
        ser.write_bool(true);
        ser.write_bool(false);

        auto data = ser.data();
        REQUIRE(data.size() == 2);
        REQUIRE(data[0] == 1);
        REQUIRE(data[1] == 0);
    }
}

TEST_CASE("MessageSerializer - Variable Length", "[network][message][serializer][unit]") {
    MessageSerializer ser;

    SECTION("varint") {
        ser.write_varint(0);
        ser.write_varint(0xfc);
        ser.write_varint(0xfd);

        auto data = ser.data();
        REQUIRE(data.size() == 5);  // 1 + 1 + 3
        REQUIRE(data[0] == 0);
        REQUIRE(data[1] == 0xfc);
        REQUIRE(data[2] == 0xfd);  // Marker
        REQUIRE(data[3] == 0xfd);  // Low byte
        REQUIRE(data[4] == 0x00);  // High byte
    }

    SECTION("string") {
        ser.write_string("hello");

        auto data = ser.data();
        REQUIRE(data.size() == 6);  // 1 (varint length) + 5
        REQUIRE(data[0] == 5);  // Length
        REQUIRE(std::string((char*)&data[1], 5) == "hello");
    }

    SECTION("empty string") {
        ser.write_string("");

        auto data = ser.data();
        REQUIRE(data.size() == 1);
        REQUIRE(data[0] == 0);  // Zero length
    }

    SECTION("bytes from pointer") {
        uint8_t bytes[] = {0x01, 0x02, 0x03};
        ser.write_bytes(bytes, 3);

        auto data = ser.data();
        REQUIRE(data.size() == 3);
        REQUIRE(data[0] == 0x01);
        REQUIRE(data[1] == 0x02);
        REQUIRE(data[2] == 0x03);
    }

    SECTION("bytes from vector") {
        std::vector<uint8_t> bytes = {0xaa, 0xbb, 0xcc};
        ser.write_bytes(bytes);

        auto data = ser.data();
        REQUIRE(data.size() == 3);
        REQUIRE(data[0] == 0xaa);
        REQUIRE(data[1] == 0xbb);
        REQUIRE(data[2] == 0xcc);
    }
}

TEST_CASE("MessageSerializer - Clear", "[network][message][serializer][unit]") {
    MessageSerializer ser;

    ser.write_uint32(0x12345678);
    REQUIRE(ser.size() == 4);

    ser.clear();
    REQUIRE(ser.size() == 0);

    ser.write_uint8(0x42);
    REQUIRE(ser.size() == 1);
}

TEST_CASE("MessageDeserializer - Primitives", "[network][message][deserializer][unit]") {
    SECTION("uint8") {
        uint8_t data[] = {0x42, 0xff};
        MessageDeserializer des(data, 2);

        REQUIRE(des.read_uint8() == 0x42);
        REQUIRE(des.read_uint8() == 0xff);
        REQUIRE(des.bytes_remaining() == 0);
        REQUIRE_FALSE(des.has_error());
    }

    SECTION("uint16 (little-endian)") {
        uint8_t data[] = {0x34, 0x12};
        MessageDeserializer des(data, 2);

        REQUIRE(des.read_uint16() == 0x1234);
        REQUIRE_FALSE(des.has_error());
    }

    SECTION("uint32 (little-endian)") {
        uint8_t data[] = {0x78, 0x56, 0x34, 0x12};
        MessageDeserializer des(data, 4);

        REQUIRE(des.read_uint32() == 0x12345678);
        REQUIRE_FALSE(des.has_error());
    }

    SECTION("uint64 (little-endian)") {
        uint8_t data[] = {0xf0, 0xde, 0xbc, 0x9a, 0x78, 0x56, 0x34, 0x12};
        MessageDeserializer des(data, 8);

        REQUIRE(des.read_uint64() == 0x123456789abcdef0ULL);
        REQUIRE_FALSE(des.has_error());
    }

    SECTION("int32") {
        uint8_t data[] = {0xff, 0xff, 0xff, 0xff};
        MessageDeserializer des(data, 4);

        REQUIRE(des.read_int32() == -1);
        REQUIRE_FALSE(des.has_error());
    }

    SECTION("int64") {
        uint8_t data[] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
        MessageDeserializer des(data, 8);

        REQUIRE(des.read_int64() == -1);
        REQUIRE_FALSE(des.has_error());
    }

    SECTION("bool") {
        uint8_t data[] = {0x01, 0x00};
        MessageDeserializer des(data, 2);

        REQUIRE(des.read_bool() == true);
        REQUIRE(des.read_bool() == false);
        REQUIRE_FALSE(des.has_error());
    }
}

TEST_CASE("MessageDeserializer - Variable Length", "[network][message][deserializer][unit]") {
    SECTION("varint") {
        uint8_t data[] = {0x00, 0xfc, 0xfd, 0xfd, 0x00};
        MessageDeserializer des(data, 5);

        REQUIRE(des.read_varint() == 0);
        REQUIRE(des.read_varint() == 0xfc);
        REQUIRE(des.read_varint() == 0xfd);
        REQUIRE_FALSE(des.has_error());
    }

    SECTION("string") {
        uint8_t data[] = {0x05, 'h', 'e', 'l', 'l', 'o'};
        MessageDeserializer des(data, 6);

        REQUIRE(des.read_string() == "hello");
        REQUIRE_FALSE(des.has_error());
    }

    SECTION("empty string") {
        uint8_t data[] = {0x00};
        MessageDeserializer des(data, 1);

        REQUIRE(des.read_string() == "");
        REQUIRE_FALSE(des.has_error());
    }

    SECTION("bytes") {
        uint8_t data[] = {0x01, 0x02, 0x03};
        MessageDeserializer des(data, 3);

        auto bytes = des.read_bytes(3);
        REQUIRE(bytes.size() == 3);
        REQUIRE(bytes[0] == 0x01);
        REQUIRE(bytes[1] == 0x02);
        REQUIRE(bytes[2] == 0x03);
        REQUIRE_FALSE(des.has_error());
    }
}

TEST_CASE("MessageDeserializer - Error Handling", "[network][message][deserializer][unit]") {
    SECTION("uint8 buffer underflow") {
        uint8_t data[] = {0x42};
        MessageDeserializer des(data, 1);

        REQUIRE(des.read_uint8() == 0x42);
        REQUIRE_FALSE(des.has_error());

        // Try to read beyond buffer
        des.read_uint8();
        REQUIRE(des.has_error());
    }

    SECTION("uint32 buffer underflow") {
        uint8_t data[] = {0x01, 0x02};  // Only 2 bytes
        MessageDeserializer des(data, 2);

        des.read_uint32();  // Needs 4 bytes
        REQUIRE(des.has_error());
    }

    SECTION("string length overflow") {
        uint8_t data[] = {0x0a, 'h', 'i'};  // Says 10 bytes but only 2 available
        MessageDeserializer des(data, 3);

        des.read_string();
        REQUIRE(des.has_error());
    }

    SECTION("bytes count overflow") {
        uint8_t data[] = {0x01, 0x02};
        MessageDeserializer des(data, 2);

        des.read_bytes(10);  // Request more than available
        REQUIRE(des.has_error());
    }
}

TEST_CASE("Message Serialization - Round Trip", "[network][message][roundtrip][unit]") {
    SECTION("Multiple primitive types") {
        MessageSerializer ser;

        ser.write_uint8(0x42);
        ser.write_uint16(0x1234);
        ser.write_uint32(0x12345678);
        ser.write_uint64(0x123456789abcdef0ULL);
        ser.write_bool(true);
        ser.write_varint(0xfd);
        ser.write_string("test");

        auto data = ser.data();
        MessageDeserializer des(data);

        REQUIRE(des.read_uint8() == 0x42);
        REQUIRE(des.read_uint16() == 0x1234);
        REQUIRE(des.read_uint32() == 0x12345678);
        REQUIRE(des.read_uint64() == 0x123456789abcdef0ULL);
        REQUIRE(des.read_bool() == true);
        REQUIRE(des.read_varint() == 0xfd);
        REQUIRE(des.read_string() == "test");

        REQUIRE(des.bytes_remaining() == 0);
        REQUIRE_FALSE(des.has_error());
    }
}

TEST_CASE("MessageDeserializer - Position Tracking", "[network][message][deserializer][unit]") {
    uint8_t data[] = {0x01, 0x02, 0x03, 0x04};
    MessageDeserializer des(data, 4);

    REQUIRE(des.position() == 0);
    REQUIRE(des.bytes_remaining() == 4);

    des.read_uint8();
    REQUIRE(des.position() == 1);
    REQUIRE(des.bytes_remaining() == 3);

    des.read_uint8();
    REQUIRE(des.position() == 2);
    REQUIRE(des.bytes_remaining() == 2);

    des.read_uint16();
    REQUIRE(des.position() == 4);
    REQUIRE(des.bytes_remaining() == 0);
}

TEST_CASE("MessageSerializer - Protocol Structures", "[network][message][serializer][unit]") {
    MessageSerializer ser;

    SECTION("InventoryVector") {
        InventoryVector inv;
        inv.type = InventoryType::MSG_BLOCK;
        inv.hash.fill(0xaa);

        ser.write_inventory_vector(inv);

        auto data = ser.data();
        REQUIRE(data.size() == 36);  // 4 (type) + 32 (hash)

        // Type should be little-endian
        REQUIRE(data[0] == static_cast<uint8_t>(InventoryType::MSG_BLOCK));

        // Hash should be raw bytes
        for (size_t i = 4; i < 36; i++) {
            REQUIRE(data[i] == 0xaa);
        }
    }
}

TEST_CASE("MessageDeserializer - Protocol Structures", "[network][message][deserializer][unit]") {
    SECTION("InventoryVector") {
        MessageSerializer ser;

        InventoryVector original;
        original.type = InventoryType::MSG_BLOCK;
        for (size_t i = 0; i < 32; i++) {
            original.hash[i] = static_cast<uint8_t>(i);
        }

        ser.write_inventory_vector(original);

        MessageDeserializer des(ser.data());
        auto decoded = des.read_inventory_vector();

        REQUIRE(decoded.type == InventoryType::MSG_BLOCK);
        for (size_t i = 0; i < 32; i++) {
            REQUIRE(decoded.hash[i] == static_cast<uint8_t>(i));
        }
        REQUIRE_FALSE(des.has_error());
    }
}

TEST_CASE("VarInt - Edge Cases", "[network][message][varint][unit]") {
    SECTION("Boundary values") {
        // Test exact boundary transitions
        REQUIRE(VarInt(0xfc).encoded_size() == 1);
        REQUIRE(VarInt(0xfd).encoded_size() == 3);

        REQUIRE(VarInt(0xffff).encoded_size() == 3);
        REQUIRE(VarInt(0x10000).encoded_size() == 5);

        REQUIRE(VarInt(0xffffffff).encoded_size() == 5);
        REQUIRE(VarInt(0x100000000ULL).encoded_size() == 9);
    }

    SECTION("Maximum value") {
        VarInt vi(0xffffffffffffffffULL);
        uint8_t buffer[9];
        size_t encoded = vi.encode(buffer);

        REQUIRE(encoded == 9);
        REQUIRE(buffer[0] == 0xff);

        VarInt decoded;
        size_t decoded_bytes = decoded.decode(buffer, 9);
        REQUIRE(decoded_bytes == 9);
        REQUIRE(decoded.value == 0xffffffffffffffffULL);
    }
}

TEST_CASE("MessageSerializer - Long String", "[network][message][serializer][unit]") {
    MessageSerializer ser;

    std::string long_str(1000, 'x');
    ser.write_string(long_str);

    auto data = ser.data();

    MessageDeserializer des(data);
    auto decoded = des.read_string();

    REQUIRE(decoded == long_str);
    REQUIRE(decoded.length() == 1000);
    REQUIRE_FALSE(des.has_error());
}

TEST_CASE("MessageDeserializer - Empty Buffer", "[network][message][deserializer][unit]") {
    uint8_t data[] = {};
    MessageDeserializer des(data, 0);

    REQUIRE(des.bytes_remaining() == 0);
    REQUIRE(des.position() == 0);

    des.read_uint8();
    REQUIRE(des.has_error());
}

TEST_CASE("Message - Ping/Pong", "[network][message][unit]") {
    SECTION("PingMessage serialize/deserialize") {
        PingMessage ping(0x123456789abcdef0ULL);

        auto data = ping.serialize();
        REQUIRE(data.size() == 8);

        PingMessage ping2;
        REQUIRE(ping2.deserialize(data.data(), data.size()));
        REQUIRE(ping2.nonce == 0x123456789abcdef0ULL);
    }

    SECTION("PongMessage serialize/deserialize") {
        PongMessage pong(0xfedcba9876543210ULL);

        auto data = pong.serialize();
        REQUIRE(data.size() == 8);

        PongMessage pong2;
        REQUIRE(pong2.deserialize(data.data(), data.size()));
        REQUIRE(pong2.nonce == 0xfedcba9876543210ULL);
    }

    SECTION("Ping command name") {
        PingMessage ping;
        REQUIRE(ping.command() == commands::PING);
    }

    SECTION("Pong command name") {
        PongMessage pong;
        REQUIRE(pong.command() == commands::PONG);
    }
}

TEST_CASE("Message - Verack", "[network][message][unit]") {
    VerackMessage verack;

    SECTION("Command name") {
        REQUIRE(verack.command() == commands::VERACK);
    }

    SECTION("Serialize") {
        auto data = verack.serialize();
        REQUIRE(data.size() == 0);  // Verack has no payload
    }

    SECTION("Deserialize") {
        uint8_t empty[] = {};
        REQUIRE(verack.deserialize(empty, 0));
    }
}

TEST_CASE("Message - GetAddr", "[network][message][unit]") {
    GetAddrMessage getaddr;

    SECTION("Command name") {
        REQUIRE(getaddr.command() == commands::GETADDR);
    }

    SECTION("Serialize") {
        auto data = getaddr.serialize();
        REQUIRE(data.size() == 0);  // GetAddr has no payload
    }

    SECTION("Deserialize") {
        uint8_t empty[] = {};
        REQUIRE(getaddr.deserialize(empty, 0));
    }
}

// ============================================================================
// DoS Protection Tests - Message Size Limits
// ============================================================================

TEST_CASE("VERSION Message - User Agent Length Enforcement", "[network][message][dos][security]") {
    // Tests for CVE fix: user_agent length must be enforced DURING deserialization
    // to prevent memory exhaustion attacks (max 256 bytes per Bitcoin Core)

    SECTION("Normal user agent - should succeed") {
        MessageSerializer s;
        s.write_int32(70015);  // version
        s.write_uint64(1);     // services
        s.write_int64(1234567890);  // timestamp

        // addr_recv (26 bytes)
        s.write_uint64(0);  // services
        std::array<uint8_t, 16> ipv6 = {0};
        s.write_bytes(ipv6.data(), 16);
        s.write_uint16(8333);  // port

        // addr_from (26 bytes)
        s.write_uint64(0);  // services
        s.write_bytes(ipv6.data(), 16);
        s.write_uint16(0);  // port

        s.write_uint64(0x123456789abcdef);  // nonce
        s.write_string("/Unicity:1.0.0/");  // user_agent (normal length)
        s.write_int32(0);  // start_height

        auto data = s.data();
        VersionMessage msg;
        REQUIRE(msg.deserialize(data.data(), data.size()));
        REQUIRE(msg.user_agent == "/Unicity:1.0.0/");
    }

    SECTION("User agent at MAX_SUBVERSION_LENGTH (256) - should succeed") {
        MessageSerializer s;
        s.write_int32(70015);
        s.write_uint64(1);
        s.write_int64(1234567890);

        // addr_recv
        s.write_uint64(0);
        std::array<uint8_t, 16> ipv6 = {0};
        s.write_bytes(ipv6.data(), 16);
        s.write_uint16(8333);

        // addr_from
        s.write_uint64(0);
        s.write_bytes(ipv6.data(), 16);
        s.write_uint16(0);

        s.write_uint64(0x123456789abcdef);

        // Create string exactly at limit (256 bytes)
        std::string max_user_agent(MAX_SUBVERSION_LENGTH, 'A');
        s.write_string(max_user_agent);
        s.write_int32(0);

        auto data = s.data();
        VersionMessage msg;
        REQUIRE(msg.deserialize(data.data(), data.size()));
        REQUIRE(msg.user_agent == max_user_agent);
        REQUIRE(msg.user_agent.length() == MAX_SUBVERSION_LENGTH);
    }

    SECTION("User agent over MAX_SUBVERSION_LENGTH - should fail") {
        MessageSerializer s;
        s.write_int32(70015);
        s.write_uint64(1);
        s.write_int64(1234567890);

        // addr_recv
        s.write_uint64(0);
        std::array<uint8_t, 16> ipv6 = {0};
        s.write_bytes(ipv6.data(), 16);
        s.write_uint16(8333);

        // addr_from
        s.write_uint64(0);
        s.write_bytes(ipv6.data(), 16);
        s.write_uint16(0);

        s.write_uint64(0x123456789abcdef);

        // Create string OVER limit (257 bytes)
        std::string oversized_user_agent(MAX_SUBVERSION_LENGTH + 1, 'A');
        s.write_string(oversized_user_agent);
        s.write_int32(0);

        auto data = s.data();
        VersionMessage msg;
        // Should fail deserialization due to limit enforcement
        REQUIRE_FALSE(msg.deserialize(data.data(), data.size()));
    }

    SECTION("Very large user agent (4KB) - should fail without OOM") {
        // This tests that we reject large strings BEFORE allocation
        MessageSerializer s;
        s.write_int32(70015);
        s.write_uint64(1);
        s.write_int64(1234567890);

        // addr_recv
        s.write_uint64(0);
        std::array<uint8_t, 16> ipv6 = {0};
        s.write_bytes(ipv6.data(), 16);
        s.write_uint16(8333);

        // addr_from
        s.write_uint64(0);
        s.write_bytes(ipv6.data(), 16);
        s.write_uint16(0);

        s.write_uint64(0x123456789abcdef);

        // Create very large string (4KB)
        std::string huge_user_agent(4096, 'B');
        s.write_string(huge_user_agent);
        s.write_int32(0);

        auto data = s.data();
        VersionMessage msg;
        // Should fail quickly without allocating 4KB
        REQUIRE_FALSE(msg.deserialize(data.data(), data.size()));
    }

    SECTION("Malformed varint for user_agent length - should fail") {
        MessageSerializer s;
        s.write_int32(70015);
        s.write_uint64(1);
        s.write_int64(1234567890);

        // addr_recv
        s.write_uint64(0);
        std::array<uint8_t, 16> ipv6 = {0};
        s.write_bytes(ipv6.data(), 16);
        s.write_uint16(8333);

        // addr_from
        s.write_uint64(0);
        s.write_bytes(ipv6.data(), 16);
        s.write_uint16(0);

        s.write_uint64(0x123456789abcdef);

        // Manually write a varint claiming huge size but with insufficient data
        s.write_uint8(0xfd);  // 3-byte varint prefix
        s.write_uint16(5000); // Claims 5000 bytes
        // But don't provide 5000 bytes - only provide a few
        s.write_string("short");
        s.write_int32(0);

        auto data = s.data();
        VersionMessage msg;
        REQUIRE_FALSE(msg.deserialize(data.data(), data.size()));
    }
}

TEST_CASE("VarInt - Non-Canonical Encoding Rejection (CVE-2018-17144 class)", "[network][message][varint][security][unit]") {
    using namespace unicity::message;

    SECTION("Value 5 with 3-byte encoding should be rejected") {
        uint8_t non_canonical[] = {0xfd, 0x05, 0x00};  // value=5 in 3 bytes
        VarInt vi;
        size_t consumed = vi.decode(non_canonical, sizeof(non_canonical));
        REQUIRE(consumed == 0);  // Should reject non-canonical encoding
    }

    SECTION("Value 0 with 3-byte encoding should be rejected") {
        uint8_t non_canonical[] = {0xfd, 0x00, 0x00};  // value=0 in 3 bytes
        VarInt vi;
        size_t consumed = vi.decode(non_canonical, sizeof(non_canonical));
        REQUIRE(consumed == 0);  // Should reject non-canonical encoding
    }

    SECTION("Value 252 (0xfc) with 3-byte encoding should be rejected") {
        uint8_t non_canonical[] = {0xfd, 0xfc, 0x00};  // value=252 in 3 bytes
        VarInt vi;
        size_t consumed = vi.decode(non_canonical, sizeof(non_canonical));
        REQUIRE(consumed == 0);  // Should reject (252 < 253, must use 1 byte)
    }

    SECTION("Value 253 (0xfd) with 5-byte encoding should be rejected") {
        uint8_t non_canonical[] = {0xfe, 0xfd, 0x00, 0x00, 0x00};  // value=253 in 5 bytes
        VarInt vi;
        size_t consumed = vi.decode(non_canonical, sizeof(non_canonical));
        REQUIRE(consumed == 0);  // Should reject (253 <= 65535, must use 3 bytes)
    }

    SECTION("Value 65536 (0x10000) with 9-byte encoding should be rejected") {
        uint8_t non_canonical[] = {0xff, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00};  // value=65536 in 9 bytes
        VarInt vi;
        size_t consumed = vi.decode(non_canonical, sizeof(non_canonical));
        REQUIRE(consumed == 0);  // Should reject (65536 <= 4294967295, must use 5 bytes)
    }

    SECTION("Canonical encodings should still work") {
        SECTION("Value 5 with 1-byte encoding (canonical)") {
            uint8_t canonical[] = {0x05};
            VarInt vi;
            size_t consumed = vi.decode(canonical, sizeof(canonical));
            REQUIRE(consumed == 1);
            REQUIRE(vi.value == 5);
        }

        SECTION("Value 253 with 3-byte encoding (canonical)") {
            uint8_t canonical[] = {0xfd, 0xfd, 0x00};
            VarInt vi;
            size_t consumed = vi.decode(canonical, sizeof(canonical));
            REQUIRE(consumed == 3);
            REQUIRE(vi.value == 253);
        }

        SECTION("Value 65536 with 5-byte encoding (canonical)") {
            uint8_t canonical[] = {0xfe, 0x00, 0x00, 0x01, 0x00};
            VarInt vi;
            size_t consumed = vi.decode(canonical, sizeof(canonical));
            REQUIRE(consumed == 5);
            REQUIRE(vi.value == 65536);
        }

        SECTION("Value 4294967296 (2^32) with 9-byte encoding (canonical)") {
            uint8_t canonical[] = {0xff, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00};
            VarInt vi;
            size_t consumed = vi.decode(canonical, sizeof(canonical));
            REQUIRE(consumed == 9);
            REQUIRE(vi.value == 4294967296ULL);
        }
    }
}
