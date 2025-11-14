// Extra tests to cover block header helpers and locator

#include "catch_amalgamated.hpp"
#include "chain/block.hpp"
#include "util/uint.hpp"

TEST_CASE("CBlockHeader helpers and fixed serialization", "[block]") {
    CBlockHeader h;
    h.SetNull();
    REQUIRE(h.IsNull());

    h.nVersion = 1;
    h.nTime = 1234567890;
    h.nBits = 0x1d00ffff;
    h.nNonce = 42;
    // Set hash fields to non-null
    uint256 prev; prev.SetHex("000102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f");
    uint256 rx;   rx.SetHex("f0e0d0c0b0a09080706050403020100ff0e0d0c0b0a09080706050403020100f");
    uint160 addr; addr.SetHex("00112233445566778899aabbccddeeff00112233");
    h.hashPrevBlock = prev;
    h.hashRandomX = rx;
    h.minerAddress = addr;

    // Not null anymore
    REQUIRE_FALSE(h.IsNull());
    REQUIRE(h.GetBlockTime() == 1234567890);

    // Fixed-size serialization round-trip
    auto fixed = h.SerializeFixed();
    REQUIRE(fixed.size() == CBlockHeader::HEADER_SIZE);

    CBlockHeader h2; h2.SetNull();
    REQUIRE(h2.Deserialize(fixed));
    REQUIRE(h2.nVersion == h.nVersion);
    REQUIRE(h2.nTime == h.nTime);
    REQUIRE(h2.nBits == h.nBits);
    REQUIRE(h2.nNonce == h.nNonce);
    REQUIRE(h2.hashPrevBlock == h.hashPrevBlock);
    REQUIRE(h2.hashRandomX == h.hashRandomX);
    REQUIRE(h2.minerAddress == h.minerAddress);

    // Generic vector serialization also round-trips
    auto v = h.Serialize();
    CBlockHeader h3; h3.SetNull();
    REQUIRE(h3.Deserialize(v));
    REQUIRE(h3.GetHash() == h.GetHash());

    // ToString contains fields
    auto s = h.ToString();
    REQUIRE(s.find("version") != std::string::npos);
}

TEST_CASE("CBlockLocator basic semantics", "[block]") {
    // Build simple locator
    uint256 a,b,c; a.SetHex("11"); b.SetHex("22"); c.SetHex("33");
    std::vector<uint256> have{c,b,a};
    CBlockLocator loc(std::move(have));
    REQUIRE(!loc.IsNull());
    REQUIRE(loc.vHave.size() == 3);
    loc.SetNull();
    REQUIRE(loc.IsNull());
}
