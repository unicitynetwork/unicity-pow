// Candidate pruning and invariants tests (requires UNICITY_TESTS)

#include "catch_amalgamated.hpp"
#include "chain/chainstate_manager.hpp"
#include "chain/chainparams.hpp"
#include "chain/block.hpp"
#include "chain/validation.hpp"
#include "test_chainstate_manager.hpp"

using namespace unicity;
using namespace unicity::chain;
using namespace unicity::validation;
using unicity::test::TestChainstateManager;

static CBlockHeader Mkh(const CBlockIndex* prev, uint32_t nTime) {
    CBlockHeader h; h.nVersion=1; h.hashPrevBlock = prev ? prev->GetBlockHash() : uint256();
    h.minerAddress.SetNull(); h.nTime=nTime; h.nBits=0x207fffff; h.nNonce=0; h.hashRandomX.SetNull(); return h;
}

static bool HasChild(const BlockManager& bm, const CBlockIndex* idx) {
    for (const auto& [hash, block] : bm.GetBlockIndex()) {
        if (block.pprev == idx) return true;
    }
    return false;
}

TEST_CASE("Candidate set invariants across activation and invalidation", "[chain][candidates]") {
    auto params = ChainParams::CreateRegTest();
    TestChainstateManager csm(*params);
    REQUIRE(csm.Initialize(params->GenesisBlock()));

    const CBlockIndex* g = csm.GetTip();

    // Add A1 and activate
    CBlockHeader A1 = Mkh(g, g->nTime + 120);
    ValidationState s; auto* pA1 = csm.AcceptBlockHeader(A1, s, true); REQUIRE(pA1);
    csm.TryAddBlockIndexCandidate(pA1);
    REQUIRE(csm.DebugCandidateCount() >= 1);
    REQUIRE(csm.ActivateBestChain());

    // After activation, candidates should be pruned (no tip, no lower-work)
    REQUIRE(csm.DebugCandidateCount() == 0);

    // Add competing fork B1 (lower work than current tip)
    CBlockHeader B1 = Mkh(g, g->nTime + 130);
    auto* pB1 = csm.AcceptBlockHeader(B1, s, true); REQUIRE(pB1);
    csm.TryAddBlockIndexCandidate(pB1);

    // Activate best chain keeps A1 as tip; B1 remains as a lower-work candidate (Core keeps candidates on no-op)
    REQUIRE(csm.ActivateBestChain());
    REQUIRE(csm.DebugCandidateCount() >= 1);

    // Extend fork to surpass tip: B2, B3
    CBlockHeader B2 = Mkh(pB1, pB1->nTime + 120);
    auto* pB2 = csm.AcceptBlockHeader(B2, s, true); REQUIRE(pB2);
    csm.TryAddBlockIndexCandidate(pB2);

    CBlockHeader B3 = Mkh(pB2, pB2->nTime + 120);
    auto* pB3 = csm.AcceptBlockHeader(B3, s, true); REQUIRE(pB3);
    csm.TryAddBlockIndexCandidate(pB3);

    // Before activation, candidate should include B3 (a leaf)
    REQUIRE(csm.DebugCandidateCount() >= 1);

    // Activate reorg to B3; candidates pruned again
    REQUIRE(csm.ActivateBestChain());
    REQUIRE(csm.DebugCandidateCount() == 0);

    // Invalidate current tip (B3) – should populate candidates without activating
    REQUIRE(csm.InvalidateBlock(pB3->GetBlockHash()));

    auto hashes = csm.DebugCandidateHashes();
    REQUIRE_FALSE(hashes.empty());

    // None of the candidates should be the invalidated block
    for (const auto& h : hashes) {
        REQUIRE(h != pB3->GetBlockHash());
    }
}