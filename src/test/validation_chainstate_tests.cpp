// Copyright (c) 2020-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
#include <chain.h>
#include <chainparams.h>
#include <coins.h>
#include <consensus/amount.h>
#include <consensus/merkle.h>
#include <consensus/validation.h>
#include <flatfile.h>
#include <hash.h>
#include <logging.h>
#include <node/blockstorage.h>
#include <node/kernel_notifications.h>
#include <node/miner.h>
#include <pow.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <random.h>
#include <script/script.h>
#include <script/solver.h>
#include <sync.h>
#include <test/util/chainstate.h>
#include <test/util/coins.h>
#include <test/util/common.h>
#include <test/util/logging.h>
#include <test/util/setup_common.h>
#include <test/util/validation.h>
#include <tinyformat.h>
#include <uint256.h>
#include <undo.h>
#include <util/check.h>
#include <util/string.h>
#include <util/threadpool.h>
#include <util/time.h>
#include <coinsstore.h>
#include <validation.h>
#include <validationinterface.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <latch>
#include <memory>
#include <mutex>
#include <optional>
#include <semaphore>
#include <string>
#include <thread>
#include <utility>
#include <vector>

class CTxMemPool;

BOOST_FIXTURE_TEST_SUITE(validation_chainstate_tests, ChainTestingSetup)

//! Test resizing coins-related Chainstate caches during runtime.
//!
BOOST_AUTO_TEST_CASE(validation_chainstate_resize_caches)
{
    ChainstateManager& manager = *Assert(m_node.chainman);
    CTxMemPool& mempool = *Assert(m_node.mempool);
    Chainstate& c1 = WITH_LOCK(cs_main, return manager.InitializeChainstate(&mempool));
    c1.InitCoinsDB(
        /*cache_size_bytes=*/8_MiB, /*in_memory=*/true, /*should_wipe=*/false);
    WITH_LOCK(::cs_main, c1.InitCoinsCache(8_MiB));
    BOOST_REQUIRE(manager.LoadGenesisBlock()); // Need at least one block loaded to be able to flush caches

    // Add a coin to the in-memory cache, upsize once, then downsize.
    {
        LOCK(::cs_main);
        const auto outpoint = AddTestCoin(m_rng, c1.CoinsTip());

        // Set a meaningless bestblock value in the coinsview cache - otherwise we won't
        // flush during ResizeCoinsCaches() and will subsequently hit an assertion.
        c1.CoinsTip().SetBestBlock(m_rng.rand256());

        BOOST_CHECK(c1.CoinsTip().HaveCoinInCache(outpoint));

        c1.ResizeCoinsCaches(
            16_MiB, // upsizing the coinsview cache
            4_MiB // downsizing the coinsdb cache
        );

        // View should still have the coin cached, since we haven't destructed the cache on upsize.
        BOOST_CHECK(c1.CoinsTip().HaveCoinInCache(outpoint));

        c1.ResizeCoinsCaches(
            4_MiB, // downsizing the coinsview cache
            8_MiB // upsizing the coinsdb cache
        );

        // The view cache should be empty since we had to destruct to downsize.
        BOOST_CHECK(!c1.CoinsTip().HaveCoinInCache(outpoint));
    }
}

BOOST_FIXTURE_TEST_CASE(connect_tip_does_not_cache_inputs_on_failed_connect, TestChain100Setup)
{
    Chainstate& chainstate{Assert(m_node.chainman)->ActiveChainstate()};

    COutPoint outpoint;
    {
        LOCK(cs_main);
        outpoint = AddTestCoin(m_rng, chainstate.CoinsTip());
        chainstate.CoinsTip().Flush(/*reallocate_cache=*/false);
    }

    CMutableTransaction tx;
    tx.vin.emplace_back(outpoint);
    tx.vout.emplace_back(MAX_MONEY, CScript{} << OP_TRUE);

    const auto tip{WITH_LOCK(cs_main, return chainstate.m_chain.Tip()->GetBlockHash())};
    const CBlock block{CreateBlock({tx}, CScript{} << OP_TRUE)};
    BOOST_CHECK(Assert(m_node.chainman)->ProcessNewBlock(std::make_shared<CBlock>(block), true, true, nullptr));

    LOCK(cs_main);
    BOOST_CHECK_EQUAL(tip, chainstate.m_chain.Tip()->GetBlockHash()); // block rejected
    BOOST_CHECK(!chainstate.CoinsTip().HaveCoinInCache(outpoint));    // input not cached
}

//! Test UpdateTip behavior for both active and background chainstates.
//!
//! When run on the background chainstate, UpdateTip should do a subset
//! of what it does for the active chainstate.
BOOST_FIXTURE_TEST_CASE(chainstate_update_tip, TestChain100Setup)
{
    ChainstateManager& chainman = *Assert(m_node.chainman);
    const auto get_notify_tip{[&]() {
        LOCK(m_node.notifications->m_tip_block_mutex);
        BOOST_REQUIRE(m_node.notifications->TipBlock());
        return *m_node.notifications->TipBlock();
    }};
    uint256 curr_tip = get_notify_tip();

    // Mine 10 more blocks, putting at us height 110 where a valid assumeutxo value can
    // be found.
    mineBlocks(10);

    // After adding some blocks to the tip, best block should have changed.
    BOOST_CHECK(get_notify_tip() != curr_tip);

    // Grab block 1 from disk; we'll add it to the background chain later.
    std::shared_ptr<CBlock> pblockone = std::make_shared<CBlock>();
    {
        LOCK(::cs_main);
        BLOCK_FILE_WORK_UNDER_CS_MAIN("a test");
        chainman.m_blockman.ReadBlock(*pblockone, *chainman.ActiveChain()[1]);
    }

    BOOST_REQUIRE(CreateAndActivateUTXOSnapshot(
        this, NoMalleation, /*reset_chainstate=*/ true));

    // Ensure our active chain is the snapshot chainstate.
    BOOST_CHECK(WITH_LOCK(::cs_main, return chainman.CurrentChainstate().m_from_snapshot_blockhash));

    curr_tip = get_notify_tip();

    // Mine a new block on top of the activated snapshot chainstate.
    mineBlocks(1);  // Defined in TestChain100Setup.

    // After adding some blocks to the snapshot tip, best block should have changed.
    BOOST_CHECK(get_notify_tip() != curr_tip);

    curr_tip = get_notify_tip();

    Chainstate& background_cs{*Assert(WITH_LOCK(::cs_main, return chainman.HistoricalChainstate()))};

    // Append the first block to the background chain.
    BlockValidationState state;
    CBlockIndex* pindex = nullptr;
    const CChainParams& chainparams = Params();
    bool newblock = false;

    // TODO: much of this is inlined from ProcessNewBlock(); just reuse PNB()
    // once it is changed to support multiple chainstates.
    {
        LOCK(::cs_main);
        bool checked = CheckBlock(*pblockone, state, chainparams.GetConsensus());
        BOOST_CHECK(checked);
        bool accepted = chainman.AcceptBlock(
            pblockone, state, &pindex, true, nullptr, &newblock, true);
        BOOST_CHECK(accepted);
    }

    // UpdateTip is called here
    bool block_added = background_cs.ActivateBestChain(state, pblockone);

    // Ensure tip is as expected
    BOOST_CHECK_EQUAL(background_cs.m_chain.Tip()->GetBlockHash(), pblockone->GetHash());

    // get_notify_tip() should be unchanged after adding a block to the background
    // validation chain.
    BOOST_CHECK(block_added);
    BOOST_CHECK_EQUAL(curr_tip, get_notify_tip());
}

//! A coin created at or below the height the coins database settled at and spent above it, before a run carried
//! it, cancels in the young tier: the database never holds it, although it is alive at that height. A
//! disconnect of the spend restores it. However the write-out runs after that are bounded, the database must
//! then hold the coin, or be settled below its creation, where a replay creates it again.
BOOST_FIXTURE_TEST_CASE(disconnect_restores_a_coin_the_database_never_held, TestChain100Setup)
{
    ChainstateManager& chainman{*Assert(m_node.chainman)};
    Chainstate& chainstate{chainman.ActiveChainstate()};
    const CScript script{GetScriptForRawPubKey(coinbaseKey.GetPubKey())};
    // A write-out run bounded at `bound` (unless it must be final), started as FlushStateToDisk() starts one and
    // waited for.
    const auto run_to{[&](int bound) {
        LOCK(::cs_main);
        BlockValidationState run_state;
        BOOST_REQUIRE(chainstate.StartWriteOutRun(run_state, bound, /*final=*/false));
        chainstate.SettleWriteOut();
    }};

    WITH_LOCK(::cs_main, chainstate.ForceFlushStateToDisk());
    const int base{WITH_LOCK(::cs_main, return chainstate.m_chain.Height())};
    BOOST_REQUIRE(WITH_LOCK(::cs_main, return chainstate.CoinsStoreTip().WrittenHead() == chainstate.m_chain.Tip()->GetBlockHash()));

    // X is created in base+1 and spent in base+5.
    const CMutableTransaction create{CreateValidMempoolTransaction(m_coinbase_txns[0], 0, 1, coinbaseKey, script, 49 * COIN, /*submit=*/false)};
    CreateAndProcessBlock({create}, script);
    const COutPoint coin{create.GetHash(), 0};
    for (int i{0}; i < 3; ++i) CreateAndProcessBlock({}, script);
    const CMutableTransaction spend{CreateValidMempoolTransaction(MakeTransactionRef(create), 0, base + 1, coinbaseKey, script, 48 * COIN, /*submit=*/false)};
    const CBlock spend_block{CreateAndProcessBlock({spend}, script)};
    CreateAndProcessBlock({}, script);

    // A run bounded at base+2: X was created and spent inside the lag, so it cancels; the database settles
    // at base+2 without it.
    run_to(base + 2);
    {
        LOCK(::cs_main);
        BOOST_REQUIRE(chainstate.CoinsStoreTip().WrittenHead() == chainstate.m_chain[base + 2]->GetBlockHash());
        BOOST_REQUIRE(!chainstate.CoinsDB().HaveCoin(coin));
    }

    // The spend is disconnected (the fork is at base+4): X is alive again.
    CBlockIndex* const spend_index{WITH_LOCK(::cs_main, return chainman.m_blockman.LookupBlockIndex(spend_block.GetHash()))};
    BlockValidationState state;
    BOOST_REQUIRE(chainstate.InvalidateBlock(state, spend_index));
    BOOST_REQUIRE(WITH_LOCK(::cs_main, return chainstate.CoinsTip().HaveCoin(coin)));

    // A run bounded between the settled height and the fork.
    run_to(base + 3);
    LOCK(::cs_main);
    chainman.m_blockman.SettleUndoWrites();
    const CBlockIndex* const written{chainman.m_blockman.LookupBlockIndex(chainstate.CoinsStoreTip().WrittenHead())};
    BOOST_REQUIRE(written);
    BOOST_CHECK_MESSAGE(chainstate.CoinsDB().HaveCoin(coin) || written->nHeight < base + 1,
                        "the database settled at " << written->nHeight << " without the restored coin");
}

//! A disconnect writes the coins it restores only on the written head's own chain. With the written head on
//! another branch, a restored coin may come from a block that no chain the database can be replayed to holds:
//! written, it would stay in the database after a crash as a coin nothing created.
BOOST_FIXTURE_TEST_CASE(disconnect_writes_no_coin_of_another_branch, TestChain100Setup)
{
    ChainstateManager& chainman{*Assert(m_node.chainman)};
    Chainstate& chainstate{chainman.ActiveChainstate()};
    const CScript script{GetScriptForRawPubKey(coinbaseKey.GetPubKey())};

    // The database is written at the tip, block T, two blocks up so that the fork can spend a coinbase.
    mineBlocks(2);
    WITH_LOCK(::cs_main, chainstate.ForceFlushStateToDisk());
    CBlockIndex* const tip{WITH_LOCK(::cs_main, return chainstate.m_chain.Tip())};
    BOOST_REQUIRE(WITH_LOCK(::cs_main, return chainstate.CoinsStoreTip().WrittenHead()) == tip->GetBlockHash());

    // A fork from T-1: T' creates a coin Y that T'+1 spends.
    BlockValidationState state;
    BOOST_REQUIRE(chainstate.InvalidateBlock(state, tip));
    const CMutableTransaction create{CreateValidMempoolTransaction(m_coinbase_txns[0], 0, 1, coinbaseKey, script, 49 * COIN, /*submit=*/false)};
    const CBlock fork1{CreateAndProcessBlock({create}, script)};
    const COutPoint y{create.GetHash(), 0};
    const CMutableTransaction spend{CreateValidMempoolTransaction(MakeTransactionRef(create), 0, tip->nHeight, coinbaseKey, script, 48 * COIN, /*submit=*/false)};
    CreateAndProcessBlock({spend}, script);
    BOOST_REQUIRE_EQUAL(WITH_LOCK(::cs_main, return chainstate.m_chain.Height()), tip->nHeight + 1);
    BOOST_REQUIRE(WITH_LOCK(::cs_main, return chainstate.CoinsStoreTip().WrittenHead()) == tip->GetBlockHash());
    // T'+1 and T' are disconnected: Y comes back with T'+1's disconnect, then goes with T''s.
    CBlockIndex* const fork1_index{WITH_LOCK(::cs_main, return chainman.m_blockman.LookupBlockIndex(fork1.GetHash()))};
    BOOST_REQUIRE(chainstate.InvalidateBlock(state, fork1_index));
    LOCK(::cs_main);
    chainman.m_blockman.SettleUndoWrites();
    BOOST_CHECK(!chainstate.CoinsTip().HaveCoin(y));
    // The coins the next run writes before its marker: Y, created at the written height but on T''s branch,
    // is not among them.
    const auto restored{chainstate.CoinsStoreTip().TakeRestored()};
    BOOST_CHECK_MESSAGE(std::none_of(restored.begin(), restored.end(), [&](const CoinWrite& write) { return write.outpoint == y; }),
                        "a coin of a branch the written head is not on is to be written");
    BOOST_CHECK(!chainstate.CoinsDB().HaveCoin(y));
}

//! A block index write records a block whose undo write is still queued with the undo data it is getting, so
//! that a write-out run need not wait for the write under cs_main: the run commits the batch only behind the
//! write. The entry itself is left as it is until the write's completion records it.
BOOST_FIXTURE_TEST_CASE(index_batch_records_a_queued_undo_write, TestChain100Setup)
{
    ChainstateManager& chainman{*Assert(m_node.chainman)};
    auto& blockman{static_cast<TestBlockManager&>(chainman.m_blockman)};
    blockman.SettleUndoWrites();
    LOCK(::cs_main);
    BLOCK_FILE_WORK_UNDER_CS_MAIN("a test");
    CBlockIndex* tip{chainman.ActiveChain().Tip()};
    const uint32_t status{tip->nStatus};
    const uint64_t undo_pos{tip->nUndoPos};
    // No entry is left to write: the claim alone has the tip's written, as it must for a block whose
    // validity does not rise when it is connected (one stored again after its files were pruned).
    {
        auto written{blockman.PrepareBlockIndexBatch()};
        BOOST_REQUIRE(blockman.SyncBatchFiles(written));
        blockman.CommitBlockIndexBatch(written);
    }
    // The tip as a block just published has it: no undo data recorded, its write claimed and queued.
    tip->nStatus &= ~BLOCK_HAVE_UNDO;
    tip->nUndoPos = 0;
    const FlatFilePos claimed{blockman.ClaimUndoPos(*tip, /*undo_size=*/100)};
    auto batch{blockman.PrepareBlockIndexBatch()};
    BOOST_REQUIRE(blockman.SyncBatchFiles(batch));
    blockman.CommitBlockIndexBatch(batch);

    CDiskBlockIndex disk;
    BOOST_REQUIRE(blockman.m_block_tree_db->Read(std::make_pair(uint8_t{'b'}, tip->GetBlockHash()), disk));
    BOOST_CHECK(disk.nStatus & BLOCK_HAVE_UNDO);
    BOOST_CHECK_EQUAL(disk.nUndoPos, claimed.nPos + node::STORAGE_HEADER_BYTES);
    BOOST_CHECK(!(tip->nStatus & BLOCK_HAVE_UNDO));

    // Put back as it was.
    blockman.UndoWriteSettled(*tip);
    tip->nStatus = status;
    tip->nUndoPos = undo_pos;
    blockman.MarkDirty(tip);
}

//! In a reorganisation the transactions of the disconnected blocks are back in the mempool, and announced,
//! before the blocks of the new branch are announced connected. With `periodic_due` the periodic write-out
//! is due when the reorganisation starts. With `pinned_over_limit` the first block the reorganisation
//! connects takes the account of pinned blocks over its limit.
static void CheckReorgAnnouncementOrder(TestChain100Setup& setup, bool periodic_due = false, bool pinned_over_limit = false)
{
    auto& m_node{setup.m_node};
    auto& m_coinbase_txns{setup.m_coinbase_txns};
    auto& coinbaseKey{setup.coinbaseKey};
    const auto CreateAndProcessBlock{[&](const std::vector<CMutableTransaction>& txns, const CScript& script) { return setup.CreateAndProcessBlock(txns, script); }};
    const auto CreateValidMempoolTransaction{[&](CTransactionRef in, uint32_t vout, int height, CKey key, CScript out, CAmount amount) {
        return setup.CreateValidMempoolTransaction(in, vout, height, key, out, amount);
    }};
    struct Recorder final : CValidationInterface {
        std::vector<std::string> events;
        void TransactionAddedToMempool(const NewMempoolTransactionInfo& tx, uint64_t) override { events.push_back("A " + tx.info.m_tx->GetHash().ToString()); }
        void BlockConnected(const kernel::ChainstateRole&, const std::shared_ptr<const CBlock>& block, const CBlockIndex*) override { events.push_back("C " + block->GetHash().ToString()); }
    };
    ChainstateManager& chainman{*Assert(m_node.chainman)};
    Chainstate& chainstate{chainman.ActiveChainstate()};
    const CScript script{CScript() << OP_TRUE};

    // Two empty blocks on top of 100, set aside; then a block 101 that mines two mempool transactions, the
    // second spending the first.
    const CBlock fork1{CreateAndProcessBlock({}, script)};
    const CBlock fork2{CreateAndProcessBlock({}, script)};
    CBlockIndex* const fork1_index{WITH_LOCK(::cs_main, return chainman.m_blockman.LookupBlockIndex(fork1.GetHash()))};
    BlockValidationState state;
    BOOST_REQUIRE(chainstate.InvalidateBlock(state, fork1_index));
    const CMutableTransaction tx{CreateValidMempoolTransaction(m_coinbase_txns[0], 0, 1, coinbaseKey, GetScriptForRawPubKey(coinbaseKey.GetPubKey()), 49 * COIN)};
    const CMutableTransaction tx2{CreateValidMempoolTransaction(MakeTransactionRef(tx), 0, 101, coinbaseKey, GetScriptForRawPubKey(coinbaseKey.GetPubKey()), 48 * COIN)};
    const CBlock mined{CreateAndProcessBlock({tx, tx2}, script)};
    BOOST_REQUIRE_EQUAL(WITH_LOCK(::cs_main, return chainstate.m_chain.Tip()->GetBlockHash()), mined.GetHash());
    BOOST_REQUIRE_EQUAL(WITH_LOCK(m_node.mempool->cs, return m_node.mempool->size()), 0U);

    // The two blocks back: the reorganisation disconnects 101 and connects them.
    auto recorder{std::make_shared<Recorder>()};
    m_node.validation_signals->RegisterSharedValidationInterface(recorder);
    {
        LOCK(::cs_main);
        chainstate.ResetBlockFailureFlags(fork1_index);
        chainman.RecalculateBestHeader();
    }
    // Past the interval of the periodic write-out: the first write-out check that asks for it finds it due.
    if (periodic_due) setup.m_clock += std::chrono::hours{2};
    // A limit of one byte, which a connected block whose signal has not left is over. A step that is not of
    // a reorganisation sets the limit anew, so it is the reorganisation's step that meets this one.
    util::ByteAccount& pinned{chainman.m_pinned_bytes};
    if (pinned_over_limit) pinned.SetLimit(1);
    BOOST_REQUIRE(chainstate.ActivateBestChain(state));
    // Both blocks were connected by that step: no other step ran and set the limit.
    if (pinned_over_limit) BOOST_CHECK_EQUAL(pinned.Limit(), 1U);
    m_node.validation_signals->SyncWithValidationInterfaceQueue();
    m_node.validation_signals->UnregisterSharedValidationInterface(recorder);
    BOOST_REQUIRE_EQUAL(WITH_LOCK(::cs_main, return chainstate.m_chain.Tip()->GetBlockHash()), fork2.GetHash());

    const std::vector<std::string> expected{"A " + CTransaction{tx}.GetHash().ToString(), "A " + CTransaction{tx2}.GetHash().ToString(), "C " + fork1.GetHash().ToString(), "C " + fork2.GetHash().ToString()};
    BOOST_CHECK_EQUAL_COLLECTIONS(recorder->events.begin(), recorder->events.end(), expected.begin(), expected.end());
}

BOOST_FIXTURE_TEST_CASE(reorg_announces_readded_transactions_before_connected_blocks, TestChain100Setup)
{
    CheckReorgAnnouncementOrder(*this);
}

//! A reorganisation from a branch of `depth` blocks to one of `depth + 1`: the blocks it disconnects are read
//! before its hold, up to `read_ahead`, and it ends at the longer branch with that branch's coins.
static void CheckReorgReadAhead(TestChain100Setup& setup, int depth, int read_ahead)
{
    ChainstateManager& chainman{*Assert(setup.m_node.chainman)};
    Chainstate& chainstate{chainman.ActiveChainstate()};
    const CScript long_script{CScript() << OP_TRUE};
    const CScript short_script{CScript() << OP_2};
    BlockValidationState state;

    // The longer branch first, then set aside: each of its blocks is read before the hold that disconnects it.
    std::vector<CBlock> long_branch;
    for (int i{0}; i < depth + 1; ++i) long_branch.push_back(setup.CreateAndProcessBlock({}, long_script));
    CBlockIndex* const long_first{WITH_LOCK(::cs_main, return chainman.m_blockman.LookupBlockIndex(long_branch.front().GetHash()))};
    BOOST_REQUIRE(chainstate.InvalidateBlock(state, long_first));
    BOOST_REQUIRE_EQUAL(WITH_LOCK(::cs_main, return chainstate.m_chain.Height()), 100);
    std::vector<CBlock> short_branch;
    for (int i{0}; i < depth; ++i) short_branch.push_back(setup.CreateAndProcessBlock({}, short_script));
    BOOST_REQUIRE_EQUAL(WITH_LOCK(::cs_main, return chainstate.m_chain.Tip()->GetBlockHash()), short_branch.back().GetHash());

    // The longer branch back. The step ends with the block that gives the tip more work than it had, the
    // branch's last, so all of the branch is its to connect.
    {
        LOCK(::cs_main);
        chainstate.ResetBlockFailureFlags(long_first);
        chainman.RecalculateBestHeader();
    }
    const auto read_before{[](const CBlock& block) { return strprintf("Disconnecting %s with its block and undo data read before the hold", block.GetHash().ToString()); }};
    bool read_below{false};
    {
        ASSERT_DEBUG_LOG(strprintf("Read %d blocks to disconnect ahead of a reorganisation from %s to %s",
                                   std::min(depth, read_ahead),
                                   short_branch.back().GetHash().ToString(), long_branch.back().GetHash().ToString()));
        // What was read is what disconnects the tip and every block down to the lowest one read...
        ASSERT_DEBUG_LOG(read_before(short_branch.back()));
        ASSERT_DEBUG_LOG(read_before(short_branch[depth - std::min(depth, read_ahead)]));
        // ...and not the block below them, which is read under the hold.
        std::optional<DebugLogHelper> below;
        if (depth > read_ahead) below.emplace(read_before(short_branch[depth - read_ahead - 1]), [&](const std::string* line) { if (line) read_below = true; return false; });
        BOOST_REQUIRE(chainstate.ActivateBestChain(state));
    }
    BOOST_CHECK(!read_below);
    setup.m_node.validation_signals->SyncWithValidationInterfaceQueue();

    LOCK(::cs_main);
    BOOST_REQUIRE_EQUAL(chainstate.m_chain.Tip()->GetBlockHash(), long_branch.back().GetHash());
    for (const CBlock& block : short_branch) BOOST_CHECK(!chainstate.CoinsTip().HaveCoin(COutPoint{block.vtx[0]->GetHash(), 0}));
    for (const CBlock& block : long_branch) BOOST_CHECK(chainstate.CoinsTip().HaveCoin(COutPoint{block.vtx[0]->GetHash(), 0}));
}

BOOST_FIXTURE_TEST_CASE(reorg_reads_its_blocks_before_its_hold, TestChain100Setup)
{
    CheckReorgReadAhead(*this, /*depth=*/2, /*read_ahead=*/16);
}

//! Deeper than what is read ahead: the disconnected blocks past the first 16 are read under the hold.
BOOST_FIXTURE_TEST_CASE(deep_reorg_reads_the_rest_under_its_hold, TestChain100Setup)
{
    CheckReorgReadAhead(*this, /*depth=*/18, /*read_ahead=*/16);
}

//! Undo data read from a position copied under cs_main, with no lock taken, is what the index's read gives;
//! the hash of another parent does not pass its checksum.
BOOST_FIXTURE_TEST_CASE(undo_read_from_a_copied_position, TestChain100Setup)
{
    ChainstateManager& chainman{*Assert(m_node.chainman)};
    node::BlockManager& blockman{chainman.m_blockman};
    const CMutableTransaction tx{CreateValidMempoolTransaction(m_coinbase_txns[0], 0, 1, coinbaseKey, GetScriptForRawPubKey(coinbaseKey.GetPubKey()), 49 * COIN)};
    CreateAndProcessBlock({tx}, CScript() << OP_TRUE);
    blockman.SettleUndoWrites();
    const CBlockIndex* const tip{WITH_LOCK(::cs_main, return chainman.ActiveChain().Tip())};
    const FlatFilePos pos{WITH_LOCK(::cs_main, return tip->GetUndoPos())};
    BOOST_REQUIRE(!pos.IsNull());

    CBlockUndo by_index, by_pos, wrong_parent;
    BOOST_REQUIRE(blockman.ReadBlockUndo(by_index, *tip));
    BOOST_REQUIRE(blockman.ReadBlockUndo(by_pos, pos, tip->pprev->GetBlockHash()));
    BOOST_REQUIRE_EQUAL(by_pos.vtxundo.size(), 1U);
    BOOST_CHECK((HashWriter{} << by_pos).GetHash() == (HashWriter{} << by_index).GetHash());
    BOOST_CHECK(!blockman.ReadBlockUndo(wrong_parent, pos, tip->GetBlockHash()));
}

//! The same with a write-out run started at every block, inside the reorganisation's step too: a run's settle
//! of the effects queue must not hand on the connected blocks' signals before the re-added transactions'.
struct WriteOutEveryBlockSetup : TestChain100Setup {
    WriteOutEveryBlockSetup() : TestChain100Setup{ChainType::REGTEST, {.extra_args = {"-test=write_out_every_block"}}} {}
};

//! A block whose write-out fails does not become the tip, and is not announced: the write is made before
//! the tip moves, as it always was.
BOOST_FIXTURE_TEST_CASE(failed_write_out_leaves_the_tip, WriteOutEveryBlockSetup)
{
    struct Recorder final : CValidationInterface {
        std::atomic<int> connected{0};
        std::atomic<int> tips{0};
        void BlockConnected(const kernel::ChainstateRole&, const std::shared_ptr<const CBlock>&, const CBlockIndex*) override { ++connected; }
        void UpdatedBlockTip(const CBlockIndex*, const CBlockIndex*, bool) override { ++tips; }
    };
    ChainstateManager& chainman{*Assert(m_node.chainman)};
    Chainstate& chainstate{chainman.ActiveChainstate()};
    BOOST_REQUIRE(chainman.m_options.test_write_out_every_block);
    const CBlockIndex* const old_tip{WITH_LOCK(::cs_main, return chainstate.m_chain.Tip())};

    // The block is stored, and not connected yet.
    const auto block{std::make_shared<const CBlock>(CreateBlock({}, CScript() << OP_TRUE))};
    BlockValidationState state;
    CBlockIndex* index{nullptr};
    bool new_block{false};
    BOOST_REQUIRE(WITH_LOCK(::cs_main, return chainman.AcceptBlock(block, state, &index, /*fRequested=*/true, /*dbp=*/nullptr, &new_block, /*min_pow_checked=*/true)));
    BOOST_REQUIRE(new_block && index);

    // Its block file cannot be opened from here on: the run its connection starts fails where it syncs the
    // file.
    const fs::path file{WITH_LOCK(::cs_main, return chainman.m_blockman.GetBlockPosFilename(index->GetBlockPos()))};
    BOOST_REQUIRE(fs::remove(file));
    BOOST_REQUIRE(fs::create_directory(file));

    const auto recorder{std::make_shared<Recorder>()};
    m_node.validation_signals->RegisterSharedValidationInterface(recorder);
    BOOST_CHECK(!chainstate.ActivateBestChain(state, block));
    m_node.validation_signals->SyncWithValidationInterfaceQueue();
    m_node.validation_signals->UnregisterSharedValidationInterface(recorder);

    BOOST_CHECK_EQUAL(m_node.exit_status.load(), EXIT_FAILURE);
    BOOST_CHECK(WITH_LOCK(::cs_main, return chainstate.m_chain.Tip()) == old_tip);
    BOOST_CHECK_EQUAL(recorder->connected.load(), 0);
    BOOST_CHECK_EQUAL(recorder->tips.load(), 0);
}

//! Drives a step of the connect window one block at a time, with nothing added to the node: the connect
//! pool's workers are held by tasks that wait (as threadpool_tests' BlockWorkers() does), ActivateBestChain()
//! runs on a thread of its own, and the test thread runs the tasks the step queues, one per call
//! (ThreadPool::ProcessTask()). Between two calls the connector waits for a block no worker will validate, so
//! what the test does there happens between two blocks of the window. Not more than that: WaitForStep()
//! returns once the tip is published and the queue has its count, and where the window had nothing to
//! refill the connector may still be at what follows a publication (a periodic write-out, the wait for the
//! signal queue, the plan of the next blocks).
//!
//! What this reaches: blocks that complete in the order they were staged, which is the order of the queue.
//! A block that completes ahead of a lower one is out of its reach. A block connected outside the window
//! stalls under it, its input fetch waits for tasks of the same pool: the workers are let go (Finish())
//! before a step comes to one.
//!
//! In a step of the window the queue holds the blocks' tasks only: a block read by its task is not freed
//! through the pool, and a write-out of a chain this small has one range, which its own thread writes.
class WindowStepper
{
    ChainstateManager& m_chainman;
    ThreadPool& m_pool;
    std::counting_semaphore<> m_release{0};
    std::vector<std::future<void>> m_held;
    bool m_released{false};
    std::thread m_connector;

    void ReleaseWorkers()
    {
        if (std::exchange(m_released, true)) return;
        m_release.release(static_cast<std::ptrdiff_t>(m_held.size()));
    }

public:
    explicit WindowStepper(ChainstateManager& chainman) : m_chainman{chainman}, m_pool{*chainman.m_connect_pool}
    {
        const size_t workers{m_pool.WorkersCount()};
        BOOST_REQUIRE_GT(workers, 0U);
        std::latch held{static_cast<std::ptrdiff_t>(workers)};
        for (size_t i{0}; i < workers; ++i) {
            m_held.push_back(*Assert(m_pool.Submit([&] {
                held.count_down();
                m_release.acquire();
            })));
        }
        held.wait();
    }
    //! Lets the workers go, which finish a step that a failed check left unfinished, and waits for it.
    ~WindowStepper()
    {
        ReleaseWorkers();
        for (auto& held : m_held) held.wait();
        if (m_connector.joinable()) m_connector.join();
    }

    //! Run `activate` (a call of ActivateBestChain()) beside the test.
    void Start(std::function<void()> activate) { m_connector = std::thread{std::move(activate)}; }
    //! Wait for it. The workers are let go first: they run what the step has queued and queues from here
    //! on, where a join alone would wait for ever.
    void Finish()
    {
        ReleaseWorkers();
        m_connector.join();
    }

    //! The tip's height, or nothing while another thread holds cs_main: the wait below is not to hang on a
    //! connector that waits under the lock for a block no worker runs.
    std::optional<int> TipHeight() const
    {
        TRY_LOCK(::cs_main, locked);
        if (!locked) return std::nullopt;
        return m_chainman.ActiveChain().Height();
    }

    //! Wait until the connector is at rest with `height` as the tip and `queued` blocks handed to the pool.
    void WaitForStep(int height, size_t queued)
    {
        const auto deadline{SteadyClock::now() + 60s};
        while (true) {
            const std::optional<int> tip{TipHeight()};
            const size_t in_queue{m_pool.WorkQueueSize()};
            if (tip == height && in_queue == queued) return;
            BOOST_REQUIRE_MESSAGE(SteadyClock::now() < deadline,
                                  strprintf("the step did not reach height %d with %u blocks queued: height %s, %u queued",
                                            height, queued, tip ? util::ToString(*tip) : "unknown (cs_main is held)", in_queue));
            UninterruptibleSleep(1ms);
        }
    }

    //! Run the task of the lowest block in flight, here.
    void RunLowestBlock() { BOOST_REQUIRE(m_pool.ProcessTask()); }
};

//! The signals of a window step, as a subscriber gets them.
struct WindowStepRecorder final : CValidationInterface {
    std::mutex mutex;
    std::vector<int> connected;
    std::vector<std::pair<int, bool>> tips;
    void BlockConnected(const kernel::ChainstateRole&, const std::shared_ptr<const CBlock>&, const CBlockIndex* index) override
    {
        const std::lock_guard lock{mutex};
        connected.push_back(index->nHeight);
    }
    void UpdatedBlockTip(const CBlockIndex* index, const CBlockIndex*, bool initial_download) override
    {
        const std::lock_guard lock{mutex};
        tips.emplace_back(index->nHeight, initial_download);
    }
};

//! A chain of BLOCKS blocks above the fixture's, stored and never connected (their undo data is still to be
//! written), with the node in initial block download and its tip too old to leave it: what the next
//! ActivateBestChain() connects in the window.
struct WindowStepSetup : TestChain100Setup {
    static constexpr int BLOCKS{6};
    int m_base_height{0};
    std::shared_ptr<WindowStepRecorder> m_recorder{std::make_shared<WindowStepRecorder>()};

    explicit WindowStepSetup(TestOpts opts = {}) : TestChain100Setup{ChainType::REGTEST, std::move(opts)}
    {
        ChainstateManager& chainman{*Assert(m_node.chainman)};
        Chainstate& chainstate{chainman.ActiveChainstate()};
        m_base_height = WITH_LOCK(::cs_main, return chainstate.m_chain.Height());
        // The blocks are made by a chain that is connected and then set aside for good. A copy of each with
        // another nonce, on top of the copy below it, is stored: a block that was never connected.
        std::vector<CBlock> blocks;
        for (int i{0}; i < BLOCKS; ++i) blocks.push_back(CreateAndProcessBlock({}, CScript() << OP_TRUE));
        CBlockIndex* const first{WITH_LOCK(::cs_main, return chainstate.m_chain[m_base_height + 1])};
        const int64_t tip_time{WITH_LOCK(::cs_main, return chainstate.m_chain.Tip()->GetBlockTime())};
        BlockValidationState state;
        BOOST_REQUIRE(chainstate.InvalidateBlock(state, first));
        m_node.validation_signals->SyncWithValidationInterfaceQueue();
        // The blocks' signals held them in the pinned account, and their disconnection found it empty.
        CheckAccountsEmpty();
        {
            LOCK(::cs_main);
            uint256 prev{first->pprev->GetBlockHash()};
            for (CBlock& block : blocks) {
                block.hashPrevBlock = prev;
                ++block.nNonce;
                while (!CheckProofOfWork(block.GetHash(), block.nBits, chainman.GetConsensus())) ++block.nNonce;
                BlockValidationState accept_state;
                CBlockIndex* index{nullptr};
                bool new_block{false};
                BOOST_REQUIRE(chainman.AcceptBlock(std::make_shared<const CBlock>(block), accept_state, &index, /*fRequested=*/true, /*dbp=*/nullptr, &new_block, /*min_pow_checked=*/true));
                BOOST_REQUIRE(new_block && index && index->GetUndoPos().IsNull());
                prev = block.GetHash();
            }
        }
        m_clock.set(std::chrono::seconds{tip_time} + 48h);
        static_cast<TestChainstateManager&>(chainman).ResetIbd();
        BOOST_REQUIRE_EQUAL(WITH_LOCK(::cs_main, return chainstate.m_chain.Height()), m_base_height);
        BOOST_REQUIRE_GT(chainman.ConnectWindow(), 1);
        m_node.validation_signals->RegisterSharedValidationInterface(m_recorder);
    }

    int Top() const { return m_base_height + BLOCKS; }

    //! Put the blocks from the base up to `top` (the best header if none is given) in the block cache, as
    //! blocks that were just received are. No task reads a file then, and the tasks are queued in the
    //! blocks' order, which WindowStepper relies on: a block on disk is read on a block I/O thread, which
    //! queues the block's task when its read is done, in no order with the others.
    void CacheStepBlocks(const CBlockIndex* top = nullptr)
    {
        ChainstateManager& chainman{*Assert(m_node.chainman)};
        BOOST_REQUIRE_GT(chainman.m_block_cache.Budget(), 0U);
        if (!top) top = WITH_LOCK(::cs_main, return chainman.m_best_header);
        for (const CBlockIndex* index{top}; index->nHeight > m_base_height; index = index->pprev) {
            const auto block{std::make_shared<CBlock>()};
            BOOST_REQUIRE(chainman.m_blockman.ReadBlock(*block, *index));
            chainman.m_block_cache.Insert(index->nHeight, block, m_base_height);
        }
    }

    //! Store a copy of the block at `original` on top of `parent`, with `extra` as one transaction more.
    std::pair<CBlock, CBlockIndex*> StoreSibling(const CBlockIndex& original, const uint256& parent, const CTransactionRef& extra = nullptr)
    {
        return StoreSibling(original, parent, extra ? std::vector<CTransactionRef>{extra} : std::vector<CTransactionRef>{});
    }

    //! Store a copy of the block at `original` on top of `parent`, with `extras` as transactions more, in order, or
    //! with `coinbase` in place of its own.
    std::pair<CBlock, CBlockIndex*> StoreSibling(const CBlockIndex& original, const uint256& parent, const std::vector<CTransactionRef>& extras, const CTransactionRef& coinbase = nullptr)
    {
        ChainstateManager& chainman{*Assert(m_node.chainman)};
        CBlock block;
        BOOST_REQUIRE(chainman.m_blockman.ReadBlock(block, original));
        block.hashPrevBlock = parent;
        if (coinbase) {
            // The commitments of transactions more would change the coinbase.
            BOOST_REQUIRE(extras.empty());
            block.vtx[0] = coinbase;
            block.hashMerkleRoot = BlockMerkleRoot(block);
        }
        if (!extras.empty()) {
            block.vtx.insert(block.vtx.end(), extras.begin(), extras.end());
            node::RegenerateCommitments(block, chainman);
        }
        while (!CheckProofOfWork(block.GetHash(), block.nBits, chainman.GetConsensus())) ++block.nNonce;
        LOCK(::cs_main);
        BlockValidationState state;
        CBlockIndex* index{nullptr};
        bool new_block{false};
        BOOST_REQUIRE(chainman.AcceptBlock(std::make_shared<const CBlock>(block), state, &index, /*fRequested=*/true, /*dbp=*/nullptr, &new_block, /*min_pow_checked=*/true));
        BOOST_REQUIRE(new_block && index);
        return {std::move(block), index};
    }

    //! The blocks in flight once the tip is at `height`.
    size_t InFlightAt(int height) const
    {
        return static_cast<size_t>(std::min(Top() - height, Assert(m_node.chainman)->ConnectWindow()));
    }

    //! Every byte charged for a block in flight has been released.
    void CheckAccountsEmpty() const
    {
        const ChainstateManager& chainman{*Assert(m_node.chainman)};
        BOOST_CHECK_EQUAL(chainman.m_block_cache.Bytes(), 0U);
        BOOST_CHECK_EQUAL(chainman.m_window_bytes.Bytes(), 0U);
        BOOST_CHECK_EQUAL(chainman.m_blockman.m_effects.PendingBytes(), 0U);
        BOOST_CHECK_EQUAL(chainman.m_pinned_bytes.Bytes(), 0U);
    }

    //! The step is over: its signals are delivered, every block's BlockConnected and every block's
    //! UpdatedBlockTip in order, and the accounts of the bytes in flight are back at zero.
    void CheckStepOver()
    {
        m_node.validation_signals->SyncWithValidationInterfaceQueue();
        m_node.validation_signals->UnregisterSharedValidationInterface(m_recorder);
        CheckAccountsEmpty();
        std::vector<int> expected;
        for (int height{m_base_height + 1}; height <= Top(); ++height) expected.push_back(height);
        const std::lock_guard lock{m_recorder->mutex};
        BOOST_CHECK_EQUAL_COLLECTIONS(m_recorder->connected.begin(), m_recorder->connected.end(), expected.begin(), expected.end());
        std::vector<int> tips;
        for (const auto& [height, initial_download] : m_recorder->tips) tips.push_back(height);
        BOOST_CHECK_EQUAL_COLLECTIONS(tips.begin(), tips.end(), expected.begin(), expected.end());
    }
};

//! What the window counted, read where the node shows it: the line a ChainstateManager logs when it is
//! destroyed, which is the counts' only way out. The fixture destroys the manager, so the line is logged
//! after the test's body: this is a base that comes before the fixture, destroyed after it, and its
//! destructor checks the line. The callback is not taken back there, the fixture has dropped the logger's
//! callbacks by then (BCLog::Logger::DisconnectTestLogger()).
struct WindowSummaryLog {
    //! The blocks TestChain100Setup connects in the window before a test's own: the genesis block and the
    //! first block above it, the two that are connected while the node is in its initial download.
    static constexpr unsigned FIXTURE_BLOCKS{2};
    std::string m_expected_blocks;
    std::string m_expected_counts;
    std::string m_logged;

    WindowSummaryLog()
    {
        LogInstance().PushBackCallback([this](const std::string& line) {
            if (line.find("Connect window: ") != std::string::npos) m_logged = line;
        });
    }
    ~WindowSummaryLog()
    {
        BOOST_CHECK_MESSAGE(!m_expected_blocks.empty(), "the test names no summary to expect");
        BOOST_CHECK_MESSAGE(m_logged.find(m_expected_blocks) != std::string::npos && m_logged.find(m_expected_counts) != std::string::npos,
                            strprintf("expected '%s ... %s', the log has '%s'", m_expected_blocks, m_expected_counts, m_logged));
    }

    //! The counts the manager is to log: the test's blocks that came to their publication in the window,
    //! the commits that met a conflict in the store, and the blocks it gave up and connected alone.
    void ExpectWindowSummary(unsigned blocks, unsigned conflicts, unsigned retries)
    {
        m_expected_blocks = strprintf("Connect window: %u blocks validated on the connect pool (", FIXTURE_BLOCKS + blocks);
        m_expected_counts = strprintf(" wide), %u conflicts, %u blocks retried alone", conflicts, retries);
    }
};

struct WindowSummarySetup : WindowSummaryLog, WindowStepSetup {
};

//! The window's blocks are published one by one as their tasks are run, the window is refilled behind each,
//! and the accounts of the bytes in flight are back at zero when the step is over. A valid chain with no
//! write-out under it meets no conflict, and no block of it is connected a second time.
BOOST_FIXTURE_TEST_CASE(window_step_block_by_block, WindowSummarySetup)
{
    ExpectWindowSummary(/*blocks=*/BLOCKS, /*conflicts=*/0, /*retries=*/0);
    ChainstateManager& chainman{*Assert(m_node.chainman)};
    Chainstate& chainstate{chainman.ActiveChainstate()};
    CacheStepBlocks();
    BlockValidationState state;
    bool activated{false};
    {
        WindowStepper stepper{chainman};
        stepper.Start([&] { activated = chainstate.ActivateBestChain(state); });
        stepper.WaitForStep(m_base_height, InFlightAt(m_base_height));
        BOOST_CHECK_GT(chainman.m_window_bytes.Bytes(), 0U);
        for (int height{m_base_height + 1}; height <= Top(); ++height) {
            stepper.RunLowestBlock();
            stepper.WaitForStep(height, InFlightAt(height));
            BOOST_CHECK(chainman.IsInitialBlockDownload());
        }
        stepper.Finish();
    }
    BOOST_CHECK(activated);
    BOOST_CHECK(state.IsValid());
    CheckStepOver();
    const std::lock_guard lock{m_recorder->mutex};
    for (const auto& [height, initial_download] : m_recorder->tips) BOOST_CHECK(initial_download);
}

//! The write-out that only a step of the window makes: the periodic one, started in the hold that published
//! a block and waited for once cs_main is released. (The write-out before the tip moves is the tip's, which
//! failed_write_out_leaves_the_tip covers.) When it fails the step ends where the run was waited for: the
//! block that started it is the tip and was announced (the periodic write follows the tip's notification),
//! no block above it is published, and the pass fails.
BOOST_FIXTURE_TEST_CASE(failed_periodic_write_out_ends_a_window_step, WindowStepSetup)
{
    ChainstateManager& chainman{*Assert(m_node.chainman)};
    Chainstate& chainstate{chainman.ActiveChainstate()};

    CacheStepBlocks();
    // A write sets the timer of the periodic one, and the clock goes past its interval: the periodic write
    // is due at the step's first block.
    {
        BlockValidationState flush_state;
        BOOST_REQUIRE(chainstate.FlushStateToDisk(flush_state, FlushStateMode::FORCE_FLUSH));
        m_clock += std::chrono::hours{2};
    }
    // A block beside the window's first is stored, which makes its block file owe a sync, and the file
    // cannot be opened from here on: the next run fails where it syncs it.
    {
        const auto beside{std::make_shared<const CBlock>(CreateBlock({}, CScript() << OP_2))};
        LOCK(::cs_main);
        BlockValidationState accept_state;
        CBlockIndex* index{nullptr};
        bool new_block{false};
        BOOST_REQUIRE(chainman.AcceptBlock(beside, accept_state, &index, /*fRequested=*/true, /*dbp=*/nullptr, &new_block, /*min_pow_checked=*/true));
        BOOST_REQUIRE(new_block && index);
        const fs::path file{chainman.m_blockman.GetBlockPosFilename(index->GetBlockPos())};
        BOOST_REQUIRE(fs::remove(file));
        BOOST_REQUIRE(fs::create_directory(file));
    }

    BlockValidationState state;
    bool activated{true};
    {
        WindowStepper stepper{chainman};
        stepper.Start([&] { activated = chainstate.ActivateBestChain(state, nullptr); });
        stepper.WaitForStep(m_base_height, InFlightAt(m_base_height));
        // The first block is published: the write-out before the tip has nothing due, the periodic write
        // is, and the run its hold starts fails.
        stepper.RunLowestBlock();
        // The blocks above it are run by the workers, and let go.
        stepper.Finish();
    }
    m_node.validation_signals->SyncWithValidationInterfaceQueue();
    m_node.validation_signals->UnregisterSharedValidationInterface(m_recorder);

    BOOST_CHECK(!activated);
    BOOST_CHECK_EQUAL(m_node.exit_status.load(), EXIT_FAILURE);
    BOOST_CHECK_EQUAL(WITH_LOCK(::cs_main, return chainstate.m_chain.Height()), m_base_height + 1);
    // The blocks above gave back what they held in memory; the block cache alone keeps the blocks that were
    // not connected.
    BOOST_CHECK_EQUAL(chainman.m_window_bytes.Bytes(), 0U);
    BOOST_CHECK_EQUAL(chainman.m_blockman.m_effects.PendingBytes(), 0U);
    BOOST_CHECK_EQUAL(chainman.m_pinned_bytes.Bytes(), 0U);
    const std::vector<int> expected{m_base_height + 1};
    const std::lock_guard lock{m_recorder->mutex};
    BOOST_CHECK_EQUAL_COLLECTIONS(m_recorder->connected.begin(), m_recorder->connected.end(), expected.begin(), expected.end());
    std::vector<int> tips;
    for (const auto& [height, initial_download] : m_recorder->tips) tips.push_back(height);
    BOOST_CHECK_EQUAL_COLLECTIONS(tips.begin(), tips.end(), expected.begin(), expected.end());
}

//! A window block whose file cannot be read. Its task ends without a verdict, and the step ends where the
//! block would be published: the window is drained, the blocks give their views back, the
//! node gets a fatal error and the pass fails with the tip where it was. Nothing is published or announced.
BOOST_FIXTURE_TEST_CASE(unreadable_block_ends_a_window_step, WindowStepSetup)
{
    ChainstateManager& chainman{*Assert(m_node.chainman)};
    Chainstate& chainstate{chainman.ActiveChainstate()};

    // The step's blocks are read from their file by their tasks (the fixture puts none in the block cache),
    // and the file is not there.
    const CBlockIndex* const first{WITH_LOCK(::cs_main, return chainman.m_best_header->GetAncestor(m_base_height + 1))};
    const fs::path file{WITH_LOCK(::cs_main, return chainman.m_blockman.GetBlockPosFilename(first->GetBlockPos()))};
    const fs::path aside{file + ".aside"};
    fs::rename(file, aside);

    BlockValidationState state;
    bool activated{true};
    {
        WindowStepper stepper{chainman};
        stepper.Start([&] { activated = chainstate.ActivateBestChain(state); });
        stepper.WaitForStep(m_base_height, InFlightAt(m_base_height));
        // The first block's task finds no file. The step waits for the blocks above it before it ends,
        // which the workers run.
        stepper.RunLowestBlock();
        stepper.Finish();
    }
    fs::rename(aside, file);
    m_node.validation_signals->SyncWithValidationInterfaceQueue();
    m_node.validation_signals->UnregisterSharedValidationInterface(m_recorder);

    BOOST_CHECK(!activated);
    BOOST_CHECK(state.IsError());
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "Failed to read block.");
    BOOST_CHECK_EQUAL(m_node.exit_status.load(), EXIT_FAILURE);
    BOOST_CHECK_EQUAL(WITH_LOCK(::cs_main, return chainstate.m_chain.Height()), m_base_height);
    CheckAccountsEmpty();
    const std::lock_guard lock{m_recorder->mutex};
    BOOST_CHECK(m_recorder->connected.empty());
    BOOST_CHECK(m_recorder->tips.empty());
}

//! A block of the window with a transaction whose coin no block creates. Its validation in the window finds
//! no coin, which decides nothing there: the step ends, and the block is connected alone, where the same
//! transaction makes it invalid. The block below it stays the tip.
BOOST_FIXTURE_TEST_CASE(invalid_window_block_is_judged_alone, WindowSummarySetup)
{
    // Two blocks came to their publication in the window, the first and the sibling, which met no conflict
    // in the store and is the one block connected again.
    ExpectWindowSummary(/*blocks=*/2, /*conflicts=*/0, /*retries=*/1);
    ChainstateManager& chainman{*Assert(m_node.chainman)};
    Chainstate& chainstate{chainman.ActiveChainstate()};
    const CBlockIndex* const first{WITH_LOCK(::cs_main, return chainman.m_best_header->GetAncestor(m_base_height + 1))};
    CBlockIndex* const second{WITH_LOCK(::cs_main, return chainman.m_best_header->GetAncestor(m_base_height + 2))};
    const CBlockIndex* const third{WITH_LOCK(::cs_main, return chainman.m_best_header->GetAncestor(m_base_height + 3))};
    // A sibling of the setup's second block with one transaction more, which spends no coin, and the setup's
    // third on top of it: a block of the window is in flight above the one that fails.
    CMutableTransaction spend;
    spend.vin.emplace_back(COutPoint{Txid::FromUint256(m_rng.rand256()), 0});
    spend.vout.emplace_back(CAmount{1}, CScript() << OP_TRUE);
    const auto [bad, bad_index]{StoreSibling(*second, first->GetBlockHash(), MakeTransactionRef(std::move(spend)))};
    const auto [above, top]{StoreSibling(*third, bad.GetHash())};
    CacheStepBlocks(top);
    // The setup's blocks from the second on are set aside, so that the sibling is what the step heads for.
    BlockValidationState invalidate_state;
    BOOST_REQUIRE(chainstate.InvalidateBlock(invalidate_state, second));
    BOOST_REQUIRE_EQUAL(WITH_LOCK(::cs_main, return chainstate.m_chain.Height()), m_base_height);

    BlockValidationState state;
    bool activated{false};
    {
        // Only a block that is connected alone logs this line (a window block is read by its task): the
        // block was connected a second time, outside the window.
        ASSERT_DEBUG_LOG("  - Load block from disk");
        WindowStepper stepper{chainman};
        stepper.Start([&] { activated = chainstate.ActivateBestChain(state); });
        stepper.WaitForStep(m_base_height, std::min<size_t>(3, chainman.ConnectWindow()));
        stepper.RunLowestBlock();
        stepper.WaitForStep(m_base_height + 1, 2);
        // The sibling's task finds the transaction's coin missing, here, with the block above it still
        // queued. The workers are let go for that block, which the step waits for, and for the sibling
        // connected alone next, outside the window.
        stepper.RunLowestBlock();
        stepper.Finish();
    }
    BOOST_CHECK(activated);
    BOOST_CHECK(state.IsValid());
    // The verdict is that of the step that connected the block alone: the window gives none.
    BOOST_CHECK_EQUAL(WITH_LOCK(::cs_main, return chainstate.m_chain.Height()), m_base_height + 1);
    BOOST_CHECK(WITH_LOCK(::cs_main, return (bad_index->nStatus & BLOCK_FAILED_VALID) != 0));
    BOOST_CHECK(!WITH_LOCK(::cs_main, return chainstate.CoinsTip().HaveCoin(COutPoint{bad.vtx.back()->GetHash(), 0})));
    BOOST_CHECK(!WITH_LOCK(::cs_main, return chainstate.CoinsTip().HaveCoin(COutPoint{above.vtx[0]->GetHash(), 0})));
    m_node.validation_signals->SyncWithValidationInterfaceQueue();
    m_node.validation_signals->UnregisterSharedValidationInterface(m_recorder);
    CheckAccountsEmpty();
    const std::vector<int> expected{m_base_height + 1};
    const std::lock_guard lock{m_recorder->mutex};
    BOOST_CHECK_EQUAL_COLLECTIONS(m_recorder->connected.begin(), m_recorder->connected.end(), expected.begin(), expected.end());
}

//! Two blocks of the window spend one coin. Each is validated against the published blocks, where the coin
//! is, so both are valid and come to one commit. There the upper block's spend finds the coin gone: it and
//! the block above it are taken out of the store again, the lower block is published, and the upper one is
//! connected alone, where the spend makes it invalid.
BOOST_FIXTURE_TEST_CASE(double_spend_in_the_window_is_taken_back, WindowSummarySetup)
{
    // The first block and the lower spender came to their publication in the window; the upper spender is
    // the one conflict and the one block connected again.
    ExpectWindowSummary(/*blocks=*/2, /*conflicts=*/1, /*retries=*/1);
    ChainstateManager& chainman{*Assert(m_node.chainman)};
    Chainstate& chainstate{chainman.ActiveChainstate()};
    BOOST_REQUIRE_GE(chainman.ConnectWindow(), 4);
    const CBlockIndex* const first{WITH_LOCK(::cs_main, return chainman.m_best_header->GetAncestor(m_base_height + 1))};
    CBlockIndex* const second{WITH_LOCK(::cs_main, return chainman.m_best_header->GetAncestor(m_base_height + 2))};
    const CBlockIndex* const third{WITH_LOCK(::cs_main, return chainman.m_best_header->GetAncestor(m_base_height + 3))};
    const CBlockIndex* const fourth{WITH_LOCK(::cs_main, return chainman.m_best_header->GetAncestor(m_base_height + 4))};
    // Two spends of the fixture's first coinbase output, which no block of the setup spends.
    const CTransactionRef funding{m_coinbase_txns[0]};
    const COutPoint coin{funding->GetHash(), 0};
    const auto spend_to{[&](CAmount amount) {
        return MakeTransactionRef(CreateValidMempoolTransaction(funding, /*input_vout=*/0, /*input_height=*/1, coinbaseKey, CScript() << OP_TRUE, amount, /*submit=*/false));
    }};
    const CTransactionRef lower_spend{spend_to(1 * COIN)};
    const CTransactionRef upper_spend{spend_to(2 * COIN)};
    BOOST_REQUIRE(lower_spend->GetHash() != upper_spend->GetHash());
    BOOST_REQUIRE(WITH_LOCK(::cs_main, return chainstate.CoinsTip().HaveCoin(coin)));
    const auto [lower, lower_index]{StoreSibling(*second, first->GetBlockHash(), lower_spend)};
    const auto [upper, upper_index]{StoreSibling(*third, lower.GetHash(), upper_spend)};
    const auto [above, top]{StoreSibling(*fourth, upper.GetHash())};
    CacheStepBlocks(top);
    BlockValidationState invalidate_state;
    BOOST_REQUIRE(chainstate.InvalidateBlock(invalidate_state, second));
    BOOST_REQUIRE_EQUAL(WITH_LOCK(::cs_main, return chainstate.m_chain.Height()), m_base_height);
    // A write sets the timer of the periodic one: none is due in the step, and the commit takes every
    // complete block.
    {
        BlockValidationState flush_state;
        BOOST_REQUIRE(chainstate.FlushStateToDisk(flush_state, FlushStateMode::FORCE_FLUSH));
    }

    BlockValidationState state;
    bool activated{false};
    {
        ASSERT_DEBUG_LOG(strprintf("A conflict in a commit, first in the block at height %d: 2 blocks taken out of the store again", m_base_height + 3));
        ASSERT_DEBUG_LOG("bad-txns-inputs-missingorspent");
        WindowStepper stepper{chainman};
        stepper.Start([&] { activated = chainstate.ActivateBestChain(state); });
        stepper.WaitForStep(m_base_height, 4);
        stepper.RunLowestBlock();
        stepper.WaitForStep(m_base_height + 1, 3);
        {
            // The connector commits and publishes under cs_main. Held here, the three blocks are complete
            // before any of them is committed, and each was validated with the coin in the store.
            LOCK(::cs_main);
            for (int i{0}; i < 3; ++i) stepper.RunLowestBlock();
        }
        stepper.Finish();
    }
    BOOST_CHECK(activated);
    BOOST_CHECK(state.IsValid());
    BOOST_CHECK_EQUAL(m_node.exit_status.load(), EXIT_SUCCESS);
    {
        LOCK(::cs_main);
        BOOST_CHECK(chainstate.m_chain.Tip() == lower_index);
        BOOST_CHECK((upper_index->nStatus & BLOCK_FAILED_VALID) != 0);
        const CCoinsViewCache& coins{chainstate.CoinsTip()};
        // The lower block's changes are there, and nothing of the two blocks above it.
        BOOST_CHECK(!coins.HaveCoin(coin));
        BOOST_CHECK(coins.HaveCoin(COutPoint{lower_spend->GetHash(), 0}));
        BOOST_CHECK(coins.HaveCoin(COutPoint{lower.vtx[0]->GetHash(), 0}));
        BOOST_CHECK(!coins.HaveCoin(COutPoint{upper_spend->GetHash(), 0}));
        BOOST_CHECK(!coins.HaveCoin(COutPoint{upper.vtx[0]->GetHash(), 0}));
        BOOST_CHECK(!coins.HaveCoin(COutPoint{above.vtx[0]->GetHash(), 0}));
    }
    m_node.validation_signals->SyncWithValidationInterfaceQueue();
    m_node.validation_signals->UnregisterSharedValidationInterface(m_recorder);
    CheckAccountsEmpty();
    const std::vector<int> expected{m_base_height + 1, m_base_height + 2};
    const std::lock_guard lock{m_recorder->mutex};
    BOOST_CHECK_EQUAL_COLLECTIONS(m_recorder->connected.begin(), m_recorder->connected.end(), expected.begin(), expected.end());
}

//! As the double spend above, with the upper block also spending an output the lower block made. The three blocks share
//! one commit, which drops that create and that spend together; the conflict then takes the upper block back, and
//! the take-back makes the lower block's output again, as the lower block stays (TakeBackCommit()).
BOOST_FIXTURE_TEST_CASE(take_back_makes_again_a_netted_output_of_a_block_that_stays, WindowSummarySetup)
{
    ExpectWindowSummary(/*blocks=*/2, /*conflicts=*/1, /*retries=*/1);
    ChainstateManager& chainman{*Assert(m_node.chainman)};
    Chainstate& chainstate{chainman.ActiveChainstate()};
    BOOST_REQUIRE_GE(chainman.ConnectWindow(), 4);
    const CBlockIndex* const first{WITH_LOCK(::cs_main, return chainman.m_best_header->GetAncestor(m_base_height + 1))};
    CBlockIndex* const second{WITH_LOCK(::cs_main, return chainman.m_best_header->GetAncestor(m_base_height + 2))};
    const CBlockIndex* const third{WITH_LOCK(::cs_main, return chainman.m_best_header->GetAncestor(m_base_height + 3))};
    const CBlockIndex* const fourth{WITH_LOCK(::cs_main, return chainman.m_best_header->GetAncestor(m_base_height + 4))};
    const CTransactionRef funding{m_coinbase_txns[0]};
    const COutPoint coin{funding->GetHash(), 0};
    const auto spend_to{[&](CAmount amount) {
        return MakeTransactionRef(CreateValidMempoolTransaction(funding, /*input_vout=*/0, /*input_height=*/1, coinbaseKey, CScript() << OP_TRUE, amount, /*submit=*/false));
    }};
    const CTransactionRef lower_spend{spend_to(1 * COIN)};
    const CTransactionRef upper_spend{spend_to(2 * COIN)};
    // Spends the lower block's output, which pays to OP_TRUE: an empty scriptSig satisfies it.
    CMutableTransaction spend_lower_output;
    spend_lower_output.vin.emplace_back(COutPoint{lower_spend->GetHash(), 0});
    spend_lower_output.vout.emplace_back(lower_spend->vout[0].nValue / 2, CScript() << OP_TRUE);
    const CTransactionRef lower_output_spent{MakeTransactionRef(std::move(spend_lower_output))};
    const COutPoint netted{lower_spend->GetHash(), 0};
    const auto [lower, lower_index]{StoreSibling(*second, first->GetBlockHash(), lower_spend)};
    const auto [upper, upper_index]{StoreSibling(*third, lower.GetHash(), std::vector<CTransactionRef>{upper_spend, lower_output_spent})};
    const auto [above, top]{StoreSibling(*fourth, upper.GetHash())};
    CacheStepBlocks(top);
    BlockValidationState invalidate_state;
    BOOST_REQUIRE(chainstate.InvalidateBlock(invalidate_state, second));
    BOOST_REQUIRE_EQUAL(WITH_LOCK(::cs_main, return chainstate.m_chain.Height()), m_base_height);
    {
        BlockValidationState flush_state;
        BOOST_REQUIRE(chainstate.FlushStateToDisk(flush_state, FlushStateMode::FORCE_FLUSH));
    }

    BlockValidationState state;
    bool activated{false};
    {
        ASSERT_DEBUG_LOG(strprintf("A conflict in a commit, first in the block at height %d: 2 blocks taken out of the store again", m_base_height + 3));
        WindowStepper stepper{chainman};
        stepper.Start([&] { activated = chainstate.ActivateBestChain(state); });
        stepper.WaitForStep(m_base_height, 4);
        stepper.RunLowestBlock();
        stepper.WaitForStep(m_base_height + 1, 3);
        {
            // As above: held here, the three blocks are complete before any of them is committed, so they share
            // one commit.
            LOCK(::cs_main);
            for (int i{0}; i < 3; ++i) stepper.RunLowestBlock();
        }
        stepper.Finish();
    }
    BOOST_CHECK(activated);
    BOOST_CHECK(state.IsValid());
    BOOST_CHECK_EQUAL(m_node.exit_status.load(), EXIT_SUCCESS);
    {
        LOCK(::cs_main);
        BOOST_CHECK(chainstate.m_chain.Tip() == lower_index);
        BOOST_CHECK((upper_index->nStatus & BLOCK_FAILED_VALID) != 0);
        const CCoinsViewCache& coins{chainstate.CoinsTip()};
        BOOST_CHECK(!coins.HaveCoin(coin));
        // The lower block's output is there again, as the lower block made it.
        BOOST_REQUIRE(coins.HaveCoin(netted));
        const Coin& again{coins.AccessCoin(netted)};
        BOOST_CHECK_EQUAL(again.nHeight, static_cast<uint32_t>(lower_index->nHeight));
        BOOST_CHECK_EQUAL(again.out.nValue, lower_spend->vout[0].nValue);
        BOOST_CHECK(again.out.scriptPubKey == lower_spend->vout[0].scriptPubKey);
        BOOST_CHECK(!coins.HaveCoin(COutPoint{lower_output_spent->GetHash(), 0}));
        BOOST_CHECK(!coins.HaveCoin(COutPoint{upper_spend->GetHash(), 0}));
    }
    m_node.validation_signals->SyncWithValidationInterfaceQueue();
    m_node.validation_signals->UnregisterSharedValidationInterface(m_recorder);
    CheckAccountsEmpty();
    const std::vector<int> expected{m_base_height + 1, m_base_height + 2};
    const std::lock_guard lock{m_recorder->mutex};
    BOOST_CHECK_EQUAL_COLLECTIONS(m_recorder->connected.begin(), m_recorder->connected.end(), expected.begin(), expected.end());
}

//! As WindowSummarySetup, on a chain where a block may repeat a lower block's coinbase, as on mainnet before BIP30
//! and BIP34: no block is checked for BIP30 (-test=bip30_off), and BIP34, which puts a block's height in its
//! coinbase, is not active.
struct RepeatedCoinbaseSetup : WindowSummaryLog, WindowStepSetup {
    RepeatedCoinbaseSetup() : WindowStepSetup{{.extra_args = {"-test=bip30_off", "-testactivationheight=bip34@1000"}}} {}
};

//! As the double spend above, with the block above the upper one repeating the coinbase of a block of the fixture,
//! whose output the coins database holds and the store's memory does not. In the commit the repeat writes over a
//! coin the store has no entry for; the take-back puts the database's coin back in its place (TakeBackCommit()).
BOOST_FIXTURE_TEST_CASE(take_back_puts_back_a_coin_written_over_from_the_database, RepeatedCoinbaseSetup)
{
    ExpectWindowSummary(/*blocks=*/2, /*conflicts=*/1, /*retries=*/1);
    ChainstateManager& chainman{*Assert(m_node.chainman)};
    Chainstate& chainstate{chainman.ActiveChainstate()};
    BOOST_REQUIRE_GE(chainman.ConnectWindow(), 4);
    const CBlockIndex* const first{WITH_LOCK(::cs_main, return chainman.m_best_header->GetAncestor(m_base_height + 1))};
    CBlockIndex* const second{WITH_LOCK(::cs_main, return chainman.m_best_header->GetAncestor(m_base_height + 2))};
    const CBlockIndex* const third{WITH_LOCK(::cs_main, return chainman.m_best_header->GetAncestor(m_base_height + 3))};
    const CBlockIndex* const fourth{WITH_LOCK(::cs_main, return chainman.m_best_header->GetAncestor(m_base_height + 4))};
    const CTransactionRef funding{m_coinbase_txns[0]};
    const COutPoint coin{funding->GetHash(), 0};
    const auto spend_to{[&](CAmount amount) {
        return MakeTransactionRef(CreateValidMempoolTransaction(funding, /*input_vout=*/0, /*input_height=*/1, coinbaseKey, CScript() << OP_TRUE, amount, /*submit=*/false));
    }};
    const CTransactionRef lower_spend{spend_to(1 * COIN)};
    const CTransactionRef upper_spend{spend_to(2 * COIN)};
    // The coinbase of the fixture's second block, whose output no block spends. The setup's fourth block holds its
    // coinbase alone, as that block did, so the repeat's witness commitment holds there.
    const CTransactionRef repeated{m_coinbase_txns[1]};
    const COutPoint written_over{repeated->GetHash(), 0};
    const auto [lower, lower_index]{StoreSibling(*second, first->GetBlockHash(), lower_spend)};
    const auto [upper, upper_index]{StoreSibling(*third, lower.GetHash(), upper_spend)};
    const auto [above, top]{StoreSibling(*fourth, upper.GetHash(), {}, repeated)};
    CacheStepBlocks(top);
    BlockValidationState invalidate_state;
    BOOST_REQUIRE(chainstate.InvalidateBlock(invalidate_state, second));
    BOOST_REQUIRE_EQUAL(WITH_LOCK(::cs_main, return chainstate.m_chain.Height()), m_base_height);
    {
        BlockValidationState flush_state;
        BOOST_REQUIRE(chainstate.FlushStateToDisk(flush_state, FlushStateMode::FORCE_FLUSH));
    }
    // The forced write emptied the store's memory: the coin to be written over is in the database only.
    const Coin old_coin{*Assert(WITH_LOCK(::cs_main, return chainstate.CoinsDB().GetCoin(written_over)))};
    BOOST_REQUIRE(!WITH_LOCK(::cs_main, return chainstate.CoinsStoreTip().HaveCoinInCache(written_over)));
    BOOST_REQUIRE_NE(old_coin.nHeight, static_cast<uint32_t>(top->nHeight));

    BlockValidationState state;
    bool activated{false};
    {
        ASSERT_DEBUG_LOG(strprintf("A conflict in a commit, first in the block at height %d: 2 blocks taken out of the store again", m_base_height + 3));
        ASSERT_DEBUG_LOG("bad-txns-inputs-missingorspent");
        WindowStepper stepper{chainman};
        stepper.Start([&] { activated = chainstate.ActivateBestChain(state); });
        stepper.WaitForStep(m_base_height, 4);
        stepper.RunLowestBlock();
        stepper.WaitForStep(m_base_height + 1, 3);
        {
            // As above: held here, the three blocks are complete before any of them is committed, so they share
            // one commit, the valid repeat included.
            LOCK(::cs_main);
            for (int i{0}; i < 3; ++i) stepper.RunLowestBlock();
        }
        stepper.Finish();
    }
    BOOST_CHECK(activated);
    BOOST_CHECK(state.IsValid());
    BOOST_CHECK_EQUAL(m_node.exit_status.load(), EXIT_SUCCESS);
    {
        LOCK(::cs_main);
        BOOST_CHECK(chainstate.m_chain.Tip() == lower_index);
        BOOST_CHECK((upper_index->nStatus & BLOCK_FAILED_VALID) != 0);
        const CCoinsViewCache& coins{chainstate.CoinsTip()};
        BOOST_CHECK(!coins.HaveCoin(coin));
        BOOST_CHECK(coins.HaveCoin(COutPoint{lower_spend->GetHash(), 0}));
        BOOST_CHECK(!coins.HaveCoin(COutPoint{upper_spend->GetHash(), 0}));
        // The coin written over is there again as the database holds it, not as the repeat made it.
        BOOST_REQUIRE(coins.HaveCoin(written_over));
        const Coin& again{coins.AccessCoin(written_over)};
        BOOST_CHECK_EQUAL(again.nHeight, old_coin.nHeight);
        BOOST_CHECK(again.IsCoinBase());
        BOOST_CHECK_EQUAL(again.out.nValue, old_coin.out.nValue);
        BOOST_CHECK(again.out.scriptPubKey == old_coin.out.scriptPubKey);
    }
    m_node.validation_signals->SyncWithValidationInterfaceQueue();
    m_node.validation_signals->UnregisterSharedValidationInterface(m_recorder);
    CheckAccountsEmpty();
    const std::vector<int> expected{m_base_height + 1, m_base_height + 2};
    const std::lock_guard lock{m_recorder->mutex};
    BOOST_CHECK_EQUAL_COLLECTIONS(m_recorder->connected.begin(), m_recorder->connected.end(), expected.begin(), expected.end());
}

//! The steps of ProcessNewBlock() with something happening between them (ChainstateManager::AcceptNewBlock(),
//! WriteNewBlock(), FinishNewBlock()). A second copy accepted while the first is being stored is neither stored
//! again nor a new block. A block whose parent is invalidated while it is being stored is refused as a known
//! invalid block (duplicate-invalid, which spares the peer), is not a new block and is not recorded as stored.
BOOST_FIXTURE_TEST_CASE(blocks_met_while_being_stored, TestChain100Setup)
{
    ChainstateManager& chainman{*Assert(m_node.chainman)};
    Chainstate& chainstate{chainman.ActiveChainstate()};
    using InProcess = ChainstateManager::NewBlockInProcess;

    const auto block{std::make_shared<const CBlock>(CreateBlock({}, CScript() << OP_TRUE))};
    InProcess first;
    BOOST_REQUIRE(chainman.AcceptNewBlock(first, block, /*force_processing=*/true, /*min_pow_checked=*/true, /*activate_async=*/false));
    BOOST_REQUIRE(first.prepared.store);
    BOOST_CHECK(first.new_block);
    InProcess second;
    BOOST_CHECK(chainman.AcceptNewBlock(second, block, /*force_processing=*/true, /*min_pow_checked=*/true, /*activate_async=*/false));
    BOOST_CHECK(!second.prepared.store);
    BOOST_CHECK(!second.new_block);
    chainman.WriteNewBlock(first);
    BOOST_CHECK(chainman.FinishNewBlock(first));
    BOOST_CHECK(first.new_block);
    chainman.WriteNewBlock(second);
    BOOST_CHECK(chainman.FinishNewBlock(second));
    BOOST_CHECK(!second.new_block);
    BOOST_REQUIRE(WITH_LOCK(::cs_main, return chainman.ActiveTip()->GetBlockHash()) == block->GetHash());

    const auto child{std::make_shared<const CBlock>(CreateBlock({}, CScript() << OP_TRUE))};
    InProcess in;
    BOOST_REQUIRE(chainman.AcceptNewBlock(in, child, /*force_processing=*/true, /*min_pow_checked=*/true, /*activate_async=*/false));
    BOOST_REQUIRE(in.prepared.store);
    CBlockIndex* const parent{WITH_LOCK(::cs_main, return chainman.ActiveTip())};
    BlockValidationState invalidate_state;
    BOOST_REQUIRE(chainstate.InvalidateBlock(invalidate_state, parent));
    chainman.WriteNewBlock(in);
    BOOST_CHECK(!chainman.FinishNewBlock(in));
    BOOST_CHECK(in.state.GetResult() == BlockValidationResult::BLOCK_CACHED_INVALID);
    BOOST_CHECK_EQUAL(in.state.GetRejectReason(), "duplicate-invalid");
    BOOST_CHECK(!in.new_block);
    LOCK(::cs_main);
    const CBlockIndex* const child_index{chainman.m_blockman.LookupBlockIndex(child->GetHash())};
    BOOST_REQUIRE(child_index);
    BOOST_CHECK(!(child_index->nStatus & BLOCK_HAVE_DATA));
    BOOST_CHECK(!chainman.IsBlockBeingStored(*child_index));
}

//! A block's bytes are on disk before its index entry says so: between WriteNewBlock() and FinishNewBlock() the
//! block is being stored and has no BLOCK_HAVE_DATA, which only the finish records. A sync of the written files
//! made after the claim of a block's place does not cover the write that follows: the file is unsynced again
//! once the write is in, also for a block that is then not recorded as stored (its parent was invalidated
//! meanwhile, so that no activation follows and nothing syncs the file before it is looked at).
BOOST_FIXTURE_TEST_CASE(a_stored_block_is_recorded_after_its_write, TestChain100Setup)
{
    ChainstateManager& chainman{*Assert(m_node.chainman)};
    Chainstate& chainstate{chainman.ActiveChainstate()};
    auto& blockman{static_cast<TestBlockManager&>(chainman.m_blockman)};
    using InProcess = ChainstateManager::NewBlockInProcess;

    const auto block{std::make_shared<const CBlock>(CreateBlock({}, CScript() << OP_TRUE))};
    InProcess in;
    BOOST_REQUIRE(chainman.AcceptNewBlock(in, block, /*force_processing=*/true, /*min_pow_checked=*/true, /*activate_async=*/false));
    BOOST_REQUIRE(in.prepared.store && in.prepared.write);
    const CBlockIndex* const index{in.prepared.pindex};
    chainman.WriteNewBlock(in);
    BOOST_REQUIRE(in.written);
    {
        LOCK(::cs_main);
        BOOST_CHECK(!(index->nStatus & BLOCK_HAVE_DATA));
        BOOST_CHECK(chainman.IsBlockBeingStored(*index));
    }
    BOOST_CHECK(chainman.FinishNewBlock(in));
    {
        LOCK(::cs_main);
        BOOST_CHECK(index->nStatus & BLOCK_HAVE_DATA);
        BOOST_CHECK(!chainman.IsBlockBeingStored(*index));
        BOOST_REQUIRE(chainman.ActiveTip() == index);
    }

    const auto child{std::make_shared<const CBlock>(CreateBlock({}, CScript() << OP_TRUE))};
    InProcess child_in;
    BOOST_REQUIRE(chainman.AcceptNewBlock(child_in, child, /*force_processing=*/true, /*min_pow_checked=*/true, /*activate_async=*/false));
    BOOST_REQUIRE(child_in.prepared.store && child_in.prepared.write);
    const int file{child_in.prepared.pos.nFile};
    CBlockIndex* const parent{WITH_LOCK(::cs_main, return chainman.ActiveTip())};
    BlockValidationState invalidate_state;
    BOOST_REQUIRE(chainstate.InvalidateBlock(invalidate_state, parent));
    {
        LOCK(::cs_main);
        BOOST_REQUIRE(blockman.SyncWrittenFiles());
        BOOST_REQUIRE(!blockman.BlockFileUnsynced(file));
    }
    chainman.WriteNewBlock(child_in);
    BOOST_REQUIRE(child_in.written);
    BOOST_CHECK(!chainman.FinishNewBlock(child_in));
    BOOST_CHECK_EQUAL(child_in.state.GetRejectReason(), "duplicate-invalid");
    LOCK(::cs_main);
    BOOST_CHECK(blockman.BlockFileUnsynced(file));
}

//! The window's account is the node's, and only a step that uses the window writes it: a block connected
//! outside the window leaves its limit and its count as they are.
BOOST_FIXTURE_TEST_CASE(step_outside_the_window_leaves_its_account, TestChain100Setup)
{
    ChainstateManager& chainman{*Assert(m_node.chainman)};
    util::ByteAccount& window{chainman.m_window_bytes};
    BOOST_REQUIRE(!chainman.IsInitialBlockDownload());
    const size_t limit{size_t{1} << 30};
    const size_t bytes{777};
    window.SetLimit(limit);
    window.Set(bytes);
    const int height{WITH_LOCK(::cs_main, return chainman.ActiveChain().Height())};
    CreateAndProcessBlock({}, CScript() << OP_TRUE);
    BOOST_REQUIRE_EQUAL(WITH_LOCK(::cs_main, return chainman.ActiveChain().Height()), height + 1);
    BOOST_CHECK_EQUAL(window.Limit(), limit);
    BOOST_CHECK_EQUAL(window.Bytes(), bytes);
    window.Set(0);
}

//! The download ends while blocks handed to the window in it are still to be connected: the step goes on in
//! the window, the initial block download it was planned in, and the blocks it connects from there are announced as blocks
//! at the tip.
BOOST_FIXTURE_TEST_CASE(window_step_outlives_the_download, WindowStepSetup)
{
    ChainstateManager& chainman{*Assert(m_node.chainman)};
    Chainstate& chainstate{chainman.ActiveChainstate()};
    BlockValidationState state;
    bool activated{false};
    const int last_in_download{m_base_height + 1};
    CacheStepBlocks();
    {
        WindowStepper stepper{chainman};
        stepper.Start([&] { activated = chainstate.ActivateBestChain(state); });
        stepper.WaitForStep(m_base_height, InFlightAt(m_base_height));
        stepper.RunLowestBlock();
        stepper.WaitForStep(last_in_download, InFlightAt(last_in_download));
        BOOST_REQUIRE(chainman.IsInitialBlockDownload());
        // The blocks above are in flight, handed over in the download. It ends here.
        BOOST_REQUIRE_GT(InFlightAt(last_in_download), 1U);
        static_cast<TestChainstateManager&>(chainman).JumpOutOfIbd();
        for (int height{last_in_download + 1}; height <= Top(); ++height) {
            stepper.RunLowestBlock();
            // The window is refilled all the same: the step keeps the copy of the latch it was planned with.
            stepper.WaitForStep(height, InFlightAt(height));
        }
        stepper.Finish();
    }
    BOOST_CHECK(activated);
    BOOST_CHECK(state.IsValid());
    BOOST_CHECK(!chainman.IsInitialBlockDownload());
    CheckStepOver();
    const std::lock_guard lock{m_recorder->mutex};
    BOOST_REQUIRE(!m_recorder->tips.empty());
    for (const auto& [height, initial_download] : m_recorder->tips) {
        BOOST_CHECK_EQUAL(initial_download, height <= last_in_download);
    }
}

//! The node is told to stop while a step of the window has blocks in flight. The step ends at the next
//! block it publishes: that block is the tip and is announced as every block is, the blocks above it are
//! waited for and give their views back, and the pass ends without a failure. Nothing of
//! them stays behind: the next pass connects them.
BOOST_FIXTURE_TEST_CASE(interrupt_ends_a_window_step_at_a_published_block, WindowSummarySetup)
{
    // No block is connected a second time for it: the blocks taken back were not published.
    ExpectWindowSummary(/*blocks=*/BLOCKS, /*conflicts=*/0, /*retries=*/0);
    ChainstateManager& chainman{*Assert(m_node.chainman)};
    Chainstate& chainstate{chainman.ActiveChainstate()};
    CacheStepBlocks();
    const int stopped_at{m_base_height + 2};
    BlockValidationState state;
    bool activated{false};
    {
        WindowStepper stepper{chainman};
        stepper.Start([&] { activated = chainstate.ActivateBestChain(state); });
        stepper.WaitForStep(m_base_height, InFlightAt(m_base_height));
        stepper.RunLowestBlock();
        stepper.WaitForStep(m_base_height + 1, InFlightAt(m_base_height + 1));
        // The blocks above are in flight, none of them applied.
        BOOST_REQUIRE_GT(InFlightAt(m_base_height + 1), 1U);
        BOOST_REQUIRE(m_interrupt());
        // The second block is published, and the step ends there. It waits for the blocks above it before
        // it takes them back, which the workers run.
        stepper.RunLowestBlock();
        stepper.Finish();
    }
    BOOST_REQUIRE(m_interrupt.reset());
    BOOST_CHECK(activated);
    BOOST_CHECK(state.IsValid());
    BOOST_CHECK_EQUAL(m_node.exit_status.load(), EXIT_SUCCESS);
    BOOST_CHECK_EQUAL(WITH_LOCK(::cs_main, return chainstate.m_chain.Height()), stopped_at);
    m_node.validation_signals->SyncWithValidationInterfaceQueue();
    // The block cache alone keeps the blocks that were not connected.
    BOOST_CHECK_EQUAL(chainman.m_window_bytes.Bytes(), 0U);
    BOOST_CHECK_EQUAL(chainman.m_blockman.m_effects.PendingBytes(), 0U);
    BOOST_CHECK_EQUAL(chainman.m_pinned_bytes.Bytes(), 0U);
    {
        const std::vector<int> expected{m_base_height + 1, stopped_at};
        const std::lock_guard lock{m_recorder->mutex};
        BOOST_CHECK_EQUAL_COLLECTIONS(m_recorder->connected.begin(), m_recorder->connected.end(), expected.begin(), expected.end());
        std::vector<int> tips;
        for (const auto& [height, initial_download] : m_recorder->tips) tips.push_back(height);
        BOOST_CHECK_EQUAL_COLLECTIONS(tips.begin(), tips.end(), expected.begin(), expected.end());
    }

    // The blocks that were taken back connect in the next pass, each announced once.
    BlockValidationState next_state;
    BOOST_REQUIRE(chainstate.ActivateBestChain(next_state));
    BOOST_CHECK_EQUAL(WITH_LOCK(::cs_main, return chainstate.m_chain.Height()), Top());
    CheckStepOver();
}

BOOST_FIXTURE_TEST_CASE(reorg_announcement_order_survives_a_run_in_the_step, WriteOutEveryBlockSetup)
{
    BOOST_REQUIRE(m_node.chainman->m_options.test_write_out_every_block);
    CheckReorgAnnouncementOrder(*this);
}

//! The same with the periodic write-out due: mempool acceptance makes no write-out check between two of the
//! transactions a reorganisation adds back, which would settle the effects queue and hand on the connected
//! blocks' signals before the second transaction's.
BOOST_FIXTURE_TEST_CASE(reorg_announcement_order_survives_a_periodic_write, TestChain100Setup)
{
    CheckReorgAnnouncementOrder(*this, /*periodic_due=*/true);
}
BOOST_FIXTURE_TEST_CASE(reorg_announcement_order_survives_a_periodic_write_and_runs, WriteOutEveryBlockSetup)
{
    CheckReorgAnnouncementOrder(*this, /*periodic_due=*/true);
}

//! The same with the pinned blocks over their limit once the reorganisation has connected its first block.
//! The step does not wait there for the signals to leave, as a step that extends the tip does: the wait hands
//! on the signals that are ready, and the first block would be announced before the transactions.
BOOST_FIXTURE_TEST_CASE(reorg_announcement_order_survives_pinned_blocks_over_their_limit, TestChain100Setup)
{
    CheckReorgAnnouncementOrder(*this, /*periodic_due=*/false, /*pinned_over_limit=*/true);
}

//! At the tip, the block that improves the tip and the pass's notifications are one hold of cs_main: a thread
//! that takes cs_main never sees a tip the kernel's tip notification has not named yet.
BOOST_FIXTURE_TEST_CASE(tip_and_its_notification_are_one_hold, TestChain100Setup)
{
    BOOST_REQUIRE(!WITH_LOCK(::cs_main, return m_node.chainman->IsInitialBlockDownload()));
    auto& notifications{*m_node.notifications};
    std::atomic<bool> stop{false};
    std::atomic<int> checks{0}, mismatches{0};
    std::thread observer{[&] {
        while (!stop) {
            LOCK(::cs_main);
            const uint256 tip{m_node.chainman->ActiveChain().Tip()->GetBlockHash()};
            const auto notified{WITH_LOCK(notifications.m_tip_block_mutex, return notifications.TipBlock())};
            if (notified && *notified != tip) ++mismatches;
            ++checks;
        }
    }};
    for (int i{0}; i < 200; ++i) CreateAndProcessBlock({}, CScript() << OP_TRUE);
    stop = true;
    observer.join();
    BOOST_CHECK(checks.load() > 0);
    BOOST_CHECK_EQUAL(mismatches.load(), 0);
}

//! A block handed to the connector (ProcessNewBlock() with activate_async) is the tip once
//! WaitForPendingBlocks() returns. A stopped connector takes nothing: the block is stored, nobody waits for
//! it, and whoever activates the chain next connects it.
BOOST_FIXTURE_TEST_CASE(connector_connects_what_it_is_handed, TestChain100Setup)
{
    ChainstateManager& chainman{*Assert(m_node.chainman)};
    BOOST_REQUIRE(!chainman.IsInitialBlockDownload());
    const CScript script{CScript() << OP_TRUE};
    const auto tip{[&] { return WITH_LOCK(::cs_main, return chainman.ActiveChain().Tip()->GetBlockHash()); }};

    const auto first{std::make_shared<const CBlock>(CreateBlock({}, script))};
    BOOST_REQUIRE(chainman.ProcessNewBlock(first, /*force_processing=*/true, /*min_pow_checked=*/true, nullptr, /*activate_async=*/true));
    chainman.WaitForPendingBlocks(chainman.IsInitialBlockDownload());
    BOOST_CHECK_EQUAL(tip(), first->GetHash());

    chainman.StopConnector();
    const auto second{std::make_shared<const CBlock>(CreateBlock({}, script))};
    BOOST_REQUIRE(chainman.ProcessNewBlock(second, /*force_processing=*/true, /*min_pow_checked=*/true, nullptr, /*activate_async=*/true));
    chainman.WaitForPendingBlocks(chainman.IsInitialBlockDownload());
    {
        LOCK(::cs_main);
        const CBlockIndex* index{chainman.m_blockman.LookupBlockIndex(second->GetHash())};
        BOOST_REQUIRE(index);
        BOOST_CHECK(index->nStatus & BLOCK_HAVE_DATA);
    }
    BOOST_CHECK_EQUAL(tip(), first->GetHash());

    BlockValidationState state;
    BOOST_REQUIRE(chainman.ActiveChainstate().ActivateBestChain(state));
    BOOST_CHECK_EQUAL(tip(), second->GetHash());
}

//! The waits and the yield go by the copy they are given, not by the latch: an operation that started in
//! the download finishes as one of the download after the latch fell.
BOOST_FIXTURE_TEST_CASE(a_copy_outlives_the_latch, TestChain100Setup)
{
    ChainstateManager& chainman{*Assert(m_node.chainman)};
    BOOST_REQUIRE(!chainman.IsInitialBlockDownload());
    // Nothing is queued: neither copy gives way, and neither wait has anything to wait for.
    BOOST_CHECK(!chainman.YieldHistoricalActivation(/*in_ibd=*/true));
    BOOST_CHECK(!chainman.YieldHistoricalActivation(/*in_ibd=*/false));
    chainman.WaitForPendingBlocks(/*in_ibd=*/true);
    chainman.WaitForPendingBlocks(/*in_ibd=*/false);

    // An activation is handed to the connector, which cannot attempt it while cs_main is held here.
    std::future<void> download;
    std::future<void> tip;
    {
        LOCK(::cs_main);
        std::async(std::launch::async, [&] { chainman.SignalConnector(); }).get();
        download = std::async(std::launch::async, [&] { chainman.WaitForPendingBlocks(/*in_ibd=*/true); });
        tip = std::async(std::launch::async, [&] { chainman.WaitForPendingBlocks(/*in_ibd=*/false); });
        // The latch says tip. The wait that was given the download's copy goes by the copy: it returns with
        // the activation still to be attempted.
        BOOST_REQUIRE(download.wait_for(std::chrono::seconds{60}) == std::future_status::ready);
        BOOST_CHECK(chainman.AnyHandOffPending());
        // The wait that was given the tip's copy is still waiting. (A slow thread can make this pass, not fail.)
        BOOST_CHECK(tip.wait_for(std::chrono::milliseconds{100}) == std::future_status::timeout);
        // The download's copy never gives way. (The tip's copy gives way only until the connector takes the
        // activation, which it does without cs_main: either answer is right here.)
        BOOST_CHECK(!chainman.YieldHistoricalActivation(/*in_ibd=*/true));
    }
    // With cs_main free the connector attempts the activation, and that wait returns too.
    tip.get();
    // The activation was taken, so the tip's copy no longer gives way either.
    BOOST_CHECK(!chainman.YieldHistoricalActivation(/*in_ibd=*/false));
}

BOOST_AUTO_TEST_SUITE_END()
