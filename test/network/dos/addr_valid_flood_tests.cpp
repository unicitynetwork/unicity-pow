#include "catch_amalgamated.hpp"
#include "infra/simulated_network.hpp"
#include "infra/simulated_node.hpp"
#include "network/protocol.hpp"
#include "network/message.hpp"

using namespace unicity;
using namespace unicity::test;
using namespace unicity::protocol;

TEST_CASE("DoS: Valid-size ADDR flood remains connected", "[dos][addr]") {
    SimulatedNetwork net(57001);
    SimulatedNode victim(1, &net);
    SimulatedNode sender(2, &net);

    REQUIRE(sender.ConnectTo(victim.GetId()));
    uint64_t t=100; net.AdvanceTime(t);

    // Build a valid-size ADDR (MAX_ADDR_SIZE entries)
    message::AddrMessage addr;
    addr.addresses.reserve(MAX_ADDR_SIZE);
    for (uint32_t i=0;i<MAX_ADDR_SIZE;i++) {
        protocol::TimestampedAddress ta;
        ta.timestamp = (uint32_t)(net.GetCurrentTime()/1000);
        // 127.0.0.(i%255)
        auto ip = boost::asio::ip::make_address_v6(
            boost::asio::ip::v4_mapped,
            boost::asio::ip::address_v4{static_cast<boost::asio::ip::address_v4::uint_type>((127u<<24)|(0u<<16)|(0u<<8)|(i%255))}
        );
        auto bytes = ip.to_bytes();
        std::copy(bytes.begin(), bytes.end(), ta.address.ip.begin());
        ta.address.services = protocol::ServiceFlags::NODE_NETWORK;
        ta.address.port = ports::REGTEST;
        addr.addresses.push_back(ta);
    }
    auto payload = addr.serialize();
    auto hdr = message::create_header(magic::REGTEST, commands::ADDR, payload);
    auto hdr_bytes = message::serialize_header(hdr);
    std::vector<uint8_t> full; full.reserve(hdr_bytes.size()+payload.size());
    full.insert(full.end(), hdr_bytes.begin(), hdr_bytes.end());
    full.insert(full.end(), payload.begin(), payload.end());

    // Send many ADDR messages (10x)
    int pre_count = victim.GetPeerCount();
    for (int k=0;k<10;k++) {
        net.SendMessage(sender.GetId(), victim.GetId(), full);
        t+=50; net.AdvanceTime(t);
    }

    // Still connected and responsive
    REQUIRE(victim.GetPeerCount()==pre_count);
}
