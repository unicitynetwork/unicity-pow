// Adversarial tests for network/peer.cpp - Attack scenarios and edge cases (ported to test2)

#include "catch_amalgamated.hpp"
#include "network/peer.hpp"
#include "network/transport.hpp"
#include "network/protocol.hpp"
#include "network/message.hpp"
#include <boost/asio.hpp>
#include <memory>
#include <vector>
#include <set>
#include <array>
#include <random>
#include <mutex>
#include <thread>

using namespace unicity;
using namespace unicity::network;

// =============================================================================
// MOCK TRANSPORT (from legacy tests)
// =============================================================================

#include "infra/mock_transport.hpp"

// =============================================================================
// HELPERS
// =============================================================================

static std::vector<uint8_t> create_test_message(
    uint32_t magic,
    const std::string& command,
    const std::vector<uint8_t>& payload)
{
    auto header = message::create_header(magic, command, payload);
    auto header_bytes = message::serialize_header(header);

    std::vector<uint8_t> full_message;
    full_message.insert(full_message.end(), header_bytes.begin(), header_bytes.end());
    full_message.insert(full_message.end(), payload.begin(), payload.end());
    return full_message;
}

static std::vector<uint8_t> create_version_message(uint32_t magic, uint64_t nonce) {
    message::VersionMessage msg;
    msg.version = protocol::PROTOCOL_VERSION;
    msg.services = protocol::NODE_NETWORK;
    msg.timestamp = 1234567890;
    msg.nonce = nonce;
    msg.user_agent = "/Test:1.0.0/";
    msg.start_height = 0;

    auto payload = msg.serialize();
    return create_test_message(magic, protocol::commands::VERSION, payload);
}

static std::vector<uint8_t> create_verack_message(uint32_t magic) {
    message::VerackMessage msg;
    auto payload = msg.serialize();
    return create_test_message(magic, protocol::commands::VERACK, payload);
}

static std::vector<uint8_t> create_ping_message(uint32_t magic, uint64_t nonce) {
    message::PingMessage msg(nonce);
    auto payload = msg.serialize();
    return create_test_message(magic, protocol::commands::PING, payload);
}

static std::vector<uint8_t> create_pong_message(uint32_t magic, uint64_t nonce) {
    message::PongMessage msg(nonce);
    auto payload = msg.serialize();
    return create_test_message(magic, protocol::commands::PONG, payload);
}

// =============================================================================
// MALFORMED MESSAGE ATTACKS
// =============================================================================

TEST_CASE("Adversarial - PartialHeaderAttack", "[adversarial][malformed]") {
    boost::asio::io_context io_context;
    auto mock_conn = std::make_shared<MockTransportConnection>();
    const uint32_t magic = protocol::magic::REGTEST;

    auto peer = Peer::create_inbound(io_context, mock_conn, magic, 0);
    peer->start();
    io_context.poll();

    SECTION("Partial header (only magic bytes)") {
        std::vector<uint8_t> partial_header(4);
        std::memcpy(partial_header.data(), &magic, 4);

        mock_conn->simulate_receive(partial_header);
        io_context.poll();

        CHECK(peer->is_connected());
        CHECK(peer->version() == 0);
    }

    SECTION("Partial header then timeout") {
        std::vector<uint8_t> partial_header(12);  // Only 12 of 24 header bytes
        mock_conn->simulate_receive(partial_header);
        io_context.poll();
        CHECK(peer->is_connected());
    }
}

TEST_CASE("Adversarial - HeaderLengthMismatch", "[adversarial][malformed]") {
    boost::asio::io_context io_context;
    auto mock_conn = std::make_shared<MockTransportConnection>();
    const uint32_t magic = protocol::magic::REGTEST;

    auto peer = Peer::create_inbound(io_context, mock_conn, magic, 0);
    peer->start();
    io_context.poll();

    SECTION("Header claims 100 bytes, send 50 bytes") {
        protocol::MessageHeader header(magic, protocol::commands::VERSION, 100);
        header.checksum = message::compute_checksum(std::vector<uint8_t>(100, 0));
        auto header_bytes = message::serialize_header(header);
        std::vector<uint8_t> partial_payload(50, 0xAA);
        std::vector<uint8_t> malicious_msg;
        malicious_msg.insert(malicious_msg.end(), header_bytes.begin(), header_bytes.end());
        malicious_msg.insert(malicious_msg.end(), partial_payload.begin(), partial_payload.end());
        mock_conn->simulate_receive(malicious_msg);
        io_context.poll();
        CHECK(peer->is_connected());
        CHECK(peer->version() == 0);
    }

    SECTION("Header claims 0 bytes, send 100 bytes") {
        protocol::MessageHeader header(magic, protocol::commands::VERSION, 0);
        header.checksum.fill(0);
        auto header_bytes = message::serialize_header(header);
        std::vector<uint8_t> unexpected_payload(100, 0xBB);
        std::vector<uint8_t> malicious_msg;
        malicious_msg.insert(malicious_msg.end(), header_bytes.begin(), header_bytes.end());
        malicious_msg.insert(malicious_msg.end(), unexpected_payload.begin(), unexpected_payload.end());
        mock_conn->simulate_receive(malicious_msg);
        io_context.poll();
        CHECK(peer->state() == PeerConnectionState::DISCONNECTED);
    }
}

TEST_CASE("Adversarial - EmptyCommandField", "[adversarial][malformed]") {
    boost::asio::io_context io_context;
    auto mock_conn = std::make_shared<MockTransportConnection>();
    const uint32_t magic = protocol::magic::REGTEST;

    auto peer = Peer::create_inbound(io_context, mock_conn, magic, 0);
    peer->start();
    io_context.poll();

    protocol::MessageHeader header;
    header.magic = magic;
    header.command.fill(0);
    header.length = 0;
    header.checksum.fill(0);

    auto header_bytes = message::serialize_header(header);
    mock_conn->simulate_receive(header_bytes);
    io_context.poll();

    CHECK(peer->state() == PeerConnectionState::DISCONNECTED);
}

TEST_CASE("Adversarial - NonPrintableCommandCharacters", "[adversarial][malformed]") {
    boost::asio::io_context io_context;
    auto mock_conn = std::make_shared<MockTransportConnection>();
    const uint32_t magic = protocol::magic::REGTEST;

    auto peer = Peer::create_inbound(io_context, mock_conn, magic, 0);
    peer->start();
    io_context.poll();

    protocol::MessageHeader header;
    header.magic = magic;
    header.command = { static_cast<char>(0xFF), static_cast<char>(0xFE), static_cast<char>(0xFD), static_cast<char>(0xFC),
                       static_cast<char>(0xFB), static_cast<char>(0xFA), static_cast<char>(0xF9), static_cast<char>(0xF8),
                       static_cast<char>(0xF7), static_cast<char>(0xF6), static_cast<char>(0xF5), static_cast<char>(0xF4) };
    header.length = 0;
    header.checksum.fill(0);

    auto header_bytes = message::serialize_header(header);
    mock_conn->simulate_receive(header_bytes);
    io_context.poll();

    CHECK(peer->state() == PeerConnectionState::DISCONNECTED);
}

// =============================================================================
// PROTOCOL STATE MACHINE ATTACKS
// =============================================================================

TEST_CASE("Adversarial - RapidVersionFlood", "[adversarial][flood]") {
    boost::asio::io_context io_context;
    auto mock_conn = std::make_shared<MockTransportConnection>();
    const uint32_t magic = protocol::magic::REGTEST;

    auto peer = Peer::create_inbound(io_context, mock_conn, magic, 0);
    peer->start();
    io_context.poll();

    auto version1 = create_version_message(magic, 54321);
    mock_conn->simulate_receive(version1);
    io_context.poll();

    CHECK(peer->version() == protocol::PROTOCOL_VERSION);
    CHECK(peer->peer_nonce() == 54321);

    for (int i = 0; i < 99; i++) {
        auto version_dup = create_version_message(magic, 99999 + i);
        mock_conn->simulate_receive(version_dup);
        io_context.poll();
    }

    CHECK(peer->version() == protocol::PROTOCOL_VERSION);
    CHECK(peer->peer_nonce() == 54321);
    CHECK(peer->is_connected());
}

TEST_CASE("Adversarial - RapidVerackFlood", "[adversarial][flood]") {
    boost::asio::io_context io_context;
    auto mock_conn = std::make_shared<MockTransportConnection>();
    const uint32_t magic = protocol::magic::REGTEST;

    auto peer = Peer::create_outbound(io_context, mock_conn, magic, 0);
    peer->start();
    io_context.poll();

    auto version = create_version_message(magic, 54321);
    mock_conn->simulate_receive(version);
    io_context.poll();

    auto verack1 = create_verack_message(magic);
    mock_conn->simulate_receive(verack1);
    io_context.poll();

    CHECK(peer->state() == PeerConnectionState::READY);

    for (int i = 0; i < 99; i++) {
        auto verack_dup = create_verack_message(magic);
        mock_conn->simulate_receive(verack_dup);
        io_context.poll();
    }

    CHECK(peer->state() == PeerConnectionState::READY);
    CHECK(peer->is_connected());
}

TEST_CASE("Adversarial - AlternatingVersionVerack", "[adversarial][protocol]") {
    boost::asio::io_context io_context;
    auto mock_conn = std::make_shared<MockTransportConnection>();
    const uint32_t magic = protocol::magic::REGTEST;

    auto peer = Peer::create_inbound(io_context, mock_conn, magic, 0);
    peer->start();
    io_context.poll();

    for (int i = 0; i < 10; i++) {
        auto version = create_version_message(magic, 50000 + i);
        mock_conn->simulate_receive(version);
        io_context.poll();
        if (!peer->is_connected()) break;
        auto verack = create_verack_message(magic);
        mock_conn->simulate_receive(verack);
        io_context.poll();
        if (!peer->is_connected()) break;
    }

    CHECK(peer->state() == PeerConnectionState::READY);
    CHECK(peer->peer_nonce() == 50000);
}

// =============================================================================
// RESOURCE EXHAUSTION ATTACKS
// =============================================================================

TEST_CASE("Adversarial - SlowDataDrip", "[adversarial][resource]") {
    boost::asio::io_context io_context;
    auto mock_conn = std::make_shared<MockTransportConnection>();
    const uint32_t magic = protocol::magic::REGTEST;

    auto peer = Peer::create_inbound(io_context, mock_conn, magic, 0);
    peer->start();
    io_context.poll();

    auto version = create_version_message(magic, 54321);
    for (size_t i = 0; i < version.size(); i++) {
        std::vector<uint8_t> single_byte = {version[i]};
        mock_conn->simulate_receive(single_byte);
        io_context.poll();
    }

    CHECK(peer->version() == protocol::PROTOCOL_VERSION);
    CHECK(peer->is_connected());
}

TEST_CASE("Adversarial - MultiplePartialMessages", "[adversarial][resource]") {
    boost::asio::io_context io_context;
    auto mock_conn = std::make_shared<MockTransportConnection>();
    const uint32_t magic = protocol::magic::REGTEST;

    auto peer = Peer::create_inbound(io_context, mock_conn, magic, 0);
    peer->start();
    io_context.poll();

    for (int i = 0; i < 10; i++) {
        std::vector<uint8_t> partial_header(12, 0xCC);
        mock_conn->simulate_receive(partial_header);
        io_context.poll();
        if (!peer->is_connected()) {
            break;
        }
    }

    CHECK(peer->state() == PeerConnectionState::DISCONNECTED);
}

TEST_CASE("Adversarial - BufferFragmentation", "[adversarial][resource]") {
    boost::asio::io_context io_context;
    auto mock_conn = std::make_shared<MockTransportConnection>();
    const uint32_t magic = protocol::magic::REGTEST;

    auto peer = Peer::create_inbound(io_context, mock_conn, magic, 0);
    peer->start();
    io_context.poll();

    auto version = create_version_message(magic, 54321);
    mock_conn->simulate_receive(version);
    io_context.poll();
    CHECK(peer->version() == protocol::PROTOCOL_VERSION);

    auto verack = create_verack_message(magic);
    mock_conn->simulate_receive(verack);
    io_context.poll();
    REQUIRE(peer->state() == PeerConnectionState::READY);

    auto bad_ping = create_ping_message(0xBADBAD, 99999);
    mock_conn->simulate_receive(bad_ping);
    io_context.poll();

    CHECK(peer->state() == PeerConnectionState::DISCONNECTED);
}

// =============================================================================
// TIMING ATTACKS
// =============================================================================

TEST_CASE("Adversarial - ExtremeTimestamps", "[adversarial][timing]") {
    boost::asio::io_context io_context;
    auto mock_conn = std::make_shared<MockTransportConnection>();
    const uint32_t magic = protocol::magic::REGTEST;

    auto peer = Peer::create_inbound(io_context, mock_conn, magic, 0);
    peer->start();
    io_context.poll();

    SECTION("Timestamp = 0 (January 1970)") {
        message::VersionMessage msg;
        msg.version = protocol::PROTOCOL_VERSION;
        msg.services = protocol::NODE_NETWORK;
        msg.timestamp = 0;
        msg.nonce = 54321;
        msg.user_agent = "/Test:1.0.0/";
        msg.start_height = 0;
        auto payload = msg.serialize();
        auto full_msg = create_test_message(magic, protocol::commands::VERSION, payload);
        mock_conn->simulate_receive(full_msg);
        io_context.poll();
        CHECK(peer->version() == protocol::PROTOCOL_VERSION);
        CHECK(peer->is_connected());
    }

    SECTION("Timestamp = MAX_INT64 (far future)") {
        message::VersionMessage msg;
        msg.version = protocol::PROTOCOL_VERSION;
        msg.services = protocol::NODE_NETWORK;
        msg.timestamp = std::numeric_limits<int64_t>::max();
        msg.nonce = 54321;
        msg.user_agent = "/Test:1.0.0/";
        msg.start_height = 0;
        auto payload = msg.serialize();
        auto full_msg = create_test_message(magic, protocol::commands::VERSION, payload);
        mock_conn->simulate_receive(full_msg);
        io_context.poll();
        CHECK(peer->version() == protocol::PROTOCOL_VERSION);
        CHECK(peer->is_connected());
    }
}

// =============================================================================
// MESSAGE SEQUENCE ATTACKS
// =============================================================================

TEST_CASE("Adversarial - OutOfOrderHandshake", "[adversarial][protocol]") {
    boost::asio::io_context io_context;
    auto mock_conn = std::make_shared<MockTransportConnection>();
    const uint32_t magic = protocol::magic::REGTEST;

    SECTION("VERACK then VERSION then VERACK (outbound)") {
        auto peer = Peer::create_outbound(io_context, mock_conn, magic, 0);
        peer->start();
        io_context.poll();
        auto verack1 = create_verack_message(magic);
        mock_conn->simulate_receive(verack1);
        io_context.poll();
        CHECK(peer->state() == PeerConnectionState::DISCONNECTED);
    }

    SECTION("Double VERSION with VERACK in between") {
        auto peer = Peer::create_inbound(io_context, mock_conn, magic, 0);
        peer->start();
        io_context.poll();
        auto version1 = create_version_message(magic, 11111);
        mock_conn->simulate_receive(version1);
        io_context.poll();
        CHECK(peer->peer_nonce() == 11111);
        auto verack = create_verack_message(magic);
        mock_conn->simulate_receive(verack);
        io_context.poll();
        CHECK(peer->state() == PeerConnectionState::READY);
        auto version2 = create_version_message(magic, 22222);
        mock_conn->simulate_receive(version2);
        io_context.poll();
        CHECK(peer->peer_nonce() == 11111);
        CHECK(peer->state() == PeerConnectionState::READY);
    }
}

TEST_CASE("Adversarial - PingFloodBeforeHandshake", "[adversarial][flood]") {
    boost::asio::io_context io_context;
    auto mock_conn = std::make_shared<MockTransportConnection>();
    const uint32_t magic = protocol::magic::REGTEST;

    auto peer = Peer::create_inbound(io_context, mock_conn, magic, 0);
    peer->start();
    io_context.poll();

    for (int i = 0; i < 10; i++) {
        auto ping = create_ping_message(magic, 1000 + i);
        mock_conn->simulate_receive(ping);
        io_context.poll();
        if (!peer->is_connected()) {
            break;
        }
    }
    CHECK(peer->state() == PeerConnectionState::DISCONNECTED);
}

// =============================================================================
// QUICK WIN TESTS
// =============================================================================

TEST_CASE("Adversarial - PongNonceMismatch", "[adversarial][protocol][quickwin]") {
    boost::asio::io_context io_context;
    auto mock_conn = std::make_shared<MockTransportConnection>();
    const uint32_t magic = protocol::magic::REGTEST;

    auto peer = Peer::create_outbound(io_context, mock_conn, magic, 0);
    peer->start();
    io_context.poll();

    auto version = create_version_message(magic, 54321);
    mock_conn->simulate_receive(version);
    io_context.poll();

    auto verack = create_verack_message(magic);
    mock_conn->simulate_receive(verack);
    io_context.poll();

    REQUIRE(peer->state() == PeerConnectionState::READY);
    mock_conn->clear_sent_messages();

    uint64_t peer_ping_nonce = 777777;
    auto ping_from_peer = create_ping_message(magic, peer_ping_nonce);
    mock_conn->simulate_receive(ping_from_peer);
    io_context.poll();
    CHECK(mock_conn->sent_message_count() == 1);

    auto wrong_pong = create_pong_message(magic, 999999);
    mock_conn->simulate_receive(wrong_pong);
    io_context.poll();
    CHECK(peer->is_connected());
}

TEST_CASE("Adversarial - DeserializationFailureFlooding", "[adversarial][malformed][quickwin]") {
    boost::asio::io_context io_context;
    auto mock_conn = std::make_shared<MockTransportConnection>();
    const uint32_t magic = protocol::magic::REGTEST;

    auto peer = Peer::create_inbound(io_context, mock_conn, magic, 0);
    peer->start();
    io_context.poll();

    auto version = create_version_message(magic, 54321);
    mock_conn->simulate_receive(version);
    io_context.poll();

    auto verack = create_verack_message(magic);
    mock_conn->simulate_receive(verack);
    io_context.poll();

    REQUIRE(peer->state() == PeerConnectionState::READY);

    SECTION("PING with payload too short") {
        std::vector<uint8_t> short_payload = {0x01, 0x02, 0x03, 0x04};
        auto malformed_ping = create_test_message(magic, protocol::commands::PING, short_payload);
        mock_conn->simulate_receive(malformed_ping);
        io_context.poll();
        CHECK(peer->state() == PeerConnectionState::DISCONNECTED);
    }

    SECTION("PING with payload too long") {
        std::vector<uint8_t> long_payload(16, 0xAA);
        auto malformed_ping = create_test_message(magic, protocol::commands::PING, long_payload);
        mock_conn->simulate_receive(malformed_ping);
        io_context.poll();
        CHECK(peer->state() == PeerConnectionState::READY);
    }

    SECTION("VERACK with unexpected payload") {
        std::vector<uint8_t> garbage_payload = {0xDE, 0xAD, 0xBE, 0xEF};
        auto malformed_verack = create_test_message(magic, protocol::commands::VERACK, garbage_payload);
        mock_conn->simulate_receive(malformed_verack);
        io_context.poll();
        CHECK(peer->state() == PeerConnectionState::DISCONNECTED);
    }
}

TEST_CASE("Adversarial - ReceiveBufferCycling", "[adversarial][resource][quickwin]") {
    boost::asio::io_context io_context;
    auto mock_conn = std::make_shared<MockTransportConnection>();
    const uint32_t magic = protocol::magic::REGTEST;

    auto peer = Peer::create_inbound(io_context, mock_conn, magic, 0);
    peer->start();
    io_context.poll();

    auto version = create_version_message(magic, 54321);
    mock_conn->simulate_receive(version);
    io_context.poll();

    auto verack = create_verack_message(magic);
    mock_conn->simulate_receive(verack);
    io_context.poll();

    REQUIRE(peer->state() == PeerConnectionState::READY);

    const size_t large_message_size = 100 * 1024;
    for (int i = 0; i < 10; i++) {
        std::vector<uint8_t> large_payload;
        large_payload.reserve(large_message_size);
        uint64_t nonce = 10000 + i;
        for (size_t j = 0; j < large_message_size / 8; j++) {
            large_payload.insert(large_payload.end(),
                                reinterpret_cast<const uint8_t*>(&nonce),
                                reinterpret_cast<const uint8_t*>(&nonce) + sizeof(nonce));
        }
        auto large_ping = create_test_message(magic, protocol::commands::PING, large_payload);
        mock_conn->simulate_receive(large_ping);
        io_context.poll();
        if (!peer->is_connected()) { FAIL("Peer disconnected after " << (i+1) << " large messages"); }
    }
    CHECK(peer->is_connected());
    CHECK(peer->stats().messages_received >= 12);
}

TEST_CASE("Adversarial - UnknownMessageFlooding", "[adversarial][flood][quickwin]") {
    boost::asio::io_context io_context;
    auto mock_conn = std::make_shared<MockTransportConnection>();
    const uint32_t magic = protocol::magic::REGTEST;

    auto peer = Peer::create_inbound(io_context, mock_conn, magic, 0);
    peer->start();
    io_context.poll();

    auto version = create_version_message(magic, 54321);
    mock_conn->simulate_receive(version);
    io_context.poll();

    auto verack = create_verack_message(magic);
    mock_conn->simulate_receive(verack);
    io_context.poll();

    REQUIRE(peer->state() == PeerConnectionState::READY);

    std::vector<std::string> fake_commands = {
        "FAKECMD1", "FAKECMD2", "XYZABC", "UNKNOWN",
        "BOGUS", "INVALID", "NOTREAL", "JUNK",
        "GARBAGE", "RANDOM"
    };

    // SECURITY: Our DoS protection disconnects after 20 unknown commands in 60 seconds
    // Send 25 unknown messages and verify peer gets disconnected
    int messages_sent = 0;
    for (int i = 0; i < 25; i++) {
        std::string fake_cmd = fake_commands[i % fake_commands.size()];
        // SECURITY: Only VERACK and GETADDR are allowed zero-length payloads
        // Unknown commands must have non-empty payloads to pass protocol validation
        std::vector<uint8_t> dummy_payload = {0x01, 0x02, 0x03, 0x04};
        auto unknown_msg = create_test_message(magic, fake_cmd, dummy_payload);
        mock_conn->simulate_receive(unknown_msg);
        io_context.poll();
        messages_sent++;
        if (!peer->is_connected()) {
            break;
        }
    }
    // Verify peer was disconnected (should disconnect after ~20 unknown commands)
    CHECK_FALSE(peer->is_connected());
    CHECK(messages_sent > 20);  // Disconnected after exceeding limit
    CHECK(messages_sent <= 25);  // Disconnected before sending all 25
}

TEST_CASE("Adversarial - StatisticsOverflow", "[adversarial][resource][quickwin]") {
    boost::asio::io_context io_context;
    auto mock_conn = std::make_shared<MockTransportConnection>();
    const uint32_t magic = protocol::magic::REGTEST;

    auto peer = Peer::create_outbound(io_context, mock_conn, magic, 0);
    peer->start();
    io_context.poll();

    auto version = create_version_message(magic, 54321);
    mock_conn->simulate_receive(version);
    io_context.poll();

    auto verack = create_verack_message(magic);
    mock_conn->simulate_receive(verack);
    io_context.poll();

    REQUIRE(peer->state() == PeerConnectionState::READY);

    for (int i = 0; i < 1000; i++) {
        auto ping = create_ping_message(magic, 5000 + i);
        mock_conn->simulate_receive(ping);
        io_context.poll();
    }
    CHECK(peer->stats().messages_received >= 1002);
    CHECK(peer->stats().bytes_received > 1000);
    CHECK(peer->is_connected());
}

TEST_CASE("Adversarial - MessageHandlerBlocking", "[adversarial][threading][p2]") {
    boost::asio::io_context io_context;
    auto mock_conn = std::make_shared<MockTransportConnection>();
    const uint32_t magic = protocol::magic::REGTEST;

    auto peer = Peer::create_outbound(io_context, mock_conn, magic, 0);

    bool handler_called = false;
    std::chrono::steady_clock::time_point handler_start;
    std::chrono::steady_clock::time_point handler_end;

    peer->set_message_handler([&](PeerPtr p, std::unique_ptr<message::Message> msg) {
        handler_called = true;
        handler_start = std::chrono::steady_clock::now();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        handler_end = std::chrono::steady_clock::now();
        return true;
    });

    peer->start();
    io_context.poll();

    auto version = create_version_message(magic, 54321);
    mock_conn->simulate_receive(version);
    io_context.poll();

    auto verack = create_verack_message(magic);
    mock_conn->simulate_receive(verack);
    io_context.poll();

    REQUIRE(peer->state() == PeerConnectionState::READY);
    REQUIRE(handler_called);

    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(handler_end - handler_start);
    CHECK(duration.count() >= 100);
    CHECK(peer->is_connected());
}

TEST_CASE("Adversarial - ConcurrentDisconnectDuringProcessing", "[adversarial][race][p2]") {
    boost::asio::io_context io_context;
    auto mock_conn = std::make_shared<MockTransportConnection>();
    const uint32_t magic = protocol::magic::REGTEST;

    auto peer = Peer::create_outbound(io_context, mock_conn, magic, 0);

    std::atomic<bool> handler_running{false};
    std::atomic<bool> disconnect_called{false};

    peer->set_message_handler([&](PeerPtr p, std::unique_ptr<message::Message> msg) {
        handler_running = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        bool still_connected = p->is_connected();
        (void)still_connected;
        handler_running = false;
        return true;
    });

    peer->start();
    io_context.poll();

    auto version = create_version_message(magic, 54321);
    mock_conn->simulate_receive(version);
    io_context.poll();

    auto verack = create_verack_message(magic);
    mock_conn->simulate_receive(verack);
    io_context.poll();

    REQUIRE(peer->state() == PeerConnectionState::READY);

    auto ping = create_ping_message(magic, 99999);
    mock_conn->simulate_receive(ping);
    peer->disconnect();
    disconnect_called = true;
    io_context.poll();

    CHECK(disconnect_called);
    CHECK(peer->state() == PeerConnectionState::DISCONNECTED);
}

TEST_CASE("Adversarial - SelfConnectionEdgeCases", "[adversarial][protocol][p2]") {
    boost::asio::io_context io_context;
    const uint32_t magic = protocol::magic::REGTEST;

    SECTION("Inbound self-connection with matching nonce") {
        auto mock_conn = std::make_shared<MockTransportConnection>();
        auto peer = Peer::create_inbound(io_context, mock_conn, magic, 0);
        peer->start();
        io_context.poll();
        auto version = create_version_message(magic, peer->get_local_nonce());
        mock_conn->simulate_receive(version);
        io_context.poll();
        CHECK(peer->state() == PeerConnectionState::DISCONNECTED);
    }

    SECTION("Outbound doesn't check self-connection") {
        auto mock_conn = std::make_shared<MockTransportConnection>();
        auto peer = Peer::create_outbound(io_context, mock_conn, magic, 0);
        peer->start();
        io_context.poll();
        auto version = create_version_message(magic, peer->get_local_nonce());
        mock_conn->simulate_receive(version);
        io_context.poll();
        CHECK(peer->version() == protocol::PROTOCOL_VERSION);
        CHECK(peer->peer_nonce() == peer->get_local_nonce());
        CHECK(peer->is_connected());
    }
}

TEST_CASE("Adversarial - MaxMessageSizeEdgeCases", "[adversarial][edge][p2]") {
    boost::asio::io_context io_context;
    auto mock_conn = std::make_shared<MockTransportConnection>();
    const uint32_t magic = protocol::magic::REGTEST;

    auto peer = Peer::create_inbound(io_context, mock_conn, magic, 0);
    peer->start();
    io_context.poll();

    auto version = create_version_message(magic, 54321);
    mock_conn->simulate_receive(version);
    io_context.poll();

    auto verack = create_verack_message(magic);
    mock_conn->simulate_receive(verack);
    io_context.poll();

    REQUIRE(peer->state() == PeerConnectionState::READY);

    SECTION("Exactly MAX_PROTOCOL_MESSAGE_LENGTH") {
        std::vector<uint8_t> max_payload(protocol::MAX_PROTOCOL_MESSAGE_LENGTH, 0xAA);
        auto max_msg = create_test_message(magic, protocol::commands::PING, max_payload);
        mock_conn->simulate_receive(max_msg);
        io_context.poll();
        CHECK(peer->is_connected());
    }

    SECTION("Exactly MAX_PROTOCOL_MESSAGE_LENGTH + 1") {
        std::vector<uint8_t> payload(protocol::MAX_PROTOCOL_MESSAGE_LENGTH + 1, 0xBB);
        protocol::MessageHeader header(magic, protocol::commands::PING,
                                      protocol::MAX_PROTOCOL_MESSAGE_LENGTH + 1);
        header.checksum = message::compute_checksum(payload);
        auto header_bytes = message::serialize_header(header);
        mock_conn->simulate_receive(header_bytes);
        io_context.poll();
        CHECK(peer->state() == PeerConnectionState::DISCONNECTED);
    }

    SECTION("Receive buffer large message handling") {
        std::vector<uint8_t> large_payload(3 * 1024 * 1024, 0xEE);
        auto large_msg = create_test_message(magic, protocol::commands::PING, large_payload);
        mock_conn->simulate_receive(large_msg);
        io_context.poll();
        CHECK(peer->is_connected());
        std::vector<uint8_t> another_large_payload(3 * 1024 * 1024, 0xFF);
        auto another_large_msg = create_test_message(magic, protocol::commands::PING, another_large_payload);
        mock_conn->simulate_receive(another_large_msg);
        io_context.poll();
        CHECK(peer->is_connected());
    }
}

TEST_CASE("Adversarial - MessageRateLimiting", "[adversarial][flood][p3]") {
    boost::asio::io_context io_context;
    auto mock_conn = std::make_shared<MockTransportConnection>();
    const uint32_t magic = protocol::magic::REGTEST;

    auto peer = Peer::create_inbound(io_context, mock_conn, magic, 0);
    peer->start();
    io_context.poll();

    auto version = create_version_message(magic, 54321);
    mock_conn->simulate_receive(version);
    io_context.poll();

    auto verack = create_verack_message(magic);
    mock_conn->simulate_receive(verack);
    io_context.poll();

    REQUIRE(peer->state() == PeerConnectionState::READY);

    for (int i = 0; i < 1000; i++) {
        auto ping = create_ping_message(magic, 8000 + i);
        mock_conn->simulate_receive(ping);
        io_context.poll();
        if (!peer->is_connected()) { break; }
    }

    CHECK(peer->is_connected());
    CHECK(peer->stats().messages_received >= 1002);
}

TEST_CASE("Adversarial - TransportCallbackOrdering", "[adversarial][race][p3]") {
    boost::asio::io_context io_context;
    auto mock_conn = std::make_shared<MockTransportConnection>();
    const uint32_t magic = protocol::magic::REGTEST;

    auto peer = Peer::create_inbound(io_context, mock_conn, magic, 0);
    peer->start();
    io_context.poll();

    SECTION("Receive callback after disconnect") {
        peer->disconnect();
        io_context.poll();  // Process the disconnect operation
        CHECK(peer->state() == PeerConnectionState::DISCONNECTED);
        auto version = create_version_message(magic, 54321);
        mock_conn->simulate_receive(version);
        io_context.poll();
        // SECURITY: After disconnect(), callbacks are cleared to prevent use-after-free
        // Messages received after disconnect() should NOT be processed
        CHECK(peer->state() == PeerConnectionState::DISCONNECTED);
        CHECK(peer->version() == 0);  // VERSION not processed (callback cleared)
    }

    SECTION("Disconnect callback fires twice") {
        auto version = create_version_message(magic, 54321);
        mock_conn->simulate_receive(version);
        io_context.poll();
        auto verack = create_verack_message(magic);
        mock_conn->simulate_receive(verack);
        io_context.poll();
        REQUIRE(peer->state() == PeerConnectionState::READY);
        peer->disconnect();
        io_context.poll();  // Process first disconnect
        CHECK(peer->state() == PeerConnectionState::DISCONNECTED);
        peer->disconnect();
        io_context.poll();  // Process second disconnect (should be no-op)
        CHECK(peer->state() == PeerConnectionState::DISCONNECTED);
    }
}

TEST_CASE("Adversarial - CommandFieldPadding", "[adversarial][malformed][p3]") {
    boost::asio::io_context io_context;
    auto mock_conn = std::make_shared<MockTransportConnection>();
    const uint32_t magic = protocol::magic::REGTEST;

    auto peer = Peer::create_inbound(io_context, mock_conn, magic, 0);
    peer->start();
    io_context.poll();

    SECTION("VERSION with null padding") {
        protocol::MessageHeader header;
        header.magic = magic;
        header.command.fill(0);
        std::string cmd = "version";
        std::copy(cmd.begin(), cmd.end(), header.command.begin());
        message::VersionMessage msg;
        msg.version = protocol::PROTOCOL_VERSION;
        msg.services = protocol::NODE_NETWORK;
        msg.timestamp = 1234567890;
        msg.nonce = 54321;
        msg.user_agent = "/Test:1.0.0/";
        msg.start_height = 0;
        auto payload = msg.serialize();
        header.length = payload.size();
        header.checksum = message::compute_checksum(payload);
        auto header_bytes = message::serialize_header(header);
        std::vector<uint8_t> full_message;
        full_message.insert(full_message.end(), header_bytes.begin(), header_bytes.end());
        full_message.insert(full_message.end(), payload.begin(), payload.end());
        mock_conn->simulate_receive(full_message);
        io_context.poll();
        CHECK(peer->version() == protocol::PROTOCOL_VERSION);
        CHECK(peer->is_connected());
    }

    SECTION("Command with trailing spaces") {
        protocol::MessageHeader header;
        header.magic = magic;
        header.command.fill(' ');
        std::string cmd = "version";
        std::copy(cmd.begin(), cmd.end(), header.command.begin());
        message::VersionMessage msg;
        msg.version = protocol::PROTOCOL_VERSION;
        msg.services = protocol::NODE_NETWORK;
        msg.timestamp = 1234567890;
        msg.nonce = 54321;
        msg.user_agent = "/Test:1.0.0/";
        msg.start_height = 0;
        auto payload = msg.serialize();
        header.length = payload.size();
        header.checksum = message::compute_checksum(payload);
        auto header_bytes = message::serialize_header(header);
        std::vector<uint8_t> full_message;
        full_message.insert(full_message.end(), header_bytes.begin(), header_bytes.end());
        full_message.insert(full_message.end(), payload.begin(), payload.end());
        mock_conn->simulate_receive(full_message);
        io_context.poll();
        bool connected = peer->is_connected();
        bool version_set = (peer->version() == protocol::PROTOCOL_VERSION);
        CHECK((connected == version_set));
    }
}
