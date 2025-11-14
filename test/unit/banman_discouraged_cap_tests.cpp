// Copyright (c) 2025 The Unicity Foundation
// Unit tests for ConnectionManager discouragement cap functionality

#include "catch_amalgamated.hpp"
#include "network/peer_lifecycle_manager.hpp"
#include "network/addr_manager.hpp"
#include <boost/asio.hpp>
#include <vector>
#include <string>

using namespace unicity::network;

// Test fixture
class DiscouragementTestFixture {
public:
    boost::asio::io_context io_context;

    std::unique_ptr<PeerLifecycleManager> CreatePeerLifecycleManager() {
        // Phase 2: ConnectionManager no longer requires AddressManager at construction
        // DiscoveryManager injection not needed for these ban-focused unit tests
        return std::make_unique<PeerLifecycleManager>(io_context);
    }
};

TEST_CASE("ConnectionManager - Discouragement Cap", "[network][peermgr][ban][unit]") {
    DiscouragementTestFixture fixture;
    auto pm = fixture.CreatePeerLifecycleManager();

    SECTION("Can discourage up to MAX_DISCOURAGED addresses") {
        // MAX_DISCOURAGED = 10000 (from peer_manager.hpp)
        const size_t MAX_DISCOURAGED = 10000;

        // Discourage MAX_DISCOURAGED addresses
        for (size_t i = 0; i < MAX_DISCOURAGED; ++i) {
            std::string addr = "10.0." + std::to_string(i / 256) + "." + std::to_string(i % 256);
            pm->Discourage(addr);
        }

        // Verify first few are discouraged
        REQUIRE(pm->IsDiscouraged("10.0.0.0"));
        REQUIRE(pm->IsDiscouraged("10.0.0.1"));
        REQUIRE(pm->IsDiscouraged("10.0.0.100"));

        // Note: Current implementation may or may not enforce MAX_DISCOURAGED cap
        // This test documents behavior with large numbers of discouragements
    }

    SECTION("SweepDiscouraged removes expired entries") {
        // Discourage a few addresses
        pm->Discourage("192.168.1.1");
        pm->Discourage("192.168.1.2");
        pm->Discourage("192.168.1.3");

        REQUIRE(pm->IsDiscouraged("192.168.1.1"));
        REQUIRE(pm->IsDiscouraged("192.168.1.2"));
        REQUIRE(pm->IsDiscouraged("192.168.1.3"));

        // Note: Discouragement has 24h TTL, so sweep won't remove them immediately
        pm->SweepDiscouraged();

        // Still discouraged (not expired yet)
        REQUIRE(pm->IsDiscouraged("192.168.1.1"));
        REQUIRE(pm->IsDiscouraged("192.168.1.2"));
        REQUIRE(pm->IsDiscouraged("192.168.1.3"));
    }

    SECTION("ClearDiscouraged removes all discouragements") {
        // Discourage many addresses
        for (int i = 0; i < 100; ++i) {
            std::string addr = "10.0.0." + std::to_string(i);
            pm->Discourage(addr);
        }

        REQUIRE(pm->IsDiscouraged("10.0.0.0"));
        REQUIRE(pm->IsDiscouraged("10.0.0.50"));
        REQUIRE(pm->IsDiscouraged("10.0.0.99"));

        // Clear all
        pm->ClearDiscouraged();

        // All should be cleared
        REQUIRE_FALSE(pm->IsDiscouraged("10.0.0.0"));
        REQUIRE_FALSE(pm->IsDiscouraged("10.0.0.50"));
        REQUIRE_FALSE(pm->IsDiscouraged("10.0.0.99"));
    }
}

TEST_CASE("ConnectionManager - Discouragement vs Bans", "[network][peermgr][ban][unit]") {
    DiscouragementTestFixture fixture;
    auto pm = fixture.CreatePeerLifecycleManager();

    SECTION("Discouraged and banned are independent") {
        // Discourage an address
        pm->Discourage("192.168.1.1");
        REQUIRE(pm->IsDiscouraged("192.168.1.1"));
        REQUIRE_FALSE(pm->IsBanned("192.168.1.1"));

        // Ban a different address
        pm->Ban("192.168.1.2", 3600);
        REQUIRE(pm->IsBanned("192.168.1.2"));
        REQUIRE_FALSE(pm->IsDiscouraged("192.168.1.2"));

        // Can have both on same address
        pm->Ban("192.168.1.3", 3600);
        pm->Discourage("192.168.1.3");
        REQUIRE(pm->IsBanned("192.168.1.3"));
        REQUIRE(pm->IsDiscouraged("192.168.1.3"));
    }

    SECTION("Clearing discouraged doesn't affect bans") {
        pm->Ban("192.168.1.1", 3600);
        pm->Discourage("192.168.1.1");

        REQUIRE(pm->IsBanned("192.168.1.1"));
        REQUIRE(pm->IsDiscouraged("192.168.1.1"));

        pm->ClearDiscouraged();

        // Ban persists, discouragement cleared
        REQUIRE(pm->IsBanned("192.168.1.1"));
        REQUIRE_FALSE(pm->IsDiscouraged("192.168.1.1"));
    }

    SECTION("Clearing bans doesn't affect discouragement") {
        pm->Ban("192.168.1.1", 3600);
        pm->Discourage("192.168.1.1");

        REQUIRE(pm->IsBanned("192.168.1.1"));
        REQUIRE(pm->IsDiscouraged("192.168.1.1"));

        pm->ClearBanned();

        // Discouragement persists, ban cleared
        REQUIRE_FALSE(pm->IsBanned("192.168.1.1"));
        REQUIRE(pm->IsDiscouraged("192.168.1.1"));
    }
}
