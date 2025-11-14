#include "catch_amalgamated.hpp"
#include "chain/miner.hpp"
#include "chain/chainparams.hpp"
#include "chain/chainstate_manager.hpp"
#include "util/uint.hpp"
#include <memory>

using namespace unicity;
using namespace unicity::chain;
using namespace unicity::mining;
using namespace unicity::validation;

// Helper to create a test chainstate manager
class MinerTestFixture {
public:
    MinerTestFixture() {
        GlobalChainParams::Select(ChainType::REGTEST);
        params = &GlobalChainParams::Get();

        chainstate = std::make_unique<ChainstateManager>(*params);

        miner = std::make_unique<CPUMiner>(*params, *chainstate);
    }

    ~MinerTestFixture() {
        if (miner && miner->IsMining()) {
            miner->Stop();
        }
    }

    const ChainParams* params;
    std::unique_ptr<ChainstateManager> chainstate;
    std::unique_ptr<CPUMiner> miner;
};

TEST_CASE("CPUMiner - Mining address management", "[miner]") {
    MinerTestFixture fixture;

    SECTION("Default mining address is null (zero)") {
        uint160 default_addr = fixture.miner->GetMiningAddress();
        REQUIRE(default_addr.IsNull());
        REQUIRE(default_addr == uint160());
    }

    SECTION("SetMiningAddress stores the address") {
        uint160 test_addr;
        test_addr.SetHex("1234567890abcdef1234567890abcdef12345678");

        fixture.miner->SetMiningAddress(test_addr);

        uint160 retrieved = fixture.miner->GetMiningAddress();
        REQUIRE(retrieved == test_addr);
        REQUIRE(retrieved.GetHex() == "1234567890abcdef1234567890abcdef12345678");
    }

    SECTION("Mining address persists across multiple set/get calls") {
        uint160 addr1;
        addr1.SetHex("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");

        fixture.miner->SetMiningAddress(addr1);
        REQUIRE(fixture.miner->GetMiningAddress() == addr1);

        // Address should still be addr1
        REQUIRE(fixture.miner->GetMiningAddress() == addr1);

        // Change to new address
        uint160 addr2;
        addr2.SetHex("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
        fixture.miner->SetMiningAddress(addr2);

        // Should now be addr2
        REQUIRE(fixture.miner->GetMiningAddress() == addr2);
        REQUIRE(fixture.miner->GetMiningAddress() != addr1);
    }

    SECTION("Can set address to null (zero)") {
        // Set non-null address first
        uint160 test_addr;
        test_addr.SetHex("1234567890abcdef1234567890abcdef12345678");
        fixture.miner->SetMiningAddress(test_addr);
        REQUIRE(!fixture.miner->GetMiningAddress().IsNull());

        // Reset to null
        uint160 null_addr;
        null_addr.SetNull();
        fixture.miner->SetMiningAddress(null_addr);

        REQUIRE(fixture.miner->GetMiningAddress().IsNull());
    }

    SECTION("Different address formats are preserved correctly") {
        // All zeros
        uint160 zeros;
        zeros.SetHex("0000000000000000000000000000000000000000");
        fixture.miner->SetMiningAddress(zeros);
        REQUIRE(fixture.miner->GetMiningAddress() == zeros);

        // All ones
        uint160 ones;
        ones.SetHex("ffffffffffffffffffffffffffffffffffffffff");
        fixture.miner->SetMiningAddress(ones);
        REQUIRE(fixture.miner->GetMiningAddress() == ones);

        // Mixed pattern
        uint160 mixed;
        mixed.SetHex("0123456789abcdef0123456789abcdef01234567");
        fixture.miner->SetMiningAddress(mixed);
        REQUIRE(fixture.miner->GetMiningAddress() == mixed);
    }
}

TEST_CASE("CPUMiner - Address validation scenarios", "[miner]") {
    MinerTestFixture fixture;

    SECTION("Valid 40-character hex address") {
        uint160 addr;
        addr.SetHex("1234567890abcdef1234567890abcdef12345678");

        fixture.miner->SetMiningAddress(addr);
        REQUIRE(fixture.miner->GetMiningAddress().GetHex() == "1234567890abcdef1234567890abcdef12345678");
    }

    SECTION("Address with uppercase hex characters") {
        uint160 addr;
        addr.SetHex("1234567890ABCDEF1234567890ABCDEF12345678");

        fixture.miner->SetMiningAddress(addr);
        // GetHex returns lowercase
        REQUIRE(fixture.miner->GetMiningAddress().GetHex() == "1234567890abcdef1234567890abcdef12345678");
    }

    SECTION("Address with mixed case") {
        uint160 addr;
        addr.SetHex("1234567890AbCdEf1234567890aBcDeF12345678");

        fixture.miner->SetMiningAddress(addr);
        REQUIRE(fixture.miner->GetMiningAddress().GetHex() == "1234567890abcdef1234567890abcdef12345678");
    }
}

TEST_CASE("CPUMiner - Mining address sticky behavior", "[miner]") {
    MinerTestFixture fixture;

    SECTION("Address persists without explicit reset") {
        // Set initial address
        uint160 addr1;
        addr1.SetHex("1111111111111111111111111111111111111111");
        fixture.miner->SetMiningAddress(addr1);

        // Verify it's set
        REQUIRE(fixture.miner->GetMiningAddress() == addr1);

        // Do NOT reset address
        // Just verify it's still there (sticky behavior)
        REQUIRE(fixture.miner->GetMiningAddress() == addr1);
        REQUIRE(fixture.miner->GetMiningAddress() == addr1);
        REQUIRE(fixture.miner->GetMiningAddress() == addr1);
    }

    SECTION("Address changes only when explicitly set") {
        uint160 addr1;
        addr1.SetHex("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");

        uint160 addr2;
        addr2.SetHex("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");

        // Set first address
        fixture.miner->SetMiningAddress(addr1);
        REQUIRE(fixture.miner->GetMiningAddress() == addr1);

        // Address should remain addr1
        REQUIRE(fixture.miner->GetMiningAddress() == addr1);

        // Only changes when explicitly set to addr2
        fixture.miner->SetMiningAddress(addr2);
        REQUIRE(fixture.miner->GetMiningAddress() == addr2);
        REQUIRE(fixture.miner->GetMiningAddress() != addr1);
    }

    SECTION("Address survives across mining operations") {
        uint160 test_addr;
        test_addr.SetHex("9999999999999999999999999999999999999999");

        fixture.miner->SetMiningAddress(test_addr);
        REQUIRE(fixture.miner->GetMiningAddress() == test_addr);

        // Simulate mining operations (without actually mining)
        // Address should still be there
        REQUIRE(fixture.miner->GetMiningAddress() == test_addr);

        // Even after checking status
        bool is_mining = fixture.miner->IsMining();
        REQUIRE(!is_mining); // Should not be mining
        REQUIRE(fixture.miner->GetMiningAddress() == test_addr);
    }
}

TEST_CASE("CPUMiner - Initial state", "[miner]") {
    MinerTestFixture fixture;

    SECTION("Miner starts in stopped state") {
        REQUIRE(!fixture.miner->IsMining());
    }

    SECTION("Initial stats are zero") {
        REQUIRE(fixture.miner->GetTotalHashes() == 0);
        REQUIRE(fixture.miner->GetBlocksFound() == 0);
        REQUIRE(fixture.miner->GetHashrate() == 0.0);
    }

    SECTION("Initial address is null") {
        REQUIRE(fixture.miner->GetMiningAddress().IsNull());
    }
}

TEST_CASE("CPUMiner - Address format edge cases", "[miner]") {
    MinerTestFixture fixture;

    SECTION("Leading zeros preserved in address") {
        uint160 addr;
        addr.SetHex("0000000000000000000000000000000012345678");

        fixture.miner->SetMiningAddress(addr);

        std::string hex = fixture.miner->GetMiningAddress().GetHex();
        REQUIRE(hex == "0000000000000000000000000000000012345678");
        REQUIRE(hex.length() == 40);
    }

    SECTION("Trailing zeros preserved in address") {
        uint160 addr;
        addr.SetHex("1234567800000000000000000000000000000000");

        fixture.miner->SetMiningAddress(addr);

        std::string hex = fixture.miner->GetMiningAddress().GetHex();
        REQUIRE(hex == "1234567800000000000000000000000000000000");
        REQUIRE(hex.length() == 40);
    }

    SECTION("All zeros is a valid address") {
        uint160 addr;
        addr.SetHex("0000000000000000000000000000000000000000");

        fixture.miner->SetMiningAddress(addr);

        REQUIRE(fixture.miner->GetMiningAddress().IsNull());
        REQUIRE(fixture.miner->GetMiningAddress().GetHex() == "0000000000000000000000000000000000000000");
    }

    SECTION("Maximum value address (all F's)") {
        uint160 addr;
        addr.SetHex("ffffffffffffffffffffffffffffffffffffffff");

        fixture.miner->SetMiningAddress(addr);

        REQUIRE(fixture.miner->GetMiningAddress().GetHex() == "ffffffffffffffffffffffffffffffffffffffff");
    }
}
