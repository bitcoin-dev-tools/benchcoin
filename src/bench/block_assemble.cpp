// Copyright (c) 2011-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bench/bench.h>
#include <bench/data/block413567.raw.h>
#include <consensus/consensus.h>
#include <node/miner.h>
#include <primitives/transaction.h>
#include <random.h>
#include <script/script.h>
#include <sync.h>
#include <test/util/mining.h>
#include <test/util/script.h>
#include <test/util/setup_common.h>
#include <validation.h>
#include <node/context.h>
#include <primitives/block.h>
#include <txmempool.h>
#include <util/check.h>

#include <vector>
#include <cassert>
#include <array>
#include <cstddef>
#include <streams.h>

using node::BlockAssembler;

static void AssembleBlock(benchmark::Bench& bench)
{
    const auto test_setup = MakeNoLogFileContext<const TestingSetup>();

    CScriptWitness witness;
    witness.stack.push_back(WITNESS_STACK_ELEM_OP_TRUE);
    BlockAssembler::Options options;
    options.coinbase_output_script = P2WSH_OP_TRUE;

    // Collect some loose transactions that spend the coinbases of our mined blocks
    constexpr size_t NUM_BLOCKS{200};
    std::array<CTransactionRef, NUM_BLOCKS - COINBASE_MATURITY + 1> txs;
    for (size_t b{0}; b < NUM_BLOCKS; ++b) {
        CMutableTransaction tx;
        tx.vin.emplace_back(MineBlock(test_setup->m_node, options));
        tx.vin.back().scriptWitness = witness;
        tx.vout.emplace_back(1337, P2WSH_OP_TRUE);
        if (NUM_BLOCKS - b >= COINBASE_MATURITY)
            txs.at(b) = MakeTransactionRef(tx);
    }
    {
        LOCK(::cs_main);

        for (const auto& txr : txs) {
            const MempoolAcceptResult res = test_setup->m_node.chainman->ProcessTransaction(txr);
            assert(res.m_result_type == MempoolAcceptResult::ResultType::VALID);
        }
    }

    bench.run([&] {
        PrepareBlock(test_setup->m_node, options);
    });
}
static void BlockAssemblerAddPackageTxns(benchmark::Bench& bench)
{
    FastRandomContext det_rand{true};
    auto testing_setup{MakeNoLogFileContext<TestChain100Setup>()};
    testing_setup->PopulateMempool(det_rand, /*num_transactions=*/1000, /*submit=*/true);
    BlockAssembler::Options assembler_options;
    assembler_options.test_block_validity = false;
    assembler_options.coinbase_output_script = P2WSH_OP_TRUE;

    bench.run([&] {
        PrepareBlock(testing_setup->m_node, assembler_options);
    });
}

static void ProcessTransactionBench(benchmark::Bench& bench)
{
    LOCK(cs_main);
    const auto testing_setup{MakeNoLogFileContext<TestChain100Setup>()};
    CTxMemPool& pool{*Assert(testing_setup->m_node.mempool)};
    ChainstateManager& chainman{*testing_setup->m_node.chainman};
    CCoinsViewCache& coins_tip{chainman.ActiveChainstate().CoinsTip()};

    CBlock block;
    DataStream(benchmark::data::block413567) >> TX_WITH_WITNESS(block);

    std::vector<CTransactionRef> txs(block.vtx.size() - 1);
    for (size_t i{1}; i < block.vtx.size(); ++i) {
        CMutableTransaction mtx{*block.vtx[i]};
        for (auto& txin : mtx.vin) {
            txin.nSequence = CTxIn::SEQUENCE_FINAL;
            txin.scriptSig.clear();
            txin.scriptWitness.stack = {WITNESS_STACK_ELEM_OP_TRUE};
        }
        txs[i - 1] = MakeTransactionRef(std::move(mtx));
    }

    for (const auto& tx : txs) {
        const Coin coin(CTxOut(2 * tx->GetValueOut(), P2WSH_OP_TRUE), 1, /*fCoinBaseIn=*/false);
        for (const auto& in : tx->vin) {
            coins_tip.AddCoin(in.prevout, Coin{coin}, /*possible_overwrite=*/false);
        }
    }

    bench.batch(txs.size()).run([&] {
        LOCK2(cs_main, pool.cs);
        assert(coins_tip.GetCacheSize());
        for (const auto& tx : txs) pool.removeRecursive(*tx, MemPoolRemovalReason::REPLACED);
        assert(pool.size() == 0);

        for (const auto& tx : txs) {
            const auto res{chainman.ProcessTransaction(tx, /*test_accept=*/false)};
            assert(res.m_result_type == MempoolAcceptResult::ResultType::VALID);
        }
    });
}

BENCHMARK(ProcessTransactionBench, benchmark::PriorityLevel::HIGH);
BENCHMARK(AssembleBlock, benchmark::PriorityLevel::HIGH);
BENCHMARK(BlockAssemblerAddPackageTxns, benchmark::PriorityLevel::LOW);
