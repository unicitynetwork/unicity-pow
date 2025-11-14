// Negative path tests for CBlockHeader deserialization

#include "catch_amalgamated.hpp"
#include "chain/block.hpp"

TEST_CASE("CBlockHeader - Deserialize negative cases", "[block]") {
    CBlockHeader h;

    SECTION("Deserialize from wrong-sized array template") {
        std::array<uint8_t, CBlockHeader::HEADER_SIZE + 1> too_big{};
        REQUIRE_FALSE(h.Deserialize(too_big));
        std::array<uint8_t, CBlockHeader::HEADER_SIZE - 1> too_small{};
        REQUIRE_FALSE(h.Deserialize(too_small));
    }

    SECTION("Deserialize from vector with insufficient size") {
        std::vector<uint8_t> v(CBlockHeader::HEADER_SIZE - 10);
        REQUIRE_FALSE(h.Deserialize(v.data(), v.size()));
    }
}
