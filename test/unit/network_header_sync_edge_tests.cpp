// Copyright (c) 2025 The Unicity Foundation
// Edge-case tests for header synchronization behavior

#include "network_test_helpers.hpp"
#include "chain/validation.hpp"
#include "chain/chainparams.hpp"
#include "network/message.hpp"
#include "test_orchestrator.hpp"
#include "network/peer_lifecycle_manager.hpp"
#include <cstring>

using namespace unicity;
using namespace unicity::test;

TEST_CASE("HeaderSync - IBD selects single sync peer among many", "[network_header_sync][edge]") {
    SimulatedNetwork net(51001);
    SetZeroLatency(net);
    net.EnableCommandTracking(true);

    // Victim in IBD
    SimulatedNode victim(1, &net);
    victim.SetBypassPOWValidation(true);

    // Connect many peers
    const int K = 6;
    std::vector<std::unique_ptr<SimulatedNode>> peers;
    for (int i = 0; i < K; ++i) {
        peers.push_back(std::make_unique<SimulatedNode>(10 + i, &net));
        victim.ConnectTo(peers.back()->GetId());
    }

    // Allow selection passes
    for (int i = 0; i < 50; ++i) {
        victim.GetNetworkManager().test_hook_check_initial_sync();
        net.AdvanceTime(net.GetCurrentTime() + 200);
    }

    // Exactly one outbound peer should have received GETHEADERS during IBD
    int distinct = net.CountDistinctPeersSent(victim.GetId(), protocol::commands::GETHEADERS);
    CHECK(distinct == 1);
}

TEST_CASE("HeaderSync - Genesis locator uses tip when no pprev (pprev trick fallback)", "[network_header_sync][edge]") {
    SimulatedNetwork net(51002);
    SetZeroLatency(net);
    net.EnableCommandTracking(true);

    // Victim at genesis
    SimulatedNode victim(20, &net);
    SimulatedNode peer(21, &net);

    victim.ConnectTo(peer.GetId());
    net.AdvanceTime(200);
    victim.GetNetworkManager().test_hook_check_initial_sync();
    net.AdvanceTime(200);

    auto payloads = net.GetCommandPayloads(victim.GetId(), peer.GetId(), protocol::commands::GETHEADERS);
    for (int i = 0; i < 10 && payloads.empty(); ++i) {
        net.AdvanceTime(net.GetCurrentTime() + 200);
        payloads = net.GetCommandPayloads(victim.GetId(), peer.GetId(), protocol::commands::GETHEADERS);
    }
    REQUIRE_FALSE(payloads.empty());

    message::GetHeadersMessage gh;
    REQUIRE(gh.deserialize(payloads.back().data(), payloads.back().size()));
    REQUIRE_FALSE(gh.block_locator_hashes.empty());

    // At genesis (no pprev), first locator should be genesis
    uint256 first;
    std::memcpy(first.data(), gh.block_locator_hashes.front().data(), 32);
    CHECK(first == unicity::chain::GlobalChainParams::Get().GenesisBlock().GetHash());
}

TEST_CASE("HeaderSync - Repeated empty HEADERS from sync peer does not thrash selection", "[network_header_sync][edge]") {
    SimulatedNetwork net(51003);
    SetZeroLatency(net);
    net.EnableCommandTracking(true);

    SimulatedNode p1(30, &net);
    SimulatedNode p2(31, &net);
    SimulatedNode victim(32, &net);

    victim.ConnectTo(p1.GetId());
    net.AdvanceTime(200);
    victim.GetNetworkManager().test_hook_check_initial_sync();
    net.AdvanceTime(200);
    victim.ConnectTo(p2.GetId());
    net.AdvanceTime(200);

    // Send multiple empty HEADERS from current sync peer (p1), triggering reselection
    for (int i = 0; i < 3; ++i) {
        message::HeadersMessage empty; auto payload = empty.serialize();
        auto hdr = message::create_header(protocol::magic::REGTEST, protocol::commands::HEADERS, payload);
        auto hdr_bytes = message::serialize_header(hdr);
        std::vector<uint8_t> full; full.reserve(hdr_bytes.size()+payload.size());
        full.insert(full.end(), hdr_bytes.begin(), hdr_bytes.end());
        full.insert(full.end(), payload.begin(), payload.end());
        net.SendMessage(p1.GetId(), victim.GetId(), full);
        net.AdvanceTime(net.GetCurrentTime() + 200);
        victim.GetNetworkManager().test_hook_check_initial_sync();
        net.AdvanceTime(net.GetCurrentTime() + 200);
    }

    // Should have solicited headers, but not from many peers
    int distinct = net.CountDistinctPeersSent(victim.GetId(), protocol::commands::GETHEADERS);
    CHECK(distinct <= 2);
}

TEST_CASE("HeaderSync - Unconnecting headers threshold triggers discouragement & cleanup", "[network_header_sync][edge]") {
    SimulatedNetwork net(51004);
    SetZeroLatency(net);
    net.EnableCommandTracking(true);

    SimulatedNode victim(40, &net);
    SimulatedNode bad(41, &net);

    // Inbound bad peer connects to victim
    bad.ConnectTo(victim.GetId());
    net.AdvanceTime(200);

    // Build a small headers batch that does NOT connect to known chain
    auto make_unconnecting_headers = [&]() {
        std::vector<CBlockHeader> headers; headers.reserve(2);
        uint256 bogus_prev; bogus_prev.SetHex("deadbeef00000000000000000000000000000000000000000000000000000000");
        uint32_t nBits = unicity::chain::GlobalChainParams::Get().GenesisBlock().nBits;
        uint32_t t0 = static_cast<uint32_t>(net.GetCurrentTime() / 1000);
        for (int i = 0; i < 2; ++i) {
            CBlockHeader h; h.nVersion=1; h.hashPrevBlock = (i==0? bogus_prev : headers.back().GetHash()); h.nTime=t0+i+1; h.nBits=nBits; h.nNonce=i+1;
            h.hashRandomX.SetHex("0000000000000000000000000000000000000000000000000000000000000000");
            headers.push_back(h);
        }
        return headers;
    };

    auto send_headers = [&](const std::vector<CBlockHeader>& hs){
        message::HeadersMessage m; m.headers = hs; auto payload = m.serialize();
        auto hdr = message::create_header(protocol::magic::REGTEST, protocol::commands::HEADERS, payload);
        auto hdr_bytes = message::serialize_header(hdr);
        std::vector<uint8_t> full; full.reserve(hdr_bytes.size()+payload.size());
        full.insert(full.end(), hdr_bytes.begin(), hdr_bytes.end());
        full.insert(full.end(), payload.begin(), payload.end());
        net.SendMessage(bad.GetId(), victim.GetId(), full);
    };

    // Send more than MAX_UNCONNECTING_HEADERS messages
    for (int i = 0; i < unicity::network::MAX_UNCONNECTING_HEADERS + 1; ++i) {
        send_headers(make_unconnecting_headers());
        net.AdvanceTime(net.GetCurrentTime() + 200);
        victim.GetNetworkManager().test_hook_check_initial_sync();
        // Periodic processing applies discouragement removal
        victim.GetNetworkManager().peer_manager().process_periodic();
    }

    // Expect peer count dropped (bad peer disconnected)
    CHECK(victim.GetPeerCount() == 0);
}

TEST_CASE("HeaderSync - Oversized HEADERS clears sync and we reselect another peer", "[network_header_sync][edge]") {
    SimulatedNetwork net(51005);
    SetZeroLatency(net);
    net.EnableCommandTracking(true);

    // Two serving peers
    SimulatedNode p1(50, &net);
    SimulatedNode p2(51, &net);

    // Victim connects to both, selects one as sync
    SimulatedNode victim(52, &net);
    victim.ConnectTo(p1.GetId());
    net.AdvanceTime(200);
    victim.GetNetworkManager().test_hook_check_initial_sync();
    net.AdvanceTime(200);
    victim.ConnectTo(p2.GetId());
    net.AdvanceTime(200);

    // Craft oversized HEADERS from current sync peer (p1): MAX+1 headers
    const size_t N = protocol::MAX_HEADERS_SIZE + 1;
    std::vector<CBlockHeader> headers; headers.reserve(N);
    uint256 prev = victim.GetTipHash();
    uint32_t nBits = unicity::chain::GlobalChainParams::Get().GenesisBlock().nBits;
    uint32_t t0 = static_cast<uint32_t>(net.GetCurrentTime() / 1000);
    for (size_t i = 0; i < N; ++i) { CBlockHeader h; h.nVersion=1; h.hashPrevBlock=prev; h.nTime=t0+i+1; h.nBits=nBits; h.nNonce=i+1; h.hashRandomX.SetHex("0000000000000000000000000000000000000000000000000000000000000000"); headers.push_back(h); prev=h.GetHash(); }

    message::HeadersMessage m; m.headers = headers; auto payload = m.serialize();
    auto hdr = message::create_header(protocol::magic::REGTEST, protocol::commands::HEADERS, payload);
    auto hdr_bytes = message::serialize_header(hdr);
    std::vector<uint8_t> full; full.reserve(hdr_bytes.size()+payload.size());
    full.insert(full.end(), hdr_bytes.begin(), hdr_bytes.end());
    full.insert(full.end(), payload.begin(), payload.end());

    // Send from p1 to victim (oversized)
    net.SendMessage(p1.GetId(), victim.GetId(), full);

    // Allow processing and reselection
    for (int i = 0; i < 20; ++i) {
        net.AdvanceTime(net.GetCurrentTime() + 200);
        victim.GetNetworkManager().test_hook_check_initial_sync();
    }

    // Verify we solicited p2 with GETHEADERS after clearing sync
    auto gh_p2 = net.GetCommandPayloads(victim.GetId(), p2.GetId(), protocol::commands::GETHEADERS);
    REQUIRE_FALSE(gh_p2.empty());
}

TEST_CASE("HeaderSync - Empty headers response when no common blocks (genesis block fix)", "[network_header_sync][edge][genesis]") {
    // Tests the fix for genesis block handling bug where code would skip genesis
    // when no common blocks found. Now should send empty HEADERS (matches Bitcoin Core).
    SimulatedNetwork net(51006);
    SetZeroLatency(net);
    net.EnableCommandTracking(true);

    SimulatedNode node1(1, &net);
    SimulatedNode node2(2, &net);

    // Connect nodes
    node2.ConnectTo(node1.GetId());
    net.AdvanceTime(500);

    // Wait for handshake to complete
    for (int i = 0; i < 20; ++i) {
        net.AdvanceTime(net.GetCurrentTime() + 100);
    }
    REQUIRE(node1.GetPeerCount() > 0);

    // Node2 sends GETHEADERS with locator containing unknown blocks
    // (simulates peer on different network or with no common history)
    message::GetHeadersMessage gh;
    gh.version = protocol::PROTOCOL_VERSION;

    // Create locator with fake hashes that don't exist in node1's chain
    uint256 fake_hash1, fake_hash2;
    fake_hash1.SetHex("deadbeef00000000000000000000000000000000000000000000000000000000");
    fake_hash2.SetHex("cafebabe00000000000000000000000000000000000000000000000000000000");

    std::array<uint8_t, 32> hash_array1, hash_array2;
    std::memcpy(hash_array1.data(), fake_hash1.data(), 32);
    std::memcpy(hash_array2.data(), fake_hash2.data(), 32);

    gh.block_locator_hashes.push_back(hash_array1);
    gh.block_locator_hashes.push_back(hash_array2);
    gh.hash_stop.fill(0);

    // Serialize and send GETHEADERS
    auto payload = gh.serialize();
    auto hdr = message::create_header(protocol::magic::REGTEST,
                                       protocol::commands::GETHEADERS, payload);
    auto hdr_bytes = message::serialize_header(hdr);
    std::vector<uint8_t> full;
    full.reserve(hdr_bytes.size() + payload.size());
    full.insert(full.end(), hdr_bytes.begin(), hdr_bytes.end());
    full.insert(full.end(), payload.begin(), payload.end());
    net.SendMessage(node2.GetId(), node1.GetId(), full);

    net.AdvanceTime(net.GetCurrentTime() + 500);

    // Verify node1 sent HEADERS response
    auto responses = net.GetCommandPayloads(node1.GetId(), node2.GetId(),
                                             protocol::commands::HEADERS);
    REQUIRE_FALSE(responses.empty());

    // Parse response
    message::HeadersMessage response;
    REQUIRE(response.deserialize(responses.back().data(), responses.back().size()));

    // Should be EMPTY (matches Bitcoin Core behavior)
    // OLD BUG: Would send from genesis+1, skipping genesis
    // NEW FIX: Sends empty headers
    CHECK(response.headers.empty());

    // Should NOT disconnect peer (this is a valid edge case, not an attack)
    CHECK(node1.GetPeerCount() > 0);
}
