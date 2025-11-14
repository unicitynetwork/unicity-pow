// Contextual validation and IBD tests for ChainstateManager

#include "catch_amalgamated.hpp"
#include "chain/chainstate_manager.hpp"
#include "chain/chainparams.hpp"
#include "chain/block.hpp"
#include "chain/pow.hpp"
#include "chain/validation.hpp"
#include "chain/notifications.hpp"
#include "test_chainstate_manager.hpp"
#include "util/time.hpp"

using namespace unicity;
using namespace unicity::chain;
using namespace unicity::validation;
using unicity::test::TestChainstateManager;

// Helper: make a child header with specified fields
static CBlockHeader MakeChild(const CBlockIndex* prev, uint32_t nTime, uint32_t nBits) {
    CBlockHeader h; h.nVersion=1; h.hashPrevBlock = prev ? prev->GetBlockHash() : uint256();
    h.minerAddress.SetNull(); h.nTime = nTime; h.nBits = nBits; h.nNonce = 0; h.hashRandomX.SetNull(); return h;
}

TEST_CASE("Contextual - bad difficulty is rejected", "[validation][contextual][chainstate_manager]") {
    auto params = ChainParams::CreateRegTest();
    TestChainstateManager csm(*params);
    csm.SetBypassContextualValidation(false); // exercise real contextual checks

    // Initialize with regtest genesis
    REQUIRE(csm.Initialize(params->GenesisBlock()));
    const CBlockIndex* tip = csm.GetTip();
    REQUIRE(tip != nullptr);

    // Expected difficulty for child of genesis
    uint32_t expected = consensus::GetNextWorkRequired(tip, *params);

    // Create child with wrong nBits
    CBlockHeader bad = MakeChild(tip, tip->nTime + 120, expected ^ 1);

    ValidationState st;
    CBlockIndex* p = csm.AcceptBlockHeader(bad, st, /*min_pow_checked=*/true);
    REQUIRE(p == nullptr);
    REQUIRE_FALSE(st.IsValid());
    REQUIRE(st.GetRejectReason() == "bad-diffbits");
}

TEST_CASE("Contextual - timestamp constraints (MTP and future)", "[validation][contextual][chainstate_manager]") {
    auto params = ChainParams::CreateRegTest();
    TestChainstateManager csm(*params);
    csm.SetBypassContextualValidation(false);

    // Init
    REQUIRE(csm.Initialize(params->GenesisBlock()));
    const CBlockIndex* tip = csm.GetTip();
    REQUIRE(tip != nullptr);

    // Add a valid block A
    uint32_t bitsA = consensus::GetNextWorkRequired(tip, *params);
    CBlockHeader A = MakeChild(tip, tip->nTime + 120, bitsA);
    {
        ValidationState s; auto* pa = csm.AcceptBlockHeader(A, s, true); REQUIRE(pa != nullptr);
        csm.TryAddBlockIndexCandidate(pa); REQUIRE(csm.ActivateBestChain());
    }

    const CBlockIndex* tipA = csm.GetTip();
    REQUIRE(tipA != nullptr);

    SECTION("time-too-old vs median time past") {
        // Child B with time <= MTP of A (equal triggers failure)
        uint32_t bitsB = consensus::GetNextWorkRequired(tipA, *params);
        CBlockHeader B = MakeChild(tipA, /*nTime*/ tipA->nTime, bitsB);
        ValidationState s; auto* pb = csm.AcceptBlockHeader(B, s, true);
        REQUIRE(pb == nullptr);
        REQUIRE_FALSE(s.IsValid());
        REQUIRE(s.GetRejectReason() == "time-too-old");
    }

    SECTION("time-too-new vs adjusted time") {
        uint32_t bitsB = consensus::GetNextWorkRequired(tipA, *params);
        // Far future time
        uint32_t future = static_cast<uint32_t>(util::GetTime() + MAX_FUTURE_BLOCK_TIME + 1000);
        CBlockHeader B = MakeChild(tipA, future, bitsB);
        ValidationState s; auto* pb = csm.AcceptBlockHeader(B, s, true);
        REQUIRE(pb == nullptr);
        REQUIRE_FALSE(s.IsValid());
        REQUIRE(s.GetRejectReason() == "time-too-new");
    }
}

// Test-only params with tiny expiration height to exercise network-expired path
class SmallExpireParams : public ChainParams {
public:
    SmallExpireParams() {
        chainType = ChainType::REGTEST;
        // Very easy
        consensus.powLimit = uint256S("0x00000000ffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
        consensus.nPowTargetSpacing = 2 * 60;
        consensus.nRandomXEpochDuration = 365ULL * 24 * 60 * 60 * 100; // keep epoch constant
        consensus.nASERTHalfLife = 60 * 60;
        consensus.nASERTAnchorHeight = 1;
        consensus.nMinimumChainWork = uint256S("0x0");
        consensus.nNetworkExpirationInterval = 3; // expire at height 3
        consensus.nNetworkExpirationGracePeriod = 1;
        consensus.nOrphanHeaderExpireTime = 12 * 60;
        consensus.nSuspiciousReorgDepth = 100;
        consensus.nAntiDosWorkBufferBlocks = 144;
        nDefaultPort = 29590;
        genesis = CreateGenesisBlock(1296688602, 2, 0x207fffff, 1);
        consensus.hashGenesisBlock = genesis.GetHash();
    }
};

TEST_CASE("Network expiration triggers reject + notification", "[validation][timebomb][chainstate_manager]") {
    auto params = std::make_unique<SmallExpireParams>();
    TestChainstateManager csm(*params);
    csm.SetBypassContextualValidation(false);

    REQUIRE(csm.Initialize(params->GenesisBlock()));

    // Subscribe to notification
    bool got_notify = false; int got_height=-1; int exp_height=-1;
    auto sub = Notifications().SubscribeNetworkExpired([&](int current_height, int expiration_height){
        got_notify = true; got_height = current_height; exp_height = expiration_height;
    });

    // Build up to height 2 (expiration-1 = 2)
    const CBlockIndex* tip = csm.GetTip();
    for (int i=0;i<2;i++) {
        uint32_t bits = consensus::GetNextWorkRequired(tip, *params);
        CBlockHeader h = MakeChild(tip, tip->nTime + 120, bits);
        ValidationState s; auto* pi = csm.AcceptBlockHeader(h, s, true); REQUIRE(pi != nullptr);
        csm.TryAddBlockIndexCandidate(pi); REQUIRE(csm.ActivateBestChain());
        tip = csm.GetTip();
    }
    REQUIRE(tip->nHeight == 2);

    // Header at expiration height (3) should be accepted during validation
    // but trigger notification when connected (Bitcoin Core pattern)
    uint32_t bits = consensus::GetNextWorkRequired(tip, *params);
    CBlockHeader expH = MakeChild(tip, tip->nTime + 120, bits);
    ValidationState s; auto* r = csm.AcceptBlockHeader(expH, s, true);
    REQUIRE(r != nullptr);  // Header accepted during validation
    REQUIRE(s.IsValid());

    // Add to candidates and activate - this triggers the expiration notification
    csm.TryAddBlockIndexCandidate(r);
    REQUIRE(csm.ActivateBestChain());

    // Notification should fire after chain activation (not during header validation)
    REQUIRE(got_notify);
    REQUIRE(got_height == 3);
    REQUIRE(exp_height == 3);

    // Try to add block BEYOND expiration (height 4) - should be refused
    tip = csm.GetTip();
    REQUIRE(tip->nHeight == 3);  // Still at expiration height
    uint32_t bits4 = consensus::GetNextWorkRequired(tip, *params);
    CBlockHeader beyond = MakeChild(tip, tip->nTime + 120, bits4);
    ValidationState s4; auto* p4 = csm.AcceptBlockHeader(beyond, s4, true);
    REQUIRE(p4 != nullptr);  // Header accepted during validation
    REQUIRE(s4.IsValid());

    // Try to activate it - should fail (refused by expiration check)
    csm.TryAddBlockIndexCandidate(p4);
    REQUIRE_FALSE(csm.ActivateBestChain());  // Returns false (not activated)

    // Tip should still be at expiration height (block not connected)
    REQUIRE(csm.GetTip()->nHeight == 3);
}

TEST_CASE("IsInitialBlockDownload latch behavior", "[validation][ibd][chainstate_manager]") {
    auto params = ChainParams::CreateRegTest();
    TestChainstateManager csm(*params);

    SECTION("Empty and genesis-only return IBD true") {
        REQUIRE(csm.IsInitialBlockDownload());
        REQUIRE(csm.Initialize(params->GenesisBlock()));
        REQUIRE(csm.IsInitialBlockDownload()); // height 0 -> still IBD
    }

    SECTION("Height>0 recent tip clears IBD and latches") {
        REQUIRE(csm.Initialize(params->GenesisBlock()));
        const CBlockIndex* tip = csm.GetTip();
        uint32_t bits = consensus::GetNextWorkRequired(tip, *params);
        uint32_t nowt = static_cast<uint32_t>(util::GetTime());
        CBlockHeader h = MakeChild(tip, nowt, bits);
        ValidationState s; auto* pi = csm.AcceptBlockHeader(h, s, true); REQUIRE(pi != nullptr);
        csm.TryAddBlockIndexCandidate(pi); REQUIRE(csm.ActivateBestChain());

        // First call should compute and latch false
        REQUIRE_FALSE(csm.IsInitialBlockDownload());
        // Second call should return false via cached latch
        REQUIRE_FALSE(csm.IsInitialBlockDownload());
    }
}
