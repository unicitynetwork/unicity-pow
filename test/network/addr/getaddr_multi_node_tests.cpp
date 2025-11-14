#include "catch_amalgamated.hpp"
#include "infra/simulated_network.hpp"
#include "infra/simulated_node.hpp"
#include "network/message.hpp"
#include "network/network_manager.hpp"
#include "network/peer_discovery_manager.hpp"
#include "test_orchestrator.hpp"
#include <cstring>

using namespace unicity;
using namespace unicity::test;
using namespace unicity::protocol;

static message::AddrMessage MakeAddrMsgIPv4(const std::string& ip_v4, uint16_t port, uint32_t ts)
{
    message::AddrMessage msg;
    protocol::TimestampedAddress ta;
    ta.timestamp = ts;
    ta.address.services = protocol::ServiceFlags::NODE_NETWORK;
    ta.address.port = port;
    std::memset(ta.address.ip.data(), 0, 10);
    ta.address.ip[10] = 0xFF; ta.address.ip[11] = 0xFF;
    int a,b,c,d; if (sscanf(ip_v4.c_str(), "%d.%d.%d.%d", &a,&b,&c,&d)==4) {
        ta.address.ip[12] = (uint8_t)a; ta.address.ip[13]=(uint8_t)b; ta.address.ip[14]=(uint8_t)c; ta.address.ip[15]=(uint8_t)d;
    }
    msg.addresses.push_back(ta);
    return msg;
}

static std::vector<uint8_t> MakeWire(const std::string& cmd, const std::vector<uint8_t>& payload) {
    auto hdr = message::create_header(magic::REGTEST, cmd, payload);
    auto hdr_bytes = message::serialize_header(hdr);
    std::vector<uint8_t> full; full.reserve(hdr_bytes.size()+payload.size());
    full.insert(full.end(), hdr_bytes.begin(), hdr_bytes.end());
    full.insert(full.end(), payload.begin(), payload.end());
    return full;
}

static std::string key_of(const protocol::NetworkAddress& a) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u:%u", a.ip[12], a.ip[13], a.ip[14], a.ip[15], a.port);
    return std::string(buf);
}

TEST_CASE("Multi-node: cross-peer echo suppression and inclusion", "[network][addr][multi]") {
    SimulatedNetwork net(49001);
    TestOrchestrator orch(&net);
    net.EnableCommandTracking(true);

    SimulatedNode A(1, &net); // server
    SimulatedNode B(2, &net); // client 1
    SimulatedNode C(3, &net); // client 2 (source of X)
    SimulatedNode D(4, &net); // client 3

    // Connect C first so A learns X before serving other peers
    REQUIRE(C.ConnectTo(A.GetId()));
    REQUIRE(orch.WaitForConnection(A, C));
    for (int i=0;i<12;++i) orch.AdvanceTime(std::chrono::milliseconds(100));

    // C announces X to A
    auto now_s = (uint32_t)(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    auto addr_msg = MakeAddrMsgIPv4("10.1.1.42", ports::REGTEST, now_s);
    auto payload = addr_msg.serialize();
    net.SendMessage(C.GetId(), A.GetId(), MakeWire(commands::ADDR, payload));
    for (int i=0;i<6;++i) orch.AdvanceTime(std::chrono::milliseconds(100));

    std::string X_key = "10.1.1.42:" + std::to_string(ports::REGTEST);

    // Now connect B and D; their auto GETADDRs should be served including X
    REQUIRE(B.ConnectTo(A.GetId()));
    REQUIRE(D.ConnectTo(A.GetId()));
    REQUIRE(orch.WaitForConnection(A, B));
    REQUIRE(orch.WaitForConnection(A, D));
    for (int i=0;i<12;++i) orch.AdvanceTime(std::chrono::milliseconds(100));

    auto pb = net.GetCommandPayloads(A.GetId(), B.GetId(), commands::ADDR);
    REQUIRE_FALSE(pb.empty());
    message::AddrMessage respB; REQUIRE(respB.deserialize(pb.back().data(), pb.back().size()));
    bool found_in_B = false; for (const auto& ta : respB.addresses) { if (key_of(ta.address) == X_key) { found_in_B = true; break; } }
    REQUIRE(found_in_B);

    auto pd = net.GetCommandPayloads(A.GetId(), D.GetId(), commands::ADDR);
    REQUIRE_FALSE(pd.empty());
}

TEST_CASE("Multi-node: once-per-connection across multiple peers", "[network][addr][multi][ratelimit]") {
    SimulatedNetwork net(49002);
    TestOrchestrator orch(&net);
    net.EnableCommandTracking(true);

    SimulatedNode A(1, &net);
    SimulatedNode P2(2, &net);
    SimulatedNode P3(3, &net);
    SimulatedNode P4(4, &net);

    REQUIRE(P2.ConnectTo(A.GetId()));
    REQUIRE(P3.ConnectTo(A.GetId()));
    REQUIRE(P4.ConnectTo(A.GetId()));
    REQUIRE(orch.WaitForConnection(A, P2));
    REQUIRE(orch.WaitForConnection(A, P3));
    REQUIRE(orch.WaitForConnection(A, P4));
    for (int i=0;i<12;++i) orch.AdvanceTime(std::chrono::milliseconds(100));

    // First round
    net.SendMessage(P2.GetId(), A.GetId(), MakeWire(commands::GETADDR, {}));
    net.SendMessage(P3.GetId(), A.GetId(), MakeWire(commands::GETADDR, {}));
    net.SendMessage(P4.GetId(), A.GetId(), MakeWire(commands::GETADDR, {}));
    for (int i=0;i<6;++i) orch.AdvanceTime(std::chrono::milliseconds(100));
    REQUIRE(net.CountCommandSent(A.GetId(), P2.GetId(), commands::ADDR) == 1);
    REQUIRE(net.CountCommandSent(A.GetId(), P3.GetId(), commands::ADDR) == 1);
    REQUIRE(net.CountCommandSent(A.GetId(), P4.GetId(), commands::ADDR) == 1);

    // Second round on same connections (should be ignored)
    net.SendMessage(P2.GetId(), A.GetId(), MakeWire(commands::GETADDR, {}));
    net.SendMessage(P3.GetId(), A.GetId(), MakeWire(commands::GETADDR, {}));
    net.SendMessage(P4.GetId(), A.GetId(), MakeWire(commands::GETADDR, {}));
    for (int i=0;i<6;++i) orch.AdvanceTime(std::chrono::milliseconds(100));
    REQUIRE(net.CountCommandSent(A.GetId(), P2.GetId(), commands::ADDR) == 1);
    REQUIRE(net.CountCommandSent(A.GetId(), P3.GetId(), commands::ADDR) == 1);
    REQUIRE(net.CountCommandSent(A.GetId(), P4.GetId(), commands::ADDR) == 1);
}

TEST_CASE("Multi-node: composition counters under mixed sources", "[network][addr][multi][composition]") {
    SimulatedNetwork net(49003);
    TestOrchestrator orch(&net);
    net.EnableCommandTracking(true);

    SimulatedNode A(1, &net);
    SimulatedNode B(2, &net);
    SimulatedNode C(3, &net);

    // Prefill AddrMan with some entries
    auto& am = A.GetNetworkManager().discovery_manager();
    for (int i = 0; i < 5; ++i) {
        NetworkAddress a; a.services = NODE_NETWORK; a.port = 9590;
        for (int j=0;j<10;++j) a.ip[j]=0; a.ip[10]=0xFF; a.ip[11]=0xFF;
        a.ip[12] = 127; a.ip[13] = 0; a.ip[14] = 3; a.ip[15] = static_cast<uint8_t>(50+i);
        am.Add(a);
    }

    // Connect B and C
    REQUIRE(B.ConnectTo(A.GetId()));
    REQUIRE(C.ConnectTo(A.GetId()));
    REQUIRE(orch.WaitForConnection(A, B));
    REQUIRE(orch.WaitForConnection(A, C));
    for (int i=0;i<12;++i) orch.AdvanceTime(std::chrono::milliseconds(100));

    // Learn two addresses via C
    auto now_s = (uint32_t)(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    auto mX = MakeAddrMsgIPv4("10.2.2.21", ports::REGTEST, now_s);
    auto mY = MakeAddrMsgIPv4("10.2.2.22", ports::REGTEST, now_s);
    net.SendMessage(C.GetId(), A.GetId(), MakeWire(commands::ADDR, mX.serialize()));
    net.SendMessage(C.GetId(), A.GetId(), MakeWire(commands::ADDR, mY.serialize()));
    for (int i=0;i<6;++i) orch.AdvanceTime(std::chrono::milliseconds(100));

    // Request from B, check composition stats
    net.SendMessage(B.GetId(), A.GetId(), MakeWire(commands::GETADDR, {}));
    for (int i=0;i<6;++i) orch.AdvanceTime(std::chrono::milliseconds(100));

    auto stats = A.GetNetworkManager().discovery_manager_for_test().GetGetAddrDebugStats();
    REQUIRE((stats.last_from_recent + stats.last_from_addrman + stats.last_from_learned) > 0);
    REQUIRE(stats.last_from_addrman >= 1);
}
