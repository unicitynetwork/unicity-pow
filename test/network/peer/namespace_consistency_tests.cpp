#include "catch_amalgamated.hpp"
#include "network/peer.hpp"
#include "network/protocol.hpp"
#include "infra/mock_transport.hpp"
#include <boost/asio.hpp>
#include <vector>

using namespace unicity;
using namespace unicity::network;

namespace {
static std::vector<uint8_t> mk_message(uint32_t magic, const std::string& cmd, const std::vector<uint8_t>& payload) {
    auto header = ::unicity::message::create_header(magic, cmd, payload);
    auto header_bytes = ::unicity::message::serialize_header(header);
    std::vector<uint8_t> out;
    out.reserve(header_bytes.size() + payload.size());
    out.insert(out.end(), header_bytes.begin(), header_bytes.end());
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}
}

TEST_CASE("Namespace consistency: fully-qualified ::unicity::message works end-to-end", "[peer][namespace]") {
    boost::asio::io_context io;
    auto conn = std::make_shared<MockTransportConnection>();
    // Inbound peer expects VERSION from remote first
    auto peer = Peer::create_inbound(io, conn, protocol::magic::REGTEST, /*height=*/0);
    peer->start();
    io.poll();

    // Build VERSION using fully-qualified ::unicity::message namespace
    ::unicity::message::VersionMessage ver;
    ver.version = protocol::PROTOCOL_VERSION;
    ver.services = protocol::NODE_NETWORK;
    ver.timestamp = 123456789;
    ver.addr_recv = protocol::NetworkAddress::from_string("127.0.0.1", protocol::ports::REGTEST);
    ver.addr_from = protocol::NetworkAddress();
    ver.nonce = 42;
    ver.user_agent = "/Test:ns/";
    ver.start_height = 0;

    auto ver_payload = ver.serialize();
    auto ver_bytes = mk_message(protocol::magic::REGTEST, protocol::commands::VERSION, ver_payload);

    conn->simulate_receive(ver_bytes);
    io.poll();

    // Now send VERACK using fully-qualified type as well
    ::unicity::message::VerackMessage verack;
    auto verack_payload = verack.serialize();
    auto verack_bytes = mk_message(protocol::magic::REGTEST, protocol::commands::VERACK, verack_payload);
    conn->simulate_receive(verack_bytes);
    io.poll();

    CHECK(peer->state() == PeerConnectionState::READY);
    CHECK(peer->successfully_connected());
}