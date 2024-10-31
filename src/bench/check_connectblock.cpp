// Copyright (c) 2016-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bench/bench.h>

#include <addresstype.h>
#include <interfaces/chain.h>
#include <kernel/cs_main.h>
#include <script/interpreter.h>
#include <sync.h>
#include <test/util/setup_common.h>
#include <validation.h>

#include <cassert>
#include <utility>
#include <vector>

std::pair<CBlock, std::unique_ptr<CBlockIndex>> CreateTestBlock(TestChain100Setup* test_setup)
{
    Chainstate& chainstate = test_setup->m_node.chainman->ActiveChainstate();

    std::vector<CKey> keys{test_setup->coinbaseKey};
    std::vector<CMutableTransaction> txs;
    auto input_tx = test_setup->m_coinbase_txns[0];
    const int num_txs = 170;
    const int num_outputs = 49;
    for (int i = 0; i < num_txs; i++) {
        Txid txid = input_tx->GetHash();
        std::vector<COutPoint> inputs;
        if (input_tx->IsCoinBase()) {
            inputs.emplace_back(txid, 0);
        } else {
            for (int i = 0; i < num_outputs; i++) {
                inputs.emplace_back(txid, i);
            }
        }

        std::vector<CTxOut> outputs;
        for (int i = 0; i < num_outputs; i++) {
            const CKey key = GenerateRandomKey();
            const WitnessV1Taproot taproot(XOnlyPubKey(key.GetPubKey()));
            const CScript scriptpubkey = GetScriptForDestination(taproot);
            outputs.emplace_back(COIN, scriptpubkey);
            keys.push_back(key);
        }
        const auto taproot_tx = test_setup->CreateValidTransaction(std::vector{input_tx}, inputs, chainstate.m_chain.Height() + 1, keys, outputs, std::nullopt, std::nullopt);
        txs.push_back(taproot_tx.first);
        input_tx = MakeTransactionRef(taproot_tx.first);
    }

    const WitnessV1Taproot taproot(XOnlyPubKey(test_setup->coinbaseKey.GetPubKey()));
    const CScript coinbase_spk = GetScriptForDestination(taproot);
    const auto test_block = test_setup->CreateBlock(txs, coinbase_spk, chainstate);

    auto pindex = std::make_unique<CBlockIndex>(test_block);
    auto test_blockhash = std::make_unique<uint256>(test_block.GetHash());
    pindex->nHeight = chainstate.m_chain.Height() + 1;
    pindex->phashBlock = test_blockhash.get();
    pindex->pprev = chainstate.m_chain.Tip();
    test_blockhash.release();

    return std::make_pair(test_block, std::move(pindex));
}

static void ConnectBlock(benchmark::Bench& bench)
{
    const auto test_setup = MakeNoLogFileContext<TestChain100Setup>();
    Chainstate& chainstate = test_setup->m_node.chainman->ActiveChainstate();
    const auto result = CreateTestBlock(test_setup.get());
    BlockValidationState test_block_state;
    bench.unit("block").run([&] {
        LOCK(cs_main);
        CCoinsViewCache viewNew(&chainstate.CoinsTip());
        assert(chainstate.ConnectBlock(result.first, test_block_state, result.second.get(), viewNew, true));
    });
}

BENCHMARK(ConnectBlock, benchmark::PriorityLevel::HIGH);
