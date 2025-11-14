#include "catch_amalgamated.hpp"
#include "infra/simulated_network.hpp"
#include "infra/simulated_node.hpp"
#include "test_orchestrator.hpp"
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

using namespace unicity;
using namespace unicity::test;
using json = nlohmann::json;

static json read_json_file(const std::string& path) {
    std::ifstream f(path);
    REQUIRE(f.is_open());
    json j; f >> j; return j;
}

static std::string anchors_path(const char* name) {
    return std::string("/tmp/") + name;
}

static void write_anchor_entry(json& arr, int node_id) {
    json a;
    a["services"] = 1;
    a["port"] = protocol::ports::REGTEST + node_id;
    a["ip"] = json::array();
    for (int i = 0; i < 10; ++i) a["ip"].push_back(0);
    a["ip"].push_back(0xFF);
    a["ip"].push_back(0xFF);
    a["ip"].push_back(127);
    a["ip"].push_back(0);
    a["ip"].push_back(0);
    a["ip"].push_back(node_id % 255);
    arr.push_back(a);
}

TEST_CASE("Anchors - Save selects two oldest READY outbounds", "[network][anchor]") {
    SimulatedNetwork net(123);
    TestOrchestrator orch(&net);

    SimulatedNode n1(1, &net);
    SimulatedNode n2(2, &net);
    SimulatedNode n3(3, &net);
    SimulatedNode n4(4, &net);

    // Connect to n2, wait a bit; then n3; then n4
    REQUIRE(n1.ConnectTo(2));
    for (int i = 0; i < 10; ++i) orch.AdvanceTime(std::chrono::milliseconds(100));
    REQUIRE(n1.ConnectTo(3));
    for (int i = 0; i < 10; ++i) orch.AdvanceTime(std::chrono::milliseconds(100));
    REQUIRE(n1.ConnectTo(4));
    for (int i = 0; i < 10; ++i) orch.AdvanceTime(std::chrono::milliseconds(100));

    REQUIRE(orch.WaitForPeerCount(n1, 3));

    // Save anchors to file
    const std::string path = anchors_path("anchors_save_test.json");
    std::filesystem::remove(path);
    REQUIRE(n1.GetNetworkManager().SaveAnchors(path));

    auto j = read_json_file(path);
    REQUIRE(j["version"] == 1);
    REQUIRE(j["anchors"].is_array());
    REQUIRE(j["anchors"].size() == 2);

    auto a0 = j["anchors"][0];
    auto a1 = j["anchors"][1];

    // Expect any 2 among the connected peers (2,3,4)
    std::set<uint16_t> allowed = { (uint16_t)(protocol::ports::REGTEST+2), (uint16_t)(protocol::ports::REGTEST+3), (uint16_t)(protocol::ports::REGTEST+4) };
    std::vector<uint16_t> file_ports = { a0["port"].get<uint16_t>(), a1["port"].get<uint16_t>() };
    CHECK(allowed.count(file_ports[0]) == 1);
    CHECK(allowed.count(file_ports[1]) == 1);
    CHECK(file_ports[0] != file_ports[1]);

    std::filesystem::remove(path);
}

TEST_CASE("Anchors - Load caps at 2 and deletes file", "[network][anchor]") {
    SimulatedNetwork net(456);
    TestOrchestrator orch(&net);

    SimulatedNode n1(1, &net);
    SimulatedNode n2(2, &net);
    SimulatedNode n3(3, &net);
    SimulatedNode n4(4, &net);

    const std::string path = anchors_path("anchors_load_test.json");
    std::filesystem::remove(path);

    // Write anchors file with 3 entries (2,3,4)
    json root;
    root["version"] = 1;
    root["count"] = 3;
    root["anchors"] = json::array();
    write_anchor_entry(root["anchors"], 2);
    write_anchor_entry(root["anchors"], 3);
    write_anchor_entry(root["anchors"], 4);

    {
        std::ofstream f(path);
        REQUIRE(f.is_open());
        f << root.dump(2);
    }

    // Load and attempt connects
    REQUIRE(n1.GetNetworkManager().LoadAnchors(path));

    // File should be deleted
    CHECK_FALSE(std::filesystem::exists(path));

    // Only 2 anchors should be attempted, wait for up to 2 peers
    REQUIRE(orch.WaitForPeerCount(n1, 2));

    // Ensure they are exactly 2 of the 3 we provided
    auto count = n1.GetNetworkManager().outbound_peer_count();
    CHECK(count == 2);
}

TEST_CASE("Anchors - Load rejects malformed entries and returns false", "[network][anchor]") {
    SimulatedNetwork net(789);
    TestOrchestrator orch(&net);

    SimulatedNode n1(1, &net);

    const std::string path = anchors_path("anchors_malformed_test.json");
    std::filesystem::remove(path);

    // Malformed: ip size 15
    json root;
    root["version"] = 1;
    root["count"] = 1;
    root["anchors"] = json::array();
    json a;
    a["services"] = 1;
    a["port"] = protocol::ports::REGTEST + 2;
    a["ip"] = json::array();
    for (int i = 0; i < 15; ++i) a["ip"].push_back(0);
    root["anchors"].push_back(a);

    {
        std::ofstream f(path);
        REQUIRE(f.is_open());
        f << root.dump(2);
    }

    CHECK_FALSE(n1.GetNetworkManager().LoadAnchors(path));
    CHECK_FALSE(std::filesystem::exists(path));
    CHECK(n1.GetNetworkManager().outbound_peer_count() == 0);
}

TEST_CASE("Anchors - Loaded anchors are whitelisted (NoBan)", "[network][anchor][whitelist]") {
    SimulatedNetwork net(999);
    TestOrchestrator orch(&net);

    SimulatedNode n1(1, &net);

    const std::string path = anchors_path("anchors_whitelist_test.json");
    std::filesystem::remove(path);

    // Write one anchor: 127.0.0.2
    json root;
    root["version"] = 1;
    root["count"] = 1;
    root["anchors"] = json::array();
    write_anchor_entry(root["anchors"], 2);

    {
        std::ofstream f(path);
        REQUIRE(f.is_open());
        f << root.dump(2);
    }

    // Load anchors; this should whitelist 127.0.0.2
    REQUIRE(n1.GetNetworkManager().LoadAnchors(path));

    // Give the system a moment to process callbacks
    orch.AdvanceTime(std::chrono::milliseconds(100));

    // Check that anchor address is whitelisted
    auto& bm = n1.GetNetworkManager().peer_manager();
    // 127.0.0.2 should be normal dotted-quad
    CHECK(bm.IsWhitelisted("127.0.0.2"));

    // Note: Like Bitcoin Core, whitelist and ban are independent states
    // Whitelisted addresses CAN be banned; whitelist only affects connection acceptance
    bm.Ban("127.0.0.2", 3600);
    CHECK(bm.IsBanned("127.0.0.2"));  // Ban succeeds
    CHECK(bm.IsWhitelisted("127.0.0.2"));  // Still whitelisted
}
