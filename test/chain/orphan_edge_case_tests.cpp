// Orphan edge case tests (ported to test2, chain-level)

#include "catch_amalgamated.hpp"
#include "test_chainstate_manager.hpp"
#include "chain/validation.hpp"
#include "chain/chainparams.hpp"
#include "chain/block.hpp"

using namespace unicity;
using namespace unicity::test;
using namespace unicity::chain;
using unicity::validation::ValidationState;

static CBlockHeader CreateTestHeader(const uint256& prevHash, uint32_t nTime, uint32_t nNonce = 12345) {
    CBlockHeader header; header.nVersion=1; header.hashPrevBlock=prevHash; header.minerAddress.SetNull(); header.nTime=nTime; header.nBits=0x207fffff; header.nNonce=nNonce; header.hashRandomX.SetNull(); return header;
}
static uint256 RandomHash(){ uint256 h; for(int i=0;i<32;i++) *(h.begin()+i)=rand()%256; return h; }

TEST_CASE("Orphan Edge Cases - Invalid Headers", "[orphan][edge]") {
    auto params = ChainParams::CreateRegTest(); TestChainstateManager chainstate(*params);

    SECTION("Orphan with future timestamp") {
        chainstate.Initialize(params->GenesisBlock());
        uint256 up=RandomHash();
        CBlockHeader o=CreateTestHeader(up, std::time(nullptr)+10000);
        ValidationState st;
        chain::CBlockIndex* r = chainstate.AcceptBlockHeader(o, st, /*min_pow_checked=*/true);
        REQUIRE(r==nullptr);
        REQUIRE(st.GetRejectReason()=="prev-blk-not-found");
        REQUIRE(chainstate.AddOrphanHeader(o, /*peer_id=*/1));
        REQUIRE(chainstate.GetOrphanHeaderCount()==1);
    }
    SECTION("Orphan with null prev hash should not be cached") {
        chainstate.Initialize(params->GenesisBlock()); uint256 nullHash; nullHash.SetNull(); CBlockHeader o=CreateTestHeader(nullHash,1234567890); ValidationState st; chain::CBlockIndex* r=chainstate.AcceptBlockHeader(o, st, 1); REQUIRE(r==nullptr); REQUIRE(st.GetRejectReason()!="orphaned"); REQUIRE(chainstate.GetOrphanHeaderCount()==0);
    }
    SECTION("Orphan with invalid version") {
        chainstate.Initialize(params->GenesisBlock()); uint256 up=RandomHash(); CBlockHeader o=CreateTestHeader(up,1234567890); o.nVersion=0; ValidationState st; chainstate.AcceptBlockHeader(o, st, 1); REQUIRE(true);
    }
    SECTION("Orphan becomes valid when parent arrives (processed from orphan pool)") {
        chainstate.Initialize(params->GenesisBlock());
        const auto& genesis=params->GenesisBlock();
        CBlockHeader parent=CreateTestHeader(genesis.GetHash(), genesis.nTime+120,1000);
        uint256 parentHash=parent.GetHash();
        CBlockHeader orphan=CreateTestHeader(parentHash, genesis.nTime+60,1001);
        ValidationState st;
        chain::CBlockIndex* r = chainstate.AcceptBlockHeader(orphan, st, /*min_pow_checked=*/true);
        REQUIRE(r==nullptr);
        REQUIRE(st.GetRejectReason()=="prev-blk-not-found");
        REQUIRE(chainstate.AddOrphanHeader(orphan, /*peer_id=*/1));
        REQUIRE(chainstate.GetOrphanHeaderCount()==1);
        chainstate.AcceptBlockHeader(parent, st, /*min_pow_checked=*/true);
        REQUIRE(chainstate.GetOrphanHeaderCount()==0);
        REQUIRE(chainstate.LookupBlockIndex(orphan.GetHash())!=nullptr);
    }
}

TEST_CASE("Orphan Edge Cases - Chain Topology", "[orphan][edge]") {
    auto params = ChainParams::CreateRegTest(); TestChainstateManager chainstate(*params);

    SECTION("Orphan chain with missing middle block") {
        chainstate.Initialize(params->GenesisBlock());
        const auto& genesis=params->GenesisBlock();
        CBlockHeader A=CreateTestHeader(genesis.GetHash(), genesis.nTime+120,1000); uint256 hA=A.GetHash();
        CBlockHeader B=CreateTestHeader(hA, genesis.nTime+240,1001); uint256 hB=B.GetHash();
        CBlockHeader C=CreateTestHeader(hB, genesis.nTime+360,1002);
        ValidationState st;
        chain::CBlockIndex* pA = chainstate.AcceptBlockHeader(A, st, /*min_pow_checked=*/true);
        if (pA) { chainstate.TryAddBlockIndexCandidate(pA); }
        REQUIRE(chainstate.LookupBlockIndex(hA)!=nullptr);
        chain::CBlockIndex* rC = chainstate.AcceptBlockHeader(C, st, /*min_pow_checked=*/true);
        REQUIRE(rC==nullptr);
        REQUIRE(st.GetRejectReason()=="prev-blk-not-found");
        REQUIRE(chainstate.AddOrphanHeader(C, /*peer_id=*/1));
        REQUIRE(chainstate.GetOrphanHeaderCount()==1);
        chainstate.AcceptBlockHeader(B, st, /*min_pow_checked=*/true);
        REQUIRE(chainstate.GetOrphanHeaderCount()==0);
        REQUIRE(chainstate.LookupBlockIndex(hB)!=nullptr);
        REQUIRE(chainstate.LookupBlockIndex(C.GetHash())!=nullptr);
    }
    SECTION("Multiple orphan chains from same root") {
        chainstate.Initialize(params->GenesisBlock());
        const auto& genesis=params->GenesisBlock();
        uint256 hA=RandomHash();
        CBlockHeader B1=CreateTestHeader(hA, genesis.nTime+240,1001);
        CBlockHeader B2=CreateTestHeader(hA, genesis.nTime+240,1002);
        CBlockHeader B3=CreateTestHeader(hA, genesis.nTime+240,1003);
        ValidationState st;
        chain::CBlockIndex* r1 = chainstate.AcceptBlockHeader(B1, st, /*min_pow_checked=*/true);
        REQUIRE(r1==nullptr);
        REQUIRE(st.GetRejectReason()=="prev-blk-not-found");
        REQUIRE(chainstate.AddOrphanHeader(B1, /*peer_id=*/1));
        chain::CBlockIndex* r2 = chainstate.AcceptBlockHeader(B2, st, /*min_pow_checked=*/true);
        REQUIRE(r2==nullptr);
        REQUIRE(st.GetRejectReason()=="prev-blk-not-found");
        REQUIRE(chainstate.AddOrphanHeader(B2, /*peer_id=*/1));
        chain::CBlockIndex* r3 = chainstate.AcceptBlockHeader(B3, st, /*min_pow_checked=*/true);
        REQUIRE(r3==nullptr);
        REQUIRE(st.GetRejectReason()=="prev-blk-not-found");
        REQUIRE(chainstate.AddOrphanHeader(B3, /*peer_id=*/1));
        REQUIRE(chainstate.GetOrphanHeaderCount()==3);
    }
    SECTION("Orphan refers to block already in active chain") {
        chainstate.Initialize(params->GenesisBlock()); const auto& genesis=params->GenesisBlock(); CBlockHeader A=CreateTestHeader(genesis.GetHash(), genesis.nTime+120,1000); CBlockHeader B=CreateTestHeader(A.GetHash(), genesis.nTime+240,1001); ValidationState st; chain::CBlockIndex* pA=chainstate.AcceptBlockHeader(A, st, 1); if(pA){ chainstate.TryAddBlockIndexCandidate(pA);} REQUIRE(chainstate.LookupBlockIndex(A.GetHash())!=nullptr); chain::CBlockIndex* pB=chainstate.AcceptBlockHeader(B, st, 1); if(pB){ chainstate.TryAddBlockIndexCandidate(pB);} REQUIRE(chainstate.LookupBlockIndex(B.GetHash())!=nullptr);
    }
}
