// Copyright (c) 2025 The Unicity Foundation
// Distributed under the MIT software license

#include "catch_amalgamated.hpp"
#include "chain/chainstate_manager.hpp"
#include "chain/chainparams.hpp"
#include "chain/block.hpp"
#include "chain/pow.hpp"
#include "chain/randomx_pow.hpp"
#include "chain/validation.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <chrono>

using namespace unicity;
using namespace unicity::chain;
using namespace unicity::validation;

static CBlockHeader MineChild(const CBlockIndex* prev, const chain::ChainParams& params, uint32_t nTime)
{
    // Build candidate header
    CBlockHeader h;
    h.nVersion = 1;
    h.hashPrevBlock = prev ? prev->GetBlockHash() : uint256();
    h.minerAddress.SetNull();
    h.nTime = nTime;
    h.nBits = consensus::GetNextWorkRequired(prev, params);
    h.nNonce = 0;
    h.hashRandomX.SetNull();

    // Mine until COMMITMENT + FULL PoW are satisfied (MINING mode produces hash)
    uint256 out_hash;
    int iter = 0;
    while (!consensus::CheckProofOfWork(h, h.nBits, params, crypto::POWVerifyMode::MINING, &out_hash)) {
        h.nNonce++;
        iter++;
        // Safety to avoid infinite loop in case of unexpected environment issues
        REQUIRE(iter < 500000);
    }
    h.hashRandomX = out_hash;
    return h;
}

TEST_CASE("Chainstate Load hardening: recompute ignores tampered chainwork", "[chain][chainstate_manager][persistence][hardening]") {
    // Initialize RandomX once for mining in this test
    crypto::InitRandomX();

    auto params = ChainParams::CreateRegTest();

    // Build a short valid chain with mined commitments
    ChainstateManager csm(*params);
    REQUIRE(csm.Initialize(params->GenesisBlock()));

    const CBlockIndex* tip = csm.GetTip();
    REQUIRE(tip != nullptr);

    // Mine 4 blocks on top of genesis
    for (int i = 1; i <= 4; ++i) {
        CBlockHeader h = MineChild(tip, *params, static_cast<uint32_t>(tip->nTime + 120));
        ValidationState st;
        REQUIRE(csm.ProcessNewBlockHeader(h, st, /*min_pow_checked=*/true));
        REQUIRE(st.IsValid());
        tip = csm.GetTip();
        REQUIRE(tip != nullptr);
        REQUIRE(tip->nHeight == i);
    }

    const uint256 orig_tip_hash = csm.GetTip()->GetBlockHash();
    const arith_uint256 orig_tip_work = csm.GetTip()->nChainWork;

    // Save to a temporary file
    const std::filesystem::path tmp_path = std::filesystem::temp_directory_path() / "chainstate_load_hardening.json";
    REQUIRE(csm.Save(tmp_path.string()));

    // Tamper on-disk chainwork fields (set to zero or garbage) without touching headers
    {
        std::ifstream in(tmp_path);
        REQUIRE(in.is_open());
        nlohmann::json root; in >> root; in.close();

        REQUIRE(root.contains("blocks"));
        REQUIRE(root["blocks"].is_array());
        for (auto& blk : root["blocks"]) {
            // Overwrite chainwork with obviously wrong value
            blk["chainwork"] = "0x0"; // zero work
        }

        std::ofstream out(tmp_path);
        REQUIRE(out.is_open());
        out << root.dump(2);
        out.close();
    }

    // Load into a fresh ChainstateManager and ensure recomputation restores true work
    ChainstateManager csm2(*params);
    REQUIRE(csm2.Load(tmp_path.string()));

    // After Load(), active tip may still reflect BlockManager's initial pick.
    // Activate to select the most-work candidate based on recomputed chainwork.
    REQUIRE(csm2.ActivateBestChain(nullptr));

    const CBlockIndex* tip2 = csm2.GetTip();
    REQUIRE(tip2 != nullptr);

    // Tip should match original chain's tip (by hash) and have the original recomputed work
    REQUIRE(tip2->GetBlockHash() == orig_tip_hash);
    REQUIRE(tip2->nChainWork == orig_tip_work);
}