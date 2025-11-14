// P2 tests: Load round-trip, locator structure, FindFork, orphan eviction

#include "catch_amalgamated.hpp"
#include "chain/chainstate_manager.hpp"
#include "chain/chainparams.hpp"
#include "chain/block.hpp"
#include "chain/validation.hpp"
#include "chain/chain.hpp"
#include "test_chainstate_manager.hpp"
#include <filesystem>
#include <thread>
#include <chrono>
#include "util/time.hpp"

using namespace unicity;
using namespace unicity::chain;
using namespace unicity::validation;
using unicity::test::TestChainstateManager;

static CBlockHeader MkChild(const CBlockIndex* prev, uint32_t nTime, uint32_t nBits = 0x207fffff) {
    CBlockHeader h; h.nVersion=1; h.hashPrevBlock = prev ? prev->GetBlockHash() : uint256();
    h.minerAddress.SetNull(); h.nTime = nTime; h.nBits = nBits; h.nNonce = 0; h.hashRandomX.SetNull(); return h;
}

TEST_CASE("Chainstate Load() round-trip reconstructs candidates and activates best tip", "[chain][persistence][chainstate_manager]") {
    auto params = ChainParams::CreateRegTest();

    // Build chain with a fork, ensuring one branch has more work/height
    TestChainstateManager csm1(*params);
    csm1.Initialize(params->GenesisBlock());

    const CBlockIndex* g = csm1.GetTip();
    // Main branch: g->A1->A2
    CBlockHeader A1 = MkChild(g, g->nTime + 120);
    ValidationState s; auto* pA1 = csm1.AcceptBlockHeader(A1, s, true); REQUIRE(pA1);
    csm1.TryAddBlockIndexCandidate(pA1); REQUIRE(csm1.ActivateBestChain());

    CBlockHeader A2 = MkChild(pA1, pA1->nTime + 120);
    auto* pA2 = csm1.AcceptBlockHeader(A2, s, true); REQUIRE(pA2);
    csm1.TryAddBlockIndexCandidate(pA2); REQUIRE(csm1.ActivateBestChain());

    // Fork: g->B1->B2->B3
    CBlockHeader B1 = MkChild(g, g->nTime + 130);
    auto* pB1 = csm1.AcceptBlockHeader(B1, s, true); REQUIRE(pB1);
    csm1.TryAddBlockIndexCandidate(pB1);

    CBlockHeader B2 = MkChild(pB1, pB1->nTime + 120);
    auto* pB2 = csm1.AcceptBlockHeader(B2, s, true); REQUIRE(pB2);
    csm1.TryAddBlockIndexCandidate(pB2);

    CBlockHeader B3 = MkChild(pB2, pB2->nTime + 120);
    auto* pB3 = csm1.AcceptBlockHeader(B3, s, true); REQUIRE(pB3);
    csm1.TryAddBlockIndexCandidate(pB3);

    REQUIRE(csm1.ActivateBestChain());
    REQUIRE(csm1.GetTip()->GetBlockHash() == pB3->GetBlockHash());

    // Save to disk
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    std::string path = "/tmp/chainstate_load_rt_" + std::to_string(now) + ".json";
    REQUIRE(csm1.Save(path));

    // New chainstate - Load and activate
    TestChainstateManager csm2(*params);
    REQUIRE(csm2.Load(path));
    REQUIRE(csm2.ActivateBestChain());

    // Should activate B3 tip
    REQUIRE(csm2.GetTip() != nullptr);
    REQUIRE(csm2.GetTip()->nHeight == 3);
    REQUIRE(csm2.GetTip()->GetBlockHash() == pB3->GetBlockHash());

    std::filesystem::remove(path);
}

TEST_CASE("GetLocator structure: step-back pattern and genesis inclusion", "[chain][locator]") {
    auto params = ChainParams::CreateRegTest();
    TestChainstateManager csm(*params);
    csm.Initialize(params->GenesisBlock());

    // Build 25-block chain
    const CBlockIndex* tip = csm.GetTip();
    ValidationState st;
    for (int i=0;i<25;i++) {
        CBlockHeader h = MkChild(tip, tip->nTime + 120);
        auto* p = csm.AcceptBlockHeader(h, st, true); REQUIRE(p);
        csm.TryAddBlockIndexCandidate(p); REQUIRE(csm.ActivateBestChain());
        tip = csm.GetTip();
    }
    REQUIRE(tip->nHeight == 25);

    // Locator from tip
    CBlockLocator loc = csm.GetLocator();
    REQUIRE(!loc.vHave.empty());
    // First entry is tip
    REQUIRE(loc.vHave[0] == tip->GetBlockHash());

    // Check first up-to-11 entries step back by 1 height
    size_t checkN = std::min<size_t>(11, loc.vHave.size());
    for (size_t i=1;i<checkN;i++) {
        const CBlockIndex* prev = csm.LookupBlockIndex(loc.vHave[i-1]); REQUIRE(prev);
        const CBlockIndex* cur  = csm.LookupBlockIndex(loc.vHave[i]); REQUIRE(cur);
        REQUIRE(cur->nHeight == prev->nHeight - 1);
    }

    // Ensure genesis is included somewhere
    bool hasGenesis=false; for (auto& h: loc.vHave){ if (h == params->GenesisBlock().GetHash()) { hasGenesis=true; break; } }
    REQUIRE(hasGenesis);

    // Locator from mid-chain index
    const CBlockIndex* mid = csm.GetBlockAtHeight(10); REQUIRE(mid);
    CBlockLocator locMid = csm.GetLocator(mid);
    REQUIRE(!locMid.vHave.empty());
    REQUIRE(locMid.vHave[0] == mid->GetBlockHash());
}

TEST_CASE("CChain::FindFork returns correct fork point", "[chain][findfork]") {
    BlockManager bm; // work with raw chain container

    // Genesis
    CBlockHeader g; g.nVersion=1; g.hashPrevBlock.SetNull(); g.minerAddress.SetNull(); g.nTime=1000; g.nBits=0x207fffff; g.nNonce=0;
    REQUIRE(bm.Initialize(g));

    // Build active chain A: g->A1->A2->A3->A4->A5
    CBlockHeader prev = g; CBlockIndex* lastA=nullptr;
    for (int i=1;i<=5;i++) {
        CBlockHeader h = MkChild((i==1? bm.GetTip(): lastA), (i==1? g.nTime: lastA->nTime) + 120);
        lastA = bm.AddToBlockIndex(h); REQUIRE(lastA);
        bm.SetActiveTip(*lastA);
    }
    REQUIRE(bm.ActiveChain().Tip() == lastA);

    // Build parallel chain B from genesis: g->B1->B2->B3->B4->B5->B6 (not active)
    CBlockIndex* lastB = bm.LookupBlockIndex(g.GetHash());
    for (int i=1;i<=6;i++) {
        CBlockHeader h = MkChild((i==1? bm.LookupBlockIndex(g.GetHash()): lastB), (i==1? g.nTime: lastB->nTime) + 90);
        lastB = bm.AddToBlockIndex(h); REQUIRE(lastB);
        // do not set active tip to keep A active
    }

    const CChain& chain = bm.ActiveChain();

    // pindex taller than active chain: force it back then find fork
    const CBlockIndex* fork = chain.FindFork(lastB);
    REQUIRE(fork != nullptr);
    REQUIRE(fork->GetBlockHash() == g.GetHash());

    // If we pass a node on active chain, fork is itself
    REQUIRE(chain.FindFork(lastA) == lastA);
}

// Params with tiny orphan expire time for eviction test
class OrphanExpireParams : public ChainParams {
public:
    OrphanExpireParams() {
        chainType = ChainType::REGTEST;
        consensus.powLimit = uint256S("0x7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
        consensus.nPowTargetSpacing = 2*60;
        consensus.nRandomXEpochDuration = 365ULL * 24 * 60 * 60 * 100;
        consensus.nASERTHalfLife = 60*60;
        consensus.nASERTAnchorHeight = 1;
        consensus.nMinimumChainWork = uint256S("0x0");
        consensus.nNetworkExpirationInterval = 0;
        consensus.nNetworkExpirationGracePeriod = 0;
        consensus.nOrphanHeaderExpireTime = 1; // 1 second
        consensus.nSuspiciousReorgDepth = 100;
        consensus.nAntiDosWorkBufferBlocks = 144;
        nDefaultPort = 29590;
        genesis = CreateGenesisBlock(1296688602, 2, 0x207fffff, 1);
        consensus.hashGenesisBlock = genesis.GetHash();
    }
};

TEST_CASE("Orphan headers time-based eviction removes expired entries", "[orphan][dos][eviction]") {
    auto params = std::make_unique<OrphanExpireParams>();
    TestChainstateManager csm(*params);

    REQUIRE(csm.Initialize(params->GenesisBlock()));

    // Create orphan with unknown parent
    uint256 unknown; unknown.SetNull(); memset((void*)unknown.data(), 0xaa, 32);
    CBlockHeader orphan = MkChild(nullptr, params->GenesisBlock().nTime + 100);
    orphan.hashPrevBlock = unknown;

    ValidationState st;
    CBlockIndex* r = csm.AcceptBlockHeader(orphan, st, /*min_pow_checked=*/true);
    REQUIRE(r == nullptr); // prev-blk-not-found -> orphan path
    REQUIRE(csm.AddOrphanHeader(orphan, /*peer_id=*/1));
    REQUIRE(csm.GetOrphanHeaderCount() == 1);

    // Advance mock time beyond expiry and evict deterministically
    int64_t base = util::GetTime();
    util::SetMockTime(base + params->GetConsensus().nOrphanHeaderExpireTime + 2);

    size_t evicted = csm.EvictOrphanHeaders();
    util::SetMockTime(0);
    REQUIRE(evicted >= 1);
    REQUIRE(csm.GetOrphanHeaderCount() == 0);
}
