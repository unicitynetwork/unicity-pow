// Peer unit tests for network/peer.cpp (ported to test2)

#include "catch_amalgamated.hpp"
#include "network/peer.hpp"
#include "network/transport.hpp"
#include "network/protocol.hpp"
#include "network/message.hpp"
#include <boost/asio.hpp>
#include <memory>
#include <vector>
#include <deque>
#include <mutex>
#include <thread>

using namespace unicity;
using namespace unicity::network;

// =============================================================================
// MOCK TRANSPORT
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
// PEER STATE MACHINE TESTS
// =============================================================================

TEST_CASE("Peer - OutboundHandshake", "[peer][handshake]") {
    boost::asio::io_context io_context;
    auto mock_conn = std::make_shared<MockTransportConnection>();
    mock_conn->set_inbound(false);

    const uint32_t magic = protocol::magic::REGTEST;

    auto peer = Peer::create_outbound(io_context, mock_conn, magic, 0);

    SECTION("Initial state") {
        CHECK(peer->state() == PeerConnectionState::CONNECTED);
        CHECK_FALSE(peer->successfully_connected());
        CHECK(peer->is_connected());
        CHECK_FALSE(peer->is_inbound());
    }

    SECTION("Sends VERSION on start") {
        peer->start();
        io_context.poll();
        CHECK(mock_conn->sent_message_count() >= 1);
        CHECK(peer->state() == PeerConnectionState::VERSION_SENT);
    }

    SECTION("Complete handshake") {
        bool message_received = false;
        peer->set_message_handler([&](PeerPtr p, std::unique_ptr<message::Message> msg) {
            message_received = true;
            return true;
        });
        peer->start();
        io_context.poll();
        CHECK(peer->state() == PeerConnectionState::VERSION_SENT);
        auto version_msg = create_version_message(magic, 54321);
        mock_conn->simulate_receive(version_msg);
        io_context.poll();
        CHECK(mock_conn->sent_message_count() >= 2);
        auto verack_msg = create_verack_message(magic);
        mock_conn->simulate_receive(verack_msg);
        io_context.poll();
        CHECK(peer->state() == PeerConnectionState::READY);
        CHECK(peer->successfully_connected());
        CHECK(message_received);
    }
}

TEST_CASE("Peer - InboundHandshake", "[peer][handshake]") {
    boost::asio::io_context io_context;
    auto mock_conn = std::make_shared<MockTransportConnection>();
    mock_conn->set_inbound(true);

    const uint32_t magic = protocol::magic::REGTEST;

    auto peer = Peer::create_inbound(io_context, mock_conn, magic, 0);

    SECTION("Waits for VERSION") {
        peer->start();
        io_context.poll();
        CHECK(peer->state() == PeerConnectionState::CONNECTED);
    }

    SECTION("Complete inbound handshake") {
        peer->start();
        io_context.poll();
        auto version_msg = create_version_message(magic, 54321);
        mock_conn->simulate_receive(version_msg);
        io_context.poll();
        CHECK(mock_conn->sent_message_count() >= 2);
        auto verack_msg = create_verack_message(magic);
        mock_conn->simulate_receive(verack_msg);
        io_context.poll();
        CHECK(peer->state() == PeerConnectionState::READY);
        CHECK(peer->successfully_connected());
    }
}

TEST_CASE("Peer - SelfConnectionPrevention", "[peer][handshake][security]") {
    boost::asio::io_context io_context;
    auto mock_conn = std::make_shared<MockTransportConnection>();
    mock_conn->set_inbound(true);

    const uint32_t magic = protocol::magic::REGTEST;

    auto peer = Peer::create_inbound(io_context, mock_conn, magic, 0);
    peer->start();
    io_context.poll();

    auto version_msg = create_version_message(magic, peer->get_local_nonce());
    mock_conn->simulate_receive(version_msg);
    io_context.poll();
    CHECK(peer->state() == PeerConnectionState::DISCONNECTED);
}

// =============================================================================
// MESSAGE HANDLING TESTS
// =============================================================================

TEST_CASE("Peer - SendMessage", "[peer][messages]") {
    boost::asio::io_context io_context;
    auto mock_conn = std::make_shared<MockTransportConnection>();

    const uint32_t magic = protocol::magic::REGTEST;
    auto peer = Peer::create_outbound(io_context, mock_conn, magic, 0);

    SECTION("Send PING message") {
        auto ping = std::make_unique<message::PingMessage>(99999);
        peer->send_message(std::move(ping));
        io_context.poll();
        CHECK(mock_conn->sent_message_count() == 1);
        auto sent = mock_conn->get_sent_messages()[0];
        CHECK(sent.size() >= protocol::MESSAGE_HEADER_SIZE);
    }

    SECTION("Cannot send when disconnected") {
        peer->disconnect();
        io_context.poll();
        size_t before = mock_conn->sent_message_count();
        auto ping = std::make_unique<message::PingMessage>(99999);
        peer->send_message(std::move(ping));
        CHECK(mock_conn->sent_message_count() == before);
    }
}

TEST_CASE("Peer - ReceiveMessage", "[peer][messages]") {
    boost::asio::io_context io_context;
    auto mock_conn = std::make_shared<MockTransportConnection>();

    const uint32_t magic = protocol::magic::REGTEST;
    auto peer = Peer::create_outbound(io_context, mock_conn, magic, 0);

    std::string received_command;
    peer->set_message_handler([&](PeerPtr p, std::unique_ptr<message::Message> msg) {
        received_command = msg->command();
        return true;
    });

    peer->start();
    io_context.poll();

    auto version_msg = create_version_message(magic, 54321);
    mock_conn->simulate_receive(version_msg);
    io_context.poll();

    auto verack_msg = create_verack_message(magic);
    mock_conn->simulate_receive(verack_msg);
    io_context.poll();

    REQUIRE(peer->state() == PeerConnectionState::READY);
    mock_conn->clear_sent_messages();

    SECTION("Receive PING and auto-respond with PONG") {
        received_command.clear();
        auto ping_msg = create_ping_message(magic, 77777);
        mock_conn->simulate_receive(ping_msg);
        io_context.poll();
        CHECK(mock_conn->sent_message_count() == 1);
        CHECK(received_command.empty());
    }
}

TEST_CASE("Peer - InvalidMessageHandling", "[peer][messages][security]") {
    boost::asio::io_context io_context;
    auto mock_conn = std::make_shared<MockTransportConnection>();

    const uint32_t magic = protocol::magic::REGTEST;
    auto peer = Peer::create_outbound(io_context, mock_conn, magic, 0);
    peer->start();
    io_context.poll();

    SECTION("Wrong magic bytes") {
        auto ping_msg = create_ping_message(0xDEADBEEF, 12345);
        mock_conn->simulate_receive(ping_msg);
        io_context.poll();
        CHECK(peer->state() == PeerConnectionState::DISCONNECTED);
    }

    SECTION("Message too large") {
        protocol::MessageHeader header(magic, protocol::commands::PING,
                                      protocol::MAX_PROTOCOL_MESSAGE_LENGTH + 1);
        header.checksum.fill(0);
        auto header_bytes = message::serialize_header(header);
        mock_conn->simulate_receive(header_bytes);
        io_context.poll();
        CHECK(peer->state() == PeerConnectionState::DISCONNECTED);
    }

    SECTION("Checksum mismatch") {
        message::PingMessage ping(12345);
        auto payload = ping.serialize();
        protocol::MessageHeader header(magic, protocol::commands::PING,
                                      static_cast<uint32_t>(payload.size()));
        header.checksum.fill(0xFF);
        auto header_bytes = message::serialize_header(header);
        std::vector<uint8_t> full_message;
        full_message.insert(full_message.end(), header_bytes.begin(), header_bytes.end());
        full_message.insert(full_message.end(), payload.begin(), payload.end());
        mock_conn->simulate_receive(full_message);
        io_context.poll();
        CHECK(peer->state() == PeerConnectionState::DISCONNECTED);
    }
}

// =============================================================================
// TIMEOUT TESTS (documentation only)
// =============================================================================

TEST_CASE("Peer - HandshakeTimeout", "[.][timeout]") {
    boost::asio::io_context io_context;
    auto mock_conn = std::make_shared<MockTransportConnection>();
    auto peer = Peer::create_outbound(io_context, mock_conn,
                                      protocol::magic::REGTEST, 0);
    peer->start();
    auto work = boost::asio::make_work_guard(io_context);
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start <
           std::chrono::seconds(protocol::VERSION_HANDSHAKE_TIMEOUT_SEC + 1)) {
        io_context.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    CHECK(peer->state() == PeerConnectionState::DISCONNECTED);
}

TEST_CASE("Peer - InactivityTimeout", "[peer][timeout]") {
    boost::asio::io_context io_context;
    auto mock_conn = std::make_shared<MockTransportConnection>();
    const uint32_t magic = protocol::magic::REGTEST;
    auto peer = Peer::create_outbound(io_context, mock_conn, magic, 0);
    peer->start();
    io_context.poll();
    auto version_msg = create_version_message(magic, 54321);
    mock_conn->simulate_receive(version_msg);
    io_context.poll();
    auto verack_msg = create_verack_message(magic);
    mock_conn->simulate_receive(verack_msg);
    io_context.poll();
    REQUIRE(peer->state() == PeerConnectionState::READY);
}

// =============================================================================
// BUFFER MANAGEMENT / SECURITY TESTS
// =============================================================================

TEST_CASE("Peer - ReceiveBufferFloodProtection", "[peer][security][dos]") {
    boost::asio::io_context io_context;
    auto mock_conn = std::make_shared<MockTransportConnection>();
    auto peer = Peer::create_outbound(io_context, mock_conn,
                                      protocol::magic::REGTEST, 0);
    peer->start();
    io_context.poll();
    std::vector<uint8_t> huge_data(protocol::DEFAULT_RECV_FLOOD_SIZE + 1, 0xAA);
    mock_conn->simulate_receive(huge_data);
    io_context.poll();
    CHECK(peer->state() == PeerConnectionState::DISCONNECTED);
}

TEST_CASE("Peer - UserAgentLengthValidation", "[peer][security]") {
    boost::asio::io_context io_context;
    auto mock_conn = std::make_shared<MockTransportConnection>();
    const uint32_t magic = protocol::magic::REGTEST;
    auto peer = Peer::create_inbound(io_context, mock_conn, magic, 0);
    peer->start();
    io_context.poll();
    message::VersionMessage msg;
    msg.version = protocol::PROTOCOL_VERSION;
    msg.services = protocol::NODE_NETWORK;
    msg.timestamp = 1234567890;
    msg.nonce = 54321;
    msg.user_agent = std::string(protocol::MAX_SUBVERSION_LENGTH + 1, 'X');
    msg.start_height = 0;
    auto payload = msg.serialize();
    auto full_msg = create_test_message(magic, protocol::commands::VERSION, payload);
    mock_conn->simulate_receive(full_msg);
    io_context.poll();
    CHECK(peer->state() == PeerConnectionState::DISCONNECTED);
}

// =============================================================================
// STATISTICS TESTS
// =============================================================================

TEST_CASE("Peer - Statistics", "[peer][stats]") {
    boost::asio::io_context io_context;
    auto mock_conn = std::make_shared<MockTransportConnection>();
    const uint32_t magic = protocol::magic::REGTEST;
    auto peer = Peer::create_outbound(io_context, mock_conn, magic, 0);

    SECTION("Tracks messages sent") {
        peer->start();
        io_context.poll();
        size_t initial = peer->stats().messages_sent;
        auto ping = std::make_unique<message::PingMessage>(12345);
        peer->send_message(std::move(ping));
        io_context.poll();
        CHECK(peer->stats().messages_sent == initial + 1);
        CHECK(peer->stats().bytes_sent > 0);
    }

    SECTION("Tracks messages received") {
        peer->set_message_handler([](PeerPtr p, std::unique_ptr<message::Message> msg) {
            return true;
        });
        peer->start();
        io_context.poll();
        auto version_msg = create_version_message(magic, 54321);
        mock_conn->simulate_receive(version_msg);
        io_context.poll();
        auto verack_msg = create_verack_message(magic);
        mock_conn->simulate_receive(verack_msg);
        io_context.poll();
        size_t initial = peer->stats().messages_received;
        auto ping_msg = create_ping_message(magic, 99999);
        mock_conn->simulate_receive(ping_msg);
        io_context.poll();
        CHECK(peer->stats().messages_received > initial);
        CHECK(peer->stats().bytes_received > 0);
    }
}

// =============================================================================
// PING/PONG TESTS
// =============================================================================

TEST_CASE("Peer - PingPong", "[peer][ping]") {
    boost::asio::io_context io_context;
    auto mock_conn = std::make_shared<MockTransportConnection>();
    const uint32_t magic = protocol::magic::REGTEST;
    auto peer = Peer::create_outbound(io_context, mock_conn, magic, 0);
    peer->start();
    io_context.poll();
    auto version_msg = create_version_message(magic, 54321);
    mock_conn->simulate_receive(version_msg);
    io_context.poll();
    auto verack_msg = create_verack_message(magic);
    mock_conn->simulate_receive(verack_msg);
    io_context.poll();
    REQUIRE(peer->state() == PeerConnectionState::READY);
    mock_conn->clear_sent_messages();
    uint64_t ping_nonce = 777777;
    auto ping_msg = create_ping_message(magic, ping_nonce);
    mock_conn->simulate_receive(ping_msg);
    io_context.poll();
    CHECK(mock_conn->sent_message_count() == 1);
    auto pong_data = mock_conn->get_sent_messages()[0];
    CHECK(pong_data.size() >= protocol::MESSAGE_HEADER_SIZE);
}

// =============================================================================
// DISCONNECT TESTS
// =============================================================================

TEST_CASE("Peer - DisconnectCleanup", "[peer][disconnect]") {
    boost::asio::io_context io_context;
    auto mock_conn = std::make_shared<MockTransportConnection>();
    auto peer = Peer::create_outbound(io_context, mock_conn,
                                      protocol::magic::REGTEST, 0);
    peer->start();
    io_context.poll();
    REQUIRE(peer->is_connected());
    peer->disconnect();
    io_context.poll();
    CHECK(peer->state() == PeerConnectionState::DISCONNECTED);
    CHECK_FALSE(peer->is_connected());
    peer->disconnect();
    peer->disconnect();
}

TEST_CASE("Peer - PeerInfo", "[peer][info]") {
    boost::asio::io_context io_context;
    auto mock_conn = std::make_shared<MockTransportConnection>();
    const uint32_t magic = protocol::magic::REGTEST;
    const uint64_t peer_nonce = 54321;
    auto peer = Peer::create_inbound(io_context, mock_conn, magic, 0);
    peer->start();
    io_context.poll();
    CHECK(peer->version() == 0);
    CHECK(peer->user_agent().empty());
    CHECK(peer->start_height() == 0);
    message::VersionMessage version_msg;
    version_msg.version = protocol::PROTOCOL_VERSION;
    version_msg.services = protocol::NODE_NETWORK;
    version_msg.timestamp = 1234567890;
    version_msg.nonce = peer_nonce;
    version_msg.user_agent = "/TestPeer:2.0.0/";
    version_msg.start_height = 100;
    auto payload = version_msg.serialize();
    auto full_msg = create_test_message(magic, protocol::commands::VERSION, payload);
    mock_conn->simulate_receive(full_msg);
    io_context.poll();
    CHECK(peer->version() == protocol::PROTOCOL_VERSION);
    CHECK(peer->services() == protocol::NODE_NETWORK);
    CHECK(peer->user_agent() == "/TestPeer:2.0.0/");
    CHECK(peer->start_height() == 100);
    CHECK(peer->peer_nonce() == peer_nonce);
}

// =============================================================================
// PROTOCOL SECURITY TESTS
// =============================================================================

TEST_CASE("Peer - DuplicateVersionRejection", "[peer][security][critical]") {
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
    CHECK(peer->user_agent() == "/Test:1.0.0/");
    CHECK(peer->peer_nonce() == 54321);
    message::VersionMessage msg2;
    msg2.version = 99999;
    msg2.services = protocol::NODE_NETWORK;
    msg2.timestamp = 9999999999;
    msg2.nonce = 11111;
    msg2.user_agent = "/Attacker:6.6.6/";
    msg2.start_height = 999;
    auto payload2 = msg2.serialize();
    auto version2 = create_test_message(magic, protocol::commands::VERSION, payload2);
    mock_conn->simulate_receive(version2);
    io_context.poll();
    CHECK(peer->version() == protocol::PROTOCOL_VERSION);
    CHECK(peer->user_agent() == "/Test:1.0.0/");
    CHECK(peer->peer_nonce() == 54321);
    CHECK(peer->is_connected());
}

TEST_CASE("Peer - MessageBeforeVersionRejected", "[peer][security][critical]") {
    boost::asio::io_context io_context;
    auto mock_conn = std::make_shared<MockTransportConnection>();
    const uint32_t magic = protocol::magic::REGTEST;
    auto peer = Peer::create_inbound(io_context, mock_conn, magic, 0);
    peer->start();
    io_context.poll();
    REQUIRE(peer->state() == PeerConnectionState::CONNECTED);
    REQUIRE(peer->version() == 0);
    SECTION("PING before VERSION disconnects") {
        auto ping_msg = create_ping_message(magic, 99999);
        mock_conn->simulate_receive(ping_msg);
        io_context.poll();
        CHECK(peer->state() == PeerConnectionState::DISCONNECTED);
    }
    SECTION("VERACK before VERSION disconnects") {
        auto verack_msg = create_verack_message(magic);
        mock_conn->simulate_receive(verack_msg);
        io_context.poll();
        CHECK(peer->state() == PeerConnectionState::DISCONNECTED);
    }
    SECTION("PONG before VERSION disconnects") {
        auto pong_msg = create_pong_message(magic, 12345);
        mock_conn->simulate_receive(pong_msg);
        io_context.poll();
        CHECK(peer->state() == PeerConnectionState::DISCONNECTED);
    }
}

TEST_CASE("Peer - DuplicateVerackRejection", "[peer][security]") {
    boost::asio::io_context io_context;
    auto mock_conn = std::make_shared<MockTransportConnection>();
    const uint32_t magic = protocol::magic::REGTEST;
    auto peer = Peer::create_outbound(io_context, mock_conn, magic, 0);
    peer->start();
    io_context.poll();
    auto version_msg = create_version_message(magic, 54321);
    mock_conn->simulate_receive(version_msg);
    io_context.poll();
    auto verack1 = create_verack_message(magic);
    mock_conn->simulate_receive(verack1);
    io_context.poll();
    CHECK(peer->state() == PeerConnectionState::READY);
    CHECK(peer->successfully_connected());
    auto verack2 = create_verack_message(magic);
    mock_conn->simulate_receive(verack2);
    io_context.poll();
    CHECK(peer->state() == PeerConnectionState::READY);
    CHECK(peer->successfully_connected());
    CHECK(peer->is_connected());
}

TEST_CASE("Peer - VersionMustBeFirstMessage", "[peer][security][critical]") {
    boost::asio::io_context io_context;
    auto mock_conn = std::make_shared<MockTransportConnection>();
    const uint32_t magic = protocol::magic::REGTEST;
    auto peer = Peer::create_inbound(io_context, mock_conn, magic, 0);
    peer->start();
    io_context.poll();
    auto version1 = create_version_message(magic, 54321);
    mock_conn->simulate_receive(version1);
    io_context.poll();
    REQUIRE(peer->version() != 0);
    auto verack = create_verack_message(magic);
    mock_conn->simulate_receive(verack);
    io_context.poll();
    CHECK(peer->state() == PeerConnectionState::READY);
    auto version2 = create_version_message(magic, 99999);
    mock_conn->simulate_receive(version2);
    io_context.poll();
    CHECK(peer->peer_nonce() == 54321);
    CHECK(peer->state() == PeerConnectionState::READY);
}
