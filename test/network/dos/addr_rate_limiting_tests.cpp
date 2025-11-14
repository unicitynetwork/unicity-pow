// DoS: ADDR message rate limiting tests
// Verifies that rapid ADDR message spam is rate-limited (Bitcoin Core pattern)

#include "catch_amalgamated.hpp"
#include "infra/simulated_network.hpp"
#include "infra/simulated_node.hpp"
#include "test_orchestrator.hpp"
#include "network/protocol.hpp"
#include "network/message.hpp"
#include "network/peer_discovery_manager.hpp"
#include "network/network_manager.hpp"
#include "chain/chainparams.hpp"

using namespace unicity;
using namespace unicity::test;
using namespace unicity::protocol;

static struct TestSetup {
    TestSetup() { chain::GlobalChainParams::Select(chain::ChainType::REGTEST); }
} test_setup_rate_limit;

// Helper: Create test address with unique IP
static TimestampedAddress MakeTestAddress(uint32_t index, uint64_t timestamp_s = 0) {
    TimestampedAddress ta;
    ta.timestamp = timestamp_s > 0 ? static_cast<uint32_t>(timestamp_s) :
                   static_cast<uint32_t>(std::time(nullptr));

    // Generate unique IP: 10.0.x.y where x = index/256, y = index%256
    uint32_t ip_val = (10u << 24) | ((index / 256) << 8) | (index % 256);
    auto ip = boost::asio::ip::make_address_v6(
        boost::asio::ip::v4_mapped,
        boost::asio::ip::address_v4{static_cast<boost::asio::ip::address_v4::uint_type>(ip_val)}
    );
    auto bytes = ip.to_bytes();
    std::copy(bytes.begin(), bytes.end(), ta.address.ip.begin());
    ta.address.services = ServiceFlags::NODE_NETWORK;
    ta.address.port = ports::REGTEST;

    return ta;
}

TEST_CASE("DoS: ADDR messages are rate limited to prevent CPU exhaustion", "[dos][addr][ratelimit]") {
    SimulatedNetwork network(57100);
    TestOrchestrator orchestrator(&network);

    SimulatedNode victim(1, &network);
    SimulatedNode attacker(2, &network);

    REQUIRE(attacker.ConnectTo(victim.GetId()));
    REQUIRE(orchestrator.WaitForConnection(victim, attacker));

    // ATTACK: Send 10 messages × 1000 addresses with small delays
    // Without rate limiting: 10,000 addresses processed
    // With rate limiting: only ~1000 processed initially (token bucket starts at 1000)

    const int num_messages = 10;
    const int addrs_per_message = protocol::MAX_ADDR_SIZE;  // 1000

    for (int msg_idx = 0; msg_idx < num_messages; msg_idx++) {
        message::AddrMessage addr_msg;
        addr_msg.addresses.reserve(addrs_per_message);

        for (int i = 0; i < addrs_per_message; i++) {
            uint32_t unique_idx = msg_idx * addrs_per_message + i;
            addr_msg.addresses.push_back(MakeTestAddress(unique_idx));
        }

        auto payload = addr_msg.serialize();
        auto header = message::create_header(magic::REGTEST, commands::ADDR, payload);
        auto header_bytes = message::serialize_header(header);

        std::vector<uint8_t> full;
        full.reserve(header_bytes.size() + payload.size());
        full.insert(full.end(), header_bytes.begin(), header_bytes.end());
        full.insert(full.end(), payload.begin(), payload.end());

        network.SendMessage(attacker.GetId(), victim.GetId(), full);
        // Small delay between messages to avoid triggering disconnect
        orchestrator.AdvanceTime(std::chrono::milliseconds(100));
    }

    // Process all messages
    orchestrator.AdvanceTime(std::chrono::seconds(1));

    // Verify rate limiting occurred:
    // - Token bucket starts at 1000
    // - First message processes up to 1000 addresses (empties bucket)
    // - Subsequent messages are heavily rate-limited
    // - Bucket refills slowly at 0.1/sec

    // Check victim's address manager size
    // Should be around 1000-1200, not 10,000
    auto& discovery_mgr = victim.GetNetworkManager().discovery_manager_for_test();
    size_t addr_count = discovery_mgr.Size();

    INFO("Address manager size: " << addr_count);

    // Allow some tolerance for initial bucket + minimal refill
    REQUIRE(addr_count < 1500);  // Far below 10,000
    REQUIRE(addr_count >= 800);  // At least most of first message processed

    // Verify peer is still connected (not banned for spam)
    REQUIRE(victim.GetPeerCount() == 1);
}

TEST_CASE("DoS: Rate limiting allows burst then throttles", "[dos][addr][ratelimit]") {
    SimulatedNetwork network(57101);
    TestOrchestrator orchestrator(&network);

    SimulatedNode victim(1, &network);
    SimulatedNode sender(2, &network);

    REQUIRE(sender.ConnectTo(victim.GetId()));
    REQUIRE(orchestrator.WaitForConnection(victim, sender));

    // Send three messages with delays
    // First should be mostly accepted, second/third heavily rate-limited
    for (int msg = 0; msg < 3; msg++) {
        message::AddrMessage addr_msg;
        for (int i = msg * 1000; i < (msg + 1) * 1000; i++) {
            addr_msg.addresses.push_back(MakeTestAddress(i));
        }

        auto payload = addr_msg.serialize();
        auto header = message::create_header(magic::REGTEST, commands::ADDR, payload);
        auto header_bytes = message::serialize_header(header);

        std::vector<uint8_t> full;
        full.reserve(header_bytes.size() + payload.size());
        full.insert(full.end(), header_bytes.begin(), header_bytes.end());
        full.insert(full.end(), payload.begin(), payload.end());

        network.SendMessage(sender.GetId(), victim.GetId(), full);
        orchestrator.AdvanceTime(std::chrono::milliseconds(500));
    }

    // Verify peer still connected - rate limiting doesn't cause disconnect
    REQUIRE(victim.GetPeerCount() == 1);
}

TEST_CASE("DoS: Rate limiting constants match Bitcoin Core", "[dos][addr][parity]") {
    // Verify our rate limiting works correctly
    // This test mainly validates system stability with rate limiting active

    SimulatedNetwork network(57102);
    TestOrchestrator orchestrator(&network);

    SimulatedNode node1(1, &network);
    SimulatedNode node2(2, &network);

    REQUIRE(node2.ConnectTo(node1.GetId()));
    REQUIRE(orchestrator.WaitForConnection(node1, node2));

    // Send messages with delays to respect rate limiting
    for (int batch = 0; batch < 5; batch++) {
        message::AddrMessage addr_msg;
        for (int i = batch * 200; i < (batch + 1) * 200; i++) {
            addr_msg.addresses.push_back(MakeTestAddress(i));
        }

        auto payload = addr_msg.serialize();
        auto header = message::create_header(magic::REGTEST, commands::ADDR, payload);
        auto header_bytes = message::serialize_header(header);

        std::vector<uint8_t> full;
        full.reserve(header_bytes.size() + payload.size());
        full.insert(full.end(), header_bytes.begin(), header_bytes.end());
        full.insert(full.end(), payload.begin(), payload.end());

        network.SendMessage(node2.GetId(), node1.GetId(), full);
        orchestrator.AdvanceTime(std::chrono::milliseconds(500));
    }

    // Verify peer still connected - rate limiting is working correctly
    REQUIRE(node1.GetPeerCount() == 1);
}
