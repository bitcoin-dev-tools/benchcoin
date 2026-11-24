// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <coins.h>
#include <coinsstore.h>
#include <dbwrapper.h>
#include <kernel/coinstats.h>
#include <node/blockstorage.h>
#include <primitives/block.h>
#include <script/script.h>
#include <test/util/coinsstore.h>
#include <test/util/random.h>
#include <test/util/setup_common.h>
#include <txdb.h>
#include <undo.h>
#include <util/threadpool.h>
#include <util/byte_units.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <functional>
#include <atomic>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
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

//! A started pool for a flush's further workers, as the node gives the store its connect pool.
static std::shared_ptr<ThreadPool> FlushHelpers()
{
    auto pool{std::make_shared<ThreadPool>("flush")};
    pool->Start(3);
    return pool;
}

BOOST_FIXTURE_TEST_SUITE(coinsstore_tests, BasicTestingSetup)

namespace {

COutPoint RandomOutpoint(FastRandomContext& rng, uint32_t txids)
{
    // A small txid space so that the same outpoints are revisited.
    uint256 h;
    h.data()[0] = static_cast<unsigned char>(rng.randrange(txids));
    return COutPoint{Txid::FromUint256(h), static_cast<uint32_t>(rng.randrange(3))};
}

Coin RandomCoin(FastRandomContext& rng, int height)
{
    CTxOut out{static_cast<CAmount>(rng.randrange(1000) + 1), CScript() << std::vector<unsigned char>(rng.randbytes(4))};
    return Coin{std::move(out), height, /*fCoinBaseIn=*/false};
}

//! The visible state of a view over a universe of outpoints.
std::map<COutPoint, std::optional<Coin>> Visible(const CCoinsView& view, const std::vector<COutPoint>& universe)
{
    std::map<COutPoint, std::optional<Coin>> out;
    for (const COutPoint& op : universe) out[op] = view.PeekCoin(op);
    return out;
}

bool Same(const std::optional<Coin>& a, const std::optional<Coin>& b)
{
    if (!a && !b) return true;
    if (!a || !b) return false;
    return a->out == b->out && a->nHeight == b->nHeight && a->fCoinBase == b->fCoinBase;
}

void CheckSameVisible(const CCoinsView& a, const CCoinsView& b, const std::vector<COutPoint>& universe)
{
    const auto va{Visible(a, universe)};
    const auto vb{Visible(b, universe)};
    for (const COutPoint& op : universe) {
        BOOST_CHECK_MESSAGE(Same(va.at(op), vb.at(op)), "mismatch at " << op.ToString());
    }
}

std::vector<COutPoint> Universe(uint32_t txids)
{
    std::vector<COutPoint> out;
    for (uint32_t t{0}; t < txids; ++t) {
        uint256 h;
        h.data()[0] = static_cast<unsigned char>(t);
        for (uint32_t n{0}; n < 3; ++n) out.emplace_back(Txid::FromUint256(h), n);
    }
    return out;
}

//! The block at `height` creates `outpoint`: the one confirmed change. Returns the number of conflicts.
uint32_t Create(CoinsStore& store, int height, const COutPoint& outpoint, Coin coin, bool possible_overwrite)
{
    std::vector<OwnedChange> changes;
    changes.push_back({.outpoint = outpoint, .coin = std::move(coin), .possible_overwrite = possible_overwrite});
    return ApplyConfirmed(store, height, changes);
}

//! The block at `height` spends `outpoint`, whichever coin is there. Returns the number of conflicts.
uint32_t Spend(CoinsStore& store, int height, const COutPoint& outpoint)
{
    std::vector<OwnedChange> changes;
    changes.push_back({.outpoint = outpoint, .coin = Coin{}});
    return ApplyConfirmed(store, height, changes);
}

//! A name for the block at `height`: any but the null hash.
uint256 BlockLabel(int height) { return uint256{static_cast<uint8_t>(1 + height % 255)}; }

//! A write-out run as the node makes one: begun, streamed to the base and committed.
std::unique_ptr<CoinsStore::WriteOutRun> WriteOut(CoinsStore& store, int bound_height, int head_height, const uint256& new_head, const std::function<uint256(int)>& block_at = BlockLabel)
{
    auto run{store.BeginWriteOut(bound_height, head_height, new_head, /*final=*/false)};
    BOOST_REQUIRE(run);
    store.StreamWriteOut(*run, block_at);
    store.CommitWriteOut(*run);
    return run;
}

//! A base that keeps the changes of the latest run written through it.
class RunBase final : public CCoinsViewBacked
{
public:
    using CCoinsViewBacked::CCoinsViewBacked;
    std::vector<CoinWrite> m_writes;
    void WriteRun(const std::vector<CoinWrite>& writes, const uint256& new_head, const uint256& old_head, bool final) override
    {
        // A run ends with a write of no changes, which closes or moves its marker.
        if (std::exchange(m_closed, writes.empty())) m_writes.clear();
        m_writes.insert(m_writes.end(), writes.begin(), writes.end());
        CCoinsViewBacked::WriteRun(writes, new_head, old_head, final);
    }

private:
    bool m_closed{false};
};

//! The lowest change height among the dirty entries, or nullopt if there is none.
std::optional<int> OldestDirty(const CoinsStore& store)
{
    const int bound{store.BoundForBytes(1, std::numeric_limits<int>::max())};
    return bound == std::numeric_limits<int>::max() ? std::nullopt : std::make_optional(bound);
}

//! Bytes of the dirty entries changed at or below `height`: the most that a bound no higher frees.
size_t DirtyBytesAtOrBelow(const CoinsStore& store, int height)
{
    const auto within{[&](size_t bytes) { return store.BoundForBytes(bytes, std::numeric_limits<int>::max()) <= height; }};
    size_t low{0};
    size_t high{1};
    while (within(high)) high *= 2;
    while (high - low > 1) {
        const size_t mid{low + (high - low) / 2};
        (within(mid) ? low : high) = mid;
    }
    return low;
}

} // namespace

//! The store, driven as a cache would be (adds, spends, child flushes, full flushes), shows the same coins
//! as a CCoinsViewCache over an identical database at every step. After a bounded run its database holds
//! every outpoint as it is, unless the outpoint changed above the height the run settled at.
BOOST_AUTO_TEST_CASE(same_as_cache_under_random_operations)
{
    constexpr uint32_t TXIDS{24};
    const auto universe{Universe(TXIDS)};
    CCoinsViewDB db_cache{MemoryDb("cache"), {}};
    CCoinsViewDB db_store{MemoryDb("store"), {}};
    CCoinsViewCache cache{&db_cache};
    RunBase base{&db_store};
    CoinsStore store{&base, /*shards=*/8, /*deterministic=*/true};
    const uint256 genesis{m_rng.rand256()};
    cache.SetBestBlock(genesis);
    store.SetBestBlock(genesis, 0);
    // The step each outpoint was last added or spent at.
    std::map<COutPoint, int> last_change;

    for (int step{1}; step < 400; ++step) {
        const uint256 block_hash{m_rng.rand256()};
        store.SetChangeHeight(step);
        // Some operations straight on the tier, some through a child cache as a block connection does.
        if (m_rng.randbool()) {
            for (int i{0}; i < 6; ++i) {
                const COutPoint op{RandomOutpoint(m_rng, TXIDS)};
                last_change[op] = step;
                if (m_rng.randrange(3) == 0) {
                    // SpendCoin() answers true for any entry it holds, spent or not, in both; the store keeps
                    // spent entries of blocks above a run's bound where the cache dropped them at its full
                    // write, so only the answer for a visible coin is comparable.
                    const bool had{cache.HaveCoin(op)};
                    BOOST_CHECK_EQUAL(had, store.HaveCoin(op));
                    const bool a{cache.SpendCoin(op)};
                    const bool b{store.SpendCoin(op)};
                    if (had) BOOST_CHECK(a && b);
                } else {
                    Coin coin{RandomCoin(m_rng, step)};
                    Coin copy{coin};
                    const bool overwrite{cache.HaveCoin(op)};
                    BOOST_CHECK_EQUAL(overwrite, store.HaveCoin(op));
                    cache.AddCoin(op, std::move(coin), overwrite);
                    store.AddCoin(op, std::move(copy), overwrite);
                }
            }
            cache.SetBestBlock(block_hash);
            store.SetBestBlock(block_hash);
        } else {
            CCoinsViewCache child_cache{&cache};
            CCoinsViewCache child_store{&store};
            for (int i{0}; i < 6; ++i) {
                const COutPoint op{RandomOutpoint(m_rng, TXIDS)};
                last_change[op] = step;
                if (m_rng.randrange(3) == 0) {
                    const bool had{child_cache.HaveCoin(op)};
                    BOOST_CHECK_EQUAL(had, child_store.HaveCoin(op));
                    const bool a{child_cache.SpendCoin(op)};
                    const bool b{child_store.SpendCoin(op)};
                    if (had) BOOST_CHECK(a && b);
                } else {
                    Coin coin{RandomCoin(m_rng, step)};
                    Coin copy{coin};
                    const bool overwrite{child_cache.HaveCoin(op)};
                    BOOST_CHECK_EQUAL(overwrite, child_store.HaveCoin(op));
                    child_cache.AddCoin(op, std::move(coin), overwrite);
                    child_store.AddCoin(op, std::move(copy), overwrite);
                }
            }
            child_cache.SetBestBlock(block_hash);
            child_store.SetBestBlock(block_hash);
            child_cache.Flush();
            child_store.Flush();
        }
        CheckSameVisible(cache, store, universe);
        BOOST_CHECK(cache.GetBestBlock() == store.GetBestBlock());

        // Now and then write out: the cache in full, the store either in full or as a lag run of its
        // oldest changes; afterwards the databases must agree on everything the store has written and
        // the views must still agree on everything.
        if (step % 25 == 0) {
            cache.Sync();
            if (m_rng.randbool()) {
                store.Sync();
                CheckSameVisible(db_cache, db_store, universe);
                BOOST_CHECK(db_store.GetBestBlock() == block_hash);
                BOOST_CHECK(db_store.GetHeadBlocks().empty());
            } else {
                const int bound{step - 1 - static_cast<int>(m_rng.randrange(19))};
                auto run{store.BeginWriteOut(bound, step, block_hash, /*final=*/false)};
                if (run) {
                    store.StreamWriteOut(*run, [&](int height) { return height == step ? block_hash : BlockLabel(height); });
                    store.CommitWriteOut(*run);
                    BOOST_CHECK(run->settled_height >= bound && run->settled_height <= step);
                    BOOST_CHECK(store.WrittenHead() == run->settled_head);
                    const auto heads{db_store.GetHeadBlocks()};
                    BOOST_REQUIRE_EQUAL(heads.size(), 2U);
                    BOOST_CHECK(heads[0] == block_hash);
                    BOOST_CHECK(db_store.GetBestBlock().IsNull());
                    // Nothing changed at or below the bound is still dirty.
                    const auto oldest{OldestDirty(store)};
                    BOOST_CHECK(!oldest || *oldest > bound);
                    // The database holds the state at the settled height: every outpoint not changed above
                    // it is there as the views have it. (An outpoint changed above it is the replay's, which
                    // applies that change again.)
                    for (const COutPoint& op : universe) {
                        if (const auto changed{last_change.find(op)}; changed != last_change.end() && changed->second > run->settled_height) continue;
                        BOOST_CHECK_MESSAGE(Same(db_store.PeekCoin(op), store.PeekCoin(op)),
                                            "after a run settled at " << run->settled_height << " the database differs at " << op.ToString());
                    }
                    cache.Sync();
                }
            }
            CheckSameVisible(cache, store, universe);
            store.SanityCheck();
        }
    }
    // A final drain leaves both databases identical and consistent.
    cache.Flush();
    store.Flush();
    CheckSameVisible(db_cache, db_store, universe);
    BOOST_CHECK(db_store.GetHeadBlocks().empty());
    BOOST_CHECK_EQUAL(store.GetCacheSize(), 0U);
}

/**
 * The settled head after a run is the block whose state the database holds, not the bound: a reorg's
 * inverse changes are tagged with the fork height so that any run carries them all, and the run that
 * does so moves the database to the tier's head. A coin a disconnect restores that was created above
 * the settled height stays out of the database until its creating block is written.
 */
BOOST_AUTO_TEST_CASE(settled_head_follows_the_state_not_the_bound)
{
    CCoinsViewDB db{MemoryDb("store"), {}};
    RunBase base{&db};
    CoinsStore store{&base, 4, true};
    const auto hash{[](int h) { return uint256{static_cast<uint8_t>(h)}; }};
    const auto outpoint{[](int n) { return COutPoint{Txid::FromUint256(uint256{static_cast<uint8_t>(100 + n)}), 0}; }};

    // Blocks 300..307 connected and drained: the database holds the state at 307.
    for (int h{300}; h <= 307; ++h) {
        store.SetBestBlock(hash(h), h);
        store.AddCoin(outpoint(h), Coin{CTxOut{CAmount{h}, CScript() << OP_TRUE}, h, false}, false);
    }
    store.SetChangeHeight(306);
    store.SpendCoin(outpoint(300)); // spent in 306
    store.SetChangeHeight(307);
    store.SpendCoin(outpoint(301)); // spent in 307
    store.Sync();
    BOOST_CHECK(store.WrittenHead() == hash(307));

    // Disconnect 307 towards a fork at 301: its inverse changes are tagged 301.
    store.SetChangeHeight(301);
    store.SpendCoin(outpoint(307));                                                              // 307's coinbase goes
    store.AddCoin(outpoint(301), Coin{CTxOut{CAmount{301}, CScript() << OP_TRUE}, 301, false}, false); // 307's spend undone
    store.SetBestBlock(hash(306));
    store.SetTierHeight(306);
    BOOST_CHECK_EQUAL(*OldestDirty(store), 301);

    // A run asked to stop at 301 is final here: the tier fell below the written head (SetTierHeight()), and
    // the changes that take the database back are tagged with the fork. It carries both and leaves nothing
    // dirty: the database then holds 306's state, and that, not 301, is what the next marker must name as
    // its old head.
    auto run{WriteOut(store, /*bound_height=*/301, /*head_height=*/306, hash(306), hash)};
    BOOST_CHECK(run->final);
    BOOST_CHECK_EQUAL(run->count, 2U);
    BOOST_CHECK(run->old_head == hash(307));
    BOOST_CHECK_EQUAL(run->settled_height, 306);
    BOOST_CHECK(store.WrittenHead() == hash(306));
    BOOST_CHECK(!OldestDirty(store));

    // Connect 307' and 308' on another branch, unwritten: 307' creates a coin that 308' spends, and 308'
    // also spends a coin the database has (created in 304).
    store.SetBestBlock(hash(57), 307);
    store.AddCoin(outpoint(57), Coin{CTxOut{CAmount{57}, CScript() << OP_TRUE}, 307, false}, false);
    store.SetBestBlock(hash(58), 308);
    store.AddCoin(outpoint(58), Coin{CTxOut{CAmount{58}, CScript() << OP_TRUE}, 308, false}, false);
    BOOST_CHECK(store.SpendCoin(outpoint(57)));
    BOOST_CHECK(store.SpendCoin(outpoint(304)));

    // Disconnect 308' towards a fork at 306. The restored coin from 304 is in the database and takes the
    // fork height; the restored coin from 307' is not, and keeps 307 so that no run below 307 writes it.
    store.SetChangeHeight(306);
    store.SpendCoin(outpoint(58));
    store.AddCoin(outpoint(304), Coin{CTxOut{CAmount{304}, CScript() << OP_TRUE}, 304, false}, false);
    store.AddCoin(outpoint(57), Coin{CTxOut{CAmount{57}, CScript() << OP_TRUE}, 307, false}, false);
    store.SetBestBlock(hash(57));
    store.SetTierHeight(307);
    auto run2{WriteOut(store, /*bound_height=*/306, /*head_height=*/307, hash(57), hash)};
    BOOST_REQUIRE_EQUAL(base.m_writes.size(), 1U);
    BOOST_CHECK(base.m_writes[0].outpoint == outpoint(304));
    BOOST_CHECK(run2->old_head == hash(306));
    // 307' remains dirty (its coinbase and the restored coin), so the database settles at 306 still.
    BOOST_CHECK_EQUAL(run2->settled_height, 306);
    BOOST_CHECK(store.WrittenHead() == hash(306));
    BOOST_CHECK(!db.HaveCoin(outpoint(57)));
    BOOST_CHECK(db.HaveCoin(outpoint(304)));

    // A final drain writes 307' in full.
    store.Sync();
    BOOST_CHECK(db.HaveCoin(outpoint(57)));
    BOOST_CHECK(!db.HaveCoin(outpoint(58)));
    BOOST_CHECK(store.WrittenHead() == hash(57));
    store.SanityCheck();
}

//! A coin created and spent above the written head cancels in the tier. Disconnecting the spend restores
//! it while the change height is the fork height, which is at or above its creation: it is tagged with
//! its creation height, so that a run bounded between the two carries it. Tagged with the fork height it
//! would be left behind while the run's settled head passed its creation, and a crash in the next run
//! would replay from above the block that creates it, which nothing then writes again.
BOOST_AUTO_TEST_CASE(restored_coin_keeps_its_creation_height)
{
    CCoinsViewDB db{MemoryDb("store"), {}};
    CoinsStore store{&db, 4, true};
    const auto hash{[](int h) { return uint256{static_cast<uint8_t>(h - 200)}; }};
    const auto coin{[](int h) { return Coin{CTxOut{CAmount{h}, CScript() << OP_TRUE}, h, false}; }};
    const COutPoint c{Txid::FromUint256(uint256{77}), 0};

    store.SetBestBlock(hash(300), 300);
    store.Sync(); // the database holds the state at 300
    BOOST_REQUIRE(store.WrittenHead() == hash(300));
    // 301 to 305; c is created in 302 and spent in 304, each block also creates a coin of its own.
    for (int h{301}; h <= 305; ++h) {
        store.SetBestBlock(hash(h), h);
        store.AddCoin(COutPoint{Txid::FromUint256(uint256{static_cast<uint8_t>(h - 200)}), 0}, coin(h), false);
        if (h == 302) store.AddCoin(c, coin(302), false);
        if (h == 304) BOOST_REQUIRE(store.SpendCoin(c));
    }
    BOOST_CHECK(!store.HaveCoinInCache(c));

    // Disconnect 305 and 304 towards a fork at 303: 304's spend of c is undone.
    store.SetChangeHeight(303);
    BOOST_REQUIRE(store.SpendCoin(COutPoint{Txid::FromUint256(uint256{105}), 0}));
    BOOST_REQUIRE(store.SpendCoin(COutPoint{Txid::FromUint256(uint256{104}), 0}));
    store.AddCoin(c, coin(302), false);
    store.SetBestBlock(hash(303));
    store.SetTierHeight(303);

    // A run bounded at 302 settles the database at 302 or below unless it carries c.
    WriteOut(store, /*bound_height=*/302, /*head_height=*/303, hash(303), hash);
    BOOST_CHECK(store.WrittenHead() == hash(302));
    BOOST_CHECK(db.HaveCoin(c));
    store.SanityCheck();
}

//! A coin created and spent inside the lag is never written, not even as an erase, and a spent entry
//! that was written is erased on the next run.
BOOST_AUTO_TEST_CASE(fresh_coins_leave_no_trace)
{
    CCoinsViewDB db{MemoryDb("store"), {}};
    RunBase base{&db};
    CoinsStore store{&base, 4, true};
    store.SetBestBlock(uint256::ONE, 1);
    const COutPoint a{Txid::FromUint256(uint256{10}), 0};
    const COutPoint b{Txid::FromUint256(uint256{11}), 0};
    store.AddCoin(a, RandomCoin(m_rng, 1), false);
    store.AddCoin(b, RandomCoin(m_rng, 1), false);
    store.SetChangeHeight(2);
    BOOST_CHECK(store.SpendCoin(a)); // fresh: dropped outright
    BOOST_CHECK_EQUAL(store.GetCacheSize(), 1U);
    WriteOut(store, 2, 2, uint256{2});
    BOOST_REQUIRE_EQUAL(base.m_writes.size(), 1U); // only b
    BOOST_CHECK(base.m_writes[0].coin.has_value());
    BOOST_CHECK(db.PeekCoin(b).has_value());
    BOOST_CHECK_EQUAL(store.GetDirtyCount(), 0U);
    // b is in the database now; spending it must produce an erase even though it was fresh once.
    store.SetChangeHeight(3);
    BOOST_CHECK(store.SpendCoin(b));
    BOOST_CHECK_EQUAL(store.GetDirtyCount(), 1U);
    WriteOut(store, 3, 3, uint256{3});
    BOOST_REQUIRE_EQUAL(base.m_writes.size(), 1U);
    BOOST_CHECK(!base.m_writes[0].coin.has_value());
    BOOST_CHECK(!db.PeekCoin(b).has_value());
    BOOST_CHECK_EQUAL(store.GetCacheSize(), 0U);
}

//! Clean entries go first, and the bound for a byte target walks the height histogram from the oldest.
BOOST_AUTO_TEST_CASE(evict_clean_and_bound_for_bytes)
{
    CCoinsViewDB db{MemoryDb("store"), {}};
    {
        CCoinsViewCache seed{&db};
        for (uint32_t i{0}; i < 50; ++i) seed.AddCoin(COutPoint{Txid::FromUint256(uint256{static_cast<uint8_t>(i + 1)}), 0}, RandomCoin(m_rng, 1), false);
        seed.SetBestBlock(uint256::ONE);
        seed.Flush();
    }
    CoinsStore store{&db, 4, true};
    store.SetBestBlock(uint256::ONE, 1);
    for (uint32_t i{0}; i < 50; ++i) BOOST_CHECK(store.HaveCoin(COutPoint{Txid::FromUint256(uint256{static_cast<uint8_t>(i + 1)}), 0}));
    BOOST_CHECK_EQUAL(store.GetCacheSize(), 50U);
    BOOST_CHECK_EQUAL(store.GetDirtyCount(), 0U);
    for (int h{2}; h <= 11; ++h) {
        store.SetChangeHeight(h);
        store.AddCoin(COutPoint{Txid::FromUint256(uint256{static_cast<uint8_t>(100 + h)}), 0}, RandomCoin(m_rng, h), false);
    }
    const size_t before{store.DynamicMemoryUsage()};
    store.EvictClean(0);
    BOOST_CHECK_EQUAL(store.GetCacheSize(), 10U);
    BOOST_CHECK(store.DynamicMemoryUsage() < before);
    BOOST_CHECK_EQUAL(*OldestDirty(store), 2);
    const size_t per_height{DirtyBytesAtOrBelow(store, 2)};
    BOOST_CHECK(per_height > 0);
    BOOST_CHECK_EQUAL(store.BoundForBytes(per_height, 11), 2);
    BOOST_CHECK_EQUAL(store.BoundForBytes(per_height * 3, 11), 4);
    BOOST_CHECK_EQUAL(store.BoundForBytes(per_height * 100, 11), 11);
    auto run{WriteOut(store, 4, 11, uint256{11})};
    BOOST_CHECK_EQUAL(run->count, 3U);
    BOOST_CHECK_EQUAL(run->settled_height, 4); // height 5 stays dirty
    BOOST_CHECK_EQUAL(store.GetDirtyCount(), 7U);
    BOOST_CHECK_EQUAL(*OldestDirty(store), 5);
    store.SanityCheck();
}

//! The dirty entries are summed by span of heights; sums, bounds and the settled height are exact to the height
//! all the same, inside a span and across spans.
BOOST_AUTO_TEST_CASE(bounds_are_exact_inside_and_across_dirty_spans)
{
    CCoinsViewDB db{MemoryDb("store"), {}};
    {
        CCoinsViewCache seed{&db};
        seed.AddCoin(COutPoint{Txid::FromUint256(uint256{200}), 0}, RandomCoin(m_rng, 1), false);
        seed.SetBestBlock(uint256::ONE);
        seed.Flush();
    }
    CoinsStore store{&db, 4, true};
    store.SetBestBlock(uint256::ONE, 1);
    const std::vector<int> heights{1000, 1020, 1030, 1040, 2100, 5000};
    for (size_t i{0}; i < heights.size(); ++i) {
        store.SetChangeHeight(heights[i]);
        store.AddCoin(COutPoint{Txid::FromUint256(uint256{static_cast<uint8_t>(i + 1)}), 0}, RandomCoin(m_rng, heights[i]), false);
    }
    store.SanityCheck();
    BOOST_CHECK_EQUAL(*OldestDirty(store), 1000);
    const size_t each{DirtyBytesAtOrBelow(store, 1000)};
    BOOST_CHECK(each > 0);
    BOOST_CHECK_EQUAL(DirtyBytesAtOrBelow(store, 999), 0U);
    BOOST_CHECK_EQUAL(DirtyBytesAtOrBelow(store, 1029), each * 2);
    BOOST_CHECK_EQUAL(DirtyBytesAtOrBelow(store, 1030), each * 3);
    BOOST_CHECK_EQUAL(DirtyBytesAtOrBelow(store, 4999), each * 5);
    BOOST_CHECK_EQUAL(store.BoundForBytes(each * 2, 5000), 1020);
    BOOST_CHECK_EQUAL(store.BoundForBytes(each * 3, 5000), 1030);
    BOOST_CHECK_EQUAL(store.BoundForBytes(each * 5, 5000), 2100);
    BOOST_CHECK_EQUAL(store.BoundForBytes(each * 7, 5000), 5000);

    // A bound inside a span takes the heights at or below it and settles just below the next height, which
    // shares its span with what was written.
    auto run{WriteOut(store, /*bound_height=*/1030, /*head_height=*/5000, uint256{50})};
    BOOST_CHECK_EQUAL(run->count, 3U);
    BOOST_CHECK_EQUAL(run->settled_height, 1039);
    BOOST_CHECK_EQUAL(*OldestDirty(store), 1040);

    // A bound between two spans leaves the spans further up alone.
    run = WriteOut(store, /*bound_height=*/2000, /*head_height=*/5000, uint256{50});
    BOOST_CHECK_EQUAL(run->count, 1U);
    BOOST_CHECK_EQUAL(run->settled_height, 2099);
    BOOST_CHECK_EQUAL(store.GetDirtyCount(), 2U);
    BOOST_CHECK_EQUAL(*OldestDirty(store), 2100);
    store.SanityCheck();
}

//! A base whose write fails once it is told to.
class FailingBase final : public CCoinsViewBacked
{
public:
    using CCoinsViewBacked::CCoinsViewBacked;
    bool m_fail{false};
    void WriteRun(const std::vector<CoinWrite>& writes, const uint256& new_head, const uint256& old_head, bool final) override
    {
        if (m_fail) throw std::runtime_error{"the write failed"};
        CCoinsViewBacked::WriteRun(writes, new_head, old_head, final);
    }
};

//! A run whose write fails is abandoned and is no longer in flight. What it collected counts as written in
//! the tier and is not in the database, so the tier refuses every write after it.
BOOST_AUTO_TEST_CASE(failed_run_ends_all_writes)
{
    CCoinsViewDB db{MemoryDb("store"), {}};
    FailingBase base{&db};
    CoinsStore store{&base, 256, true};
    store.SetBestBlock(uint256::ONE, 1);
    const COutPoint outpoint{Txid::FromUint256(m_rng.rand256()), 0};
    store.AddCoin(outpoint, RandomCoin(m_rng, 1), false);
    BOOST_CHECK(!store.FlushFailed());
    base.m_fail = true;
    BOOST_CHECK_THROW(store.Sync(), std::runtime_error);
    BOOST_CHECK(!store.RunInFlight());
    BOOST_CHECK(store.FlushFailed());
    base.m_fail = false;
    BOOST_CHECK_THROW(store.Flush(), std::runtime_error);
    BOOST_CHECK(!db.PeekCoin(outpoint));
    // A forced sync refuses as well: it would close the marker over what the failed run had collected.
    BOOST_CHECK_THROW(store.Sync(), std::runtime_error);
    BOOST_CHECK(!db.PeekCoin(outpoint));
    // And so does a run started directly.
    BOOST_CHECK_THROW(store.BeginWriteOut(/*bound_height=*/1, /*head_height=*/1, uint256::ONE, /*final=*/true), std::runtime_error);
    BOOST_CHECK(!store.RunInFlight());
}

//! Counts what is written through it: the runs, their changes, and what they come to as database batches.
class CountingBase final : public CCoinsViewBacked
{
public:
    using CCoinsViewBacked::CCoinsViewBacked;
    Mutex m_mutex;
    size_t m_runs GUARDED_BY(m_mutex){0};
    size_t m_changes GUARDED_BY(m_mutex){0};
    uint64_t m_bytes GUARDED_BY(m_mutex){0};
    void WriteRun(const std::vector<CoinWrite>& writes, const uint256& new_head, const uint256& old_head, bool final) override EXCLUSIVE_LOCKS_REQUIRED(!m_mutex)
    {
        {
            LOCK(m_mutex);
            ++m_runs;
            m_changes += writes.size();
            for (const CoinWrite& write : writes) m_bytes += write.coin ? 45 + write.coin->out.scriptPubKey.size() : 38;
        }
        CCoinsViewBacked::WriteRun(writes, new_head, old_head, final);
    }
};

//! A flush whose helpers' pool is busy with tasks that wait (as the connect pool's receive tasks wait for
//! cs_main, which the flush's caller may hold) writes every range itself and returns; the helper tasks that come
//! up later find their ranges taken and return at once.
BOOST_AUTO_TEST_CASE(flush_with_a_busy_pool_writes_every_range_itself)
{
    if (std::thread::hardware_concurrency() < 2) return; // one core: the flush has one worker and no helpers
    static constexpr uint64_t BATCH{4000};
    CCoinsViewDB db{MemoryDb("store"), {}};
    CountingBase base{&db};
    auto pool{FlushHelpers()};
    std::promise<void> release;
    const std::shared_future<void> released{release.get_future().share()};
    std::atomic<int> waiting{0};
    for (size_t i{0}; i < pool->WorkersCount(); ++i) {
        BOOST_REQUIRE(pool->Submit([&waiting, released] { ++waiting; released.wait(); }));
    }
    while (waiting.load() < static_cast<int>(pool->WorkersCount())) std::this_thread::yield();
    {
        CoinsStore store{&base, 256, true};
        store.SetBaseWrites(BATCH, /*concurrent=*/true);
        store.SetFlushHelpers(pool);
        store.SetBestBlock(uint256::ONE, 1);
        std::map<COutPoint, Coin> expected;
        for (int i{0}; i < 2000; ++i) {
            const COutPoint outpoint{Txid::FromUint256(m_rng.rand256()), 0};
            Coin coin{RandomCoin(m_rng, 1)};
            expected.emplace(outpoint, coin);
            store.AddCoin(outpoint, std::move(coin), false);
        }
        store.Flush();
        BOOST_CHECK_EQUAL(store.GetCacheSize(), 0U);
        BOOST_CHECK(db.GetBestBlock() == uint256::ONE);
        for (const auto& [outpoint, coin] : expected) {
            const std::optional<Coin> found{db.PeekCoin(outpoint)};
            BOOST_REQUIRE(found);
            BOOST_CHECK(found->out == coin.out);
        }
        release.set_value();
        // The helper tasks queued behind the waiting ones run and return before the store goes.
        pool->Stop();
    }
}

//! As above, with the helper tasks still queued when the store is gone: they come up after it was destroyed, find
//! their ranges taken and return, touching nothing of the store or of the flush, whose state went with them.
BOOST_AUTO_TEST_CASE(flush_helpers_coming_up_after_the_store_return_at_once)
{
    if (std::thread::hardware_concurrency() < 2) return; // one core: the flush has one worker and no helpers
    static constexpr uint64_t BATCH{4000};
    CCoinsViewDB db{MemoryDb("store"), {}};
    CountingBase base{&db};
    auto pool{FlushHelpers()};
    std::promise<void> release;
    const std::shared_future<void> released{release.get_future().share()};
    std::atomic<int> waiting{0};
    for (size_t i{0}; i < pool->WorkersCount(); ++i) {
        BOOST_REQUIRE(pool->Submit([&waiting, released] { ++waiting; released.wait(); }));
    }
    while (waiting.load() < static_cast<int>(pool->WorkersCount())) std::this_thread::yield();
    {
        CoinsStore store{&base, 256, true};
        store.SetBaseWrites(BATCH, /*concurrent=*/true);
        store.SetFlushHelpers(pool);
        store.SetBestBlock(uint256::ONE, 1);
        for (int i{0}; i < 2000; ++i) store.AddCoin(COutPoint{Txid::FromUint256(m_rng.rand256()), 0}, RandomCoin(m_rng, 1), false);
        store.Flush();
        BOOST_CHECK_EQUAL(store.GetCacheSize(), 0U);
        BOOST_CHECK(db.GetBestBlock() == uint256::ONE);
        // The flush handed ranges to the pool, whose workers all wait: its tasks are queued still.
        BOOST_REQUIRE_GT(pool->WorkQueueSize(), 0U);
    }
    // The store is gone; the queued helper tasks run now.
    release.set_value();
    pool->Stop();
    BOOST_CHECK_EQUAL(pool->WorkQueueSize(), 0U);
    BOOST_CHECK(db.GetBestBlock() == uint256::ONE);
}

//! A flush fills its batches across the shards up to the batch size, a batch not being a shard's worth of
//! changes, with several workers writing; the database then holds every change and the tier is empty.
BOOST_AUTO_TEST_CASE(flush_fills_batches_across_shards)
{
    static constexpr uint64_t BATCH{4000};
    CCoinsViewDB db{MemoryDb("store"), {}};
    CountingBase base{&db};
    CoinsStore store{&base, 256, true};
    store.SetBaseWrites(BATCH, /*concurrent=*/true);
    store.SetFlushHelpers(FlushHelpers());
    store.SetBestBlock(uint256::ONE, 1);
    std::map<COutPoint, std::optional<Coin>> expected;
    std::vector<COutPoint> first;
    for (int i{0}; i < 2000; ++i) {
        const COutPoint outpoint{Txid::FromUint256(m_rng.rand256()), 0};
        Coin coin{RandomCoin(m_rng, 1)};
        expected[outpoint] = coin;
        first.push_back(outpoint);
        store.AddCoin(outpoint, std::move(coin), false);
    }
    store.Sync(); // in the database, so that spending them owes an erase
    store.SetBestBlock(uint256{2}, 2);
    for (size_t i{0}; i < first.size(); i += 4) {
        BOOST_REQUIRE(store.SpendCoin(first[i]));
        expected[first[i]] = std::nullopt;
    }
    for (int i{0}; i < 2000; ++i) {
        const COutPoint outpoint{Txid::FromUint256(m_rng.rand256()), 0};
        Coin coin{RandomCoin(m_rng, 2)};
        expected[outpoint] = coin;
        store.AddCoin(outpoint, std::move(coin), false);
    }
    WITH_LOCK(base.m_mutex, base.m_runs = 0; base.m_changes = 0; base.m_bytes = 0);
    store.Flush();
    BOOST_CHECK_EQUAL(store.GetCacheSize(), 0U);
    BOOST_CHECK(db.GetBestBlock() == uint256{2});
    BOOST_CHECK(db.GetHeadBlocks().empty());
    for (const auto& [outpoint, coin] : expected) {
        BOOST_CHECK_MESSAGE(Same(db.PeekCoin(outpoint), coin), "mismatch at " << outpoint.ToString());
    }
    {
        LOCK(base.m_mutex);
        BOOST_CHECK_EQUAL(base.m_changes, 2500U);
        // Batches of whole shards, each within a shard's changes of the batch size, then what each worker
        // had left, and the run that closes the marker: far fewer and far larger than a run for each of the
        // 256 shards would be.
        BOOST_CHECK_GE(base.m_runs, base.m_bytes / (BATCH + 100));
        BOOST_CHECK_LE(base.m_runs, base.m_bytes / (BATCH - 1000) + 5);
    }
    // Not under the base's lock: the flush workers take it under a shard's mutex, which this takes.
    store.SanityCheck();
}

//! Records the range of txids of every run written through it, in the order the database has its keys in.
class RangeBase final : public CCoinsViewBacked
{
public:
    using CCoinsViewBacked::CCoinsViewBacked;
    Mutex m_mutex;
    std::vector<std::pair<uint256, uint256>> m_ranges GUARDED_BY(m_mutex);
    //! Runs whose changes were not in the descending order of the keys.
    size_t m_unordered GUARDED_BY(m_mutex){0};
    void WriteRun(const std::vector<CoinWrite>& writes, const uint256& new_head, const uint256& old_head, bool final) override EXCLUSIVE_LOCKS_REQUIRED(!m_mutex)
    {
        if (!writes.empty()) {
            const auto [lo, hi]{std::ranges::minmax_element(writes, {}, [](const CoinWrite& write) { return write.outpoint.hash.ToUint256(); })};
            const bool ordered{std::ranges::is_sorted(writes, [](const CoinWrite& a, const CoinWrite& b) { return b.outpoint < a.outpoint; })};
            LOCK(m_mutex);
            m_ranges.emplace_back(lo->outpoint.hash.ToUint256(), hi->outpoint.hash.ToUint256());
            if (!ordered) ++m_unordered;
        }
        CCoinsViewBacked::WriteRun(writes, new_head, old_head, final);
    }
};

//! A flush of shards that are small against a batch writes batches of whole neighbouring shards: ranges
//! of the database's keys that do not reach into one another, from several workers.
BOOST_AUTO_TEST_CASE(flush_batches_are_disjoint_key_ranges)
{
    static constexpr uint64_t BATCH{20000};
    CCoinsViewDB db{MemoryDb("store"), {}};
    RangeBase base{&db};
    CoinsStore store{&base, 64};
    store.SetBaseWrites(BATCH, /*concurrent=*/true);
    store.SetFlushHelpers(FlushHelpers());
    store.SetBestBlock(uint256::ONE, 1);
    std::map<COutPoint, Coin> expected;
    for (int i{0}; i < 4000; ++i) {
        const COutPoint outpoint{Txid::FromUint256(m_rng.rand256()), uint32_t(i % 3)};
        Coin coin{RandomCoin(m_rng, 1)};
        expected[outpoint] = coin;
        store.AddCoin(outpoint, std::move(coin), false);
    }
    store.Flush();
    BOOST_CHECK_EQUAL(store.GetCacheSize(), 0U);
    BOOST_CHECK(db.GetBestBlock() == uint256::ONE);
    BOOST_CHECK(db.GetHeadBlocks().empty());
    for (const auto& [outpoint, coin] : expected) {
        BOOST_CHECK_MESSAGE(Same(db.PeekCoin(outpoint), coin), "mismatch at " << outpoint.ToString());
    }
    LOCK(base.m_mutex);
    // A shard's share is a fifth of a batch or so: a batch is a few shards, and there are many batches.
    BOOST_CHECK_GE(base.m_ranges.size(), 8U);
    BOOST_CHECK_LE(base.m_ranges.size(), 64U);
    std::ranges::sort(base.m_ranges);
    for (size_t i{1}; i < base.m_ranges.size(); ++i) {
        BOOST_CHECK_MESSAGE(base.m_ranges[i - 1].second < base.m_ranges[i].first, "batches " << i - 1 << " and " << i << " overlap");
    }
}

//! A shard larger than a batch is written as one run in the order of its keys, the outputs of one
//! transaction among themselves included, and never as two runs that both cover its range: the database
//! cuts the run at its batch size, and holds every change.
BOOST_AUTO_TEST_CASE(flush_never_ends_a_batch_inside_a_shard)
{
    static constexpr uint64_t BATCH{4000};
    CCoinsViewDB db{MemoryDb("store"), {.batch_write_bytes = BATCH, .simulate_crash_ratio = 0}};
    RangeBase base{&db};
    CoinsStore store{&base, 4};
    store.SetBaseWrites(BATCH, /*concurrent=*/true);
    store.SetFlushHelpers(FlushHelpers());
    store.SetBestBlock(uint256::ONE, 1);
    std::map<COutPoint, Coin> expected;
    for (int i{0}; i < 500; ++i) {
        const Txid txid{Txid::FromUint256(m_rng.rand256())};
        for (uint32_t n{0}; n < 4; ++n) {
            Coin coin{RandomCoin(m_rng, 1)};
            expected[COutPoint{txid, n}] = coin;
            store.AddCoin(COutPoint{txid, n}, std::move(coin), false);
        }
    }
    store.Flush();
    BOOST_CHECK_EQUAL(store.GetCacheSize(), 0U);
    BOOST_CHECK(db.GetBestBlock() == uint256::ONE);
    BOOST_CHECK(db.GetHeadBlocks().empty());
    for (const auto& [outpoint, coin] : expected) {
        BOOST_CHECK_MESSAGE(Same(db.PeekCoin(outpoint), coin), "mismatch at " << outpoint.ToString());
    }
    LOCK(base.m_mutex);
    // A shard's share is six batches or so, and it is one run.
    BOOST_CHECK_EQUAL(base.m_ranges.size(), 4U);
    BOOST_CHECK_EQUAL(base.m_unordered, 0U);
    std::ranges::sort(base.m_ranges);
    for (size_t i{1}; i < base.m_ranges.size(); ++i) {
        BOOST_CHECK_MESSAGE(base.m_ranges[i - 1].second < base.m_ranges[i].first, "batches " << i - 1 << " and " << i << " overlap");
    }
}

//! A run that stops at a bound ends by recording the settled head in the database: the old head on disk is
//! the block the tier counts as written, never the head of the run before.
BOOST_AUTO_TEST_CASE(streamed_run_records_the_settled_head)
{
    CCoinsViewDB db{MemoryDb("store"), {}};
    CoinsStore store{&db, 4, true};
    const auto hash{[](int height) { return uint256{static_cast<uint8_t>(height)}; }};
    const auto add{[&](int height) {
        store.SetBestBlock(hash(height), height);
        store.AddCoin(COutPoint{Txid::FromUint256(uint256{static_cast<uint8_t>(100 + height)}), 0}, RandomCoin(m_rng, height), false);
    }};
    for (int height{1}; height <= 4; ++height) add(height);

    auto run{store.BeginWriteOut(/*bound_height=*/2, /*head_height=*/4, hash(4), /*final=*/false)};
    BOOST_REQUIRE(run);
    store.StreamWriteOut(*run, hash);
    store.CommitWriteOut(*run);
    BOOST_CHECK_EQUAL(run->count, 2U);
    BOOST_CHECK_EQUAL(run->settled_height, 2);
    BOOST_CHECK((db.GetHeadBlocks() == std::vector<uint256>{hash(4), hash(2)}));
    BOOST_CHECK(store.WrittenHead() == hash(2));

    // The next run starts from that head and leaves its own.
    add(5);
    run = store.BeginWriteOut(/*bound_height=*/3, /*head_height=*/5, hash(5), /*final=*/false);
    BOOST_REQUIRE(run);
    BOOST_CHECK(run->old_head == hash(2));
    store.StreamWriteOut(*run, hash);
    store.CommitWriteOut(*run);
    BOOST_CHECK((db.GetHeadBlocks() == std::vector<uint256>{hash(5), hash(3)}));
    BOOST_CHECK(store.WrittenHead() == hash(3));
    store.SanityCheck();
}

//! A streamed run whose chunk holds one change, so that every change is a chunk of its own and a chunk ends
//! inside each shard that holds entries, leaves the database, the heads and the tier as a run of one chunk leaves
//! them, once it is committed. The database at a chunk boundary is not looked at.
BOOST_AUTO_TEST_CASE(streamed_run_in_chunks_of_one_change)
{
    const auto hash{[](int height) { return uint256{static_cast<uint8_t>(height)}; }};
    static constexpr int HEIGHTS{6};
    static constexpr int PER_HEIGHT{8};
    const auto outpoint{[](int height, int n) { return COutPoint{Txid::FromUint256(uint256{static_cast<uint8_t>(height * 16 + n)}), 0}; }};
    std::vector<Coin> coins;
    for (int height{1}; height <= HEIGHTS; ++height) {
        for (int n{0}; n < PER_HEIGHT; ++n) coins.push_back(RandomCoin(m_rng, height));
    }
    const auto coin{[&](int height, int n) { return coins[(height - 1) * PER_HEIGHT + n]; }};
    const auto add{[&](CoinsStore& store, int height) {
        store.SetBestBlock(hash(height), height);
        for (int n{0}; n < PER_HEIGHT; ++n) store.AddCoin(outpoint(height, n), coin(height, n), false);
    }};

    CCoinsViewDB db_one{MemoryDb("one"), {}};
    CCoinsViewDB db_many{MemoryDb("many"), {}};
    CoinsStore store_one{&db_one, 4, true};
    CoinsStore store_many{&db_many, 4, true};
    // Returns the number of changes the run wrote.
    const auto run_to{[&](CoinsStore& store, uint64_t batch_bytes) {
        // Two heights that are in the database before the run, so that spends of their coins owe an erase.
        add(store, 1);
        add(store, 2);
        store.Sync();
        add(store, 3);
        for (int n{0}; n < PER_HEIGHT / 2; ++n) BOOST_REQUIRE(store.SpendCoin(outpoint(1, n)));
        add(store, 4);
        add(store, 5);
        // A coin the database holds and one it never held, both spent above the run's bound.
        BOOST_REQUIRE(store.SpendCoin(outpoint(2, 0)));
        BOOST_REQUIRE(store.SpendCoin(outpoint(4, 0)));
        add(store, 6);
        auto run{store.BeginWriteOut(/*bound_height=*/4, /*head_height=*/6, hash(6), /*final=*/false)};
        BOOST_REQUIRE(run);
        store.StreamWriteOut(*run, hash, batch_bytes);
        store.CommitWriteOut(*run);
        store.SanityCheck();
        return run->count;
    }};
    const size_t count_one{run_to(store_one, DEFAULT_DB_CACHE_BATCH)};
    const size_t count_many{run_to(store_many, /*batch_bytes=*/1)};
    BOOST_CHECK_GT(count_one, 1U);
    BOOST_CHECK_EQUAL(count_one, count_many);

    const auto check_equal{[&] {
        BOOST_CHECK(db_one.GetHeadBlocks() == db_many.GetHeadBlocks());
        BOOST_CHECK(db_one.GetBestBlock() == db_many.GetBestBlock());
        BOOST_CHECK(store_one.WrittenHead() == store_many.WrittenHead());
        for (int height{1}; height <= HEIGHTS; ++height) {
            for (int n{0}; n < PER_HEIGHT; ++n) {
                const auto one{db_one.GetCoin(outpoint(height, n))};
                const auto many{db_many.GetCoin(outpoint(height, n))};
                BOOST_REQUIRE_EQUAL(one.has_value(), many.has_value());
                if (one) BOOST_CHECK(one->out == many->out && one->nHeight == many->nHeight && one->fCoinBase == many->fCoinBase);
                BOOST_CHECK_EQUAL(store_one.HaveCoin(outpoint(height, n)), store_many.HaveCoin(outpoint(height, n)));
            }
        }
    }};
    check_equal();
    // The run wrote something of its heights and nothing above its settled head.
    BOOST_CHECK(db_many.GetCoin(outpoint(3, 1)).has_value());
    BOOST_CHECK(!db_many.GetCoin(outpoint(6, 1)).has_value());
    // What the bound decides: the coins of the first height spent below it are erased and the others of
    // that height are there, and a coin the database holds that is spent above the bound is there still.
    // A coin made and spent before the database held it is never written.
    for (int n{0}; n < PER_HEIGHT; ++n) BOOST_CHECK_EQUAL(db_many.GetCoin(outpoint(1, n)).has_value(), n >= PER_HEIGHT / 2);
    BOOST_CHECK(db_many.GetCoin(outpoint(2, 0)).has_value());
    BOOST_CHECK(!db_many.GetCoin(outpoint(4, 0)).has_value());

    // And the rest, written by each store the way it writes: the same again.
    store_one.Sync();
    store_many.Sync();
    check_equal();
    BOOST_CHECK(db_many.GetBestBlock() == hash(6));
    BOOST_CHECK(db_many.GetCoin(outpoint(6, 1)).has_value());
    BOOST_CHECK(!db_many.GetCoin(outpoint(2, 0)).has_value());
    BOOST_CHECK(!db_many.GetCoin(outpoint(4, 0)).has_value());
    store_many.SanityCheck();
}

//! A base whose n-th write fails, counted from one; no write fails while n is zero.
class FailingAtBase final : public CCoinsViewBacked
{
public:
    using CCoinsViewBacked::CCoinsViewBacked;
    int m_calls{0};
    int m_fail_at{0};
    void WriteRun(const std::vector<CoinWrite>& writes, const uint256& new_head, const uint256& old_head, bool final) override
    {
        if (++m_calls == m_fail_at) throw std::runtime_error{"the write failed"};
        CCoinsViewBacked::WriteRun(writes, new_head, old_head, final);
    }
};

//! A streamed run that ends at any of its writes, as a node that stops there leaves it, leaves a database
//! the replay completes: the marker names the run's head and the head before the run, and applying the
//! blocks between the two again, as Chainstate::ReplayBlocks() does through a cache on the database, gives
//! the database of a node that never stopped. Every write of a run of one change per chunk is tried.
BOOST_AUTO_TEST_CASE(streamed_run_cut_at_any_write_is_completed_by_a_replay)
{
    const auto hash{[](int height) { return uint256{static_cast<uint8_t>(height)}; }};
    static constexpr int HEIGHTS{6};
    static constexpr int PER_HEIGHT{8};
    static constexpr int WRITTEN{2}; //!< the height the database is at before the run
    const auto outpoint{[](int height, int n) { return COutPoint{Txid::FromUint256(uint256{static_cast<uint8_t>(height * 16 + n)}), 0}; }};
    std::vector<Coin> coins;
    for (int height{1}; height <= HEIGHTS; ++height) {
        for (int n{0}; n < PER_HEIGHT; ++n) coins.push_back(RandomCoin(m_rng, height));
    }
    const auto coin{[&](int height, int n) { return coins[(height - 1) * PER_HEIGHT + n]; }};
    // The changes of a block: its coins, and at two heights spends of coins the database holds and of one
    // it never held.
    const auto block{[&](auto& view, int height, bool overwrite) {
        for (int n{0}; n < PER_HEIGHT; ++n) view.AddCoin(outpoint(height, n), coin(height, n), overwrite);
        if (height == 3) {
            for (int n{0}; n < PER_HEIGHT / 2; ++n) view.SpendCoin(outpoint(1, n));
        } else if (height == 5) {
            view.SpendCoin(outpoint(2, 0));
            view.SpendCoin(outpoint(4, 0));
        }
    }};

    // The database of a node that never stopped.
    CCoinsViewDB db_whole{MemoryDb("whole"), {}};
    {
        CCoinsViewCache cache{&db_whole};
        for (int height{1}; height <= HEIGHTS; ++height) block(cache, height, /*overwrite=*/false);
        cache.SetBestBlock(hash(HEIGHTS));
        cache.Flush();
    }

    // Returns the number of writes the run made, all of them if none failed.
    const auto run_cut_at{[&](int fail_at) {
        CCoinsViewDB db{MemoryDb("cut"), {}};
        FailingAtBase base{&db};
        CoinsStore store{&base, 4, true};
        for (int height{1}; height <= HEIGHTS; ++height) {
            store.SetBestBlock(hash(height), height);
            block(store, height, /*overwrite=*/false);
            if (height == WRITTEN) store.Sync();
        }
        auto run{store.BeginWriteOut(/*bound_height=*/4, /*head_height=*/HEIGHTS, hash(HEIGHTS), /*final=*/false)};
        BOOST_REQUIRE(run);
        base.m_calls = 0;
        base.m_fail_at = fail_at;
        if (fail_at == 0) {
            store.StreamWriteOut(*run, hash, /*batch_bytes=*/1);
            store.CommitWriteOut(*run);
            return base.m_calls;
        }
        BOOST_CHECK_THROW(store.StreamWriteOut(*run, hash, /*batch_bytes=*/1), std::runtime_error);
        // The run is the caller's to abandon, as the write-out's thread does.
        store.AbandonWriteOut();
        BOOST_CHECK(!store.RunInFlight());
        BOOST_CHECK(store.FlushFailed());
        if (fail_at == 1) {
            // Nothing of the run is in the database.
            BOOST_CHECK(db.GetHeadBlocks().empty());
            BOOST_CHECK(db.GetBestBlock() == hash(WRITTEN));
        } else {
            BOOST_CHECK((db.GetHeadBlocks() == std::vector<uint256>{hash(HEIGHTS), hash(WRITTEN)}));
            BOOST_CHECK(db.GetBestBlock().IsNull());
        }

        // The replay: the blocks above the old head again, whatever the database holds of them.
        {
            CCoinsViewCache replay{&db};
            for (int height{WRITTEN + 1}; height <= HEIGHTS; ++height) block(replay, height, /*overwrite=*/true);
            replay.SetBestBlock(hash(HEIGHTS));
            replay.Flush();
        }
        BOOST_CHECK(db.GetHeadBlocks().empty());
        BOOST_CHECK(db.GetBestBlock() == hash(HEIGHTS));
        for (int height{1}; height <= HEIGHTS; ++height) {
            for (int n{0}; n < PER_HEIGHT; ++n) {
                BOOST_CHECK_MESSAGE(Same(db.PeekCoin(outpoint(height, n)), db_whole.PeekCoin(outpoint(height, n))),
                                    "cut at write " << fail_at << ": the database differs at coin " << n << " of height " << height);
            }
        }
        return base.m_calls;
    }};

    const int writes{run_cut_at(0)};
    // A chunk for each change at or below the run's bound, and the write that closes the marker.
    BOOST_REQUIRE_GT(writes, 2 * PER_HEIGHT);
    for (int fail_at{1}; fail_at <= writes; ++fail_at) BOOST_CHECK_EQUAL(run_cut_at(fail_at), fail_at);
}

//! As streamed_run_cut_at_any_write_is_completed_by_a_replay, for a run whose marker's two heads lie on different
//! branches. The database holds the old branch's tip, above the fork, when a reorganisation takes the old blocks
//! back below it through a child cache and connects a longer branch: the next run is final whatever its bound
//! (BeginWriteOut()), and its marker names the new branch's tip and the old branch's. A node that stops at any of
//! the run's writes starts as Chainstate::ReplayBlocks() does with such a marker: it takes the blocks from the old
//! head down to the fork back with their undo data and applies the new branch's blocks up to the new head, which
//! gives the database of a node that never stopped. A node that stops at the run's first write has a database at
//! the old branch's tip, and the reorganisation made again does the same.
BOOST_AUTO_TEST_CASE(streamed_run_cut_across_a_fork_is_completed_by_a_replay)
{
    const auto old_hash{[](int height) { return uint256{static_cast<uint8_t>(height)}; }};
    const auto new_hash{[](int height) { return uint256{static_cast<uint8_t>(100 + height)}; }};
    static constexpr int FORK{2};
    static constexpr int OLD_TIP{4};
    static constexpr int NEW_TIP{5};
    static constexpr int PER_HEIGHT{4};
    const auto outpoint{[](int height, int n, bool new_branch) {
        return COutPoint{Txid::FromUint256(uint256{static_cast<uint8_t>((new_branch ? 0x80 : 0) + height * 16 + n)}), 0};
    }};
    const auto coin{[](int height, int n) { return Coin{CTxOut{CAmount{height * 10 + n + 1}, CScript() << OP_TRUE}, height, false}; }};
    // The old branch's blocks: their coins, and in 3 spends of coins of 1. The undo of one of them. The new
    // branch's blocks: their coins, and in 4 spends of a coin of 1 that the undo of 3 restores and of a coin of 2.
    const auto old_block{[&](auto& view, int height, bool overwrite) {
        for (int n{0}; n < PER_HEIGHT; ++n) view.AddCoin(outpoint(height, n, false), coin(height, n), overwrite);
        if (height == 3) {
            for (int n{0}; n < PER_HEIGHT / 2; ++n) view.SpendCoin(outpoint(1, n, false));
        }
    }};
    const auto old_undo{[&](auto& view, int height, bool overwrite) {
        for (int n{0}; n < PER_HEIGHT; ++n) view.SpendCoin(outpoint(height, n, false));
        if (height == 3) {
            for (int n{0}; n < PER_HEIGHT / 2; ++n) view.AddCoin(outpoint(1, n, false), coin(1, n), overwrite);
        }
    }};
    const auto new_block{[&](auto& view, int height, bool overwrite) {
        for (int n{0}; n < PER_HEIGHT; ++n) view.AddCoin(outpoint(height, n, true), coin(height, n + PER_HEIGHT), overwrite);
        if (height == 4) {
            view.SpendCoin(outpoint(1, 0, false));
            view.SpendCoin(outpoint(2, 0, false));
        }
    }};
    // From the old branch's tip to the new one's.
    const auto reorganise{[&](auto& view, bool overwrite) {
        for (int height{OLD_TIP}; height > FORK; --height) old_undo(view, height, overwrite);
        for (int height{FORK + 1}; height <= NEW_TIP; ++height) new_block(view, height, overwrite);
    }};

    // The database of a node that never stopped.
    CCoinsViewDB db_whole{MemoryDb("whole"), {}};
    {
        CCoinsViewCache cache{&db_whole};
        for (int height{1}; height <= OLD_TIP; ++height) old_block(cache, height, /*overwrite=*/false);
        reorganise(cache, /*overwrite=*/false);
        cache.SetBestBlock(new_hash(NEW_TIP));
        cache.Flush();
    }

    // Returns the number of writes the run made, all of them if none failed.
    const auto run_cut_at{[&](int fail_at) {
        CCoinsViewDB db{MemoryDb("cut"), {}};
        FailingAtBase base{&db};
        CoinsStore store{&base, 4, true};
        for (int height{1}; height <= OLD_TIP; ++height) {
            store.SetBestBlock(old_hash(height), height);
            old_block(store, height, /*overwrite=*/false);
            // The database holds 1, whose coins 3 spends, and then the old branch's tip.
            if (height == 1 || height == OLD_TIP) store.Sync();
        }
        // Blocks 4 and 3 disconnected towards the fork, as a disconnect does: their inverse changes are tagged with
        // the fork height.
        store.SetChangeHeight(FORK);
        {
            CCoinsViewCache child{&store};
            for (int height{OLD_TIP}; height > FORK; --height) old_undo(child, height, /*overwrite=*/false);
            child.SetBestBlock(old_hash(FORK));
            child.Flush();
        }
        store.SetTierHeight(FORK);
        for (int height{FORK + 1}; height <= NEW_TIP; ++height) {
            store.SetBestBlock(new_hash(height), height);
            new_block(store, height, /*overwrite=*/false);
        }
        auto run{store.BeginWriteOut(/*bound_height=*/FORK + 1, /*head_height=*/NEW_TIP, new_hash(NEW_TIP), /*final=*/false)};
        BOOST_REQUIRE(run);
        BOOST_CHECK(run->final);
        BOOST_CHECK(run->old_head == old_hash(OLD_TIP));
        base.m_calls = 0;
        base.m_fail_at = fail_at;
        if (fail_at == 0) {
            store.StreamWriteOut(*run, new_hash, /*batch_bytes=*/1);
            store.CommitWriteOut(*run);
        } else {
            BOOST_CHECK_THROW(store.StreamWriteOut(*run, new_hash, /*batch_bytes=*/1), std::runtime_error);
            store.AbandonWriteOut();
            if (fail_at == 1) {
                BOOST_CHECK(db.GetHeadBlocks().empty());
                BOOST_CHECK(db.GetBestBlock() == old_hash(OLD_TIP));
            } else {
                BOOST_CHECK((db.GetHeadBlocks() == std::vector<uint256>{new_hash(NEW_TIP), old_hash(OLD_TIP)}));
                BOOST_CHECK(db.GetBestBlock().IsNull());
            }
            // The old branch back to the fork and the new one up to its tip, whatever the database holds of either.
            CCoinsViewCache replay{&db};
            reorganise(replay, /*overwrite=*/true);
            replay.SetBestBlock(new_hash(NEW_TIP));
            replay.Flush();
        }
        BOOST_CHECK(db.GetHeadBlocks().empty());
        BOOST_CHECK(db.GetBestBlock() == new_hash(NEW_TIP));
        for (int height{1}; height <= NEW_TIP; ++height) {
            for (int n{0}; n < PER_HEIGHT; ++n) {
                for (const bool new_branch : {false, true}) {
                    BOOST_CHECK_MESSAGE(Same(db.PeekCoin(outpoint(height, n, new_branch)), db_whole.PeekCoin(outpoint(height, n, new_branch))),
                                        "cut at write " << fail_at << ": the database differs at coin " << n << " of height " << height
                                                        << (new_branch ? " of the new branch" : " of the old branch"));
                }
            }
        }
        return base.m_calls;
    }};

    const int writes{run_cut_at(0)};
    // A chunk for each change of the run, which carries both branches' changes, and the write that closes the marker.
    BOOST_REQUIRE_GT(writes, 2 * PER_HEIGHT);
    for (int fail_at{1}; fail_at <= writes; ++fail_at) BOOST_CHECK_EQUAL(run_cut_at(fail_at), fail_at);
}

//! A base whose n-th write fails, as FailingAtBase, and that keeps the order in which a run's changes reach it.
class CutRecordingBase final : public CCoinsViewBacked
{
public:
    using CCoinsViewBacked::CCoinsViewBacked;
    int m_calls{0};
    int m_fail_at{0};
    std::vector<COutPoint> m_order;
    void WriteRun(const std::vector<CoinWrite>& writes, const uint256& new_head, const uint256& old_head, bool final) override
    {
        if (++m_calls == m_fail_at) throw std::runtime_error{"the write failed"};
        for (const CoinWrite& write : writes) m_order.push_back(write.outpoint);
        CCoinsViewBacked::WriteRun(writes, new_head, old_head, final);
    }
};

//! A coin created and spent above the written head cancels in the tier, and a run that settles the database
//! between the two leaves the database without it, although the state there has it. A reorganisation that
//! disconnects the spend restores it (KeepWritten()), and the next run's owner writes it before the run's marker
//! (CCoinsViewDB::WriteCoins()): from the marker on, the database names a head of the new branch, from which a
//! replay never makes the coin again. A crash before that write, after it, or at any of the run's writes leaves a
//! database that the replay and the reorganisation made again with the undo data complete to the database of a
//! node that never stopped. The restored coin is not fresh any more: spent before the run, it is written to the
//! database as an erase, where a fresh one would be dropped in the tier while the database keeps the coin.
BOOST_AUTO_TEST_CASE(restored_coin_written_before_the_run_survives_a_cut)
{
    const auto old_hash{[](int height) { return uint256{static_cast<uint8_t>(height - 200)}; }};
    const auto new_hash{[](int height) { return uint256{static_cast<uint8_t>(height - 100)}; }};
    const auto block_at{[&](int height) { return height <= 303 ? old_hash(height) : new_hash(height); }};
    std::map<uint256, std::pair<bool, int>> where; // a block's hash: on the new branch, and its height
    for (int height{300}; height <= 305; ++height) where[old_hash(height)] = {false, height};
    for (int height{304}; height <= 305; ++height) where[new_hash(height)] = {true, height};
    const auto coin{[](int height, int n) { return Coin{CTxOut{CAmount{height * 10 + n}, CScript() << OP_TRUE}, height, false}; }};
    // The coins of a block, each the output of a transaction of its own, and c.
    static constexpr int PER_BLOCK{4};
    const auto own{[](int height, int n, bool new_branch) {
        return COutPoint{Txid::FromUint256(uint256{static_cast<uint8_t>((new_branch ? 0x80 : 0) + (height - 300) * 8 + n)}), 0};
    }};
    const COutPoint c{Txid::FromUint256(uint256{0xf0}), 0};
    const Coin c_coin{coin(302, 9)};
    std::vector<COutPoint> universe{c};
    for (int height{301}; height <= 305; ++height) {
        for (int n{0}; n < PER_BLOCK; ++n) {
            universe.push_back(own(height, n, false));
            universe.push_back(own(height, n, true));
        }
    }
    // The old branch 301 to 305: c is created in 302 and spent in 304, which spends two coins of 301 as well; all
    // three cancel in the tier and come back with the undo of 304, the 301 coins on either side of c in the
    // order of the block's inputs. Its undo, and the new branch 304 and 305, whose 305 may spend c.
    const auto old_block{[&](auto& view, int height, bool overwrite) {
        for (int n{0}; n < PER_BLOCK; ++n) view.AddCoin(own(height, n, false), coin(height, n), overwrite);
        if (height == 302) view.AddCoin(c, Coin{c_coin}, overwrite);
        if (height == 304) {
            view.SpendCoin(own(301, 0, false));
            view.SpendCoin(c);
            view.SpendCoin(own(301, 1, false));
        }
    }};
    const auto old_undo{[&](auto& view, int height, bool overwrite) {
        for (int n{0}; n < PER_BLOCK; ++n) view.SpendCoin(own(height, n, false));
        if (height == 304) {
            view.AddCoin(own(301, 0, false), coin(301, 0), overwrite);
            view.AddCoin(c, Coin{c_coin}, overwrite);
            view.AddCoin(own(301, 1, false), coin(301, 1), overwrite);
        }
    }};
    const auto new_block{[&](auto& view, int height, bool overwrite, bool spend_c) {
        for (int n{0}; n < PER_BLOCK; ++n) view.AddCoin(own(height, n, true), coin(height, n + PER_BLOCK), overwrite);
        if (height == 305 && spend_c) view.SpendCoin(c);
    }};
    // The database of a node that never stopped, at 305 of the new branch.
    const auto reference{[&](CCoinsViewDB& db, bool spend_c) {
        CCoinsViewCache cache{&db};
        for (int height{301}; height <= 305; ++height) old_block(cache, height, /*overwrite=*/false);
        old_undo(cache, 305, /*overwrite=*/false);
        old_undo(cache, 304, /*overwrite=*/false);
        for (int height{304}; height <= 305; ++height) new_block(cache, height, /*overwrite=*/false, spend_c);
        cache.SetBestBlock(new_hash(305));
        cache.Flush();
    }};
    CCoinsViewDB db_whole{MemoryDb("whole"), {}};
    reference(db_whole, /*spend_c=*/false);
    BOOST_REQUIRE(db_whole.HaveCoin(c));
    // The store up to the new branch's 305, before the run that follows the reorganisation.
    const auto prepare{[&](CoinsStore& store, const CCoinsViewDB& db, bool spend_c) {
        store.SetBestBlock(old_hash(300), 300);
        store.Sync(); // the database holds the state at 300
        for (int height{301}; height <= 305; ++height) {
            store.SetBestBlock(old_hash(height), height);
            old_block(store, height, /*overwrite=*/false);
        }
        BOOST_CHECK(!store.HaveCoinInCache(c)); // created and spent in the tier: cancelled
        // A run settles the database at 303, between c's creation and its spend.
        WriteOut(store, /*bound_height=*/303, /*head_height=*/305, old_hash(305), old_hash);
        BOOST_REQUIRE(store.WrittenHead() == old_hash(303));
        BOOST_CHECK(!db.HaveCoin(c));
        // Disconnect 305 and 304 towards the fork at 303. DisconnectTip() hands the coins 304 spent, which
        // the database may never have held, to the next run.
        store.SetChangeHeight(303);
        old_undo(store, 305, /*overwrite=*/false);
        old_undo(store, 304, /*overwrite=*/false);
        store.SetBestBlock(old_hash(303));
        store.SetTierHeight(303);
        store.KeepWritten({CoinWrite{own(301, 0, false), coin(301, 0)}, CoinWrite{c, c_coin}, CoinWrite{own(301, 1, false), coin(301, 1)}});
        for (int height{304}; height <= 305; ++height) {
            store.SetBestBlock(new_hash(height), height);
            new_block(store, height, /*overwrite=*/false, spend_c);
        }
    }};
    // A start after a crash: Chainstate::ReplayBlocks() applies again the blocks above the old head on the new
    // head's branch (the old head is below the fork or on that branch here), and ActivateBestChain() then makes
    // the reorganisation again from the undo data where the database is at the old branch's tip.
    const auto recover{[&](CCoinsViewDB& db) {
        const std::vector<uint256> heads{db.GetHeadBlocks()};
        uint256 at{db.GetBestBlock()};
        CCoinsViewCache view{&db};
        if (!heads.empty()) {
            BOOST_REQUIRE_EQUAL(heads.size(), 2U);
            const auto [on_new, new_height]{where.at(heads[0])};
            const auto [old_on_new, old_height]{where.at(heads[1])};
            BOOST_REQUIRE(old_height <= 303 || old_on_new == on_new);
            for (int height{old_height + 1}; height <= new_height; ++height) {
                if (on_new) {
                    new_block(view, height, /*overwrite=*/true, /*spend_c=*/false);
                } else {
                    old_block(view, height, /*overwrite=*/true);
                }
            }
            // ReplayBlocks() writes the replay at the new head, which closes the marker, before anything else.
            view.SetBestBlock(heads[0]);
            view.Flush();
            at = heads[0];
        }
        if (at == old_hash(305)) {
            old_undo(view, 305, /*overwrite=*/true);
            old_undo(view, 304, /*overwrite=*/true);
            for (int height{304}; height <= 305; ++height) new_block(view, height, /*overwrite=*/true, /*spend_c=*/false);
        } else {
            BOOST_REQUIRE(at == new_hash(305));
        }
        view.SetBestBlock(new_hash(305));
        view.Flush();
    }};
    const auto check_complete{[&](const CCoinsViewDB& db, const CCoinsViewDB& whole, const std::string& what) {
        BOOST_CHECK(db.GetHeadBlocks().empty());
        BOOST_CHECK(db.GetBestBlock() == new_hash(305));
        for (const COutPoint& outpoint : universe) {
            BOOST_CHECK_MESSAGE(Same(db.PeekCoin(outpoint), whole.PeekCoin(outpoint)), what << ": the database differs at " << outpoint.hash.GetHex() << ":" << outpoint.n);
        }
    }};

    // Returns the number of writes the run made, all of them if none failed. With `prepared` false the node stops
    // before the restored coin is written.
    const auto run_cut_at{[&](int fail_at, bool prepared) {
        CCoinsViewDB db{MemoryDb("cut"), {}};
        CutRecordingBase base{&db};
        CoinsStore store{&base, 4, true};
        prepare(store, db, /*spend_c=*/false);
        auto run{store.BeginWriteOut(/*bound_height=*/304, /*head_height=*/305, new_hash(305), /*final=*/false)};
        BOOST_REQUIRE(run);
        BOOST_REQUIRE_EQUAL(run->restored.size(), 3U); // the run took the restored coins
        BOOST_CHECK(store.Restored().empty());
        if (!prepared) {
            store.AbandonWriteOut();
            recover(db);
            check_complete(db, db_whole, "stopped before the restored coin was written");
            return 0;
        }
        db.WriteCoins(run->restored);
        BOOST_CHECK(db.HaveCoin(c));
        base.m_calls = 0;
        base.m_fail_at = fail_at;
        base.m_order.clear();
        if (fail_at == 0) {
            store.StreamWriteOut(*run, block_at, /*batch_bytes=*/1);
            store.CommitWriteOut(*run);
            // The run carries the restored coins too, and some of them after its first write: a cut there would
            // leave the database under the new head without them but for the write before the marker.
            BOOST_CHECK(std::ranges::any_of(run->restored, [&](const CoinWrite& kept) {
                const auto at{std::ranges::find(base.m_order, kept.outpoint)};
                return at != base.m_order.end() && at != base.m_order.begin();
            }));
        } else {
            BOOST_CHECK_THROW(store.StreamWriteOut(*run, block_at, /*batch_bytes=*/1), std::runtime_error);
            store.AbandonWriteOut();
        }
        recover(db);
        check_complete(db, db_whole, "cut at write " + std::to_string(fail_at));
        return base.m_calls;
    }};
    run_cut_at(0, /*prepared=*/false);
    const int writes{run_cut_at(0, /*prepared=*/true)};
    BOOST_REQUIRE_GT(writes, 2);
    for (int fail_at{1}; fail_at <= writes; ++fail_at) BOOST_CHECK_EQUAL(run_cut_at(fail_at, /*prepared=*/true), fail_at);

    // c spent in the new branch's 305, before the run: the spend of a coin the database may hold is an erase.
    CCoinsViewDB db_spent{MemoryDb("spent whole"), {}};
    reference(db_spent, /*spend_c=*/true);
    BOOST_REQUIRE(!db_spent.HaveCoin(c));
    CCoinsViewDB db{MemoryDb("spent"), {}};
    {
        CoinsStore store{&db, 4, true};
        prepare(store, db, /*spend_c=*/true);
        auto run{store.BeginWriteOut(/*bound_height=*/304, /*head_height=*/305, new_hash(305), /*final=*/false)};
        BOOST_REQUIRE(run);
        db.WriteCoins(run->restored);
        store.StreamWriteOut(*run, block_at);
        store.CommitWriteOut(*run);
        store.Sync();
        store.SanityCheck();
    }
    BOOST_CHECK(!db.HaveCoin(c));
    check_complete(db, db_spent, "c spent before the run");
}

//! A block that creates an outpoint over an erase the database is still owed keeps that erase on the books:
//! the entry stays filed at the height of the spend, under the coin, and the coin is written over the one
//! the database has.
BOOST_AUTO_TEST_CASE(owed_erase_survives_a_create)
{
    CCoinsViewDB db{MemoryDb("store"), {}};
    CoinsStore store{&db, 4, true};
    store.SetBestBlock(uint256{1}, 1);
    const COutPoint x{Txid::FromUint256(uint256{10}), 0};
    store.AddCoin(x, RandomCoin(m_rng, 1), false);
    store.Sync(); // x is in the database
    store.SetChangeHeight(2);
    BOOST_CHECK(store.SpendCoin(x)); // spent, dirty: the erase is owed
    BOOST_CHECK_EQUAL(*OldestDirty(store), 2);

    // Block 3 makes x again (a duplicate, as BIP30 allowed once).
    BOOST_CHECK_EQUAL(Create(store, 3, x, RandomCoin(m_rng, 3), false), 0U);
    store.SetBestBlock(uint256{3}, 3);
    BOOST_CHECK(store.HaveCoin(x));
    BOOST_CHECK_EQUAL(store.GetDirtyCount(), 1U);
    // The erase still owed keeps its tag, the spend at 2, under the coin made at 3.
    BOOST_CHECK_EQUAL(*OldestDirty(store), 2);
    store.SanityCheck();
    store.Sync();
    const auto coin{db.PeekCoin(x)};
    BOOST_REQUIRE(coin.has_value());
    BOOST_CHECK_EQUAL(coin->nHeight, 3U);
    BOOST_CHECK_EQUAL(store.GetDirtyCount(), 0U);
    store.SanityCheck();
}

//! The tier's byte count stays exact for coins whose scripts are on the heap, through the two paths that
//! empty an entry and keep it: a spend with no taker for the coin, and the spend of a coin made over an
//! erase still owed. (An emptied script gives its memory back, CScript::clear().)
BOOST_AUTO_TEST_CASE(emptied_entries_count_no_script_bytes)
{
    CCoinsViewDB db{MemoryDb("store"), {}};
    CoinsStore store{&db, 4, true};
    store.SetBestBlock(uint256{1}, 1);
    const auto big{[](int height) { return Coin{CTxOut{CAmount{1000}, CScript{} << std::vector<unsigned char>(100, 0x42)}, height, /*fCoinBaseIn=*/false}; }};
    const COutPoint x{Txid::FromUint256(uint256{10}), 0};
    store.AddCoin(x, big(1), false);
    store.Sync(); // x is in the database
    store.SetChangeHeight(2);
    BOOST_CHECK(store.SpendCoin(x)); // no taker: the entry stays, spent and dirty
    store.SanityCheck();
    Create(store, 3, x, big(3), false);
    store.SanityCheck();
    BOOST_CHECK_EQUAL(Spend(store, 4, x), 0U); // the erase is owed still: the entry is emptied again
    BOOST_CHECK(!store.HaveCoin(x));
    BOOST_CHECK_EQUAL(store.GetDirtyCount(), 1U);
    store.SanityCheck();
    store.Sync();
    BOOST_CHECK(!db.HaveCoin(x));
    store.SanityCheck();
}

//! Blocks the database holds are disconnected and connected again, past the written head. The erase the
//! disconnect left is tagged with the fork height and the coin is made again above it: a run that stopped
//! between the two would write the erase under a marker whose replay starts at the written head, where the
//! coin is taken to be there. Such a run writes everything.
BOOST_AUTO_TEST_CASE(run_after_a_disconnect_below_the_written_head_is_final)
{
    CCoinsViewDB db{MemoryDb("store"), {}};
    RunBase base{&db};
    CoinsStore store{&base, 4, true};
    store.SetBestBlock(uint256{1}, 1);
    const COutPoint x{Txid::FromUint256(uint256{10}), 0};
    store.SetBestBlock(uint256{2}, 2);
    store.AddCoin(x, RandomCoin(m_rng, 2), false);
    store.SetBestBlock(uint256{3}, 3);
    store.Sync(); // the database is at block 3, with x
    BOOST_CHECK(db.HaveCoin(x));

    // Blocks 3 and 2 are disconnected: the erase of x is a change of the fork height.
    store.SetChangeHeight(1);
    BOOST_CHECK(store.SpendCoin(x));
    store.SetBestBlock(uint256{1});
    store.SetTierHeight(1);
    // The same blocks are connected again, and two more.
    Create(store, 2, x, RandomCoin(m_rng, 2), false);
    for (uint8_t height{2}; height <= 5; ++height) store.SetBestBlock(uint256{height}, height);

    // A run asked to stop at block 1 writes everything: the database keeps x.
    auto run{WriteOut(store, /*bound_height=*/1, /*head_height=*/5, uint256{5})};
    BOOST_CHECK(run->final);
    for (const auto& write : base.m_writes) BOOST_CHECK(write.outpoint != x || write.coin.has_value());
    BOOST_CHECK(db.HaveCoin(x));
    BOOST_CHECK_EQUAL(run->settled_height, 5);
    BOOST_CHECK_EQUAL(store.GetDirtyCount(), 0U);
    store.SanityCheck();

    // With that run begun, a later one stops at its bound again.
    store.SetBestBlock(uint256{6}, 6);
    store.AddCoin(COutPoint{Txid::FromUint256(uint256{11}), 0}, RandomCoin(m_rng, 6), false);
    store.SetBestBlock(uint256{7}, 7);
    store.AddCoin(COutPoint{Txid::FromUint256(uint256{12}), 0}, RandomCoin(m_rng, 7), false);
    store.SetBestBlock(uint256{8}, 8);
    run = WriteOut(store, /*bound_height=*/6, /*head_height=*/8, uint256{8});
    BOOST_CHECK(!run->final);
    BOOST_CHECK_EQUAL(run->count, 1U);
    BOOST_CHECK_EQUAL(run->settled_height, 6);
    store.SanityCheck();
}

//! Only a coinbase may overwrite an unspent coin (the chain's two BIP30 duplicates): committed through the
//! view, the second coinbase stands and the first is overwritten for good.
BOOST_AUTO_TEST_CASE(block_apply_view_coinbase_over_a_confirmed_coin)
{
    CCoinsViewDB db{MemoryDb("store"), {}};
    CoinsStore store{&db, 4, true};
    store.SetBestBlock(uint256::ONE, 1);
    const COutPoint cb{Txid::FromUint256(uint256{11}), 0};
    const Coin cb_coin{CTxOut{CAmount{5000}, CScript{} << OP_TRUE}, 1, /*fCoinBaseIn=*/true};
    store.AddCoin(cb, Coin{cb_coin}, false); // confirmed, dirty: the database does not have it yet
    util::NotNull pool{std::make_shared<ThreadPool>("test")}; // no workers: lookups are served inline
    BlockApplyView view{store, pool, /*deterministic=*/true};
    const CBlock block;
    const CBlockUndo undo;

    view.SetHeight(2);
    view.AddCoin(cb, Coin{CTxOut{CAmount{6000}, CScript{} << OP_TRUE}, 2, /*fCoinBaseIn=*/true}, /*possible_overwrite=*/true);
    view.SetBestBlock(uint256{2});
    view.CommitAlone(block, undo);
    view.ConfirmCommitted(2);
    const auto second{store.PeekCoin(cb)};
    BOOST_REQUIRE(second);
    BOOST_CHECK_EQUAL(second->out.nValue, CAmount{6000});
    BOOST_CHECK_EQUAL(second->nHeight, 2U);
    store.SanityCheck();
}

//! The view commits a valid block's changes to the store as the changes of its height: readers see nothing
//! before, everything after, and a block whose view is given back leaves no trace.
BOOST_AUTO_TEST_CASE(block_apply_view_commits_a_block)
{
    CCoinsViewDB db{MemoryDb("store"), {}};
    CoinsStore store{&db, 4, true};
    store.SetBestBlock(uint256::ONE, 1);
    const COutPoint old_coin{Txid::FromUint256(uint256{10}), 0};
    store.AddCoin(old_coin, RandomCoin(m_rng, 1), false);
    util::NotNull pool{std::make_shared<ThreadPool>("test")}; // no workers: lookups are served inline
    BlockApplyView view{store, pool, /*deterministic=*/true};
    const CBlock block;
    const CBlockUndo undo;

    // A block that spends old_coin, creates a and b, and spends a within itself.
    const COutPoint a{Txid::FromUint256(uint256{20}), 0};
    const COutPoint b{Txid::FromUint256(uint256{21}), 0};
    BOOST_CHECK(!view.InUse());
    view.SetHeight(2);
    BOOST_CHECK(view.InUse());
    BOOST_CHECK(view.HaveCoin(old_coin));
    Coin spent;
    BOOST_CHECK(view.SpendCoin(old_coin, &spent));
    BOOST_CHECK(!spent.IsSpent());
    view.AddCoin(a, RandomCoin(m_rng, 2), false);
    view.AddCoin(b, RandomCoin(m_rng, 2), false);
    BOOST_CHECK(view.HaveCoin(a));
    BOOST_CHECK(view.SpendCoin(a));
    BOOST_CHECK(!view.HaveCoin(a));
    view.SetBestBlock(uint256{2});
    // Readers see nothing of it yet.
    BOOST_CHECK(store.HaveCoin(old_coin));
    BOOST_CHECK(!store.HaveCoin(b));
    view.CommitAlone(block, undo);
    BOOST_CHECK(!store.HaveCoin(old_coin));
    BOOST_CHECK(store.HaveCoin(b));
    BOOST_CHECK(!store.HaveCoinInCache(a)); // created and spent inside the block: no trace
    view.ConfirmCommitted(2);
    BOOST_CHECK(!view.InUse());
    BOOST_CHECK(store.GetBestBlock() == uint256{2});
    store.SanityCheck();

    // A block whose view is given back leaves the store as it was.
    view.SetHeight(3);
    const COutPoint c{Txid::FromUint256(uint256{22}), 0};
    view.AddCoin(c, RandomCoin(m_rng, 3), false);
    BOOST_CHECK(view.SpendCoin(b));
    view.Release();
    BOOST_CHECK(!view.InUse());
    BOOST_CHECK(store.HaveCoin(b));
    BOOST_CHECK(!store.HaveCoinInCache(c));
    store.SanityCheck();

    // As does a view destroyed with a block.
    {
        BlockApplyView other{store, pool, true};
        other.SetHeight(3);
        other.AddCoin(c, RandomCoin(m_rng, 3), false);
        BOOST_CHECK(other.SpendCoin(b));
    }
    BOOST_CHECK(store.HaveCoin(b));
    BOOST_CHECK(!store.HaveCoinInCache(c));
    store.SanityCheck();
}

//! The map nodes one thread frees, more than a thread keeps, serve another thread's insertions, and the
//! coins that land in them are whole.
BOOST_AUTO_TEST_CASE(freed_nodes_serve_another_thread)
{
    CCoinsViewDB db{MemoryDb("store"), {}};
    CoinsStore store{&db, 4, true};
    store.SetBestBlock(uint256::ONE, 1);
    constexpr uint32_t COUNT{5000};
    const auto outpoint{[](uint8_t round, uint32_t i) { return COutPoint{Txid::FromUint256(uint256{round}), i}; }};
    const auto add{[&](uint8_t round) {
        for (uint32_t i{0}; i < COUNT; ++i) {
            store.AddCoin(outpoint(round, i), Coin{CTxOut{CAmount{i} + round, CScript{} << OP_TRUE}, 1, false}, false);
        }
    }};
    std::thread{add, uint8_t{1}}.join();
    BOOST_CHECK_EQUAL(store.GetCacheSize(), COUNT);
    // Fresh coins: each spend frees its node, two full lists stay with the thread and the rest change hands.
    store.SetChangeHeight(2);
    uint32_t spent{0};
    std::thread{[&] {
        for (uint32_t i{0}; i < COUNT; ++i) spent += store.SpendCoin(outpoint(1, i));
    }}.join();
    BOOST_CHECK_EQUAL(spent, COUNT);
    BOOST_CHECK_EQUAL(store.GetCacheSize(), 0U);
    std::thread{add, uint8_t{2}}.join();
    BOOST_CHECK_EQUAL(store.GetCacheSize(), COUNT);
    for (uint32_t i{0}; i < COUNT; ++i) {
        BOOST_CHECK(!store.PeekCoin(outpoint(1, i)));
        const std::optional<Coin> coin{store.PeekCoin(outpoint(2, i))};
        BOOST_REQUIRE(coin);
        BOOST_CHECK_EQUAL(coin->out.nValue, CAmount{i} + 2);
    }
    // Many threads that each end with nodes on their lists: the nodes come back, and no region is carved
    // for the next thread.
    const size_t regions{CoinsStore::NodeRegionBytes()};
    for (uint8_t round{3}; round < 203; ++round) {
        std::thread{[&] {
            for (uint32_t i{0}; i < COUNT; ++i) store.SpendCoin(outpoint(static_cast<uint8_t>(round - 1), i));
        }}.join();
        BOOST_REQUIRE_EQUAL(store.GetCacheSize(), 0U);
        std::thread{add, round}.join();
    }
    BOOST_CHECK_EQUAL(store.GetCacheSize(), COUNT);
    BOOST_CHECK_EQUAL(CoinsStore::NodeRegionBytes(), regions);
}

//! A create that takes no coin to be at its outpoint and meets one is a conflict: the change is not made
//! and the report names it, and the other changes of the block are made. A create that may overwrite
//! takes the coin's place, and the report has the coin that was there.
BOOST_AUTO_TEST_CASE(create_over_a_visible_coin_is_a_conflict)
{
    CCoinsViewDB db{MemoryDb("store"), {}};
    CoinsStore store{&db, 4, true};
    store.SetBestBlock(uint256::ONE, 1);
    const COutPoint live{Txid::FromUint256(uint256{10}), 0};
    const COutPoint vacant{Txid::FromUint256(uint256{11}), 0};
    const COutPoint other{Txid::FromUint256(uint256{12}), 0};
    const Coin first{RandomCoin(m_rng, 1)};
    store.AddCoin(live, Coin{first}, false);

    std::vector<OwnedChange> changes;
    changes.push_back({.outpoint = live, .coin = RandomCoin(m_rng, 2)});
    changes.push_back({.outpoint = vacant, .coin = RandomCoin(m_rng, 2)});
    CoinsStore::ConfirmedReport report;
    BOOST_CHECK_EQUAL(ApplyConfirmed(store, 2, changes, &report), 1U);
    BOOST_REQUIRE_EQUAL(report.conflicted.size(), 1U);
    BOOST_CHECK(*report.conflicted[0] == live);
    BOOST_CHECK(report.overwritten.empty());
    // The coin was not touched.
    BOOST_CHECK(Assert(store.PeekCoin(live))->out == first.out);
    BOOST_CHECK(store.HaveCoin(vacant));
    store.SetBestBlock(uint256{2}, 2);
    store.SanityCheck();

    // Creates that may overwrite, one over the coin and one over nothing.
    const Coin second{CTxOut{first.out.nValue + 1, first.out.scriptPubKey}, 3, /*fCoinBaseIn=*/true};
    std::vector<OwnedChange> over;
    over.push_back({.outpoint = live, .coin = Coin{second}, .possible_overwrite = true});
    over.push_back({.outpoint = other, .coin = RandomCoin(m_rng, 3), .possible_overwrite = true});
    CoinsStore::ConfirmedReport over_report;
    BOOST_CHECK_EQUAL(ApplyConfirmed(store, 3, over, &over_report), 0U);
    BOOST_CHECK(over_report.conflicted.empty());
    BOOST_REQUIRE_EQUAL(over_report.overwritten.size(), 1U);
    BOOST_CHECK(*over_report.overwritten[0].first == live);
    BOOST_CHECK(over_report.overwritten[0].second.out == first.out);
    BOOST_CHECK_EQUAL(over_report.overwritten[0].second.nHeight, 1U);
    BOOST_CHECK(Assert(store.PeekCoin(live))->out == second.out);
    BOOST_CHECK(store.HaveCoin(other));
    store.SetBestBlock(uint256{3}, 3);
    store.SanityCheck();
}

//! A spend is of the coin its block validated against (CoinsStore::Change::known_coin). Where the outpoint
//! was spent and made again since (a transaction made twice) the coin there is another, and the spend is
//! a conflict; so is the spend of an outpoint whose coin is spent and of one that never had a coin. The
//! conflicts are not made and the report names them; the other spends of the block are made, one of them
//! of a coin that only the database holds.
BOOST_AUTO_TEST_CASE(spend_of_another_coin_is_a_conflict)
{
    CCoinsViewDB db{MemoryDb("store"), {}};
    CoinsStore store{&db, 4, true};
    store.SetBestBlock(uint256::ONE, 1);
    const COutPoint twice{Txid::FromUint256(uint256{10}), 0};
    const COutPoint once{Txid::FromUint256(uint256{11}), 0};
    const COutPoint stored{Txid::FromUint256(uint256{12}), 0};
    const COutPoint gone{Txid::FromUint256(uint256{13}), 0};
    const COutPoint missing{Txid::FromUint256(uint256{14}), 0};
    const Coin stored_coin{RandomCoin(m_rng, 1)};
    const Coin gone_coin{RandomCoin(m_rng, 1)};
    store.AddCoin(stored, Coin{stored_coin}, false);
    store.AddCoin(gone, Coin{gone_coin}, false);
    store.Sync();
    store.EvictClean(0);
    BOOST_REQUIRE_EQUAL(store.GetCacheSize(), 0U); // both coins are in the database only
    store.SetBestBlock(uint256{2}, 2);
    Coin first{RandomCoin(m_rng, 2)};
    first.fCoinBase = true;
    const Coin once_coin{RandomCoin(m_rng, 2)};
    store.AddCoin(twice, Coin{first}, false);
    store.AddCoin(once, Coin{once_coin}, false);

    // Blocks 3 and 4: two coins are spent, and the outpoint of one is made again with the same output.
    BOOST_CHECK_EQUAL(Spend(store, 3, twice), 0U);
    BOOST_CHECK_EQUAL(Spend(store, 3, gone), 0U);
    store.SetBestBlock(uint256{3}, 3);
    Coin second{first};
    second.nHeight = 4;
    BOOST_CHECK_EQUAL(Create(store, 4, twice, Coin{second}, false), 0U);
    store.SetBestBlock(uint256{4}, 4);

    // Block 5 spends the coins as it found them before those two blocks.
    const auto spend_of{[](const COutPoint& outpoint, const Coin& coin) {
        Coin known{coin};
        known.out.SetNull(); // spent, with the height and the coinbase flag of the coin
        return OwnedChange{.outpoint = outpoint, .coin = std::move(known), .known_coin = true};
    }};
    std::vector<OwnedChange> changes;
    changes.push_back(spend_of(twice, first));
    changes.push_back(spend_of(once, once_coin));
    changes.push_back(spend_of(stored, stored_coin));
    changes.push_back(spend_of(gone, gone_coin));
    changes.push_back(spend_of(missing, once_coin));
    CoinsStore::ConfirmedReport report;
    BOOST_CHECK_EQUAL(ApplyConfirmed(store, 5, changes, &report), 3U);
    BOOST_CHECK_EQUAL(report.conflicted.size(), 3U);
    std::set<COutPoint> conflicted;
    for (const COutPoint* outpoint : report.conflicted) conflicted.insert(*outpoint);
    BOOST_CHECK(conflicted == (std::set<COutPoint>{twice, gone, missing}));
    // The coin made again is there still; the other spends are made.
    BOOST_CHECK_EQUAL(Assert(store.PeekCoin(twice))->nHeight, 4U);
    BOOST_CHECK(!store.HaveCoin(once));
    BOOST_CHECK(!store.HaveCoin(stored));
    store.SanityCheck();

    // Validated against the second coin, the spend is made.
    std::vector<OwnedChange> again;
    again.push_back(spend_of(twice, second));
    BOOST_CHECK_EQUAL(ApplyConfirmed(store, 5, again), 0U);
    store.SetBestBlock(uint256{5}, 5);
    BOOST_CHECK(!store.HaveCoin(twice));
    store.Sync();
    BOOST_CHECK(!db.HaveCoin(stored));
    BOOST_CHECK(!db.HaveCoin(gone));
    store.SanityCheck();
}

//! A disconnect that spends a coin made again over an owed erase keeps the erase's height: the outpoint was
//! spent at 2 and made again at 4, and the disconnect back to 3 spends it at the fork height. A run bounded at
//! 2 must carry the erase, or settle below it; settled at 2 with the old coin still written, the database
//! would hold a coin no replay from there takes away.
BOOST_AUTO_TEST_CASE(disconnect_keeps_the_owed_erase_height)
{
    CCoinsViewDB db{MemoryDb("store"), {}};
    CoinsStore store{&db, 4, true};
    store.SetBestBlock(uint256{1}, 1);
    const COutPoint x{Txid::FromUint256(uint256{30}), 0};
    store.AddCoin(x, RandomCoin(m_rng, 1), false);
    store.Sync(); // x is in the database
    BOOST_REQUIRE(db.HaveCoin(x));
    store.SetBestBlock(uint256{2}, 2);
    BOOST_REQUIRE(store.SpendCoin(x)); // the erase is owed, as of 2
    // Another change at 2, so that a run bounded there has something to write either way.
    store.AddCoin(COutPoint{Txid::FromUint256(uint256{31}), 0}, RandomCoin(m_rng, 2), false);
    store.SetBestBlock(uint256{3}, 3);
    Create(store, 4, x, RandomCoin(m_rng, 4), false); // made again (a duplicate), over the owed erase
    store.SetBestBlock(uint256{4}, 4);

    // Block 4 disconnected towards the fork at 3: its coin is spent at the fork height.
    store.SetChangeHeight(3);
    BOOST_REQUIRE(store.SpendCoin(x));
    store.SetBestBlock(uint256{3});
    store.SetTierHeight(3);

    const auto run{WriteOut(store, /*bound_height=*/2, /*head_height=*/3, uint256{3})};
    BOOST_CHECK_MESSAGE(!db.HaveCoin(x) || run->settled_height < 2,
                        "the database settled at " << run->settled_height << " with a coin spent at 2");
    store.SanityCheck();
}

//! The same for a block's spend: the outpoint was spent at 2 and made again at 4, over the owed erase, and
//! block 5 spends that coin: the entry keeps the erase's height. A run bounded at 2 must carry the erase,
//! or settle below it.
BOOST_AUTO_TEST_CASE(block_spend_keeps_the_owed_erase_height)
{
    CCoinsViewDB db{MemoryDb("store"), {}};
    CoinsStore store{&db, 4, true};
    store.SetBestBlock(uint256{1}, 1);
    const COutPoint x{Txid::FromUint256(uint256{30}), 0};
    store.AddCoin(x, RandomCoin(m_rng, 1), false);
    store.Sync(); // x is in the database
    BOOST_REQUIRE(db.HaveCoin(x));
    store.SetBestBlock(uint256{2}, 2);
    BOOST_REQUIRE(store.SpendCoin(x)); // the erase is owed, as of 2
    // Another change at 2, so that a run bounded there has something to write either way.
    store.AddCoin(COutPoint{Txid::FromUint256(uint256{31}), 0}, RandomCoin(m_rng, 2), false);
    store.SetBestBlock(uint256{3}, 3);
    Create(store, 4, x, RandomCoin(m_rng, 4), false); // made again (a duplicate), over the owed erase
    store.SetBestBlock(uint256{4}, 4);
    BOOST_REQUIRE_EQUAL(Spend(store, 5, x), 0U); // spent by a block, the erase of 2 still owed
    store.SetBestBlock(uint256{5}, 5);

    const auto run{WriteOut(store, /*bound_height=*/2, /*head_height=*/5, uint256{5})};
    BOOST_CHECK_MESSAGE(!db.HaveCoin(x) || run->settled_height < 2,
                        "the database settled at " << run->settled_height << " with a coin spent at 2");
    store.SanityCheck();
}

//! A coin a disconnect puts back over an owed erase keeps the erase owed: the outpoint was spent at 2, made again at
//! 4 and spent by block 5, and the disconnect of block 5 puts the coin of 4 back through a child cache. A run bounded
//! at 2, which a change of 3 left behind keeps at 2, writes the erase, not the coin of 4. A database settled at 2
//! that held the coin would keep it where the blocks above 2 are later taken back and the next run's marker opens
//! with 2 for both heads: nothing in a replay would take it out (LagRuns' counterexample).
BOOST_AUTO_TEST_CASE(restore_over_an_owed_erase_keeps_it_owed)
{
    CCoinsViewDB db{MemoryDb("store"), {}};
    CoinsStore store{&db, 4, true};
    store.SetBestBlock(uint256{1}, 1);
    const COutPoint x{Txid::FromUint256(uint256{30}), 0};
    store.AddCoin(x, RandomCoin(m_rng, 1), false);
    store.Sync(); // x is in the database
    store.SetBestBlock(uint256{2}, 2);
    BOOST_REQUIRE(store.SpendCoin(x)); // the erase is owed, as of 2
    store.AddCoin(COutPoint{Txid::FromUint256(uint256{31}), 0}, RandomCoin(m_rng, 2), false);
    store.SetBestBlock(uint256{3}, 3);
    store.AddCoin(COutPoint{Txid::FromUint256(uint256{32}), 0}, RandomCoin(m_rng, 3), false);
    const Coin made{RandomCoin(m_rng, 4)};
    Create(store, 4, x, Coin{made}, false); // made again (a duplicate), over the owed erase
    store.SetBestBlock(uint256{4}, 4);
    BOOST_REQUIRE_EQUAL(Spend(store, 5, x), 0U);
    store.SetBestBlock(uint256{5}, 5);

    // Block 5 disconnected: the coin it spent is put back through a child cache, as a disconnect does.
    store.SetChangeHeight(4);
    {
        CCoinsViewCache child{&store};
        child.AddCoin(x, Coin{made}, /*possible_overwrite=*/true);
        child.SetBestBlock(uint256{4});
        child.Flush();
    }
    store.SetTierHeight(4);
    BOOST_REQUIRE(store.HaveCoin(x));
    store.SanityCheck();

    BOOST_REQUIRE_EQUAL(WriteOut(store, /*bound_height=*/2, /*head_height=*/4, uint256{4})->settled_height, 2);
    BOOST_CHECK_MESSAGE(!db.HaveCoin(x), "the database settled at 2 with the coin made at 4");
    BOOST_CHECK(store.HaveCoin(x));
    store.SanityCheck();
}

//! A base that counts its reads.
class ReadCountingBase final : public CCoinsViewBacked
{
public:
    using CCoinsViewBacked::CCoinsViewBacked;
    mutable int m_reads{0};
    std::optional<Coin> GetCoin(const COutPoint& outpoint) const override
    {
        ++m_reads;
        return CCoinsViewBacked::GetCoin(outpoint);
    }
    std::optional<Coin> PeekCoin(const COutPoint& outpoint) const override
    {
        ++m_reads;
        return CCoinsViewBacked::PeekCoin(outpoint);
    }
};

//! A window block's view reads a coin that only the database holds once: the read brings the coin into the
//! tier, and the spend finds it there at the commit. Given back, the view leaves the coin as the database
//! has it. Without the switch (a block at the tip) the read leaves nothing behind and the commit reads again.
BOOST_AUTO_TEST_CASE(block_apply_view_reads_a_database_coin_once)
{
    CCoinsViewDB db{MemoryDb("store"), {}};
    ReadCountingBase base{&db};
    CoinsStore store{&base, 4, true};
    store.SetBestBlock(uint256::ONE, 1);
    const COutPoint spent_coin{Txid::FromUint256(uint256{10}), 0};
    const COutPoint kept_coin{Txid::FromUint256(uint256{11}), 0};
    store.AddCoin(spent_coin, RandomCoin(m_rng, 1), false);
    store.AddCoin(kept_coin, RandomCoin(m_rng, 1), false);
    store.Sync();
    store.EvictClean(0);
    BOOST_REQUIRE_EQUAL(store.GetCacheSize(), 0U); // both coins are in the database only
    util::NotNull pool{std::make_shared<ThreadPool>("test")}; // no workers: lookups are served inline
    BlockApplyView view{store, pool, /*deterministic=*/true};
    // No undo data: a spend that misses the store at the commit reads the base.
    const CBlock block;
    const CBlockUndo undo;

    base.m_reads = 0;
    view.SetHeight(2);
    view.SetKeepBaseReads(true);
    BOOST_CHECK(view.SpendCoin(spent_coin));
    BOOST_CHECK_EQUAL(base.m_reads, 1);
    BOOST_CHECK(store.HaveCoinInCache(spent_coin)); // clean, as the database has it
    view.SetBestBlock(uint256{2});
    view.CommitAlone(block, undo);
    view.ConfirmCommitted(2);
    BOOST_CHECK_EQUAL(base.m_reads, 1); // the commit found the entry
    BOOST_CHECK(!store.HaveCoin(spent_coin));
    store.SanityCheck();
    store.Sync();
    BOOST_CHECK(!db.PeekCoin(spent_coin).has_value()); // the erase was owed and is written

    base.m_reads = 0;
    view.SetHeight(3);
    view.SetKeepBaseReads(true);
    BOOST_CHECK(view.SpendCoin(kept_coin));
    BOOST_CHECK_EQUAL(base.m_reads, 1);
    view.Release();
    BOOST_CHECK(store.HaveCoin(kept_coin));
    BOOST_CHECK_EQUAL(base.m_reads, 1);
    store.SanityCheck();
    store.Sync();
    BOOST_CHECK(db.PeekCoin(kept_coin).has_value());

    // The switch went with the block: the next one reads as at the tip.
    store.EvictClean(0);
    BOOST_REQUIRE(!store.HaveCoinInCache(kept_coin));
    base.m_reads = 0;
    view.SetHeight(3);
    BOOST_CHECK(view.SpendCoin(kept_coin));
    BOOST_CHECK_EQUAL(base.m_reads, 1);
    BOOST_CHECK(!store.HaveCoinInCache(kept_coin));
    view.SetBestBlock(uint256{3});
    view.CommitAlone(block, undo);
    BOOST_CHECK_EQUAL(base.m_reads, 2);
    view.ConfirmCommitted(3);
    BOOST_CHECK(!store.HaveCoin(kept_coin));
    store.SanityCheck();
}

//! A base whose read of one outpoint waits, once, until the test lets it go, and that counts its reads by kind:
//! GetCoin() is the store's read that keeps the coin, PeekCoin() the one that does not.
class GatedBase final : public CCoinsViewBacked
{
public:
    using CCoinsViewBacked::CCoinsViewBacked;
    COutPoint m_gated;
    std::shared_future<void> m_open;
    mutable std::promise<void> m_entered;
    mutable std::atomic<int> m_gets{0};
    mutable std::atomic<int> m_peeks{0};
    mutable std::atomic<bool> m_gated_read_done{false};
    std::optional<Coin> GetCoin(const COutPoint& outpoint) const override
    {
        ++m_gets;
        return Gated(outpoint, [&] { return CCoinsViewBacked::GetCoin(outpoint); });
    }
    std::optional<Coin> PeekCoin(const COutPoint& outpoint) const override
    {
        ++m_peeks;
        return Gated(outpoint, [&] { return CCoinsViewBacked::PeekCoin(outpoint); });
    }

private:
    mutable std::atomic_flag m_gate_taken{};
    template <typename F>
    std::optional<Coin> Gated(const COutPoint& outpoint, F read) const
    {
        if (outpoint != m_gated || m_gate_taken.test_and_set()) return read();
        m_entered.set_value();
        m_open.wait();
        std::optional<Coin> coin{read()};
        m_gated_read_done = true;
        return coin;
    }
};

//! The view's reset, with a prefetch worker reading the base for the view, waits for that read before it clears
//! the block's state (BlockApplyView::Reset()), and the next block reads as the reset left the view: the first
//! block's read kept its coin in the store (SetKeepBaseReads()), the next block's leaves nothing there.
BOOST_AUTO_TEST_CASE(block_apply_view_reset_waits_for_its_prefetch)
{
    CCoinsViewDB db{MemoryDb("store"), {}};
    const COutPoint first{Txid::FromUint256(uint256{10}), 0};
    const COutPoint second{Txid::FromUint256(uint256{11}), 0};
    {
        CCoinsViewCache cache{&db};
        cache.AddCoin(first, RandomCoin(m_rng, 1), false);
        cache.AddCoin(second, RandomCoin(m_rng, 1), false);
        cache.SetBestBlock(uint256::ONE);
        cache.Flush();
    }
    GatedBase base{&db};
    base.m_gated = first;
    std::promise<void> open;
    base.m_open = open.get_future().share();
    std::future<void> entered{base.m_entered.get_future()};
    CoinsStore store{&base, 4, true};
    store.SetBestBlock(uint256::ONE, 1);
    util::NotNull pool{std::make_shared<ThreadPool>("test")};
    pool->Start(2);
    // A block whose one transaction after the coinbase spends `outpoint`.
    const auto spending{[](const COutPoint& outpoint) {
        CMutableTransaction coinbase;
        coinbase.vin.emplace_back();
        coinbase.vout.emplace_back(1, CScript{} << OP_TRUE);
        CMutableTransaction tx;
        tx.vin.emplace_back(outpoint);
        tx.vout.emplace_back(1, CScript{} << OP_TRUE);
        CBlock block;
        block.vtx = {MakeTransactionRef(std::move(coinbase)), MakeTransactionRef(std::move(tx))};
        return block;
    }};
    const CBlock block_first{spending(first)};
    const CBlock block_second{spending(second)};
    {
        BlockApplyView view{store, pool, /*deterministic=*/true};
        view.SetHeight(2);
        view.SetKeepBaseReads(true);
        std::thread opener;
        {
            const auto guard{view.StartFetching(block_first)};
            // A worker reads `first` from the base for the view, and waits there.
            entered.wait();
            opener = std::thread{[&open] { open.set_value(); }};
        } // the view's reset
        BOOST_CHECK(base.m_gated_read_done); // the reset returned after the read
        opener.join();
        BOOST_CHECK(!view.InUse());
        BOOST_CHECK_EQUAL(base.m_gets.load(), 1);
        BOOST_CHECK_EQUAL(base.m_peeks.load(), 0);
        BOOST_CHECK(store.HaveCoinInCache(first)); // kept, clean, as the switch asked

        // The reset cleared the switch: the next block's read peeks and leaves nothing in the store.
        view.SetHeight(3);
        {
            const auto guard{view.StartFetching(block_second)};
            BOOST_CHECK(view.HaveCoin(second));
        }
        BOOST_CHECK_EQUAL(base.m_gets.load(), 1);
        BOOST_CHECK_EQUAL(base.m_peeks.load(), 1);
        BOOST_CHECK(!store.HaveCoinInCache(second));
    }
    pool->Stop();
    store.SanityCheck();
}

//! A base whose read of one outpoint, once, takes the coin and then waits until the test lets it go: the reader
//! holds what the base had before anything the test does meanwhile.
class LateReturnBase final : public CCoinsViewBacked
{
public:
    using CCoinsViewBacked::CCoinsViewBacked;
    COutPoint m_gated;
    std::shared_future<void> m_open;
    mutable std::promise<void> m_entered;
    std::optional<Coin> GetCoin(const COutPoint& outpoint) const override
    {
        std::optional<Coin> coin{CCoinsViewBacked::GetCoin(outpoint)};
        if (outpoint == m_gated && !m_gate_taken.test_and_set()) {
            m_entered.set_value();
            m_open.wait();
        }
        return coin;
    }

private:
    mutable std::atomic_flag m_gate_taken{};
};

//! A lookup that misses reads the base with the shard unlocked (GetCoin()). Meanwhile the owner spends the coin,
//! which brings it into the tier, and a run's chunk or a flush writes the erase and drops the spent entry: the
//! lookup holds a coin that neither the tier nor the database has any more. It looks again, as the shard's count
//! of settles moved, and finds the coin gone; taking what it read would put the spent coin back in the tier,
//! clean, over a database without it.
BOOST_AUTO_TEST_CASE(lookup_across_a_settle_looks_again)
{
    const COutPoint x{Txid::FromUint256(uint256{40}), 0};
    // `land` writes the spend: a run or a flush.
    const auto lookup_across{[&](const std::function<void(CoinsStore&)>& land) {
        CCoinsViewDB db{MemoryDb("store"), {}};
        {
            CCoinsViewCache cache{&db};
            cache.AddCoin(x, RandomCoin(m_rng, 1), false);
            cache.SetBestBlock(uint256{1});
            cache.Flush();
        }
        LateReturnBase base{&db};
        base.m_gated = x;
        std::promise<void> open;
        base.m_open = open.get_future().share();
        std::future<void> entered{base.m_entered.get_future()};
        CoinsStore store{&base, 4, true};
        store.SetBestBlock(uint256{1}, 1);
        std::optional<Coin> seen;
        std::thread reader{[&] { seen = store.GetCoin(x); }};
        // The reader missed, and holds the coin the base had.
        entered.wait();
        store.SetBestBlock(uint256{2}, 2);
        BOOST_CHECK(store.SpendCoin(x));
        land(store);
        BOOST_CHECK(!db.HaveCoin(x));
        open.set_value();
        reader.join();
        BOOST_CHECK(!seen);
        BOOST_CHECK(!store.HaveCoinInCache(x));
        store.SanityCheck();
    }};
    lookup_across([](CoinsStore& store) { WriteOut(store, /*bound_height=*/2, /*head_height=*/2, uint256{2}); });
    lookup_across([](CoinsStore& store) { store.Flush(); });
}

//! A block connected alone gives its undo data to the commit: the spend of a coin that only the database
//! holds is made from the coin there, without a second read of the base, and the erase is owed and
//! written. Undo data that does not fit the block or one of its transactions is not used, and the commit
//! reads the base as it does without any. A coin given for the spend that is not the one the block
//! validated against is a conflict.
BOOST_AUTO_TEST_CASE(block_apply_view_takes_a_missed_coin_from_the_undo)
{
    CCoinsViewDB db{MemoryDb("store"), {}};
    ReadCountingBase base{&db};
    CoinsStore store{&base, 4, true};
    store.SetBestBlock(uint256::ONE, 1);
    const Coin coin{RandomCoin(m_rng, 1)};

    // A block: a coinbase and one transaction that spends a coin of the block's own, with the undo data
    // of that spend.
    struct Alone {
        COutPoint outpoint;
        CBlock block;
        CBlockUndo undo;
    };
    const auto make_block{[&](uint8_t n) {
        Alone made;
        made.outpoint = COutPoint{Txid::FromUint256(uint256{n}), 0};
        store.AddCoin(made.outpoint, Coin{coin}, false);
        made.block.vtx.push_back(MakeTransactionRef(CMutableTransaction{}));
        CMutableTransaction spender;
        spender.vin.emplace_back(made.outpoint);
        made.block.vtx.push_back(MakeTransactionRef(spender));
        made.undo.vtxundo.emplace_back();
        made.undo.vtxundo[0].vprevout.push_back(coin);
        return made;
    }};
    const Alone fits{make_block(10)};
    // Undo data for another number of transactions is not the block's.
    Alone other_count{make_block(11)};
    other_count.undo.vtxundo.emplace_back();
    // Nor is a transaction's undo data with another number of coins than the transaction has inputs.
    Alone longer{make_block(12)};
    longer.undo.vtxundo[0].vprevout.push_back(coin);
    const COutPoint wrong{Txid::FromUint256(uint256{13}), 0};
    store.AddCoin(wrong, Coin{coin}, false);
    store.Sync();
    store.EvictClean(0);
    BOOST_REQUIRE_EQUAL(store.GetCacheSize(), 0U); // the coins are in the database only
    util::NotNull pool{std::make_shared<ThreadPool>("test")}; // no workers: lookups are served inline
    BlockApplyView view{store, pool, /*deterministic=*/true};

    // The block connected and committed: the reads of the base that took.
    int height{1};
    const auto commit{[&](const Alone& alone) {
        ++height;
        base.m_reads = 0;
        view.SetHeight(height);
        BOOST_CHECK(view.SpendCoin(alone.outpoint));
        BOOST_CHECK_EQUAL(base.m_reads, 1);
        BOOST_CHECK(!store.HaveCoinInCache(alone.outpoint)); // at the tip the lookup leaves nothing behind
        view.SetBestBlock(uint256{static_cast<uint8_t>(height)});
        view.CommitAlone(alone.block, alone.undo);
        const int reads{base.m_reads};
        view.ConfirmCommitted(height);
        BOOST_CHECK(!store.HaveCoin(alone.outpoint));
        store.SanityCheck();
        return reads;
    }};
    BOOST_CHECK_EQUAL(commit(fits), 1); // the commit took the coin from the undo data
    store.Sync();
    BOOST_CHECK(!db.PeekCoin(fits.outpoint).has_value()); // the erase was owed and is written
    BOOST_CHECK_EQUAL(commit(other_count), 2);
    BOOST_CHECK_EQUAL(commit(longer), 2);
    store.Sync();
    BOOST_CHECK(!db.PeekCoin(other_count.outpoint).has_value());
    BOOST_CHECK(!db.PeekCoin(longer.outpoint).has_value());

    // A coin given for a spend that misses the store, and not the one the block validated its spend
    // against: a conflict, as for any other coin met in place of the known one. Nothing is spent and the
    // base is not asked.
    Coin known{coin};
    known.out.SetNull();
    Coin given{coin};
    given.fCoinBase = true;
    std::vector<CoinsStore::Change> changes;
    changes.push_back({.outpoint = &wrong, .coin = &known, .possible_overwrite = false, .known_coin = true});
    CoinsStore::ConfirmedReport report;
    base.m_reads = 0;
    BOOST_CHECK_EQUAL(store.ApplyConfirmed(std::move(changes), height + 1, &report, [&](const COutPoint&) -> const Coin* { return &given; }), 1U);
    BOOST_CHECK_EQUAL(base.m_reads, 0);
    BOOST_REQUIRE_EQUAL(report.conflicted.size(), 1U);
    BOOST_CHECK(*report.conflicted[0] == wrong);
    BOOST_CHECK(store.HaveCoin(wrong));
    store.SanityCheck();
}

BOOST_AUTO_TEST_CASE(block_apply_view_takes_the_missed_coins_of_several_inputs_from_the_undo)
{
    CCoinsViewDB db{MemoryDb("store"), {}};
    ReadCountingBase base{&db};
    CoinsStore store{&base, 4, true};
    // Three coins, each made at a height of its own: one is told from another.
    std::vector<COutPoint> outpoints;
    std::vector<Coin> coins;
    for (uint8_t i{0}; i < 3; ++i) {
        store.SetBestBlock(uint256{static_cast<uint8_t>(1 + i)}, 1 + i);
        outpoints.emplace_back(Txid::FromUint256(uint256{static_cast<uint8_t>(20 + i)}), 0);
        coins.push_back(RandomCoin(m_rng, 1 + i));
        store.AddCoin(outpoints.back(), Coin{coins.back()}, false);
    }
    store.Sync();
    store.EvictClean(0);
    BOOST_REQUIRE_EQUAL(store.GetCacheSize(), 0U); // the coins are in the database only
    util::NotNull pool{std::make_shared<ThreadPool>("test")}; // no workers: lookups are served inline
    BlockApplyView view{store, pool, /*deterministic=*/true};

    // The block: a coinbase, a transaction with two inputs and one with one. The undo data has an entry
    // for each transaction but the coinbase, with a coin for each input in the inputs' order.
    CBlock block;
    block.vtx.push_back(MakeTransactionRef(CMutableTransaction{}));
    CMutableTransaction two_inputs;
    two_inputs.vin.emplace_back(outpoints[0]);
    two_inputs.vin.emplace_back(outpoints[1]);
    block.vtx.push_back(MakeTransactionRef(two_inputs));
    CMutableTransaction one_input;
    one_input.vin.emplace_back(outpoints[2]);
    block.vtx.push_back(MakeTransactionRef(one_input));
    CBlockUndo undo;
    undo.vtxundo.resize(2);
    undo.vtxundo[0].vprevout.push_back(coins[0]);
    undo.vtxundo[0].vprevout.push_back(coins[1]);
    undo.vtxundo[1].vprevout.push_back(coins[2]);

    // One read for each coin, at the lookups; the commit takes all three from the undo data. Each is the
    // coin its input was validated against, which the commit would meet as a conflict otherwise: none
    // went to another input's outpoint.
    base.m_reads = 0;
    view.SetHeight(4);
    for (const COutPoint& outpoint : outpoints) BOOST_CHECK(view.SpendCoin(outpoint));
    BOOST_CHECK_EQUAL(base.m_reads, 3);
    view.SetBestBlock(uint256{4});
    view.CommitAlone(block, undo);
    BOOST_CHECK_EQUAL(base.m_reads, 3);
    view.ConfirmCommitted(4);
    store.SanityCheck();
    for (const COutPoint& outpoint : outpoints) BOOST_CHECK(!store.HaveCoin(outpoint));
    store.Sync();
    for (const COutPoint& outpoint : outpoints) BOOST_CHECK(!db.HaveCoin(outpoint));
}

//! A write-out run that is not final leaves the database without a best block until a later run closes its marker:
//! there is then no block whose UTXO set the database holds, and the statistics are not given, for any hash type.
//! Before the run opens the marker and after the final one closes it, they are.
BOOST_FIXTURE_TEST_CASE(utxo_stats_refused_while_a_run_marker_is_open, TestChain100Setup)
{
    CCoinsViewDB db{MemoryDb("stats"), {}};
    node::BlockManager& blockman{m_node.chainman->m_blockman};
    const auto [tip_hash, tip_height]{WITH_LOCK(::cs_main, return std::pair(m_node.chainman->ActiveTip()->GetBlockHash(), m_node.chainman->ActiveHeight()))};
    const auto write{[&](uint8_t txid, bool final) {
        std::vector<CoinWrite> writes;
        writes.push_back({COutPoint{Txid::FromUint256(uint256{txid}), 0}, Coin{CTxOut{CAmount{1000}, CScript{} << OP_TRUE}, 1, /*fCoinBaseIn=*/false}});
        db.WriteRun(writes, tip_hash, tip_hash, final);
    }};
    const auto stats{[&](kernel::CoinStatsHashType type) { return kernel::ComputeUTXOStats(type, db, blockman); }};
    write(10, /*final=*/true);
    const auto before{stats(kernel::CoinStatsHashType::HASH_SERIALIZED)};
    BOOST_REQUIRE(before);
    BOOST_CHECK_EQUAL(before->nHeight, tip_height);
    BOOST_CHECK_EQUAL(before->coins_count, 1U);

    write(11, /*final=*/false);
    BOOST_CHECK(db.GetBestBlock().IsNull());
    for (const auto type : {kernel::CoinStatsHashType::HASH_SERIALIZED, kernel::CoinStatsHashType::MUHASH, kernel::CoinStatsHashType::NONE}) {
        BOOST_CHECK(!stats(type));
    }

    db.WriteRun({}, tip_hash, tip_hash, /*final=*/true);
    const auto after{stats(kernel::CoinStatsHashType::HASH_SERIALIZED)};
    BOOST_REQUIRE(after);
    BOOST_CHECK_EQUAL(after->nHeight, tip_height);
    BOOST_CHECK_EQUAL(after->coins_count, 2U);
}

BOOST_AUTO_TEST_SUITE_END()
