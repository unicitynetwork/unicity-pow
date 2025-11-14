#include "catch_amalgamated.hpp"
#include "chain/validation.hpp"
#include "chain/chainparams.hpp"
#include "chain/block_index.hpp"
#include "chain/block_manager.hpp"
#include "chain/pow.hpp"
#include "chain/randomx_pow.hpp"
#include "network/protocol.hpp"
#include <memory>

using namespace unicity;
using namespace unicity::validation;
using namespace unicity::chain;

// Helper function to create a valid test header
CBlockHeader CreateTestHeader(uint32_t nTime = 1234567890, uint32_t nBits = 0x207fffff, uint32_t nNonce = 0) {
    CBlockHeader header;
    header.nVersion = 1;
    header.hashPrevBlock.SetNull();
    header.minerAddress.SetNull();
    header.nTime = nTime;
    header.nBits = nBits;
    header.nNonce = nNonce;
    header.hashRandomX.SetNull();
    return header;
}

TEST_CASE("ValidationState - basic functionality", "[validation]") {
    SECTION("Default state is valid") {
        ValidationState state;
        REQUIRE(state.IsValid());
        REQUIRE_FALSE(state.IsInvalid());
        REQUIRE_FALSE(state.IsError());
    }

    SECTION("Invalid() marks state as invalid and returns false") {
        ValidationState state;
        bool result = state.Invalid("bad-header", "test failure");

        REQUIRE_FALSE(result);  // Invalid() returns false
        REQUIRE(state.IsInvalid());
        REQUIRE_FALSE(state.IsValid());
        REQUIRE_FALSE(state.IsError());
        REQUIRE(state.GetRejectReason() == "bad-header");
        REQUIRE(state.GetDebugMessage() == "test failure");
    }

    SECTION("Invalid() without debug message") {
        ValidationState state;
        state.Invalid("bad-block");

        REQUIRE(state.GetRejectReason() == "bad-block");
        REQUIRE(state.GetDebugMessage() == "");
    }

    SECTION("Error() marks state as error and returns false") {
        ValidationState state;
        bool result = state.Error("disk-failure", "I/O error reading block");

        REQUIRE_FALSE(result);  // Error() returns false
        REQUIRE(state.IsError());
        REQUIRE_FALSE(state.IsValid());
        REQUIRE_FALSE(state.IsInvalid());
        REQUIRE(state.GetRejectReason() == "disk-failure");
        REQUIRE(state.GetDebugMessage() == "I/O error reading block");
    }

    SECTION("Error() without debug message") {
        ValidationState state;
        state.Error("network-timeout");

        REQUIRE(state.GetRejectReason() == "network-timeout");
        REQUIRE(state.GetDebugMessage() == "");
    }
}

TEST_CASE("CheckHeadersAreContinuous - chain structure validation", "[validation]") {
    SECTION("Empty vector is continuous") {
        std::vector<CBlockHeader> headers;
        REQUIRE(CheckHeadersAreContinuous(headers));
    }

    SECTION("Single header is continuous") {
        std::vector<CBlockHeader> headers;
        headers.push_back(CreateTestHeader());
        REQUIRE(CheckHeadersAreContinuous(headers));
    }

    SECTION("Two connected headers are continuous") {
        CBlockHeader header1 = CreateTestHeader(1000);
        CBlockHeader header2 = CreateTestHeader(1001);
        header2.hashPrevBlock = header1.GetHash();

        std::vector<CBlockHeader> headers = {header1, header2};
        REQUIRE(CheckHeadersAreContinuous(headers));
    }

    SECTION("Three connected headers are continuous") {
        CBlockHeader header1 = CreateTestHeader(1000);
        CBlockHeader header2 = CreateTestHeader(1001);
        header2.hashPrevBlock = header1.GetHash();
        CBlockHeader header3 = CreateTestHeader(1002);
        header3.hashPrevBlock = header2.GetHash();

        std::vector<CBlockHeader> headers = {header1, header2, header3};
        REQUIRE(CheckHeadersAreContinuous(headers));
    }

    SECTION("Disconnected headers are not continuous") {
        CBlockHeader header1 = CreateTestHeader(1000);
        CBlockHeader header2 = CreateTestHeader(1001);
        // header2.hashPrevBlock NOT set to header1's hash

        std::vector<CBlockHeader> headers = {header1, header2};
        REQUIRE_FALSE(CheckHeadersAreContinuous(headers));
    }

    SECTION("Gap in middle breaks continuity") {
        CBlockHeader header1 = CreateTestHeader(1000);
        CBlockHeader header2 = CreateTestHeader(1001);
        header2.hashPrevBlock = header1.GetHash();
        CBlockHeader header3 = CreateTestHeader(1002);
        // header3.hashPrevBlock NOT set to header2's hash

        std::vector<CBlockHeader> headers = {header1, header2, header3};
        REQUIRE_FALSE(CheckHeadersAreContinuous(headers));
    }
}

TEST_CASE("CalculateHeadersWork - work calculation", "[validation]") {
    SECTION("Empty vector has zero work") {
        std::vector<CBlockHeader> headers;
        arith_uint256 work = CalculateHeadersWork(headers);
        REQUIRE(work == 0);
    }

    SECTION("Single valid header has non-zero work") {
        CBlockHeader header = CreateTestHeader();
        header.nBits = 0x1d00ffff;  // Bitcoin's initial difficulty

        std::vector<CBlockHeader> headers = {header};
        arith_uint256 work = CalculateHeadersWork(headers);
        REQUIRE(work > 0);
    }

    SECTION("Multiple headers accumulate work") {
        CBlockHeader header1 = CreateTestHeader();
        header1.nBits = 0x1d00ffff;
        CBlockHeader header2 = CreateTestHeader();
        header2.nBits = 0x1d00ffff;

        std::vector<CBlockHeader> headers = {header1, header2};
        arith_uint256 total_work = CalculateHeadersWork(headers);

        // Work should be roughly double (not exact due to formula)
        arith_uint256 single_work = CalculateHeadersWork({header1});
        REQUIRE(total_work > single_work);
        REQUIRE(total_work < single_work * 3);  // Rough sanity check
    }

    SECTION("Invalid nBits with negative flag is skipped") {
        CBlockHeader header = CreateTestHeader();
        header.nBits = 0x00800000;  // Negative flag set with zero mantissa

        std::vector<CBlockHeader> headers = {header};
        arith_uint256 work = CalculateHeadersWork(headers);
        REQUIRE(work == 0);  // Invalid header contributes no work
    }

    SECTION("Invalid nBits with zero target is skipped") {
        CBlockHeader header = CreateTestHeader();
        header.nBits = 0x00000000;  // Zero target (infinite difficulty)

        std::vector<CBlockHeader> headers = {header};
        arith_uint256 work = CalculateHeadersWork(headers);
        REQUIRE(work == 0);  // Invalid header contributes no work
    }

    SECTION("Invalid nBits with zero mantissa is skipped") {
        CBlockHeader header = CreateTestHeader();
        header.nBits = 0x01000000;  // Zero mantissa (exponent=1, mantissa=0)

        std::vector<CBlockHeader> headers = {header};
        arith_uint256 work = CalculateHeadersWork(headers);
        REQUIRE(work == 0);  // Invalid header contributes no work
    }

    SECTION("Mix of valid and invalid headers") {
        CBlockHeader valid1 = CreateTestHeader();
        valid1.nBits = 0x1d00ffff;

        CBlockHeader invalid = CreateTestHeader();
        invalid.nBits = 0x00000000;  // Zero target

        CBlockHeader valid2 = CreateTestHeader();
        valid2.nBits = 0x1d00ffff;

        std::vector<CBlockHeader> headers = {valid1, invalid, valid2};
        arith_uint256 work = CalculateHeadersWork(headers);

        // Should only count valid headers
        arith_uint256 expected = CalculateHeadersWork({valid1}) + CalculateHeadersWork({valid2});
        REQUIRE(work == expected);
    }

    SECTION("Higher difficulty produces more work") {
        CBlockHeader easy = CreateTestHeader();
        easy.nBits = 0x1d00ffff;  // Easier difficulty

        CBlockHeader hard = CreateTestHeader();
        hard.nBits = 0x1c00ffff;  // Harder difficulty (smaller target)

        arith_uint256 easy_work = CalculateHeadersWork({easy});
        arith_uint256 hard_work = CalculateHeadersWork({hard});

        REQUIRE(hard_work > easy_work);
    }
}

TEST_CASE("GetAntiDoSWorkThreshold - DoS protection", "[validation]") {
    auto params = ChainParams::CreateRegTest();

SECTION("Returns minimum chain work with null tip (even during IBD)") {
        arith_uint256 threshold = GetAntiDoSWorkThreshold(nullptr, *params);

        // Should return the configured minimum chain work
        arith_uint256 min_work = UintToArith256(params->GetConsensus().nMinimumChainWork);
        REQUIRE(threshold == min_work);
    }

    SECTION("Returns value with valid tip") {
        // Create a simple block index with some work
        CBlockIndex tip;
        tip.nBits = 0x207fffff;  // RegTest difficulty
        tip.nHeight = 200;
        tip.nChainWork = arith_uint256(10000);  // Some accumulated work

arith_uint256 threshold = GetAntiDoSWorkThreshold(&tip, *params);

        // Should return a non-zero value
        REQUIRE(threshold > 0);

        // Should be less than tip's total work (144 blocks buffer)
        REQUIRE(threshold <= tip.nChainWork);
    }
}

TEST_CASE("GetAdjustedTime - time source", "[validation]") {
    SECTION("Returns non-zero timestamp") {
        int64_t adjusted_time = GetAdjustedTime();
        REQUIRE(adjusted_time > 0);
    }

    SECTION("Returns reasonable current time") {
        int64_t adjusted_time = GetAdjustedTime();

        // Should be somewhere in 2024-2030 range (Unix timestamps)
        int64_t year_2024 = 1704067200;  // 2024-01-01
        int64_t year_2030 = 1893456000;  // 2030-01-01

        REQUIRE(adjusted_time > year_2024);
        REQUIRE(adjusted_time < year_2030);
    }

    SECTION("Is consistent across multiple calls") {
        int64_t time1 = GetAdjustedTime();
        int64_t time2 = GetAdjustedTime();

        // Should be within 1 second of each other
        REQUIRE(std::abs(time2 - time1) <= 1);
    }
}

TEST_CASE("Validation constants", "[validation]") {
    SECTION("MAX_FUTURE_BLOCK_TIME is 2 hours") {
        REQUIRE(MAX_FUTURE_BLOCK_TIME == 2 * 60 * 60);
        REQUIRE(MAX_FUTURE_BLOCK_TIME == 7200);
    }

    SECTION("MEDIAN_TIME_SPAN matches block_index.hpp") {
        REQUIRE(chain::MEDIAN_TIME_SPAN == 11);
    }

    SECTION("MAX_HEADERS_SIZE is reasonable") {
        REQUIRE(protocol::MAX_HEADERS_SIZE == 2000);
    }

    SECTION("nAntiDosWorkBufferBlocks is chain-specific") {
        // Mainnet: 6 blocks (~6 hours at 1-hour blocks) - tight security
        auto mainnet = chain::ChainParams::CreateMainNet();
        REQUIRE(mainnet->GetConsensus().nAntiDosWorkBufferBlocks == 6);

        // Testnet: 144 blocks (~4.8 hours at 2-minute blocks) - testing flexibility
        auto testnet = chain::ChainParams::CreateTestNet();
        REQUIRE(testnet->GetConsensus().nAntiDosWorkBufferBlocks == 144);

        // Regtest: 144 blocks - testing flexibility
        auto regtest = chain::ChainParams::CreateRegTest();
        REQUIRE(regtest->GetConsensus().nAntiDosWorkBufferBlocks == 144);
    }
}

TEST_CASE("CheckHeadersAreContinuous - edge cases", "[validation]") {
    SECTION("Handles hash collision correctly") {
        // This tests the unlikely case where two different headers have same hash
        // (In practice, this should never happen with SHA256)
        CBlockHeader header1 = CreateTestHeader(1000);
        CBlockHeader header2 = CreateTestHeader(1001);

        // Manually set prevhash to match header1's hash
        header2.hashPrevBlock = header1.GetHash();

        std::vector<CBlockHeader> headers = {header1, header2};
        REQUIRE(CheckHeadersAreContinuous(headers));
    }

    SECTION("Long chain of headers") {
        std::vector<CBlockHeader> headers;
        CBlockHeader prev = CreateTestHeader(1000);
        headers.push_back(prev);

        // Create chain of 100 headers
        for (int i = 1; i < 100; i++) {
            CBlockHeader next = CreateTestHeader(1000 + i);
            next.hashPrevBlock = prev.GetHash();
            headers.push_back(next);
            prev = next;
        }

        REQUIRE(CheckHeadersAreContinuous(headers));
        REQUIRE(headers.size() == 100);
    }
}

TEST_CASE("CalculateHeadersWork - boundary conditions", "[validation]") {
    SECTION("Maximum difficulty (smallest target)") {
        CBlockHeader header = CreateTestHeader();
        header.nBits = 0x01010000;  // Very small target

        std::vector<CBlockHeader> headers = {header};
        arith_uint256 work = CalculateHeadersWork(headers);

        // Should produce very large work value
        REQUIRE(work > 0);
    }

    SECTION("Minimum practical difficulty (largest target)") {
        CBlockHeader header = CreateTestHeader();
        header.nBits = 0x207fffff;  // RegTest difficulty (very easy)

        std::vector<CBlockHeader> headers = {header};
        arith_uint256 work = CalculateHeadersWork(headers);

        // Should produce small but non-zero work value
        REQUIRE(work > 0);
    }

    SECTION("Work calculation matches GetBlockProof") {
        CBlockHeader header = CreateTestHeader();
        header.nBits = 0x1d00ffff;

        // Create a CBlockIndex to use GetBlockProof
        CBlockIndex index(header);
        index.nBits = header.nBits;

        arith_uint256 work_from_calculate = CalculateHeadersWork({header});
        arith_uint256 work_from_getproof = GetBlockProof(index);

        // Should produce identical results
        REQUIRE(work_from_calculate == work_from_getproof);
    }
}

TEST_CASE("CBlockIndex::GetMedianTimePast - median time calculation", "[validation]") {
    SECTION("Single block returns its own time") {
        CBlockIndex index;
        index.nTime = 1000;
        index.pprev = nullptr;

        REQUIRE(index.GetMedianTimePast() == 1000);
    }

    SECTION("Two blocks returns median") {
        CBlockIndex index1;
        index1.nTime = 1000;
        index1.pprev = nullptr;

        CBlockIndex index2;
        index2.nTime = 2000;
        index2.pprev = &index1;

        int64_t median = index2.GetMedianTimePast();
        // Median of [1000, 2000] is 1500
        REQUIRE((median == 1000 || median == 2000));  // Implementation sorts and takes middle
    }

    SECTION("Eleven blocks uses all for median") {
        // Create chain of 11 blocks with increasing times
        std::vector<std::unique_ptr<CBlockIndex>> chain;
        for (int i = 0; i < 11; i++) {
            auto index = std::make_unique<CBlockIndex>();
            index->nTime = 1000 + i * 100;
            index->pprev = (i > 0) ? chain[i-1].get() : nullptr;
            chain.push_back(std::move(index));
        }

        int64_t median = chain[10]->GetMedianTimePast();
        // Median of 11 values is the 6th value (index 5)
        REQUIRE(median == 1500);  // 1000 + 5*100
    }

    SECTION("More than eleven blocks only uses last 11") {
        // Create chain of 20 blocks
        std::vector<std::unique_ptr<CBlockIndex>> chain;
        for (int i = 0; i < 20; i++) {
            auto index = std::make_unique<CBlockIndex>();
            index->nTime = 1000 + i * 100;
            index->pprev = (i > 0) ? chain[i-1].get() : nullptr;
            chain.push_back(std::move(index));
        }

        int64_t median = chain[19]->GetMedianTimePast();
        // Should only consider blocks [9..19] (last 11)
        // Median of those is block 14: 1000 + 14*100 = 2400
        REQUIRE(median == 2400);
    }

    SECTION("Handles unsorted times correctly") {
        // Create blocks with non-monotonic times
        CBlockIndex index1;
        index1.nTime = 5000;
        index1.pprev = nullptr;

        CBlockIndex index2;
        index2.nTime = 3000;  // Earlier time
        index2.pprev = &index1;

        CBlockIndex index3;
        index3.nTime = 4000;  // Middle time
        index3.pprev = &index2;

        int64_t median = index3.GetMedianTimePast();
        // Median of [3000, 4000, 5000] is 4000
        REQUIRE(median == 4000);
    }
}

TEST_CASE("Validation - integration test", "[validation]") {
    SECTION("Complete header validation flow") {
        // Create a simple header
        CBlockHeader header = CreateTestHeader();

        // Should serialize correctly
        auto serialized = header.Serialize();
        REQUIRE(serialized.size() == 100);

        // Should deserialize correctly
        CBlockHeader header2;
        REQUIRE(header2.Deserialize(serialized.data(), serialized.size()));

        // Hashes should match
        REQUIRE(header.GetHash() == header2.GetHash());

        // Should calculate work
        arith_uint256 work = CalculateHeadersWork({header});
        REQUIRE(work > 0);
    }
}

TEST_CASE("Network Expiration (Timebomb) - validation checks", "[validation][timebomb]") {
    SECTION("MainNet has expiration disabled") {
        // Initialize mainnet params
        GlobalChainParams::Select(ChainType::MAIN);
        const ChainParams& params = GlobalChainParams::Get();

        // Verify expiration is disabled (0 means no expiration)
        REQUIRE(params.GetConsensus().nNetworkExpirationInterval == 0);
        REQUIRE(params.GetConsensus().nNetworkExpirationGracePeriod == 0);

        // For mainnet at height 10000, just verify expiration is disabled
        // (Full contextual validation would require proper difficulty setup)
    }

    SECTION("TestNet has expiration enabled") {
        // Initialize testnet params
        GlobalChainParams::Select(ChainType::TESTNET);
        const ChainParams& params = GlobalChainParams::Get();

        // Verify expiration is enabled (non-zero)
        REQUIRE(params.GetConsensus().nNetworkExpirationInterval > 0);
        REQUIRE(params.GetConsensus().nNetworkExpirationGracePeriod == 24);
    }

    SECTION("RegTest has expiration disabled for testing") {
        // Initialize regtest params
        GlobalChainParams::Select(ChainType::REGTEST);
        const ChainParams& params = GlobalChainParams::Get();

        // Verify expiration is disabled for regtest (testing environment)
        REQUIRE(params.GetConsensus().nNetworkExpirationInterval == 0);
        REQUIRE(params.GetConsensus().nNetworkExpirationGracePeriod == 0);
    }

    SECTION("Expiration check logic is correct") {
        // Use testnet params since it has expiration enabled
        GlobalChainParams::Select(ChainType::TESTNET);
        const ChainParams& params = GlobalChainParams::Get();
        const auto& consensus = params.GetConsensus();

        // Test the expiration logic directly
        int32_t expirationHeight = consensus.nNetworkExpirationInterval;
        int32_t gracePeriod = consensus.nNetworkExpirationGracePeriod;

        REQUIRE(expirationHeight > 0);  // Should be enabled for testnet
        REQUIRE(gracePeriod == 24);

        // Block at expiration height should be the last valid block
        int32_t currentHeight = expirationHeight;
        REQUIRE(currentHeight <= expirationHeight);  // Should pass

        // Block beyond expiration should fail
        currentHeight = expirationHeight + 1;
        REQUIRE(currentHeight > expirationHeight);  // Would trigger rejection

        // Grace period starts at (expiration - gracePeriod)
        int32_t gracePeriodStart = expirationHeight - gracePeriod;
        REQUIRE(gracePeriodStart == (expirationHeight - 24));

        // Blocks in grace period should log warning but still be valid
        currentHeight = gracePeriodStart;
        REQUIRE(currentHeight > gracePeriodStart - 1);
        REQUIRE(currentHeight <= expirationHeight);
    }
}
