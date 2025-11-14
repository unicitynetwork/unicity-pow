// Copyright (c) 2025 The Unicity Foundation
// Distributed under the MIT software license
// Unit tests for network-adjusted time (timedata)

#include "chain/timedata.hpp"
#include "network/protocol.hpp"
#include "util/time.hpp"
#include "catch_amalgamated.hpp"
#include <ctime>

using namespace unicity::chain;
using namespace unicity::protocol;

// Helper to add N peers with given offsets
static void AddPeers(const std::vector<int64_t>& offsets) {
    for (size_t i = 0; i < offsets.size(); i++) {
        // Create unique NetworkAddress for each peer (use different IPs)
        NetworkAddress addr = NetworkAddress::from_string(
            "192.168.1." + std::to_string(i), 8333, NODE_NETWORK);
        AddTimeData(addr, offsets[i]);
    }
}

TEST_CASE("TimeData - Initial state", "[timedata]") {
    TestOnlyResetTimeData();
    REQUIRE(GetTimeOffset() == 0);
}

TEST_CASE("TimeData - Need 4 peers to get first update", "[timedata]") {
    TestOnlyResetTimeData();

    // CMedianFilter starts with initial 0
    // Add 4 peers: filter has [0, 10, 12, 15, 20] = 5 elements (odd) -> updates!
    // Sorted: [0, 10, 12, 15, 20], median = 12
    AddPeers({10, 20, 15, 12});

    REQUIRE(GetTimeOffset() == 12);
}

TEST_CASE("TimeData - 5 peers = 6 total (even), no update from previous", "[timedata]") {
    TestOnlyResetTimeData();

    // First add 4 peers to get initial offset
    // Filter: [0, 10, 20, 15, 12] = 5 elements (odd) -> offset = 12
    AddPeers({10, 20, 15, 12});
    REQUIRE(GetTimeOffset() == 12);  // Confirm we have offset

    // Now add 5th peer: [0, 10, 20, 15, 12, 18] = 6 elements (even) -> no update
    NetworkAddress addr_extra = NetworkAddress::from_string("192.168.1.100", 8333, NODE_NETWORK);
    AddTimeData(addr_extra, 18);

    REQUIRE(GetTimeOffset() == 12);  // Still 12, no update on even count
}

TEST_CASE("TimeData - 6 peers = 7 total (odd), updates", "[timedata]") {
    TestOnlyResetTimeData();

    // Filter: [0, 10, 20, 15, 12, 18, 25] = 7 elements (odd) -> updates!
    // Sorted: [0, 10, 12, 15, 18, 20, 25], median = 15
    AddPeers({10, 20, 15, 12, 18, 25});

    REQUIRE(GetTimeOffset() == 15);
}

TEST_CASE("TimeData - Negative offsets", "[timedata]") {
    TestOnlyResetTimeData();

    // Filter: [0, -30, -20, -25, -22] = 5 elements (odd)
    // Sorted: [-30, -25, -22, -20, 0], median = -22
    AddPeers({-30, -20, -25, -22});

    REQUIRE(GetTimeOffset() == -22);
}

TEST_CASE("TimeData - Mixed positive and negative", "[timedata]") {
    TestOnlyResetTimeData();

    // Filter: [0, -10, 5, -3, 8] = 5 elements (odd)
    // Sorted: [-10, -3, 0, 5, 8], median = 0
    AddPeers({-10, 5, -3, 8});

    REQUIRE(GetTimeOffset() == 0);
}

TEST_CASE("TimeData - Small offsets well within cap are applied", "[timedata]") {
    TestOnlyResetTimeData();

    // All peers report we're 65 seconds behind; with ±70 minutes cap this is within limit
    AddPeers({65, 65, 65, 65});

    // Offset should reflect the median (65)
    REQUIRE(GetTimeOffset() == 65);
}

TEST_CASE("TimeData - Small negative offsets well within cap are applied", "[timedata]") {
    TestOnlyResetTimeData();

    // All peers report we're 65 seconds ahead (-65 seconds); within ±70 minutes cap
    AddPeers({-65, -65, -65, -65});

    // Offset should reflect the median (-65)
    REQUIRE(GetTimeOffset() == -65);
}

TEST_CASE("TimeData - Exactly at +1 minute boundary", "[timedata]") {
    TestOnlyResetTimeData();

// Exactly at the limit (60 seconds)
    int64_t max_adj = DEFAULT_MAX_TIME_ADJUSTMENT;
    AddPeers({max_adj, max_adj, max_adj, max_adj});

// Filter: [0, 60, 60, 60, 60] = 5 elements
    // Sorted: [0, 4200, 4200, 4200, 4200], median = 4200
    // Should be accepted (within limit)
    REQUIRE(GetTimeOffset() == max_adj);
}

TEST_CASE("TimeData - Exactly at -1 minute boundary", "[timedata]") {
    TestOnlyResetTimeData();

    int64_t max_adj = DEFAULT_MAX_TIME_ADJUSTMENT;
    AddPeers({-max_adj, -max_adj, -max_adj, -max_adj});

// Filter: [0, -60, -60, -60, -60] = 5 elements
    // Sorted: [-4200, -4200, -4200, -4200, 0], median = -4200
    REQUIRE(GetTimeOffset() == -max_adj);
}

TEST_CASE("TimeData - One second over +1 minute limit", "[timedata]") {
    TestOnlyResetTimeData();

    int64_t over_limit = DEFAULT_MAX_TIME_ADJUSTMENT + 1;
    AddPeers({over_limit, over_limit, over_limit, over_limit});

    // Should be rejected (over limit)
    REQUIRE(GetTimeOffset() == 0);
}

TEST_CASE("TimeData - Duplicate peer addresses ignored", "[timedata]") {
    TestOnlyResetTimeData();

    // Same peer tries to submit multiple times
    NetworkAddress addr1 = NetworkAddress::from_string("192.168.1.1", 8333, NODE_NETWORK);
    NetworkAddress addr2 = NetworkAddress::from_string("192.168.1.2", 8333, NODE_NETWORK);
    NetworkAddress addr3 = NetworkAddress::from_string("192.168.1.3", 8333, NODE_NETWORK);
    NetworkAddress addr4 = NetworkAddress::from_string("192.168.1.4", 8333, NODE_NETWORK);

    AddTimeData(addr1, 10);
    AddTimeData(addr1, 50);  // Ignored (duplicate)
    AddTimeData(addr1, 100); // Ignored (duplicate)
    AddTimeData(addr2, 20);
    AddTimeData(addr3, 15);
    AddTimeData(addr4, 12);

    // Only first sample from 192.168.1.1 (offset=10) counted
    // Filter: [0, 10, 20, 15, 12] = 5 elements (odd)
    // Sorted: [0, 10, 12, 15, 20], median = 12
    REQUIRE(GetTimeOffset() == 12);
}

TEST_CASE("TimeData - Outlier resistance", "[timedata]") {
    TestOnlyResetTimeData();

    // Most peers agree (~10s offset), but one attacker claims huge offset
    // This tests that median is resistant to outliers
    // Filter: [0, 10, 12, 11, 3000] = 5 elements (odd)
    // Sorted: [0, 10, 11, 12, 3000], median = 11 (attacker's 3000 doesn't affect result)
    AddPeers({10, 12, 11, 3000});

    REQUIRE(GetTimeOffset() == 11);
}

TEST_CASE("TimeData - Eclipse attack with majority", "[timedata]") {
    TestOnlyResetTimeData();

    // Attacker controls 3 out of 4 peers, trying to push time forward
    // Filter: [0, 5000, 5000, 5000, 10] = 5 elements (odd)
    // Sorted: [0, 10, 5000, 5000, 5000], median = 5000
    AddPeers({5000, 5000, 5000, 10});

    // Median would be 5000, but it exceeds ±70 min cap
    // So offset stays at 0 (protection against eclipse attack)
    REQUIRE(GetTimeOffset() == 0);
}

TEST_CASE("TimeData - Small variations around zero", "[timedata]") {
    TestOnlyResetTimeData();

    // Peers have very small offsets (good clock sync)
    // Filter: [0, -2, -1, 1, 2] = 5 elements (odd)
    // Sorted: [-2, -1, 0, 1, 2], median = 0
    AddPeers({-2, -1, 1, 2});

    REQUIRE(GetTimeOffset() == 0);
}

TEST_CASE("TimeData - 8 peers (9 total, odd)", "[timedata]") {
    TestOnlyResetTimeData();

    // Add 8 samples + initial 0 = 9 total (odd)
    // Filter: [0, 100, 110, 105, 95, 108, 102, 98, 106] = 9 elements
    // Sorted: [0, 95, 98, 100, 102, 105, 106, 108, 110], median = 102
    AddPeers({100, 110, 105, 95, 108, 102, 98, 106});

    REQUIRE(GetTimeOffset() == 102);
}

TEST_CASE("TimeData - Reset functionality", "[timedata]") {
    TestOnlyResetTimeData();

    // Add samples
    AddPeers({10, 20, 15, 12});
    REQUIRE(GetTimeOffset() == 12);

    // Reset
    TestOnlyResetTimeData();
    REQUIRE(GetTimeOffset() == 0);

    // Should accept same peer addresses again after reset
    AddPeers({50, 55, 52, 48});
    // Filter: [0, 50, 55, 52, 48] = 5 elements
    // Sorted: [0, 48, 50, 52, 55], median = 50
    REQUIRE(GetTimeOffset() == 50);
}

TEST_CASE("TimeData - CMedianFilter basic", "[timedata]") {
    // Test the median filter directly
    CMedianFilter<int> filter(5, 0);

    REQUIRE(filter.size() == 1);
    REQUIRE(filter.median() == 0);

    filter.input(10);
    REQUIRE(filter.size() == 2);
    REQUIRE(filter.median() == 5);  // (0 + 10) / 2

    filter.input(20);
    REQUIRE(filter.size() == 3);
    REQUIRE(filter.median() == 10);  // Middle of [0, 10, 20]

    filter.input(5);
    REQUIRE(filter.size() == 4);
    REQUIRE(filter.median() == 7);  // (5 + 10) / 2 for [0, 5, 10, 20]

    filter.input(15);
    REQUIRE(filter.size() == 5);
    REQUIRE(filter.median() == 10);  // Middle of [0, 5, 10, 15, 20]
}

TEST_CASE("TimeData - CMedianFilter rolling window", "[timedata]") {
    // Test that filter maintains max size and evicts oldest
    CMedianFilter<int> filter(3, 0);

    filter.input(10);
    filter.input(20);
    REQUIRE(filter.size() == 3);  // [0, 10, 20]
    REQUIRE(filter.median() == 10);

    // Add 4th element, should evict 0 (oldest)
    filter.input(30);
    REQUIRE(filter.size() == 3);  // [10, 20, 30]
    REQUIRE(filter.median() == 20);

    // Add 5th element, should evict 10
    filter.input(5);
    REQUIRE(filter.size() == 3);  // [20, 30, 5]
    REQUIRE(filter.median() == 20);  // Sorted: [5, 20, 30]
}

TEST_CASE("TimeData - Large realistic offsets", "[timedata]") {
    TestOnlyResetTimeData();

    // Test with larger realistic offsets (minutes range)
    // Filter: [0, 60, 120, 90, 75] = 5 elements (odd)
    // Sorted: [0, 60, 75, 90, 120], median = 75
    AddPeers({60, 120, 90, 75});

    REQUIRE(GetTimeOffset() == 75);
}

TEST_CASE("TimeData - Zero offsets (perfect sync)", "[timedata]") {
    TestOnlyResetTimeData();

    // All peers report zero offset (perfect clock sync)
    // Filter: [0, 0, 0, 0, 0] = 5 elements (odd)
    // Median: 0
    AddPeers({0, 0, 0, 0});

    REQUIRE(GetTimeOffset() == 0);
}

TEST_CASE("TimeData - Gradual accumulation", "[timedata]") {
    TestOnlyResetTimeData();

    //  1 peer: [0, 10] = 2 elements -> no update
    NetworkAddress peer1 = NetworkAddress::from_string("10.0.0.1", 8333, NODE_NETWORK);
    AddTimeData(peer1, 10);
    REQUIRE(GetTimeOffset() == 0);

    // 2 peers: [0, 10, 20] = 3 elements -> no update
    NetworkAddress peer2 = NetworkAddress::from_string("10.0.0.2", 8333, NODE_NETWORK);
    AddTimeData(peer2, 20);
    REQUIRE(GetTimeOffset() == 0);

    // 3 peers: [0, 10, 20, 15] = 4 elements -> no update
    NetworkAddress peer3 = NetworkAddress::from_string("10.0.0.3", 8333, NODE_NETWORK);
    AddTimeData(peer3, 15);
    REQUIRE(GetTimeOffset() == 0);

    // 4 peers: [0, 10, 20, 15, 12] = 5 elements (odd) -> UPDATE!
    // Sorted: [0, 10, 12, 15, 20], median = 12
    NetworkAddress peer4 = NetworkAddress::from_string("10.0.0.4", 8333, NODE_NETWORK);
    AddTimeData(peer4, 12);
    REQUIRE(GetTimeOffset() == 12);

    // 5 peers: [0, 10, 20, 15, 12, 18] = 6 elements (even) -> no update
    NetworkAddress peer5 = NetworkAddress::from_string("10.0.0.5", 8333, NODE_NETWORK);
    AddTimeData(peer5, 18);
    REQUIRE(GetTimeOffset() == 12);  // Still 12

    // 6 peers: [0, 10, 20, 15, 12, 18, 14] = 7 elements (odd) -> UPDATE!
    // Sorted: [0, 10, 12, 14, 15, 18, 20], median = 14
    NetworkAddress peer6 = NetworkAddress::from_string("10.0.0.6", 8333, NODE_NETWORK);
    AddTimeData(peer6, 14);
    REQUIRE(GetTimeOffset() == 14);
}
