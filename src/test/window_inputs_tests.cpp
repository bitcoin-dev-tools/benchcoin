// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Tests for a block connected on a BlockApplyView, at the tip and in the connect window: the view against a
// plain cache, a window block's inputs resolved by position, and the view's answers over a window's outputs.
// The window's outputs are a stub here: WindowOutputs lives in validation.cpp alone.

#include <coins.h>
#include <coinsstore.h>
#include <consensus/amount.h>
#include <consensus/merkle.h>
#include <consensus/validation.h>
#include <dbwrapper.h>
#include <kernel/chainparams.h>
#include <pow.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <test/util/setup_common.h>
#include <txdb.h>
#include <undo.h>
#include <util/byte_units.h>
#include <util/not_null.h>
#include <util/threadpool.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

//! Parameters of an in-memory LevelDB for a test, every field named.
static DBParams MemoryDb(const char* name)
{
    return DBParams{
        .path = name,
        .cache_bytes = 1_MiB,
        .memory_only = true,
        .wipe_data = false,
        .obfuscate = false,
        .bloom_filter = true,
        .options = {},
    };
}

BOOST_FIXTURE_TEST_SUITE(window_inputs_tests, BasicTestingSetup)

namespace {

//! The regtest parameters and what ConnectBlock() needs beside them, without a ChainstateManager.
struct Chain {
    std::unique_ptr<const CChainParams> params{CChainParams::RegTest()};
    ValidationCache cache{1 << 20, 1 << 20};

    const Consensus::Params& Consensus() const { return params->GetConsensus(); }

    //! A block at `height` on `prev_hash`: a coinbase that pays the subsidy and `fees`, then `txs`, mined to
    //! regtest difficulty.
    CBlock MakeBlock(int height, const uint256& prev_hash, const std::vector<CTransactionRef>& txs, CAmount fees) const
    {
        CMutableTransaction coinbase;
        coinbase.vin.emplace_back(COutPoint{}, CScript{} << height << OP_0);
        coinbase.vout.emplace_back(GetBlockSubsidy(height, Consensus()) + fees, CScript{} << OP_TRUE);

        CBlock block;
        block.nVersion = 4;
        block.hashPrevBlock = prev_hash;
        block.nTime = params->GenesisBlock().nTime + height;
        block.nBits = params->GenesisBlock().nBits;
        block.vtx.push_back(MakeTransactionRef(std::move(coinbase)));
        for (const CTransactionRef& tx : txs) block.vtx.push_back(tx);
        block.hashMerkleRoot = BlockMerkleRoot(block);
        while (!CheckProofOfWork(block.GetHash(), block.nBits, Consensus())) ++block.nNonce;
        return block;
    }

    //! The context of `block` at `height`, every buried deployment active as on regtest.
    BlockValidationContext Context(const CBlock& block, int height) const
    {
        const DeploymentFlags deployments{.heightincb = true, .dersig = true, .cltv = true, .csv = true, .segwit = true};
        const int64_t genesis_time{params->GenesisBlock().nTime};
        return BlockValidationContext{
            .params = Consensus(),
            .now = NodeClock::now(),
            .deployments = deployments,
            .check_scripts = true,
            .block_hash = block.GetHash(),
            .prev_block_hash = block.hashPrevBlock,
            .height = height,
            .prev_median_time_past = genesis_time,
            .enforce_bip30 = true,
            .median_time_past_at = [genesis_time](int) { return genesis_time; },
        };
    }

    //! ConnectBlock() of a block at the tip, or of any block on a plain cache.
    bool Connect(const CBlock& block, const BlockValidationContext& ctx, CCoinsViewCache& view, BlockValidationState& state, ConnectBlockResult& result)
    {
        return ConnectBlock(block, ctx, view, /*check_queue=*/nullptr, cache, /*just_check=*/false, state, result);
    }

    //! ConnectBlock() of a window block on `view`, staged with the block and its overlay set: the inputs are
    //! resolved by position into `coins` first and the view is told of each transaction, as a window's worker
    //! does (Chainstate::RunWindowValidation()).
    bool ConnectInWindow(const CBlock& block, const BlockValidationContext& ctx, BlockApplyView& view, std::vector<Coin>& coins, BlockValidationState& state, ConnectBlockResult& result)
    {
        view.ResolveInputs(block, coins);
        BlockValidationContext window_ctx{ctx};
        window_ctx.input_coins = coins;
        window_ctx.window_tx = [&view](uint32_t position, const CTransaction* tx, std::span<const Coin> tx_coins) { view.SetTransaction(position, tx, tx_coins); };
        return ConnectBlock(block, window_ctx, view, /*check_queue=*/nullptr, cache, /*just_check=*/false, state, result);
    }
};

/**
 * The window's outputs as a test gives them: blocks by height, each making its transactions' outputs. It
 * answers as WindowOutputs does: a block below the viewer's decides its transactions' outputs, the viewer's
 * own block those of its transactions below the own limit, a block above none. Nothing is published.
 */
class StubWindow final : public WindowCoinSource
{
public:
    //! With it set, NamedIds() names every transaction: a source may name more than it holds.
    bool m_name_all{false};
    mutable int m_named_calls{0};

    void Index(int height, std::vector<CTransactionRef> txs) { m_blocks.emplace_back(height, std::move(txs)); }

    bool FindCoin(const COutPoint& outpoint, int viewer_height, uint32_t own_limit, std::optional<Coin>& coin) const override
    {
        coin.reset();
        for (const auto& [height, txs] : m_blocks) {
            if (height > viewer_height) continue;
            const size_t limit{height == viewer_height ? std::min<size_t>(own_limit, txs.size()) : txs.size()};
            for (size_t i{0}; i < limit; ++i) {
                const CTransaction& tx{*txs[i]};
                if (tx.GetHash() != outpoint.hash) continue;
                if (outpoint.n < tx.vout.size() && !tx.vout[outpoint.n].scriptPubKey.IsUnspendable()) {
                    coin.emplace(tx.vout[outpoint.n], height, tx.IsCoinBase());
                }
                return true;
            }
        }
        return false;
    }

    void FindInputs(std::span<const COutPoint> outpoints, std::span<const uint32_t> positions, int viewer_height,
                    std::vector<uint8_t>& decided, std::vector<Coin>& coins) const override
    {
        for (size_t i{0}; i < outpoints.size(); ++i) {
            std::optional<Coin> coin;
            if (!FindCoin(outpoints[i], viewer_height, positions[i], coin)) continue;
            decided[i] = 1;
            if (coin) coins[i] = std::move(*coin);
        }
    }

    void NamedIds(std::span<const CTransactionRef> txs, int viewer_height, std::vector<uint8_t>& named) const override
    {
        ++m_named_calls;
        named.assign(txs.size(), m_name_all ? 1 : 0);
        for (size_t i{0}; i < txs.size(); ++i) {
            for (const auto& [height, made] : m_blocks) {
                if (height >= viewer_height) continue;
                for (const CTransactionRef& tx : made) {
                    if (tx->GetHash() == txs[i]->GetHash()) named[i] = 1;
                }
            }
        }
    }

private:
    std::vector<std::pair<int, std::vector<CTransactionRef>>> m_blocks;
};

bool SameCoin(const Coin& a, const Coin& b)
{
    return a.out == b.out && a.nHeight == b.nHeight && a.fCoinBase == b.fCoinBase;
}

bool SameEntry(const std::optional<Coin>& a, const std::optional<Coin>& b)
{
    if (!a || !b) return !a && !b;
    return SameCoin(*a, *b);
}

void CheckSameUndo(const CBlockUndo& a, const CBlockUndo& b)
{
    BOOST_REQUIRE_EQUAL(a.vtxundo.size(), b.vtxundo.size());
    for (size_t t{0}; t < a.vtxundo.size(); ++t) {
        BOOST_REQUIRE_EQUAL(a.vtxundo[t].vprevout.size(), b.vtxundo[t].vprevout.size());
        for (size_t i{0}; i < a.vtxundo[t].vprevout.size(); ++i) {
            BOOST_CHECK_MESSAGE(SameCoin(a.vtxundo[t].vprevout[i], b.vtxundo[t].vprevout[i]), "undo of transaction " << t + 1 << " input " << i);
        }
    }
}

//! A transaction spending `prevouts`, with an anyone-can-spend output of each of `values`.
CTransactionRef Spend(const std::vector<COutPoint>& prevouts, const std::vector<CAmount>& values)
{
    CMutableTransaction tx;
    tx.version = 2;
    for (const COutPoint& prevout : prevouts) tx.vin.emplace_back(prevout, CScript{}, CTxIn::SEQUENCE_FINAL);
    for (const CAmount value : values) tx.vout.emplace_back(value, CScript{} << OP_TRUE);
    return MakeTransactionRef(std::move(tx));
}

//! A transaction of its own id, by `tag`, with two outputs, spendable or not.
CTransactionRef Made(uint32_t tag, bool spendable = true)
{
    CMutableTransaction tx;
    tx.vin.emplace_back(COutPoint{Txid::FromUint256(uint256::ONE), tag});
    const CScript script{spendable ? CScript{} << OP_TRUE : CScript{} << OP_RETURN};
    for (int o{0}; o < 2; ++o) tx.vout.emplace_back(COIN, script);
    return MakeTransactionRef(std::move(tx));
}

//! A base cache at `prev_hash` holding `coin` under `outpoint`, as the store's database holds it.
std::unique_ptr<CCoinsViewCache> SeededCache(const uint256& prev_hash, const COutPoint& outpoint, const Coin& coin)
{
    auto base{std::make_unique<CCoinsViewCache>(&CoinsViewEmpty::Get())};
    base->SetBestBlock(prev_hash);
    base->AddCoin(outpoint, Coin{coin}, /*possible_overwrite=*/false);
    return base;
}

//! The coin the blocks at height 2 spend, made at height 1.
const COutPoint SEEDED_OUT{Txid::FromUint256(uint256::ONE), 0};

} // namespace

//! A valid block (a coinbase, a spend of a database coin, and a spend of an output of that spend within the
//! block) connected on the view and committed into the store leaves the coins a plain cache over the same
//! coins leaves once flushed, with the same undo data. The same block in a window, its inputs resolved by
//! position and its in-block spend found in the window's outputs, gives the same undo data too.
BOOST_AUTO_TEST_CASE(block_apply_view_connects_a_block_as_a_cache_does)
{
    Chain chain;
    const uint256 prev{uint256{2}};
    const Coin seeded{CTxOut{COIN, CScript{} << OP_TRUE}, /*nHeightIn=*/1, /*fCoinBaseIn=*/false};
    const CAmount fee{COIN / 100};
    const CTransactionRef first{Spend({SEEDED_OUT}, {COIN / 2, COIN / 2 - fee})};
    const CTransactionRef second{Spend({COutPoint{first->GetHash(), 0}}, {COIN / 2 - fee})};
    const CBlock block{chain.MakeBlock(2, prev, {first, second}, 2 * fee)};
    const BlockValidationContext ctx{chain.Context(block, 2)};

    auto base{SeededCache(prev, SEEDED_OUT, seeded)};
    CCoinsViewCache cache{base.get()};
    BlockValidationState cache_state;
    ConnectBlockResult cache_result;
    BOOST_REQUIRE_MESSAGE(chain.Connect(block, ctx, cache, cache_state, cache_result), cache_state.ToString());
    cache.Flush();

    CCoinsViewDB db{MemoryDb("store"), {}};
    CoinsStore store{&db, 4, true};
    store.SetBestBlock(prev, 1);
    store.AddCoin(SEEDED_OUT, Coin{seeded}, false);
    store.Sync();
    store.EvictClean(0);
    BOOST_REQUIRE_EQUAL(store.GetCacheSize(), 0U); // the coin is in the database only
    util::NotNull pool{std::make_shared<ThreadPool>("test")}; // no workers: lookups are served inline
    BlockApplyView view{store, pool, /*deterministic=*/true};

    // In a window, where the block's outputs are the window's: the undo data, and nothing in the store.
    StubWindow window;
    window.Index(2, block.vtx);
    view.SetHeight(2);
    view.SetBestBlock(prev);
    view.SetOverlay(&window, 2);
    std::vector<Coin> coins;
    BlockValidationState window_state;
    ConnectBlockResult window_result;
    BOOST_REQUIRE_MESSAGE(chain.ConnectInWindow(block, ctx, view, coins, window_state, window_result), window_state.ToString());
    CheckSameUndo(window_result.undo, cache_result.undo);
    view.Release();
    BOOST_CHECK_EQUAL(store.GetCacheSize(), 0U);

    // At the tip, committed alone.
    view.SetHeight(2);
    view.SetBestBlock(prev);
    BlockValidationState state;
    ConnectBlockResult result;
    BOOST_REQUIRE_MESSAGE(chain.Connect(block, ctx, view, state, result), state.ToString());
    CheckSameUndo(result.undo, cache_result.undo);
    view.CommitAlone(block, result.undo);
    view.ConfirmCommitted(2);
    store.SanityCheck();

    BOOST_CHECK(store.GetBestBlock() == block.GetHash());
    BOOST_CHECK(base->GetBestBlock() == block.GetHash());
    const std::vector<COutPoint> universe{SEEDED_OUT, COutPoint{block.vtx[0]->GetHash(), 0}, COutPoint{first->GetHash(), 0},
                                          COutPoint{first->GetHash(), 1}, COutPoint{second->GetHash(), 0}};
    for (const COutPoint& outpoint : universe) {
        BOOST_CHECK_MESSAGE(SameEntry(store.PeekCoin(outpoint), base->PeekCoin(outpoint)), "mismatch at " << outpoint.ToString());
    }
    BOOST_CHECK(!store.HaveCoin(SEEDED_OUT));
    BOOST_CHECK(!store.HaveCoin(COutPoint{first->GetHash(), 0}));
    BOOST_CHECK(store.HaveCoin(COutPoint{second->GetHash(), 0}));
}

//! A block that spends a coin twice, and one that spends a coin there is none of, is rejected for the same
//! reason by a plain cache, by the view at the tip and by the view in a window, and the store is left as it
//! was: its coin unspent, its best block where it was and no entry brought in.
BOOST_AUTO_TEST_CASE(block_apply_view_rejects_a_block_as_a_cache_does)
{
    Chain chain;
    const uint256 prev{uint256{2}};
    const Coin seeded{CTxOut{COIN, CScript{} << OP_TRUE}, /*nHeightIn=*/1, /*fCoinBaseIn=*/false};
    const CBlock double_spend{chain.MakeBlock(2, prev, {Spend({SEEDED_OUT}, {COIN / 2}), Spend({SEEDED_OUT}, {COIN / 4})}, 0)};
    const COutPoint missing{Txid::FromUint256(uint256{99}), 0};
    const CBlock missing_input{chain.MakeBlock(2, prev, {Spend({missing}, {COIN / 2})}, 0)};

    CCoinsViewDB db{MemoryDb("store"), {}};
    CoinsStore store{&db, 4, true};
    store.SetBestBlock(prev, 1);
    store.AddCoin(SEEDED_OUT, Coin{seeded}, false);
    store.Sync();
    store.EvictClean(0);
    util::NotNull pool{std::make_shared<ThreadPool>("test")}; // no workers: lookups are served inline
    BlockApplyView view{store, pool, /*deterministic=*/true};

    for (const CBlock& block : {double_spend, missing_input}) {
        const BlockValidationContext ctx{chain.Context(block, 2)};
        auto base{SeededCache(prev, SEEDED_OUT, seeded)};
        CCoinsViewCache cache{base.get()};
        BlockValidationState cache_state;
        ConnectBlockResult cache_result;
        BOOST_CHECK(!chain.Connect(block, ctx, cache, cache_state, cache_result));
        BOOST_CHECK_EQUAL(cache_state.GetRejectReason(), "bad-txns-inputs-missingorspent");

        view.SetHeight(2);
        view.SetBestBlock(prev);
        BlockValidationState state;
        ConnectBlockResult result;
        BOOST_CHECK(!chain.Connect(block, ctx, view, state, result));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), cache_state.GetRejectReason());
        view.Release();

        StubWindow window;
        window.Index(2, block.vtx);
        view.SetHeight(2);
        view.SetBestBlock(prev);
        view.SetOverlay(&window, 2);
        std::vector<Coin> coins;
        BlockValidationState window_state;
        ConnectBlockResult window_result;
        BOOST_CHECK(!chain.ConnectInWindow(block, ctx, view, coins, window_state, window_result));
        BOOST_CHECK_EQUAL(window_state.GetRejectReason(), cache_state.GetRejectReason());
        view.Release();

        BOOST_CHECK(SameEntry(store.PeekCoin(SEEDED_OUT), seeded));
        BOOST_CHECK(store.GetBestBlock() == prev);
        BOOST_CHECK_EQUAL(store.GetCacheSize(), 0U);
        store.SanityCheck();
    }
}

//! FetchInto() reads the tier alone, at the positions it is given in any order: a coin found lands at its own
//! position, a coin only the database has, a spent entry and a position not asked for are not found, and
//! what is not found is left as it was.
BOOST_AUTO_TEST_CASE(fetch_into_fills_the_positions_it_is_given)
{
    CCoinsViewDB db{MemoryDb("store"), {}};
    CoinsStore store{&db, 4, true};
    store.SetBestBlock(uint256::ONE, 1);
    const std::vector<COutPoint> outpoints{
        COutPoint{Txid::FromUint256(uint256{10}), 0}, // in the database
        COutPoint{Txid::FromUint256(uint256{11}), 0}, // in the tier
        COutPoint{Txid::FromUint256(uint256{12}), 0}, // spent in the tier
        COutPoint{Txid::FromUint256(uint256{13}), 0}, // nowhere, and not asked for
        COutPoint{Txid::FromUint256(uint256{14}), 1}, // in the tier
    };
    std::vector<Coin> made;
    for (uint32_t i{0}; i < outpoints.size(); ++i) made.emplace_back(CTxOut{COIN * (i + 1), CScript{} << OP_TRUE}, 1, false);
    store.AddCoin(outpoints[0], Coin{made[0]}, false);
    store.AddCoin(outpoints[2], Coin{made[2]}, false);
    store.Sync();
    store.EvictClean(0);
    store.AddCoin(outpoints[1], Coin{made[1]}, false);
    store.AddCoin(outpoints[4], Coin{made[4]}, false);
    BOOST_REQUIRE(store.SpendCoin(outpoints[2]));

    const Coin untouched{CTxOut{7, CScript{} << OP_FALSE}, 9, true};
    std::vector<Coin> coins(outpoints.size(), untouched);
    std::vector<uint8_t> found;
    const std::vector<uint32_t> which{4, 2, 0, 1};
    store.FetchInto(outpoints, which, coins, found);
    BOOST_REQUIRE_EQUAL(found.size(), outpoints.size());
    const std::vector<uint8_t> expected_found{0, 1, 0, 0, 1};
    for (size_t i{0}; i < outpoints.size(); ++i) {
        BOOST_CHECK_EQUAL(found[i], expected_found[i]);
        BOOST_CHECK(SameCoin(coins[i], found[i] ? made[i] : untouched));
    }
    store.SanityCheck();
}

//! A window block's inputs come back by input position from the three places a coin is: the window's outputs
//! (a block below in the window, and an earlier transaction of the block itself), the tier and the database;
//! the inputs the window does not decide are not a prefix, so the store reads at positions out of line. The
//! block connected with them keeps the same coins in its undo data, input for input, and a database coin read
//! for a window block stays in the tier. An input whose coin the tier has spent, or that no place has, comes
//! back spent, though the database still holds the first.
BOOST_AUTO_TEST_CASE(resolve_inputs_by_position_from_window_tier_and_database)
{
    Chain chain;
    CCoinsViewDB db{MemoryDb("store"), {}};
    CoinsStore store{&db, 4, true};
    store.SetBestBlock(uint256::ONE, 1);
    const COutPoint in_db{Txid::FromUint256(uint256{10}), 0};
    const COutPoint in_tier{Txid::FromUint256(uint256{11}), 0};
    const COutPoint spent_in_tier{Txid::FromUint256(uint256{12}), 0};
    const Coin db_coin{CTxOut{COIN, CScript{} << OP_TRUE}, 1, false};
    const Coin tier_coin{CTxOut{2 * COIN, CScript{} << OP_TRUE}, 1, false};
    store.AddCoin(in_db, Coin{db_coin}, false);
    store.AddCoin(spent_in_tier, Coin{db_coin}, false);
    store.Sync();
    store.EvictClean(0);
    store.AddCoin(in_tier, Coin{tier_coin}, false);
    BOOST_REQUIRE(store.SpendCoin(spent_in_tier));

    // Block 2 is in the window, unpublished; block 3 spends one of its outputs.
    const CTransactionRef lower{Spend({COutPoint{Txid::FromUint256(uint256{20}), 0}}, {3 * COIN})};
    const CAmount fee{COIN / 100};
    const CTransactionRef first{Spend({COutPoint{lower->GetHash(), 0}, in_db}, {2 * COIN, 2 * COIN - fee})};
    const CTransactionRef second{Spend({COutPoint{first->GetHash(), 0}, in_tier}, {4 * COIN - fee})};
    const uint256 prev{uint256{2}};
    const CBlock block{chain.MakeBlock(3, prev, {first, second}, 2 * fee)};
    StubWindow window;
    window.Index(2, {lower});
    window.Index(3, block.vtx);

    util::NotNull pool{std::make_shared<ThreadPool>("test")}; // no workers: lookups are served inline
    BlockApplyView view{store, pool, /*deterministic=*/true};
    view.SetHeight(3);
    view.SetBestBlock(prev);
    view.SetKeepBaseReads(true);
    view.SetOverlay(&window, 3);
    std::vector<Coin> coins;
    BlockValidationState state;
    ConnectBlockResult result;
    BOOST_REQUIRE_MESSAGE(chain.ConnectInWindow(block, chain.Context(block, 3), view, coins, state, result), state.ToString());
    const std::vector<Coin> expected{Coin{lower->vout[0], 2, false}, db_coin, Coin{first->vout[0], 3, false}, tier_coin};
    BOOST_REQUIRE_EQUAL(coins.size(), expected.size());
    for (size_t i{0}; i < expected.size(); ++i) BOOST_CHECK_MESSAGE(SameCoin(coins[i], expected[i]), "input " << i);
    CBlockUndo expected_undo;
    expected_undo.vtxundo.resize(2);
    expected_undo.vtxundo[0].vprevout = {expected[0], expected[1]};
    expected_undo.vtxundo[1].vprevout = {expected[2], expected[3]};
    CheckSameUndo(result.undo, expected_undo);
    BOOST_CHECK(store.HaveCoinInCache(in_db));
    view.Release();

    CBlock unfound;
    unfound.vtx.push_back(MakeTransactionRef(CMutableTransaction{}));
    unfound.vtx.push_back(Spend({spent_in_tier, COutPoint{Txid::FromUint256(uint256{99}), 0}}, {COIN}));
    view.SetHeight(3);
    view.SetOverlay(&window, 3);
    view.ResolveInputs(unfound, coins);
    BOOST_REQUIRE_EQUAL(coins.size(), 2U);
    BOOST_CHECK(coins[0].IsSpent());
    BOOST_CHECK(coins[1].IsSpent());
    BOOST_CHECK(db.HaveCoin(spent_in_tier));
    view.Release();
    store.SanityCheck();
}

//! Before a window block's first transaction the view answers the BIP30 check for all of the block's
//! transactions at once, asking the window once; that answer is the one HaveCoin() gives output by output,
//! whether no output, one or all are present, in the tier, in the database or in a block below in the window,
//! where the window's answer stands over the store's, and whether or not the window names more than it holds.
//! The viewer's own block and a block above it are not the window's to answer for then.
BOOST_AUTO_TEST_CASE(have_any_output_agrees_with_have_coin)
{
    CCoinsViewDB db{MemoryDb("store"), {}};
    CoinsStore store{&db, 4, true};
    store.SetBestBlock(uint256::ONE, 1);
    const CTransactionRef none_a{Made(1)};
    const CTransactionRef none_b{Made(2)};
    const CTransactionRef in_tier{Made(3)};
    const CTransactionRef in_db{Made(4)};
    const CTransactionRef in_window{Made(5)};
    const CTransactionRef shadowed{Made(6, /*spendable=*/false)};
    const CTransactionRef own{Made(7)};
    const CTransactionRef above{Made(8)};
    store.AddCoin(COutPoint{in_db->GetHash(), 0}, Coin{in_db->vout[0], 1, false}, false);
    store.Sync();
    store.EvictClean(0);
    store.AddCoin(COutPoint{in_tier->GetHash(), 1}, Coin{in_tier->vout[1], 1, false}, false);
    store.AddCoin(COutPoint{shadowed->GetHash(), 0}, Coin{CTxOut{COIN, CScript{} << OP_TRUE}, 1, false}, false);
    StubWindow window;
    window.Index(2, {in_window, shadowed});
    window.Index(3, {own});
    window.Index(4, {above});

    util::NotNull pool{std::make_shared<ThreadPool>("test")}; // no workers: lookups are served inline
    const auto at_once{[&](const std::vector<CTransactionRef>& txs) {
        BlockApplyView view{store, pool, /*deterministic=*/true};
        view.SetHeight(3);
        view.SetOverlay(&window, 3);
        return view.HaveAnyOutput(txs);
    }};
    const auto one_by_one{[&](const std::vector<CTransactionRef>& txs) {
        BlockApplyView view{store, pool, /*deterministic=*/true};
        view.SetHeight(3);
        view.SetOverlay(&window, 3);
        for (const CTransactionRef& tx : txs) {
            for (uint32_t o{0}; o < tx->vout.size(); ++o) {
                if (view.HaveCoin(COutPoint{tx->GetHash(), o})) return true;
            }
        }
        return false;
    }};
    const std::vector<std::pair<std::vector<CTransactionRef>, bool>> cases{
        {{none_a, none_b}, false},
        {{none_a, in_tier}, true},
        {{in_db, none_b}, true},
        {{none_a, in_window}, true},
        {{in_tier, in_db, in_window}, true},
        {{shadowed}, false},
        {{own, above}, false},
    };
    for (const bool name_all : {false, true}) {
        window.m_name_all = name_all;
        for (const auto& [txs, present] : cases) {
            window.m_named_calls = 0;
            const bool answer{at_once(txs)};
            BOOST_CHECK_EQUAL(window.m_named_calls, 1);
            BOOST_CHECK_EQUAL(answer, one_by_one(txs));
            BOOST_CHECK_EQUAL(answer, present);
        }
    }
    store.SanityCheck();
}

//! SetTransaction() sets the own limit: a transaction of the viewer's own block counts in the window's
//! outputs below the position given and not at it or above, a block below counts whatever the position and
//! a block above never does. Past the first transaction the BIP30 answer is asked output by output, so that it
//! sees the block's own outputs as a lookup does. A view given back starts again at no own transaction.
BOOST_AUTO_TEST_CASE(own_limit_follows_set_transaction)
{
    CCoinsViewDB db{MemoryDb("store"), {}};
    CoinsStore store{&db, 4, true};
    store.SetBestBlock(uint256::ONE, 1);
    const CTransactionRef lower{Made(1)};
    const std::vector<CTransactionRef> own{Made(2), Made(3), Made(4), Made(5)};
    const CTransactionRef upper{Made(6)};
    StubWindow window;
    window.Index(2, {lower});
    window.Index(3, own);
    window.Index(4, {upper});
    util::NotNull pool{std::make_shared<ThreadPool>("test")}; // no workers: lookups are served inline
    BlockApplyView view{store, pool, /*deterministic=*/true};

    const std::vector<CTransactionRef> second_only{own[1]};
    for (uint32_t limit{0}; limit <= own.size(); ++limit) {
        view.SetHeight(3);
        view.SetOverlay(&window, 3);
        view.SetTransaction(limit, nullptr, {});
        window.m_named_calls = 0;
        BOOST_CHECK_EQUAL(view.HaveAnyOutput(second_only), 1 < limit);
        BOOST_CHECK_EQUAL(window.m_named_calls, limit == 0 ? 1 : 0);
        BOOST_CHECK(view.HaveCoin(COutPoint{lower->GetHash(), 0}));
        BOOST_CHECK(!view.HaveCoin(COutPoint{upper->GetHash(), 0}));
        for (uint32_t i{0}; i < own.size(); ++i) {
            BOOST_CHECK_EQUAL(view.HaveCoin(COutPoint{own[i]->GetHash(), 0}), i < limit);
        }
        view.Release();
    }
    view.SetHeight(3);
    view.SetOverlay(&window, 3);
    BOOST_CHECK(!view.HaveCoin(COutPoint{own[0]->GetHash(), 0}));
    view.Release();
}

//! While a transaction is checked, a lookup made with one of its inputs' own prevouts is answered from the
//! coins SetTransaction() gave, a spent one included; the same outpoint held elsewhere is the view's to answer,
//! as is every lookup once no transaction is set.
BOOST_AUTO_TEST_CASE(transaction_inputs_answered_by_address)
{
    CCoinsViewDB db{MemoryDb("store"), {}};
    CoinsStore store{&db, 4, true};
    store.SetBestBlock(uint256::ONE, 1);
    const COutPoint held{Txid::FromUint256(uint256{10}), 0};
    store.AddCoin(held, Coin{CTxOut{5 * COIN, CScript{} << OP_TRUE}, 1, false}, false);
    const CTransactionRef tx{Spend({held}, {COIN})};
    const COutPoint& prevout{tx->vin[0].prevout};
    const COutPoint copy{prevout};
    util::NotNull pool{std::make_shared<ThreadPool>("test")}; // no workers: lookups are served inline
    BlockApplyView view{store, pool, /*deterministic=*/true};
    view.SetHeight(2);

    const std::vector<Coin> given{Coin{CTxOut{7 * COIN, CScript{} << OP_TRUE}, 1, false}};
    view.SetTransaction(1, tx.get(), given);
    BOOST_CHECK_EQUAL(view.AccessCoin(prevout).out.nValue, 7 * COIN);
    BOOST_CHECK(view.HaveCoin(prevout));
    BOOST_CHECK_EQUAL(view.AccessCoin(copy).out.nValue, 5 * COIN);

    const std::vector<Coin> spent(1);
    view.SetTransaction(1, tx.get(), spent);
    BOOST_CHECK(!view.HaveCoin(prevout));
    BOOST_CHECK(view.HaveCoin(copy));

    view.SetTransaction(2, nullptr, {});
    BOOST_CHECK_EQUAL(view.AccessCoin(prevout).out.nValue, 5 * COIN);
    view.Release();
    store.SanityCheck();
}

BOOST_AUTO_TEST_SUITE_END()
