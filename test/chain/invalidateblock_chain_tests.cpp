// Chain-level invalidateblock tests (ported to test2)

#include <catch_amalgamated.hpp>
#include "chain/chainstate_manager.hpp"
#include "chain/validation.hpp"
#include "chain/chainparams.hpp"
#include "chain/block.hpp"
#include "chain/pow.hpp"
#include "chain/randomx_pow.hpp"
#include "util/time.hpp"

using namespace unicity;
using namespace unicity::validation;
using namespace unicity::chain;

class InvalidateBlockChainFixture2 {
public:
    InvalidateBlockChainFixture2() : params(ChainParams::CreateRegTest()) {
        crypto::InitRandomX();
        CBlockHeader genesis = params->GenesisBlock();
        chainstate.Initialize(genesis);
        genesis_hash = genesis.GetHash();
    }
    uint256 MineBlock() {
        auto* tip = chainstate.GetTip(); REQUIRE(tip != nullptr);
        CBlockHeader header; header.nVersion=1; header.hashPrevBlock=tip->GetBlockHash(); header.minerAddress=uint160(); header.nTime=tip->nTime+120; header.nBits=consensus::GetNextWorkRequired(tip, *params); header.nNonce=0;
        uint256 rx; while(!consensus::CheckProofOfWork(header, header.nBits, *params, crypto::POWVerifyMode::MINING, &rx)) { header.nNonce++; if(header.nNonce==0) header.nTime++; }
        header.hashRandomX=rx; ValidationState st; REQUIRE(chainstate.ProcessNewBlockHeader(header, st, /*min_pow_checked=*/true)); return header.GetHash();
    }
    const CBlockIndex* Get(const uint256& h){ return chainstate.LookupBlockIndex(h); }
    std::unique_ptr<ChainParams> params; ChainstateManager chainstate{*params}; uint256 genesis_hash;
};

TEST_CASE("InvalidateBlock (chain) - Basic invalidation", "[invalidateblock][chain]") {
    InvalidateBlockChainFixture2 fx; uint256 b1=fx.MineBlock(); uint256 b2=fx.MineBlock(); uint256 b3=fx.MineBlock(); auto* tip=fx.chainstate.GetTip(); REQUIRE(tip); CHECK(tip->nHeight==3); CHECK(tip->GetBlockHash()==b3);
    bool ok=fx.chainstate.InvalidateBlock(b2); REQUIRE(ok); auto* b2i=fx.Get(b2); REQUIRE(b2i); CHECK(b2i->status.failure == BlockStatus::VALIDATION_FAILED); auto* b3i=fx.Get(b3); REQUIRE(b3i); CHECK(b3i->status.failure == BlockStatus::ANCESTOR_FAILED); auto* b1i=fx.Get(b1); REQUIRE(b1i); CHECK(b1i->IsValid()); tip=fx.chainstate.GetTip(); REQUIRE(tip); CHECK(tip->nHeight==1); CHECK(tip->GetBlockHash()==b1);
}

TEST_CASE("InvalidateBlock (chain) - Invalidate genesis", "[invalidateblock][chain]") {
    InvalidateBlockChainFixture2 fx; bool ok=fx.chainstate.InvalidateBlock(fx.genesis_hash); CHECK(!ok); auto* g=fx.Get(fx.genesis_hash); REQUIRE(g); CHECK(g->IsValid()); CHECK(fx.chainstate.GetTip()==g);
}
