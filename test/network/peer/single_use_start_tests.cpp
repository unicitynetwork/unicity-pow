#include "catch_amalgamated.hpp"
#include "network/peer.hpp"
#include "infra/mock_transport.hpp"
#include "util/logging.hpp"
#include <boost/asio.hpp>
#include <memory>
#include <sstream>
#include <spdlog/sinks/ostream_sink.h>

using namespace unicity;
using namespace unicity::network;

TEST_CASE("Peer start() is single-use: duplicate and restart attempts are ignored", "[peer][lifecycle][single_use]") {
    // Outbound peer with an open mock connection starts in CONNECTED state
    boost::asio::io_context io;
    auto conn = std::make_shared<MockTransportConnection>();
    const uint32_t magic = protocol::magic::REGTEST;
    auto peer = Peer::create_outbound(io, conn, magic, /*start_height=*/0);

    // Attach a temporary sink to capture network logs
    auto net_logger = unicity::util::LogManager::GetLogger("network");
    auto old_level = net_logger->level();
    // Only capture error-level logs to avoid heavy TRACE logging overhead
    net_logger->set_level(spdlog::level::err);
    std::ostringstream oss;
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(oss);
    sink->set_level(spdlog::level::err);
    auto& sinks = net_logger->sinks();
    sinks.push_back(sink);

    // 1) First start() should send exactly one VERSION
    peer->start();
    io.poll();
    io.restart();
    REQUIRE(conn->sent_message_count() == 1);

    // 2) Second start() while still connected should be ignored (no extra send)
    peer->start();
    io.poll();
    io.restart();
    CHECK(conn->sent_message_count() == 1);

    // 3) After disconnect, start() again should be rejected and log an error mentioning single-use
    peer->disconnect();
    io.poll();
    io.restart();
    REQUIRE(peer->state() == PeerConnectionState::DISCONNECTED);

    peer->start();
    io.poll();
    io.restart();

    // Verify error log emitted
    auto logs = oss.str();
    CHECK(logs.find("single-use") != std::string::npos);

    // Cleanup: remove our sink and restore level so other tests are unaffected
    sinks.pop_back();
    net_logger->set_level(old_level);
}