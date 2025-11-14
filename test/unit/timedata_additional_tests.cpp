// Additional TimeData tests focusing on AddTimeData behavior

#include "catch_amalgamated.hpp"
#include "chain/timedata.hpp"
#include "network/protocol.hpp"

using namespace unicity;
using namespace unicity::chain;
using namespace unicity::protocol;

static NetworkAddress A(uint32_t v4){ return NetworkAddress::from_ipv4(NODE_NETWORK, v4, 9590); }

TEST_CASE("TimeData - median update and limits", "[timedata][add]") {
    TestOnlyResetTimeData();

    // 5 samples added; note CMedianFilter includes an initial 0, so after 5 samples
    // the internal size is 6 (even) and the last update occurred at 4 samples (size=5),
    // yielding a median of 20.
    AddTimeData(A(0x01010101), 10);   // +10s
    AddTimeData(A(0x02020202), 20);   // +20s
    AddTimeData(A(0x03030303), 30);   // +30s
    AddTimeData(A(0x04040404), 40);   // +40s
    AddTimeData(A(0x05050505), 50);   // +50s

    REQUIRE(GetTimeOffset() == 20);

    // 6th sample makes total size odd (7 including initial 0) → median updates to 30
    AddTimeData(A(0x06060606), 60);
    REQUIRE(GetTimeOffset() == 30);

    // Add large positive sample beyond DEFAULT_MAX_TIME_ADJUSTMENT; with only one outlier
    // the median remains unchanged (still within range) and size becomes even → no update
    int64_t too_far = DEFAULT_MAX_TIME_ADJUSTMENT + 600; // > +70 min
    AddTimeData(A(0x07070707), too_far);
    REQUIRE(GetTimeOffset() == 30);
}

TEST_CASE("TimeData - duplicate source ignored and size cap", "[timedata][add]") {
    TestOnlyResetTimeData();

    auto addr = A(0x0A0A0A0A);
    AddTimeData(addr, 5);
    AddTimeData(addr, 1000); // duplicate source ignored

    // Need to reach odd size >=5 to trigger update
    AddTimeData(A(0x0B0B0B0B), 5);
    AddTimeData(A(0x0C0C0C0C), 5);
    AddTimeData(A(0x0D0D0D0D), 5);
    AddTimeData(A(0x0E0E0E0E), 5); // size=5 (one duplicate ignored) → median=5

    REQUIRE(GetTimeOffset() == 5);
}
