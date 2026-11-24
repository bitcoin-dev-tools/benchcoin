// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <coinsstore.h>

#include <logging.h>
#include <memusage.h>
#include <util/check.h>
#include <util/threadpool.h>


#include <algorithm>
#include <condition_variable>
#include <exception>
#include <functional>
#include <unordered_map>
#include <limits>
#include <map>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#ifndef WIN32
#include <sys/mman.h>
#endif

CoinsStore::CoinsStore(CCoinsView* base_view, size_t shards, bool deterministic)
    : CCoinsViewCache(base_view, deterministic), m_shards(std::max<size_t>(1, shards))
{
    for (Shard& shard : m_shards) {
        LOCK(shard.mutex);
        Resized(shard); // whatever bucket array an empty map has
    }
}

CoinsStore::~CoinsStore() = default;

// ---------------------------------------------------------------------------------------------------------
// Entry bookkeeping

CoinsStore::Totals& CoinsStore::MyTotals() const
{
    // Stripes are dealt in turn; threads beyond their number share, which costs a shared line and nothing else.
    static std::atomic<size_t> next{0};
    static thread_local const size_t stripe{next.fetch_add(1, std::memory_order_relaxed) % TOTALS_STRIPES};
    return m_totals[stripe];
}

size_t CoinsStore::SumTotals(std::atomic<int64_t> Totals::* field) const
{
    int64_t total{0};
    for (const Totals& totals : m_totals) total += (totals.*field).load(std::memory_order_relaxed);
    // Read beside a change under way, an addition can be missed whose removal is seen.
    return static_cast<size_t>(std::max<int64_t>(0, total));
}

void* CoinsStore::HintedRegion(size_t bytes)
{
    assert(bytes != 0 && bytes % HUGE_PAGE_BYTES == 0);
    void* region{::operator new(bytes, std::align_val_t{HUGE_PAGE_BYTES})};
#ifdef MADV_HUGEPAGE
    // Advice the system is free to ignore, as it does where transparent huge pages are off.
    (void)madvise(region, bytes, MADV_HUGEPAGE);
#endif
    return region;
}

int64_t CoinsStore::NodeBytes()
{
    // A node's own size, not what the general allocator would hold for it: nodes are carved back to
    // back from the depot's regions, with no header and no rounding between them.
    return static_cast<int64_t>(sizeof(Map::Cell));
}

size_t CoinsStore::SpanOf(int height)
{
    return static_cast<size_t>(std::max(height, 0)) >> DIRTY_SPAN_BITS;
}

CoinsStore::Bucket& CoinsStore::DirtyBucket(Shard& shard, int height) const
{
    AssertLockHeld(shard.mutex);
    const size_t span{SpanOf(height)};
    if (span >= shard.dirty_spans.size()) {
        const auto before{static_cast<int64_t>(memusage::DynamicUsage(shard.dirty_spans))};
        shard.dirty_spans.resize(span + 1);
        MyTotals().bytes.fetch_add(static_cast<int64_t>(memusage::DynamicUsage(shard.dirty_spans)) - before, std::memory_order_relaxed);
    }
    return shard.dirty_spans[span];
}

void CoinsStore::FreeDirtySpans(Shard& shard) const
{
    AssertLockHeld(shard.mutex);
    MyTotals().bytes.fetch_sub(static_cast<int64_t>(memusage::DynamicUsage(shard.dirty_spans)), std::memory_order_relaxed);
    std::vector<Bucket>{}.swap(shard.dirty_spans);
}

void CoinsStore::AddUsage(Shard& shard, size_t bytes) const
{
    AssertLockHeld(shard.mutex);
    MyTotals().bytes.fetch_add(static_cast<int64_t>(bytes), std::memory_order_relaxed);
}

void CoinsStore::SubUsage(Shard& shard, size_t bytes) const
{
    AssertLockHeld(shard.mutex);
    MyTotals().bytes.fetch_sub(static_cast<int64_t>(bytes), std::memory_order_relaxed);
}

void CoinsStore::Resized(Shard& shard) const
{
    AssertLockHeld(shard.mutex);
    // A cell for each entry and the bucket array.
    const size_t entries{shard.map.size()};
    const size_t buckets{shard.map.bucket_count()};
    if (entries == shard.entries && buckets == shard.buckets) return;
    const int64_t delta{static_cast<int64_t>(entries) - static_cast<int64_t>(shard.entries)};
    int64_t bytes{NodeBytes() * delta};
    if (buckets != shard.buckets) {
        bytes += static_cast<int64_t>(memusage::MallocUsage(sizeof(void*) * buckets)) - static_cast<int64_t>(memusage::MallocUsage(sizeof(void*) * shard.buckets));
    }
    Totals& totals{MyTotals()};
    totals.entries.fetch_add(delta, std::memory_order_relaxed);
    totals.bytes.fetch_add(bytes, std::memory_order_relaxed);
    shard.entries = entries;
    shard.buckets = buckets;
}

int64_t CoinsStore::DirtyBytes(const Entry& entry)
{
    // The node and the coin's script, as DynamicMemoryUsage() counts them: what the tier's usage falls by
    // when the entry is written out and then evicted (or, for a spent one, erased).
    return NodeBytes() + static_cast<int64_t>(entry.coin.DynamicMemoryUsage());
}

void CoinsStore::Detach(Shard& shard, Node& node) const
{
    AssertLockHeld(shard.mutex);
    Entry& entry{node.second};
    if (entry.list == LIST_DIRTY) {
        Bucket& bucket{shard.dirty_spans[SpanOf(entry.height)]};
        bucket.bytes -= DirtyBytes(entry);
        Assume(bucket.bytes >= 0);
        Assert(shard.dirty_count > 0);
        --shard.dirty_count;
        MyTotals().dirty.fetch_sub(1, std::memory_order_relaxed);
        if (shard.dirty_count == 0) FreeDirtySpans(shard);
    }
    entry.list = LIST_NONE;
}

void CoinsStore::Attach(Shard& shard, Node& node) const
{
    AssertLockHeld(shard.mutex);
    Entry& entry{node.second};
    Assume(entry.list == LIST_NONE);
    if (entry.dirty) {
        DirtyBucket(shard, entry.height).bytes += DirtyBytes(entry);
        ++shard.dirty_count;
        MyTotals().dirty.fetch_add(1, std::memory_order_relaxed);
        entry.list = LIST_DIRTY;
    } else {
        entry.list = LIST_CLEAN;
    }
}

void CoinsStore::EraseDetached(Shard& shard, Map::iterator it) const
{
    AssertLockHeld(shard.mutex);
    Assume(it->second.list == LIST_NONE);
    // A run holds a pointer to every entry it collected until their chunk settles.
    Assert(!it->second.collected);
    shard.map.erase(it);
    Resized(shard);
}

void CoinsStore::EraseEntry(Shard& shard, Map::iterator it) const
{
    AssertLockHeld(shard.mutex);
    Entry& entry{it->second};
    Detach(shard, *it);
    SubUsage(shard, entry.coin.DynamicMemoryUsage());
    EraseDetached(shard, it);
}

void CoinsStore::EraseEntry(Shard& shard, Node& node) const
{
    AssertLockHeld(shard.mutex);
    Entry& entry{node.second};
    Detach(shard, node);
    SubUsage(shard, entry.coin.DynamicMemoryUsage());
    Assert(!entry.collected); // as in EraseDetached()
    // A copy of the key, which goes with the node.
    const COutPoint outpoint{node.first};
    shard.map.erase(outpoint);
    Resized(shard);
}

// ---------------------------------------------------------------------------------------------------------
// Lookups

std::optional<Coin> CoinsStore::SeenCoin(Entry& entry)
{
    if (entry.coin.IsSpent()) return std::nullopt;
    Reference(entry);
    return entry.coin;
}

std::optional<Coin> CoinsStore::Lookup(const COutPoint& outpoint) const
{
    Shard& shard{ShardFor(outpoint)};
    while (true) {
        uint64_t settles;
        {
            LOCK(shard.mutex);
            if (auto it{shard.map.find(outpoint)}; it != shard.map.end()) return SeenCoin(it->second);
            settles = shard.settles;
        }
        // A miss: read the base with the shard unlocked, then re-take the shard and insert unless another
        // thread brought the entry in meanwhile (in which case its state wins, whatever it is).
        std::optional<Coin> coin{base->GetCoin(outpoint)};
        LOCK(shard.mutex);
        // A write-out settled entries of the shard meanwhile: the coin may have been spent, written as an
        // erase and dropped from the tier since the miss, with the base read before the erase. Look again.
        if (shard.settles != settles) continue;
        auto it{shard.map.find(outpoint)};
        if (it != shard.map.end()) return SeenCoin(it->second);
        if (!coin) return std::nullopt;
        Assert(!coin->IsSpent());
        it = shard.map.try_emplace(outpoint).first;
        Resized(shard);
        it->second.coin = *coin;
        Transit(it->second, coins_entry::ABSENT, Event::BaseCopy, static_cast<int>(coin->nHeight));
        AddUsage(shard, it->second.coin.DynamicMemoryUsage());
        Attach(shard, *it);
        return coin;
    }
}

std::optional<Coin> CoinsStore::GetCoin(const COutPoint& outpoint) const
{
    return Lookup(outpoint);
}

std::optional<Coin> CoinsStore::PeekCoin(const COutPoint& outpoint) const
{
    Shard& shard{ShardFor(outpoint)};
    {
        LOCK(shard.mutex);
        if (auto it{shard.map.find(outpoint)}; it != shard.map.end()) return SeenCoin(it->second);
    }
    return base->PeekCoin(outpoint);
}

bool CoinsStore::HaveCoin(const COutPoint& outpoint) const
{
    return Lookup(outpoint).has_value();
}

bool CoinsStore::HaveCoinInCache(const COutPoint& outpoint) const
{
    Shard& shard{ShardFor(outpoint)};
    LOCK(shard.mutex);
    const auto it{shard.map.find(outpoint)};
    return it != shard.map.end() && !it->second.coin.IsSpent();
}

static const Coin STORE_EMPTY_COIN;

const Coin& CoinsStore::AccessCoin(const COutPoint& outpoint) const
{
    // A copy, good until the thread's next call: a reference into a shard would not do (see the class).
    std::optional<Coin> coin{Lookup(outpoint)};
    if (!coin) return STORE_EMPTY_COIN;
    static thread_local Coin t_coin;
    t_coin = std::move(*coin);
    return t_coin;
}

uint256 CoinsStore::GetBestBlock() const
{
    if (m_tier_block_hash.IsNull()) m_tier_block_hash = base->GetBestBlock();
    return m_tier_block_hash;
}

void CoinsStore::SetBestBlock(const uint256& block_hash)
{
    m_tier_block_hash = block_hash;
}

void CoinsStore::SetBestBlock(const uint256& block_hash, int height)
{
    m_tier_block_hash = block_hash;
    m_change_height = height;
    SetTierHeight(height);
}

// ---------------------------------------------------------------------------------------------------------
void CoinsStore::KeepWritten(std::vector<CoinWrite> restored)
{
    for (const CoinWrite& write : restored) {
        Shard& shard{ShardFor(write.outpoint)};
        LOCK(shard.mutex);
        if (auto it{shard.map.find(write.outpoint)}; it != shard.map.end()) Transit(it->second, Event::KeptWritten);
    }
    m_restored.insert(m_restored.end(), std::make_move_iterator(restored.begin()), std::make_move_iterator(restored.end()));
}

// ---------------------------------------------------------------------------------------------------------
// Changes

void CoinsStore::AddCoin(const COutPoint& outpoint, Coin&& coin, bool possible_overwrite)
{
    assert(!coin.IsSpent());
    if (coin.out.scriptPubKey.IsUnspendable()) return;
    Shard& shard{ShardFor(outpoint)};
    LOCK(shard.mutex);
    auto [it, inserted]{shard.map.try_emplace(outpoint)};
    if (inserted) Resized(shard);
    Entry& entry{it->second};
    const State from{StateOf(entry)};
    bool fresh{false};
    if (!possible_overwrite) {
        if (!entry.coin.IsSpent()) {
            throw std::logic_error("Attempted to overwrite an unspent coin (when possible_overwrite is false)");
        }
        // A spent entry that is dirty records a spentness not yet in the database; re-adding the coin must
        // not make it fresh, or a later spend would drop the entry and the database would keep the coin.
        // A new entry, or a spent entry already written, can be fresh. (See CCoinsViewCache::AddCoin.)
        fresh = !entry.dirty;
    }
    if (!inserted) {
        Detach(shard, *it);
        SubUsage(shard, entry.coin.DynamicMemoryUsage());
    }
    entry.coin = std::move(coin);
    Transit(entry, from, fresh ? Event::ConfirmedNew : Event::ConfirmedWrite, TagFor(entry.coin));
    AddUsage(shard, entry.coin.DynamicMemoryUsage());
    Attach(shard, *it);
}

void CoinsStore::EmplaceCoinInternalDANGER(const COutPoint& outpoint, Coin&& coin)
{
    Shard& shard{ShardFor(outpoint)};
    LOCK(shard.mutex);
    const size_t usage{coin.DynamicMemoryUsage()};
    auto [it, inserted]{shard.map.try_emplace(outpoint)};
    if (inserted) Resized(shard);
    if (!inserted) return;
    Entry& entry{it->second};
    entry.coin = std::move(coin);
    Transit(entry, coins_entry::ABSENT, entry.coin.IsSpent() ? Event::ConfirmedSpend : Event::ConfirmedWrite, m_change_height);
    AddUsage(shard, usage);
    Attach(shard, *it);
}

//! Whether `coin` is the one a block validated its spend against. Two coins of an outpoint are told
//! apart by where they were made: the outputs of a transaction made twice are the same.
template <typename AnyCoin>
static bool IsCoin(const AnyCoin& coin, const Coin& known)
{
    return coin.nHeight == known.nHeight && coin.fCoinBase == known.fCoinBase;
}

void CoinsStore::SpendEntryAt(Shard& shard, Map::iterator it, int height)
{
    AssertLockHeld(shard.mutex);
    Entry& entry{it->second};
    const State from{StateOf(entry)};
    Detach(shard, *it);
    SubUsage(shard, entry.coin.DynamicMemoryUsage());
    if (entry.fresh) {
        EraseDetached(shard, it);
        return;
    }
    entry.coin.Clear();
    Transit(entry, from, Event::ConfirmedSpend, height);
    Attach(shard, *it);
}

void CoinsStore::CreateLocked(Shard& shard, const COutPoint& outpoint, uint64_t hash, Coin&& coin, const CScript* script, bool possible_overwrite, int height)
{
    AssertLockHeld(shard.mutex);
    auto [it, inserted]{shard.map.try_emplace(outpoint, hash)};
    Entry& entry{it->second};
    Assert(inserted || entry.coin.IsSpent() || possible_overwrite);
    if (inserted) Resized(shard);
    const State from{StateOf(entry)};
    // As AddCoin().
    const bool fresh{!possible_overwrite && !entry.dirty};
    // A dirty spent entry is an erase the database does not have yet (the output of a disconnected block
    // that the new branch makes again): it is owed under the create and the entry stays at its height, so
    // that a run bounded below the create writes the erase (CollectChunk()). Filed at the create's height
    // the erase would be lost if the create were taken back before a run carried it.
    const bool owes{!inserted && entry.dirty && entry.coin.IsSpent() && entry.height < height};
    // A coin made below `height` is one put back where a commit is taken back. As where a disconnect puts
    // one back (TagFor()): made above the settled height it is not in the database, and it is filed under
    // the height it was made at, so that no run bounded between the two settles the database without it.
    const int made{static_cast<int>(coin.nHeight)};
    const int filed{made < height && made > m_written_height ? made : height};
    if (!inserted) {
        Detach(shard, *it);
        SubUsage(shard, entry.coin.DynamicMemoryUsage());
    }
    entry.coin = std::move(coin);
    if (script) entry.coin.out.scriptPubKey = *script;
    Transit(entry, from, owes ? Event::CoinOverErase : fresh ? Event::ConfirmedNew : Event::ConfirmedWrite, filed);
    AddUsage(shard, entry.coin.DynamicMemoryUsage());
    Attach(shard, *it);
}

uint32_t CoinsStore::ApplyConfirmed(std::vector<Change>&& changes, int height, ConfirmedReport* report, const std::function<const Coin*(const COutPoint&)>& base_coin)
{
    // No run is in flight: a run writes the tier as of its head, and a change it has not collected yet would be
    // lost to it (a fresh coin's spend erases the coin the head still has; a restored coin's erase would be
    // written under a head that has the coin). The chainstate lands a run before it commits (SettleWriteOut()).
    Assert(!m_run_in_flight);
    std::erase_if(changes, [](const Change& change) { return !change.coin->IsSpent() && (change.script ? *change.script : change.coin->out.scriptPubKey).IsUnspendable(); });
    if (changes.empty()) return 0;
    std::vector<uint32_t> shard_of;
    std::vector<uint64_t> hashes;
    shard_of.reserve(changes.size());
    hashes.reserve(changes.size());
    for (const Change& change : changes) {
        shard_of.push_back(ShardIndex(*change.outpoint));
        hashes.push_back(KeyHash(*change.outpoint));
    }
    uint32_t conflicts{0};
    std::vector<const Change*> misses;
    VisitShards(shard_of, [&](Shard& shard, std::span<const uint32_t> items) EXCLUSIVE_LOCKS_REQUIRED(shard.mutex) {
        shard.map.PrefetchAll(hashes, items);
        for (const uint32_t i : items) {
            const Change& change{changes[i]};
            const COutPoint& outpoint{*change.outpoint};
            if (change.coin->IsSpent()) {
                const auto it{shard.map.find(outpoint, hashes[i])};
                if (it == shard.map.end()) {
                    misses.push_back(&change);
                } else if (it->second.coin.IsSpent() || (change.known_coin && !IsCoin(it->second.coin, *change.coin))) {
                    ++conflicts;
                    if (report) report->conflicted.push_back(change.outpoint);
                } else {
                    SpendEntryAt(shard, it, height);
                }
                continue;
            }
            const auto old{shard.map.find(outpoint, hashes[i])};
            const bool over{old != shard.map.end() && !old->second.coin.IsSpent()};
            if (over && !change.possible_overwrite) {
                ++conflicts;
                if (report) report->conflicted.push_back(change.outpoint);
                continue;
            }
            if (over && report) report->overwritten.emplace_back(change.outpoint, old->second.coin);
            if (old == shard.map.end() && change.possible_overwrite && report) report->unread.push_back(change.outpoint);
            CreateLocked(shard, outpoint, hashes[i], std::move(*change.coin), change.script, change.possible_overwrite, height);
        }
    });
    // Coins the tier no longer holds (evicted since the block read them): from the base, then spent.
    for (const Change* change : misses) {
        const Coin* const given{base_coin ? base_coin(*change->outpoint) : nullptr};
        const std::optional<Coin> coin{given ? std::optional<Coin>{*given} : Lookup(*change->outpoint)};
        if (!coin || (change->known_coin && !IsCoin(*coin, *change->coin))) {
            ++conflicts;
            if (report) report->conflicted.push_back(change->outpoint);
            continue;
        }
        Shard& shard{ShardFor(*change->outpoint)};
        LOCK(shard.mutex);
        // An eviction may have taken the entry again since the lookup brought it in (a run's completion evicts
        // clean entries): the database's coin is put back first, as the lookup did.
        auto [it, inserted]{shard.map.try_emplace(*change->outpoint)};
        if (inserted) {
            Resized(shard);
            it->second.coin = *coin;
            Transit(it->second, coins_entry::ABSENT, Event::BaseCopy, static_cast<int>(coin->nHeight));
            AddUsage(shard, it->second.coin.DynamicMemoryUsage());
            Attach(shard, *it);
        }
        if (it->second.coin.IsSpent()) {
            ++conflicts;
            if (report) report->conflicted.push_back(change->outpoint);
            continue;
        }
        SpendEntryAt(shard, it, height);
    }
    return conflicts;
}

void CoinsStore::SpendEntry(Shard& shard, Map::iterator it, Coin* moveto)
{
    AssertLockHeld(shard.mutex);
    // As CCoinsViewCache::SpendCoin: an entry that is present is "spent" whether or not it still holds a
    // coin, so that a caller spending an already spent entry gets the same answer from both.
    Entry& entry{it->second};
    const State from{StateOf(entry)};
    Detach(shard, *it);
    SubUsage(shard, entry.coin.DynamicMemoryUsage());
    if (moveto) *moveto = std::move(entry.coin);
    if (entry.fresh) {
        // Never written: the database has nothing to erase.
        EraseDetached(shard, it);
        return;
    }
    entry.coin.Clear();
    // Under the change height, or where an earlier change the database does not have is filed lower
    // (coins_entry::Height::Oldest).
    Transit(entry, from, Event::ConfirmedSpend, m_change_height);
    Attach(shard, *it);
}

bool CoinsStore::SpendCoin(const COutPoint& outpoint, Coin* moveto)
{
    Shard& shard{ShardFor(outpoint)};
    {
        LOCK(shard.mutex);
        if (auto it{shard.map.find(outpoint)}; it != shard.map.end()) {
            SpendEntry(shard, it, moveto);
            return true;
        }
    }
    // Not in the tier: bring the coin in from the base, then spend it; nothing to spend if the base lacks it.
    if (!Lookup(outpoint)) return false;
    LOCK(shard.mutex);
    auto it{shard.map.find(outpoint)};
    if (it == shard.map.end()) return false;
    SpendEntry(shard, it, moveto);
    return true;
}

void CoinsStore::Uncache(const COutPoint& outpoint)
{
    Shard& shard{ShardFor(outpoint)};
    LOCK(shard.mutex);
    auto it{shard.map.find(outpoint)};
    if (it == shard.map.end() || it->second.dirty) return;
    TraceUncache(outpoint, it->second.coin);
    EraseEntry(shard, it);
}

void CoinsStore::BatchWrite(CoinsViewCacheCursor& cursor, const uint256& block_hash)
{
    // As for a commit (ApplyConfirmed()): a disconnected block's changes never meet a run in flight.
    Assert(!m_run_in_flight);
    for (auto it{cursor.Begin()}; it != cursor.End(); it = cursor.NextAndMaybeErase(*it)) {
        if (!it->second.IsDirty()) continue;
        const COutPoint& outpoint{it->first};
        Shard& shard{ShardFor(outpoint)};
        LOCK(shard.mutex);
        auto [ours, inserted]{shard.map.try_emplace(outpoint)};
        if (inserted) Resized(shard);
        Entry& entry{ours->second};
        const State from{StateOf(entry)};
        Event event;
        if (inserted) {
            if (it->second.IsFresh() && it->second.coin.IsSpent()) {
                // Created and spent inside the child: nothing to record.
                EraseDetached(shard, ours);
                continue;
            }
            entry.coin = cursor.WillErase(*it) ? std::move(it->second.coin) : it->second.coin;
            event = entry.coin.IsSpent() ? Event::ConfirmedSpend : it->second.IsFresh() ? Event::ConfirmedNew : Event::ConfirmedWrite;
        } else {
            if (it->second.IsFresh() && !entry.coin.IsSpent()) {
                throw std::logic_error("FRESH flag misapplied to coin that exists in parent cache");
            }
            if (entry.fresh && it->second.coin.IsSpent()) {
                // Never written and now spent: drop it.
                EraseEntry(shard, ours);
                continue;
            }
            Detach(shard, *ours);
            SubUsage(shard, entry.coin.DynamicMemoryUsage());
            entry.coin = cursor.WillErase(*it) ? std::move(it->second.coin) : it->second.coin;
            // Not made fresh: the entry may be in the database already (see CCoinsViewCache::BatchWrite).
            event = entry.coin.IsSpent() ? Event::ConfirmedSpend : Event::BatchOverwrite;
        }
        Transit(entry, from, event, entry.coin.IsSpent() ? m_change_height : TagFor(entry.coin));
        AddUsage(shard, entry.coin.DynamicMemoryUsage());
        Attach(shard, *ours);
    }
    SetBestBlock(block_hash);
}

template <typename Work>
void CoinsStore::VisitShards(const std::vector<uint32_t>& shard_of, Work work) const
{
    const size_t shards{m_shards.size()};
    // A counting sort of the items by shard: shard s has the items order[starts[s]] to order[starts[s + 1]].
    std::vector<uint32_t> starts(shards + 1, 0);
    for (const uint32_t s : shard_of) ++starts[s + 1];
    for (size_t s{0}; s < shards; ++s) starts[s + 1] += starts[s];
    std::vector<uint32_t> order(shard_of.size());
    {
        std::vector<uint32_t> next(starts.begin(), starts.end() - 1);
        for (uint32_t i{0}; i < shard_of.size(); ++i) order[next[shard_of[i]]++] = i;
    }
    const auto items{[&](size_t s) { return std::span<const uint32_t>{order}.subspan(starts[s], starts[s + 1] - starts[s]); }};
    std::vector<uint32_t> busy;
    for (size_t s{0}; s < shards; ++s) {
        if (starts[s] == starts[s + 1]) continue;
        Shard& shard{m_shards[s]};
        TRY_LOCK(shard.mutex, lock);
        if (!lock) {
            busy.push_back(s);
            continue;
        }
        work(shard, items(s));
    }
    for (const uint32_t b : busy) {
        Shard& shard{m_shards[b]};
        LOCK(shard.mutex);
        work(shard, items(b));
    }
}

std::vector<std::pair<COutPoint, Coin>> CoinsStore::Fetch(std::vector<COutPoint>&& outpoints) const
{
    std::vector<uint32_t> shard_of;
    std::vector<uint64_t> hashes;
    shard_of.reserve(outpoints.size());
    hashes.reserve(outpoints.size());
    for (const COutPoint& outpoint : outpoints) {
        shard_of.push_back(ShardIndex(outpoint));
        hashes.push_back(KeyHash(outpoint));
    }
    std::vector<std::pair<COutPoint, Coin>> found;
    found.reserve(outpoints.size());
    VisitShards(shard_of, [&](Shard& shard, std::span<const uint32_t> items) EXCLUSIVE_LOCKS_REQUIRED(shard.mutex) {
        shard.map.PrefetchAll(hashes, items);
        for (const uint32_t i : items) {
            const COutPoint& outpoint{outpoints[i]};
            const auto it{shard.map.find(outpoint, hashes[i])};
            if (it == shard.map.end() || it->second.coin.IsSpent()) continue;
            Reference(it->second);
            found.emplace_back(outpoint, it->second.coin);
        }
    });
    return found;
}

void CoinsStore::MayHave(std::span<const COutPoint> outpoints, std::vector<uint8_t>& may) const
{
    may.assign(outpoints.size(), 0);
    std::vector<uint32_t> shard_of;
    std::vector<uint64_t> hashes;
    shard_of.reserve(outpoints.size());
    hashes.reserve(outpoints.size());
    for (const COutPoint& outpoint : outpoints) {
        shard_of.push_back(ShardIndex(outpoint));
        hashes.push_back(KeyHash(outpoint));
    }
    VisitShards(shard_of, [&](Shard& shard, std::span<const uint32_t> items) EXCLUSIVE_LOCKS_REQUIRED(shard.mutex) {
        shard.map.PrefetchAll(hashes, items);
        for (const uint32_t i : items) {
            if (shard.map.find(outpoints[i], hashes[i]) != shard.map.end()) may[i] = 1;
        }
    });
    // With no shard held, as Lookup() reads the base. A coin the base has may have been spent since: the
    // caller asks again, one by one.
    for (size_t i{0}; i < outpoints.size(); ++i) {
        if (!may[i] && base->HaveCoin(outpoints[i])) may[i] = 1;
    }
}

// ---------------------------------------------------------------------------------------------------------
// Sizes, eviction and the write-out

unsigned int CoinsStore::GetCacheSize() const
{
    return static_cast<unsigned int>(SumTotals(&Totals::entries));
}

size_t CoinsStore::GetDirtyCount() const noexcept
{
    return SumTotals(&Totals::dirty);
}

size_t CoinsStore::DynamicMemoryUsage() const
{
    // The maps' nodes at their size (NodeBytes()) and bucket arrays, the shards' arrays of dirty span sums, and
    // the coins: summed as they change (Totals), since the connector asks for every block
    // it publishes and must neither queue behind an applier holding a shard nor read every shard.
    return SumTotals(&Totals::bytes);
}

int CoinsStore::BoundForBytes(size_t bytes, int tip_height) const
{
    // Merge the shards' byte sums span by span and walk them from the oldest up; in the span the bound
    // lies in, the entries' own heights find it.
    std::vector<int64_t> merged;
    for (const Shard& shard : m_shards) {
        LOCK(shard.mutex);
        if (merged.size() < shard.dirty_spans.size()) merged.resize(shard.dirty_spans.size());
        for (size_t span{0}; span < shard.dirty_spans.size(); ++span) merged[span] += shard.dirty_spans[span].bytes;
    }
    int64_t freed{0};
    for (size_t span{0}; span < merged.size(); ++span) {
        if (freed + merged[span] < static_cast<int64_t>(bytes)) {
            freed += merged[span];
            continue;
        }
        // The span's entries by height, from a walk of the maps of the shards that have any.
        std::vector<int64_t> by_height(size_t{1} << DIRTY_SPAN_BITS);
        for (const Shard& shard : m_shards) {
            LOCK(shard.mutex);
            if (span >= shard.dirty_spans.size() || shard.dirty_spans[span].bytes == 0) continue;
            for (const auto& [outpoint, entry] : shard.map) {
                if (entry.dirty && SpanOf(entry.height) == span) by_height[entry.height & ((1 << DIRTY_SPAN_BITS) - 1)] += DirtyBytes(entry);
            }
        }
        for (size_t i{0}; i < by_height.size(); ++i) {
            freed += by_height[i];
            if (by_height[i] != 0 && freed >= static_cast<int64_t>(bytes)) return std::min<int>(static_cast<int>((span << DIRTY_SPAN_BITS) + i), tip_height);
        }
        // The span lost entries between the two passes: the bound lies further up.
    }
    return tip_height;
}

size_t CoinsStore::EvictClean(size_t target_bytes)
{
    // Round the shards, a batch of the map from where each one's sweep stands at a time, so that the shards
    // age together and no shard's lock is held for long. A clean entry with its mark set keeps its place
    // for one sweep; a shard with no clean entry is passed over.
    static constexpr size_t BATCH{256};
    size_t freed{0};
    bool progress{true};
    while (progress && DynamicMemoryUsage() > target_bytes) {
        progress = false;
        for (Shard& shard : m_shards) {
            if (DynamicMemoryUsage() <= target_bytes) break;
            LOCK(shard.mutex);
            if (shard.map.size() == shard.dirty_count) continue;
            progress = true;
            Map::iterator it{shard.map.from(shard.clock)};
            for (size_t n{0}; n < BATCH && shard.map.size() > shard.dirty_count; ++n) {
                if (it == shard.map.end()) it = shard.map.begin();
                Map::iterator next{it};
                ++next;
                Entry& entry{it->second};
                if (!entry.dirty) {
                    if (entry.referenced) {
                        entry.referenced = false;
                    } else {
                        freed += static_cast<size_t>(DirtyBytes(entry));
                        EraseEntry(shard, it);
                    }
                }
                it = next;
            }
            shard.clock = it == shard.map.end() ? 0 : it.bucket();
        }
    }
    return freed;
}

std::unique_ptr<CoinsStore::WriteOutRun> CoinsStore::BeginWriteOut(int bound_height, int head_height, const uint256& new_head, bool final)
{
    // After a run or flush that failed nothing is written (FlushFailed()), whoever starts the run.
    if (m_flush_failed) throw std::runtime_error{"the coins are not written: an earlier flush failed"};
    Assert(!m_run_in_flight);
    if (!final) {
        bool any{false};
        for (const Shard& shard : m_shards) {
            LOCK(shard.mutex);
            // The shard's first span with entries holds its lowest heights: below the bound's span any of them
            // is under the bound, above it none; in it the entries' own heights say.
            for (size_t span{0}; span < shard.dirty_spans.size(); ++span) {
                if (shard.dirty_spans[span].bytes == 0) continue;
                if (span < SpanOf(bound_height)) {
                    any = true;
                } else if (span == SpanOf(bound_height)) {
                    for (const auto& [outpoint, entry] : shard.map) {
                        if (entry.dirty && entry.height <= bound_height) {
                            any = true;
                            break;
                        }
                    }
                }
                break;
            }
            if (any) break;
        }
        // Nothing to write and no head to move: the database already holds the bound.
        if (!any && bound_height <= m_written_height) return nullptr;
    }
    // Changes made below the written head (a disconnect's) are tagged with the fork height while the
    // database holds the state of a block above it. Stopping at a bound would write some of them under a
    // marker whose replay starts from the written head, which restores nothing of what they took.
    final = final || m_fell_below_written;
    auto run{std::make_unique<WriteOutRun>()};
    run->restored = TakeRestored();
    run->new_head = new_head;
    run->old_head = m_written_head;
    run->bound_height = final ? head_height : bound_height;
    run->head_height = head_height;
    run->final = final;
    // Until every shard has been collected the bound stands in for the settled height (TagFor()).
    m_inflight_settled_height = run->bound_height;
    m_run_in_flight = true;
    // A final run carries every change made so far; SetTierHeight() notes a disconnect below its head.
    if (final) m_fell_below_written = false;
    return run;
}

//! About what a change adds to a database batch: the key and the framing, and for a coin its height, its
//! amount and its script. Within a few percent for the common scripts and seldom below, so that a chunk
//! collected up to the batch size is one batch.
static uint64_t BatchBytes(const CoinWrite& write)
{
    return write.coin ? 45 + write.coin->out.scriptPubKey.size() : 38;
}

//! Buffers of SortWrites(), kept between calls.
struct WriteSorter {
    std::vector<std::pair<uint64_t, uint32_t>> order;
    std::vector<CoinWrite> sorted;
};

/**
 * Put the changes from `from` on in the order of the database's keys, descending: by the leading bytes
 * of the txid and, where they are equal, by the outpoint (the order of the keys, but among the outputs
 * past the 16511th of one transaction, whose index is a byte longer in the key). The database finds the
 * place of a key next to the last one it took faster than the place of a key anywhere, and a batch it
 * cuts at its batch size (CCoinsViewDB::WriteRun()) is then cut along its keys.
 */
static void SortWrites(std::vector<CoinWrite>& writes, size_t from, WriteSorter& sorter)
{
    const size_t count{writes.size() - from};
    if (count < 2) return;
    sorter.order.clear();
    sorter.order.reserve(count);
    for (size_t i{0}; i < count; ++i) {
        sorter.order.emplace_back(ReadBE64(writes[from + i].outpoint.hash.ToUint256().data()), static_cast<uint32_t>(i));
    }
    std::sort(sorter.order.begin(), sorter.order.end(), [&](const auto& a, const auto& b) {
        if (a.first != b.first) return a.first > b.first;
        return writes[from + b.second].outpoint < writes[from + a.second].outpoint;
    });
    sorter.sorted.clear();
    sorter.sorted.reserve(count);
    for (const auto& [prefix, i] : sorter.order) sorter.sorted.push_back(std::move(writes[from + i]));
    std::move(sorter.sorted.begin(), sorter.sorted.end(), writes.begin() + from);
    sorter.sorted.clear();
}

void CoinsStore::CollectChunk(Shard& shard, WriteOutRun& run, uint64_t limit, uint64_t& size, std::vector<CoinWrite>& writes, std::vector<Node*>& nodes) const
{
    AssertLockHeld(shard.mutex);
    // A walk of the whole map. A chunk is written and settled before the shard is walked again, so a walk
    // that starts over finds what it took clean or gone; one that ends with the chunk full has seen part of
    // the changes left behind, and the walk that finishes the shard sees them all.
    if (shard.dirty_count == 0) return;
    // The oldest change this shard keeps for a later run.
    int oldest_left{std::numeric_limits<int>::max()};
    {
        for (Map::iterator it{shard.map.begin()}; it != shard.map.end(); ++it) {
            Node* node{&*it};
            Entry& entry{node->second};
            if (!entry.dirty || entry.collected) continue;
            if (!run.final && entry.height > run.bound_height) {
                oldest_left = std::min<int>(oldest_left, entry.height);
                continue;
            }
            if (size >= limit) {
                run.oldest_left = std::min<int>(run.oldest_left, oldest_left);
                return;
            }
            if (const int created{static_cast<int>(entry.coin.nHeight)}; entry.owed && !run.final && created > run.bound_height) {
                // The coin is not the run's to write, the erase under it is: left behind, it would be missing
                // from a database whose marker closes at the run's head. Written, the entry owes nothing but
                // its coin, at the coin's height, for a later run.
                writes.push_back({node->first, std::nullopt});
                size += BatchBytes(writes.back());
                ++run.count;
                Detach(shard, *node);
                Transit(entry, Event::RunWritesOwed, created);
                Attach(shard, *node);
                run.oldest_left = std::min<int>(run.oldest_left, created);
                continue;
            }
            writes.push_back({node->first, entry.coin.IsSpent() ? std::nullopt : std::make_optional(entry.coin)});
            nodes.push_back(node);
            size += BatchBytes(writes.back());
            ++run.count;
            run.bytes += static_cast<size_t>(DirtyBytes(entry));
            // Not fresh from here on (StreamWriteOut()).
            Transit(entry, Event::RunCollects);
        }
    }
    // The oldest change left behind in this shard: the database holds the state one below it once the run
    // has landed.
    run.oldest_left = std::min<int>(run.oldest_left, oldest_left);
}

void CoinsStore::SettleNode(Shard& shard, Node& node) const
{
    AssertLockHeld(shard.mutex);
    Entry& entry{node.second};
    if (!entry.collected) return; // Changed since: stays dirty with its new change.
    Detach(shard, node);
    // Clean, and what the database holds is anyone's.
    Transit(entry, Event::RunLands);
    if (entry.coin.IsSpent()) {
        // The erase is in the database; the entry has nothing left to say.
        EraseEntry(shard, node);
        return;
    }
    Attach(shard, node);
}

void CoinsStore::StreamWriteOut(WriteOutRun& run, const std::function<uint256(int)>& block_at, uint64_t batch_bytes)
{
    // At least one change to a chunk, whatever the limit.
    const uint64_t limit{std::max<uint64_t>(batch_bytes, 1)};
    uint64_t size{0};
    std::vector<CoinWrite> writes;
    std::vector<Node*> nodes;
    // The shards the chunk took entries from, each with the offset of its first node. A chunk fills across
    // shards, so that a small run is a few database batches and not one for every shard.
    std::vector<std::pair<Shard*, size_t>> spans;
    WriteSorter sorter;
    const auto write_chunk{[&] {
        if (writes.empty()) return;
        // Nothing below goes by a write's place: the nodes are settled by their own list.
        SortWrites(writes, 0, sorter);
        // Under the run's two heads (BeginWriteOut()).
        base->WriteRun(writes, run.new_head, run.old_head, /*final=*/false);
        ++run.chunks;
        for (size_t i{0}; i < spans.size(); ++i) {
            Shard& shard{*spans[i].first};
            const size_t end{i + 1 < spans.size() ? spans[i + 1].second : nodes.size()};
            LOCK(shard.mutex);
            for (size_t n{spans[i].second}; n < end; ++n) SettleNode(shard, *nodes[n]);
            ++shard.settles;
        }
        writes.clear();
        nodes.clear();
        spans.clear();
        size = 0;
    }};
    // From the highest shard down, for the reason given in Flush().
    for (size_t index{m_shards.size()}; index-- > 0;) {
        Shard& shard{m_shards[index]};
        while (true) {
            const size_t before{nodes.size()};
            WITH_LOCK(shard.mutex, CollectChunk(shard, run, limit, size, writes, nodes));
            if (nodes.size() > before) spans.emplace_back(&shard, before);
            // Below the limit the shard has nothing more for this run, and the chunk goes on to the next.
            if (size < limit) break;
            write_chunk();
        }
    }
    write_chunk();
    run.settled_height = std::min(run.head_height, run.oldest_left == std::numeric_limits<int>::max() ? run.head_height : run.oldest_left - 1);
    Assume(run.final || run.settled_height >= run.bound_height);
    if (run.final) {
        base->WriteRun({}, run.new_head, run.old_head, /*final=*/true);
        return;
    }
    // The database now holds the settled head's state, with every outpoint the run wrote at its value in
    // new_head: the pair the next run starts from. On disk before the tier counts the run as written, so
    // that the old head recorded there is never below what pruning goes by.
    run.settled_head = block_at(run.settled_height);
    base->WriteRun({}, run.new_head, run.settled_head, /*final=*/false);
}

void CoinsStore::CommitWriteOut(const WriteOutRun& run)
{
    Assert(m_run_in_flight);
    Assert(run.final || !run.settled_head.IsNull());
    m_written_head = run.final ? run.new_head : run.settled_head;
    m_written_height = run.settled_height;
    m_run_in_flight = false;
}

void CoinsStore::AbandonWriteOut()
{
    // What the run was to carry is unwritten still, a final run's changes from below the written head too.
    m_fell_below_written = true;
    m_run_in_flight = false;
    // Nothing may close the run's marker: no run and no flush follows, and the next start replays from the
    // old head.
    m_flush_failed = true;
}

//! What the workers of a flush share.
struct CoinsStore::Wipe {
    const uint256 best;
    const uint256 old_head;
    //! The size of a batch.
    const uint64_t limit;
    //! A worker's write failed: the others take no further shard.
    std::atomic<bool> failed{false};
};

//! The most workers a flush has. The database takes one batch at a time: these keep it busy, and more
//! would wait for it.
static constexpr size_t MAX_WIPE_WORKERS{4};

//! A worker of a flush: its shards, the batch it is filling, and the shards whose changes are in it.
struct CoinsStore::WipeWorker {
    //! The worker's shards, [begin, end), which it takes from the highest down.
    size_t begin{0};
    size_t end{0};
    //! Called once, when the worker's first batch is in the database.
    std::function<void()> after_first_batch;
    //! The batch: shard after shard from the highest down, each in the descending order of the keys.
    std::vector<CoinWrite> writes;
    WriteSorter sorter;
    //! What `writes` comes to as a database batch (BatchBytes()).
    uint64_t size{0};
    //! Shards the worker has emptied, locked until the batch with the last of their changes has landed.
    std::vector<Shard*> held;
    std::exception_ptr error;
};

void CoinsStore::WriteWipeBatch(const Wipe& wipe, WipeWorker& worker)
{
    if (!worker.writes.empty()) {
        // Under the flush's two heads, as a run's chunk (BeginWriteOut()).
        base->WriteRun(worker.writes, wipe.best, wipe.old_head, /*final=*/false);
        worker.writes.clear();
        worker.size = 0;
        if (worker.after_first_batch) std::exchange(worker.after_first_batch, nullptr)();
    }
    for (Shard* shard : worker.held) shard->mutex.unlock();
    worker.held.clear();
}

uint64_t CoinsStore::WipeShard(Shard& shard, WipeWorker& worker)
{
    AssertLockHeld(shard.mutex);
    // The span sums go with the entries.
    FreeDirtySpans(shard);
    MyTotals().dirty.fetch_sub(static_cast<int64_t>(shard.dirty_count), std::memory_order_relaxed);
    shard.dirty_count = 0;
    shard.clock = 0;
    uint64_t added{0};
    // Where the shard's changes begin in the batch.
    const size_t from{worker.writes.size()};
    const auto add{[&](CoinWrite&& write) {
        const uint64_t bytes{BatchBytes(write)};
        added += bytes;
        worker.size += bytes;
        worker.writes.push_back(std::move(write));
    }};
    for (auto it{shard.map.begin()}; it != shard.map.end();) {
        Entry& entry{it->second};
        // As EraseEntry(), with the coin's usage taken before the coin is moved out.
        const size_t usage{entry.coin.DynamicMemoryUsage()};
        if (entry.dirty) add({it->first, entry.coin.IsSpent() ? std::nullopt : std::make_optional(std::move(entry.coin))});
        SubUsage(shard, usage);
        it = shard.map.erase(it);
    }
    // The map hands its entries out in the order of their hashes, and no batch ends during the walk for
    // that reason: what the walk has passed at any point is spread over the shard's whole range of keys,
    // and so is the rest, and two batches with a part each would cover the same range.
    SortWrites(worker.writes, from, worker.sorter);
    shard.map.rehash(0);
    Resized(shard);
    ++shard.settles;
    return added;
}

//! The most shards a flush's worker keeps locked for one batch. Shards with few changes would otherwise
//! all end up in one batch, and ThreadSanitizer's deadlock detector follows no more than 64 mutexes held
//! by a thread. A batch that ends for this reason is whole shards like any other.
static constexpr size_t MAX_BATCH_SHARDS{32};

void CoinsStore::RunWipeWorker(Wipe& wipe, WipeWorker& worker)
{
    Shard* walked{nullptr};
    try {
        for (size_t index{worker.end}; index-- > worker.begin && !wipe.failed.load(std::memory_order_relaxed);) {
            if (worker.held.size() >= MAX_BATCH_SHARDS) WriteWipeBatch(wipe, worker);
            Shard& shard{m_shards[index]};
            // The shard stays locked beyond its walk, among the others the batch took changes from, and
            // they are unlocked together and not in the reverse order: the lock order checks know of the
            // shard for its walk only.
            EnterCritical("shard.mutex", __FILE__, __LINE__, static_cast<ShardMutex::unique_lock::mutex_type*>(&shard.mutex));
            shard.mutex.lock();
            walked = &shard;
            const uint64_t added{WipeShard(shard, worker)};
            walked = nullptr;
            LeaveCritical();
            worker.held.push_back(&shard);
            // A batch ends between two shards when it is full, or when the next shard, taken to be as
            // large as this one, would not fit. A shard larger than a batch is a batch of its own, which
            // the database cuts at its batch size along the keys (CCoinsViewDB::WriteRun()).
            if (worker.size >= wipe.limit || worker.size + added > wipe.limit) WriteWipeBatch(wipe, worker);
        }
        WriteWipeBatch(wipe, worker);
    } catch (const std::runtime_error&) {
        // The base's write failed. Flush() throws it on once every worker is through.
        worker.error = std::current_exception();
        wipe.failed.store(true, std::memory_order_relaxed);
        if (walked) {
            LeaveCritical();
            walked->mutex.unlock();
        }
        for (Shard* shard : worker.held) shard->mutex.unlock();
        worker.held.clear();
    }
}

void CoinsStore::Flush(bool /*reallocate_cache*/)
{
    // One pass: a shard's changes are written as its entries are taken out of it, the coins moved rather
    // than copied. A shard stays locked until the batch with the last of its changes is in the database: a
    // reader of the shard (a block's validation, when the flush is forced on a running node) waits for the
    // write rather than finding a coin in neither place. Every call of WriteRun() names the same two heads,
    // and a last one closes the marker.
    //
    // The batches are ranges of the database's keys that do not overlap (SetBaseWrites()). The shards are
    // taken from the highest down, because the database's marker goes out with the first batch and its
    // head-blocks key sorts above every coin's: that batch then covers the top of the range, not all of
    // it. The marker's other keys are in that first batch and in the last, tiny one that closes it, and in
    // none between (CCoinsViewDB::WriteRun()). With several workers each has a range of neighbouring
    // shards of its own, and all but the one with the highest range start once its first batch, the
    // marker's, is in the database.
    const uint256 best{GetBestBlock()};
    const int head_height{m_tier_height >= 0 ? m_tier_height : m_change_height};
    // See below: the shutdown's flush after one that failed fails as well, and writes nothing.
    if (m_flush_failed) throw std::runtime_error{"the coins are not written: an earlier flush failed"};
    // The restored coins are the caller's (FlushStateToDisk() writes them); a wipe does not, and would lose them.
    Assert(m_restored.empty());
    Assert(!m_run_in_flight);
    m_run_in_flight = true;
    Wipe wipe{.best = best, .old_head = m_written_head, .limit = std::max<uint64_t>(m_batch_bytes, 1)};
    // A worker for every two batches there may be, so that each has batches to take turns with, and no
    // more than there are cores: a change is 38 bytes of a batch at least. A flush of a few batches, a
    // small cache's, has one.
    const uint64_t cores{std::max(1U, std::thread::hardware_concurrency())};
    const uint64_t wanted{m_base_concurrent ? GetDirtyCount() * uint64_t{38} / wipe.limit / 2 : 1};
    std::vector<WipeWorker> workers(std::clamp<uint64_t>(wanted, 1, std::min<uint64_t>({MAX_WIPE_WORKERS, cores, m_shards.size()})));
    for (size_t i{0}; i < workers.size(); ++i) {
        workers[i].begin = m_shards.size() * i / workers.size();
        workers[i].end = m_shards.size() * (i + 1) / workers.size();
    }
    // noexcept: tasks of the pool write into `wipe` and `workers` until they are waited for below, so nothing may
    // leave this scope early. A failed write is the worker's error (RunWipeWorker()); anything else, a failed
    // allocation, terminates.
    [&]() noexcept {
        // The worker of the highest range runs here, and hands the others to the helpers' pool once its first
        // batch is in. A range is written by whoever claims it first: a task of the pool, or this thread once
        // its own range is done. This thread may hold cs_main, and the pool's workers may be busy with tasks
        // waiting for it, so it never waits for a task that has not started: it takes the ranges no task has
        // claimed, and waits only for those a task is writing. A task that comes up once its range is taken
        // returns at once; what it shares with the flush outlives the call.
        struct Helpers {
            Mutex mutex;
            std::condition_variable cv;
            std::vector<bool> claimed GUARDED_BY(mutex);
            size_t running GUARDED_BY(mutex){0};
        };
        const auto helpers{std::make_shared<Helpers>()};
        WITH_LOCK(helpers->mutex, helpers->claimed.assign(workers.size(), false));
        WipeWorker& first{workers.back()};
        if (workers.size() > 1 && m_flush_helpers) {
            first.after_first_batch = [&] {
                for (size_t i{0}; i + 1 < workers.size(); ++i) {
                    // noexcept: a failed write is the worker's error; anything else terminates rather than
                    // leave the flush waiting for the range.
                    auto task{[this, helpers, i, &wipe, &workers]() noexcept {
                        {
                            LOCK(helpers->mutex);
                            if (helpers->claimed[i]) return;
                            helpers->claimed[i] = true;
                            ++helpers->running;
                        }
                        WAITED_FOR_UNDER_LOCK("a flush helper"); // a forced flush waits with cs_main held
                        RunWipeWorker(wipe, workers[i]);
                        LOCK(helpers->mutex);
                        --helpers->running;
                        helpers->cv.notify_all();
                    }};
                    // A pool that takes no task leaves the ranges to this thread.
                    if (!m_flush_helpers->Submit(std::move(task), /*front=*/true)) break;
                }
            };
        }
        RunWipeWorker(wipe, first);
        // The ranges no task has claimed (all of them where the highest range had nothing to write), one after
        // the other, from the highest down.
        for (size_t i{workers.size() - 1}; i-- > 0;) {
            {
                LOCK(helpers->mutex);
                if (helpers->claimed[i]) continue;
                helpers->claimed[i] = true;
            }
            RunWipeWorker(wipe, workers[i]);
        }
        WAIT_LOCK(helpers->mutex, lock);
        helpers->cv.wait(lock, [&]() EXCLUSIVE_LOCKS_REQUIRED(helpers->mutex) { return helpers->running == 0; });
    }();
    for (const WipeWorker& worker : workers) {
        if (!worker.error) continue;
        // The changes of the batch that failed are gone from memory and not in the database, which is
        // left under the marker of a run that did not end: the next start replays from the old head.
        // Nothing may close that marker. The run stays in flight for that reason, so that no write-out
        // run is started, and a later flush throws.
        m_flush_failed = true;
        std::rethrow_exception(worker.error);
    }
    try {
        // Every shard's changes are in the database: consistent with the best block again.
        base->WriteRun({}, best, wipe.old_head, /*final=*/true);
    } catch (const std::runtime_error&) {
        // As a worker's failure above: the marker stays open, and nothing writes again.
        m_flush_failed = true;
        throw;
    }
    m_written_head = best;
    m_written_height = head_height;
    m_fell_below_written = false;
    m_run_in_flight = false;
}

void CoinsStore::Sync()
{
    // As Flush(): after a flush or run that failed nothing is written, so that the database stays under the
    // marker of the failed one and the next start replays from its old head.
    if (m_flush_failed) throw std::runtime_error{"the coins are not written: an earlier flush failed"};
    // The restored coins are the caller's (FlushStateToDisk() writes them); BeginWriteOut() would take them
    // into a run that does not write them.
    Assert(m_restored.empty());
    const uint256 best{GetBestBlock()};
    const int head_height{m_tier_height >= 0 ? m_tier_height : m_change_height};
    auto run{BeginWriteOut(/*bound_height=*/head_height, head_height, /*new_head=*/best, /*final=*/true)};
    try {
        StreamWriteOut(*run, /*block_at=*/{}, m_batch_bytes);
    } catch (const std::runtime_error&) {
        AbandonWriteOut();
        throw;
    }
    CommitWriteOut(*run);
}

void CoinsStore::Reset() noexcept
{
    // The tier is never reset; a CCoinsViewCache child is. Nothing to do.
}

void CoinsStore::SanityCheck() const
{
    size_t total_bytes{0};
    size_t total_entries{0};
    size_t total_dirty{0};
    for (const Shard& shard : m_shards) {
        LOCK(shard.mutex);
        size_t usage{0};
        size_t dirty{0};
        std::map<size_t, int64_t> by_span;
        for (const auto& [outpoint, entry] : shard.map) {
            usage += entry.coin.DynamicMemoryUsage();
            if (entry.dirty) {
                ++dirty;
                by_span[SpanOf(entry.height)] += DirtyBytes(entry);
            }
            // The entry is in a state an entry may rest in (coins_entry::IsStored() says which and why).
            assert(coins_entry::IsStored(StateOf(entry)));
            // Each entry is counted as its state calls for.
            assert(entry.list == (entry.dirty ? LIST_DIRTY : LIST_CLEAN));
        }
        assert(dirty == shard.dirty_count);
        assert(shard.map.size() == shard.entries);
        assert(shard.map.bucket_count() == shard.buckets);
        total_bytes += NodeBytes() * shard.map.size() + memusage::MallocUsage(sizeof(void*) * shard.map.bucket_count()) + memusage::DynamicUsage(shard.dirty_spans) + usage;
        total_entries += shard.map.size();
        total_dirty += dirty;
        // The span sums are the dirty entries' bytes.
        assert(shard.dirty_spans.empty() == (dirty == 0));
        for (size_t span{0}; span < shard.dirty_spans.size(); ++span) {
            const auto it{by_span.find(span)};
            assert(shard.dirty_spans[span].bytes == (it == by_span.end() ? 0 : it->second));
            if (it != by_span.end()) by_span.erase(it);
        }
        assert(by_span.empty());
    }
    // The totals are the shards' figures summed. (The check is the owner's, made with no change under way.)
    assert(DynamicMemoryUsage() == total_bytes);
    assert(GetCacheSize() == total_entries);
    assert(SumTotals(&Totals::dirty) == total_dirty);
}

// ---------------------------------------------------------------------------------------------------------
// BlockApplyView

BlockApplyView::BlockApplyView(CoinsStore& store, util::NotNullSharedPtr<ThreadPool> thread_pool, bool deterministic)
    : CoinsViewOverlay(&store, std::move(thread_pool), deterministic), m_store{store}
{
}

BlockApplyView::~BlockApplyView()
{
    StopFetching(); // the prefetch workers read the view
}

void BlockApplyView::Prefetch(const CBlock& block)
{
    size_t inputs{0};
    for (size_t i{1}; i < block.vtx.size(); ++i) inputs += block.vtx[i]->vin.size();
    std::vector<COutPoint> outpoints;
    outpoints.reserve(inputs);
    // Kept by the thread: a list as long as the block has inputs is not allocated for every block.
    static thread_local std::vector<uint32_t> positions;
    positions.clear();
    for (size_t i{1}; i < block.vtx.size(); ++i) {
        for (const CTxIn& in : block.vtx[i]->vin) {
            outpoints.push_back(in.prevout);
            if (m_overlay) positions.push_back(static_cast<uint32_t>(i));
        }
    }
    if (m_overlay) {
        // An input a lower window block makes is not asked of the store, which does not have it yet. Its
        // coin goes into the view from where it is found, with no list between.
        m_overlay->FindCoins(outpoints, positions, m_overlay_height, [&](const COutPoint& outpoint, Coin&& coin) {
            const size_t usage{coin.DynamicMemoryUsage()};
            if (cacheCoins.try_emplace(outpoint, std::move(coin)).second) cachedCoinsUsage += usage;
        });
    }
    for (auto& [outpoint, coin] : m_store.Fetch(std::move(outpoints))) {
        const size_t usage{coin.DynamicMemoryUsage()};
        if (cacheCoins.try_emplace(outpoint, std::move(coin)).second) cachedCoinsUsage += usage;
    }
}

bool BlockApplyView::SpendCoin(const COutPoint& outpoint, Coin* moveto)
{
    Coin local;
    Coin& spent{moveto ? *moveto : local};
    if (!CoinsViewOverlay::SpendCoin(outpoint, &spent)) return false;
    // What the view keeps of a coin the base had is the spend. It keeps with it which coin that was, for
    // the store to compare at the apply (CoinsStore::Change::known_coin).
    if (const auto it{cacheCoins.find(outpoint)}; it != cacheCoins.end()) {
        it->second.coin.nHeight = spent.nHeight;
        it->second.coin.fCoinBase = spent.fCoinBase;
    }
    return true;
}

bool BlockApplyView::HaveAnyOutput(std::span<const CTransactionRef> txs) const
{
    // A block connected alone reads through its prefetch, and a block that is past its first transaction
    // reads its own outputs in the overlay: one by one.
    if (!m_overlay || m_own_limit != 0) return CoinsViewOverlay::HaveAnyOutput(txs);
    static thread_local std::vector<uint8_t> named;
    m_overlay->NamedIds(txs, m_overlay_height, named);
    std::vector<COutPoint> outpoints;
    for (size_t i{0}; i < txs.size(); ++i) {
        const CTransaction& tx{*txs[i]};
        for (uint32_t o{0}; o < tx.vout.size(); ++o) {
            const COutPoint outpoint{tx.GetHash(), o};
            if (named[i]) {
                if (HaveCoin(outpoint)) return true;
            } else if (const auto it{cacheCoins.find(outpoint)}; it != cacheCoins.end()) {
                if (!it->second.coin.IsSpent()) return true;
            } else {
                outpoints.push_back(outpoint);
            }
        }
    }
    // What neither the view nor the window has is the store's.
    static thread_local std::vector<uint8_t> may;
    m_store.MayHave(outpoints, may);
    for (size_t i{0}; i < outpoints.size(); ++i) {
        if (may[i] && HaveCoin(outpoints[i])) return true;
    }
    return false;
}

void BlockApplyView::CommitAlone(const CBlock& block, const CBlockUndo& undo)
{
    std::vector<CoinsStore::Change> changes;
    changes.reserve(m_dirty_count);
    const uint32_t own_height{static_cast<uint32_t>(m_height)};
    for (auto& [outpoint, entry] : cacheCoins) {
        if (!entry.IsDirty()) continue;
        // A dirty spent entry is the spend of a coin the base had (a fresh one spent is erased from the view
        // instead); a dirty unspent entry is a create, which overwrote a coin the base had unless fresh.
        // The coin spent is known by its height (SpendCoin()), unless it is one the block made itself over
        // a coin of the base: the store then holds what was there before.
        const bool known_coin{entry.coin.IsSpent() && entry.coin.nHeight != own_height};
        // Only a coinbase may overwrite an unspent coin (the chain's two BIP30 duplicates): a transaction
        // that creates over one repeats an earlier transaction whose inputs are spent, and its block is
        // invalid. The rule relies on BIP30 or BIP34 keeping such a block out: with neither
        // (-test=bip30_off on a chain where BIP34 is not active yet) a block that spends an output and
        // makes it again with a repeated transaction is not handled.
        const bool may_overwrite{!entry.IsFresh() && !entry.coin.IsSpent() && entry.coin.IsCoinBase()};
        // The entry itself: the store takes the coin from where it is, and the view is emptied after.
        changes.push_back({.outpoint = &outpoint, .coin = &entry.coin, .possible_overwrite = may_overwrite, .known_coin = known_coin});
    }
    // The coins the block spent, by outpoint, made only once a spend misses the store.
    std::unordered_map<COutPoint, const Coin*, SaltedOutpointHasher> spent;
    const auto base_coin{[&](const COutPoint& outpoint) -> const Coin* {
        if (spent.empty() && undo.vtxundo.size() + 1 == block.vtx.size()) {
            for (size_t i{1}; i < block.vtx.size(); ++i) {
                const CTransaction& tx{*block.vtx[i]};
                const CTxUndo& tx_undo{undo.vtxundo[i - 1]};
                if (tx_undo.vprevout.size() != tx.vin.size()) continue;
                for (size_t j{0}; j < tx.vin.size(); ++j) spent.emplace(tx.vin[j].prevout, &tx_undo.vprevout[j]);
            }
        }
        const auto it{spent.find(outpoint)};
        return it == spent.end() ? nullptr : it->second;
    }};
    // Nothing changes the store beside the block: what it read is what it changes.
    Assert(m_store.ApplyConfirmed(std::move(changes), m_height, /*report=*/nullptr, base_coin) == 0);
    // The store has taken the entries' coins: the view is emptied here, as a window block's is, and not by the
    // reset that follows the block's publication.
    ChangesCommitted();
}

void BlockApplyView::ChangesCommitted()
{
    const size_t entries{cacheCoins.size()};
    cacheCoins.clear();
    // The map's pool hands the nodes it took back out in the order they were freed, which after a few
    // blocks is no order: every entry the next block makes, finds or walks here is then somewhere else in
    // memory last touched many blocks ago. A new pool hands them out by address. The map is sized for as
    // many entries again, so that it does not grow step by step in every block.
    ReallocateCache();
    cacheCoins.reserve(entries);
    cachedCoinsUsage = 0;
    m_dirty_count = 0;
}

void BlockApplyView::ConfirmCommitted(int height)
{
    StopFetching(); // the prefetch workers read the view
    m_store.SetBestBlock(GetBestBlock(), height);
    Reset();
}

std::optional<Coin> BlockApplyView::ReadBase(const COutPoint& outpoint) const
{
    // An outpoint an unpublished window block made is the window's to answer.
    if (m_overlay) {
        std::optional<Coin> coin;
        if (m_overlay->FindCoin(outpoint, m_overlay_height, m_own_limit, coin)) return coin;
    }
    // A window block's read leaves the coin in the tier, clean (SetKeepBaseReads()).
    return m_keep_base_reads ? m_store.GetCoin(outpoint) : m_store.PeekCoin(outpoint);
}

void BlockApplyView::Reset() noexcept
{
    StopFetching(); // the prefetch workers read the view
    m_keep_base_reads = false;
    m_overlay = nullptr;
    m_own_limit = 0;
    m_height = -1;
    CoinsViewOverlay::Reset();
}
