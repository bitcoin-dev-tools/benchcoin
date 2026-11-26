// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_COINSSTORE_H
#define BITCOIN_COINSSTORE_H

#include <coins.h>
#include <script/script.h>
#include <coins_entry_machine.h>
#include <crypto/common.h>
#include <kernel/caches.h>
#include <memusage.h>
#include <undo.h>
#include <primitives/transaction.h>
#include <sync.h>
#include <uint256.h>
#include <util/check.h>
#include <util/hasher.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

/**
 * A coin as the store's entries hold it: the amount, the height and the coinbase flag of a Coin, and its output
 * script inline up to INLINE (32) bytes, which covers P2PKH, P2SH and P2WPKH outputs as they are. The three standard
 * outputs that are longer are kept as their 32 bytes of key or hash, the kind saying what goes around them: P2WSH and
 * P2TR (34 bytes) and P2PK with a compressed key (35). Any other longer script is on the heap. Nothing is lost: a
 * Coin converted to a StoredCoin and back is the same coin. 44 bytes aligned to four, where a Coin is 56 aligned to
 * eight, so that an entry and its key pack without padding (CoinsStore::Map::Cell).
 */
class StoredCoin
{
    //! The amount in seven bytes, as bytes (an amount would align the coin to eight): every amount an output may
    //! carry (MoneyRange()) fits, and all seven bytes set is the spent marker.
    static constexpr uint64_t SPENT_VALUE{(uint64_t{1} << 56) - 1};
    unsigned char m_value[7]{0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    void SetValue(CAmount amount)
    {
        Assert(amount == -1 || (amount >= 0 && static_cast<uint64_t>(amount) < SPENT_VALUE));
        const uint64_t value{amount == -1 ? SPENT_VALUE : static_cast<uint64_t>(amount)};
        for (size_t i{0}; i < sizeof(m_value); ++i) m_value[i] = static_cast<unsigned char>(value >> (8 * i));
    }

public:
    static constexpr size_t INLINE{32};
    //! The amount, read without making a Coin.
    CAmount Amount() const
    {
        uint64_t value{0};
        for (size_t i{0}; i < sizeof(m_value); ++i) value |= uint64_t{m_value[i]} << (8 * i);
        return value == SPENT_VALUE ? CAmount{-1} : static_cast<CAmount>(value);
    }

    StoredCoin() = default;
    StoredCoin(const StoredCoin&) = delete;
    StoredCoin& operator=(const StoredCoin&) = delete;
    StoredCoin& operator=(const Coin& coin)
    {
        AssignOutput(coin.out.nValue, coin.nHeight, coin.fCoinBase, coin.out.scriptPubKey);
        return *this;
    }
    //! As the assignment of a Coin of these parts, without the Coin.
    void AssignOutput(CAmount value, uint32_t height, bool coinbase, const CScript& script)
    {
        Release();
        SetValue(value);
        nHeight = height;
        fCoinBase = coinbase;
        SetScript(script.data(), script.size());
    }
    ~StoredCoin() { Release(); }

    //! As Coin::Clear().
    void Clear()
    {
        Release();
        SetValue(-1);
        nHeight = 0;
        fCoinBase = false;
    }

    //! The coin as a Coin: what lookups hand out.
    Coin ToCoin() const
    {
        Coin coin;
        if (IsSpent()) return coin;
        coin.out.nValue = Amount();
        if (m_kind == HEAP) {
            const unsigned char* bytes{HeapBlock() + sizeof(uint32_t)};
            coin.out.scriptPubKey.assign(bytes, bytes + HeapSize());
        } else {
            unsigned char bytes[INLINE + 3];
            const size_t size{Expand(bytes)};
            coin.out.scriptPubKey.assign(bytes, bytes + size);
        }
        coin.nHeight = nHeight;
        coin.fCoinBase = fCoinBase;
        return coin;
    }
    operator Coin() const { return ToCoin(); }

    //! As Coin::IsSpent() (CTxOut::IsNull()).
    bool IsSpent() const { return Amount() == -1; }
    //! As Coin::DynamicMemoryUsage(): the heap memory of a script kept on the heap.
    size_t DynamicMemoryUsage() const { return m_kind == HEAP ? memusage::MallocUsage(sizeof(uint32_t) + HeapSize()) : 0; }

private:
    //! The script's kind: its length where it is inline as it is (0 to INLINE), one of the standard outputs kept as
    //! their 32 bytes, or HEAP, where m_bytes holds the pointer and the allocation the script's length (four bytes)
    //! and then its bytes.
    static constexpr uint8_t P2WSH{INLINE + 1}, P2TR{INLINE + 2}, P2PK_EVEN{INLINE + 3}, P2PK_ODD{INLINE + 4}, HEAP{0xff};
    static_assert(P2PK_ODD < HEAP && INLINE >= sizeof(unsigned char*));
    uint8_t m_kind{0};

public:
    //! One type for both, as in Coin: a compiler that starts a new unit where the type changes (MSVC) would
    //! not pack a bool into the height's word.
    uint32_t nHeight : 31 {0};
    uint32_t fCoinBase : 1 {0};

private:
    unsigned char m_bytes[INLINE];

    unsigned char* HeapBlock() const
    {
        unsigned char* block;
        std::memcpy(&block, m_bytes, sizeof(block));
        return block;
    }
    size_t HeapSize() const
    {
        uint32_t size;
        std::memcpy(&size, HeapBlock(), sizeof(size));
        return size;
    }
    //! The script of a coin not on the heap, written to `out` (INLINE + 3 bytes at most). Returns its length.
    size_t Expand(unsigned char* out) const
    {
        switch (m_kind) {
        case P2WSH:
        case P2TR:
            out[0] = m_kind == P2WSH ? OP_0 : OP_1;
            out[1] = 32;
            std::memcpy(out + 2, m_bytes, 32);
            return 34;
        case P2PK_EVEN:
        case P2PK_ODD:
            out[0] = 33;
            out[1] = m_kind == P2PK_EVEN ? 0x02 : 0x03;
            std::memcpy(out + 2, m_bytes, 32);
            out[34] = OP_CHECKSIG;
            return 35;
        default: {
            // Inline as it is: the kind is the script's length, at most INLINE.
            const size_t size{std::min<size_t>(m_kind, INLINE)};
            if (size) std::memcpy(out, m_bytes, size);
            return size;
        }
        }
    }
    void SetScript(const unsigned char* bytes, size_t size)
    {
        if (size <= INLINE) {
            m_kind = static_cast<uint8_t>(size);
            if (size) std::memcpy(m_bytes, bytes, size);
            return;
        }
        if (size == 34 && (bytes[0] == OP_0 || bytes[0] == OP_1) && bytes[1] == 32) {
            m_kind = bytes[0] == OP_0 ? P2WSH : P2TR;
            std::memcpy(m_bytes, bytes + 2, 32);
            return;
        }
        if (size == 35 && bytes[0] == 33 && (bytes[1] == 0x02 || bytes[1] == 0x03) && bytes[34] == OP_CHECKSIG) {
            m_kind = bytes[1] == 0x02 ? P2PK_EVEN : P2PK_ODD;
            std::memcpy(m_bytes, bytes + 2, 32);
            return;
        }
        auto* block{new unsigned char[sizeof(uint32_t) + size]};
        const uint32_t size32{static_cast<uint32_t>(size)};
        std::memcpy(block, &size32, sizeof(size32));
        std::memcpy(block + sizeof(uint32_t), bytes, size);
        std::memcpy(m_bytes, &block, sizeof(block));
        m_kind = HEAP;
    }
    void Release() noexcept
    {
        if (m_kind == HEAP) delete[] HeapBlock();
        m_kind = 0;
    }
};
static_assert(sizeof(StoredCoin) == 44 && alignof(StoredCoin) == 4);

/**
 * The young tier of the UTXO set: the in-memory layer above the coins
 * database that holds every coin changed since it was last written out and
 * clean copies of old coins that lookups brought in. It is the chainstate's
 * tip view, and it is a CCoinsViewCache to its callers: the same entry
 * semantics (an entry is DIRTY when it differs from the database and FRESH
 * when the database is known not to hold it, so a spent FRESH entry needs no
 * erase) behind the same interface, with the base class's map unused.
 *
 *  - The map is sharded by the leading bytes of the txid, a shard being a
 *    range of the database's keys, with one mutex per shard, held across the
 *    lookup, compare and update of one entry, so that several threads can
 *    mutate distinct entries at once.
 *  - Every entry carries the height of its last change. A write-out run
 *    takes the entries changed at or below a height bound (BeginWriteOut),
 *    streams them to the database a chunk at a time under its two-head
 *    marker (StreamWriteOut), each chunk becoming clean as it lands, and
 *    ends by recording the head the database then holds (CommitWriteOut).
 *    What stays in memory is the lag: the changes of the last blocks, among
 *    which coins created and spent since the bound are never written at all.
 *  - Clean entries can be evicted on their own (EvictClean), so the budget
 *    is met before any dirty entry has to be written.
 *  - A block's changes are made once the block is valid, at its height
 *    (ApplyConfirmed), by several threads on disjoint shards. A change that
 *    does not find the coin it expects is not made and is reported, and the
 *    caller takes the block's other changes back.
 *
 * The owner serialises commits and runs (under cs_main); the shard locks make
 * lookups from other threads safe meanwhile. AccessCoin() returns a reference
 * to the calling thread's copy of the coin, valid until the thread's next
 * call: no lock of the caller keeps a commit from spending the entry.
 */
class CoinsStore : public CCoinsViewCache
{
    struct Entry;
    //! A map node: an outpoint and its entry. Nodes never move, so a run in flight refers to entries by node
    //! pointer.
    using Node = std::pair<const COutPoint, Entry>;

public:
    //! MapNodeRegion(), UnmapNodeRegion() and the size of their regions, for tests.
    static void* MapNodeRegionForTest() { return MapNodeRegion(); }
    static void UnmapNodeRegionForTest(void* region) noexcept { UnmapNodeRegion(region); }
    static constexpr size_t NodeRegionBytesForTest() { return NODE_REGION_BYTES; }

    //! A write-out run between BeginWriteOut() and CommitWriteOut().
    struct WriteOutRun {
        //! Coins a disconnect restored (KeepWritten()), handed to this run: written before anything else of
        //! it and before its marker, as plain coins (CCoinsViewDB::WriteCoins()), by the run's owner and not
        //! by StreamWriteOut(), which never reads them. An owner that cannot write them abandons the run
        //! (AbandonWriteOut()) rather than stream it.
        std::vector<CoinWrite> restored;
        //! The number of changes collected.
        size_t count{0};
        //! The tier's best block when the run was prepared: the marker's new head. Every write is the
        //! outpoint's value in that block's state.
        uint256 new_head;
        //! The block whose state the database held before the run (the settled head of the previous run):
        //! the marker's old head.
        uint256 old_head;
        //! Every change at or below this height is carried.
        int bound_height{-1};
        //! The height of new_head.
        int head_height{-1};
        //! The lowest change height among the dirty entries the run leaves behind, as far as collected.
        int oldest_left{std::numeric_limits<int>::max()};
        //! The height whose state the database holds once the run has landed: that of new_head when no
        //! dirty change remains behind, else one below the oldest change that remains. At least
        //! bound_height, since everything at or below the bound is carried. Known once every shard has
        //! been collected.
        int settled_height{-1};
        //! The block at settled_height on the chain of new_head: the next run's old head. StreamWriteOut()
        //! fills it in.
        uint256 settled_head;
        //! Whether the run drains the tier: the database is consistent with new_head afterwards and the
        //! marker is replaced by the best block.
        bool final{false};
        size_t bytes{0};
    };

    //! The number of shards unless the caller names one: many against any number of threads a machine
    //! runs, since a thread that finds a shard taken waits far longer than the hold it waits out. An
    //! empty shard is a few hundred bytes.
    static constexpr size_t DEFAULT_SHARDS{256};
    //! `deterministic` is the cache base's (CCoinsViewCache), whose own map the store does not use; the shards'
    //! key hash keeps its random salt (KeyHash()), which places entries and decides no outcome.
    explicit CoinsStore(CCoinsView* base_view, size_t shards = DEFAULT_SHARDS, bool deterministic = false);

    // CCoinsView
    //! The entry's coin as a copy, bringing it in from the base on a miss; nullopt if absent or spent.
    std::optional<Coin> GetCoin(const COutPoint& outpoint) const override;
    std::optional<Coin> PeekCoin(const COutPoint& outpoint) const override;
    bool HaveCoin(const COutPoint& outpoint) const override;
    uint256 GetBestBlock() const override;
    //! A child cache's dirty entries become confirmed changes of this tier at the current change height.
    void BatchWrite(CoinsViewCacheCursor& cursor, const uint256& block_hash) override;

    // CCoinsViewCache: changes one by one (the disconnect path, snapshot loading, tests).
    void SetBestBlock(const uint256& block_hash) override;
    bool HaveCoinInCache(const COutPoint& outpoint) const override;
    const Coin& AccessCoin(const COutPoint& outpoint) const override;
    void AddCoin(const COutPoint& outpoint, Coin&& coin, bool possible_overwrite) override;
    //! Insert a coin known to be absent from the tier and the base (snapshot loading). Marked dirty, not fresh.
    void EmplaceCoinInternalDANGER(const COutPoint& outpoint, Coin&& coin) override;
    bool SpendCoin(const COutPoint& outpoint, Coin* moveto = nullptr) override;
    //! Write everything dirty synchronously through the base, as a final run, and drop every entry. The
    //! coins disconnects restored are not among what it writes: the caller writes Restored() first.
    void Flush(bool reallocate_cache = true) override;
    //! Write everything dirty synchronously through the base, as a final run, and keep the entries clean.
    //! As Flush(), the caller writes Restored() first.
    void Sync();
    /**
     * How the base takes its writes: the size of a batch (the database's batch size), and whether several
     * threads may call its WriteRun() at once (the database's may). Flush() and Sync() write batches of up
     * to that size, each the changes of neighbouring shards in the order of their keys: a range of the
     * database's keys that no other batch of the flush reaches into (ShardIndex()). A flush of more than
     * a few batches to a base that allows it has up to four workers, a range of the shards each,
     * collecting and serializing its next batch while the database is busy with another's: the calling
     * thread and tasks of the pool given to SetFlushHelpers().
     */
    void SetBaseWrites(uint64_t batch_bytes, bool concurrent)
    {
        m_batch_bytes = batch_bytes;
        m_base_concurrent = concurrent;
    }
    //! The pool a flush's further workers run on, as tasks (the node's connect pool). Without one, the
    //! calling thread writes every range, one after the other.
    void SetFlushHelpers(std::shared_ptr<ThreadPool> pool) { m_flush_helpers = std::move(pool); }
    void Uncache(const COutPoint& outpoint) override;
    unsigned int GetCacheSize() const override;
    size_t GetDirtyCount() const noexcept override;
    size_t DynamicMemoryUsage() const override;
    //! The regions the nodes of every store are carved from, which are never given back: beyond the live
    //! nodes, which DynamicMemoryUsage() counts, they hold the free ones, which nothing else counts.
    static size_t NodeRegionBytes() { return s_node_region_bytes.load(std::memory_order_relaxed); }
    void SanityCheck() const override;
    //! Not meaningful for the tier.
    void Reset() noexcept override;

    // The tier's own interface.
    //! Set the block and the height tagged onto the changes that follow.
    void SetBestBlock(const uint256& block_hash, int height);
    //! The height tagged onto the changes that follow. Set to the fork height before disconnecting blocks, so
    //! that the inverse changes are the oldest and the next run carries them all.
    void SetChangeHeight(int height) { m_change_height = height; }
    //! The height of the tier's best block, for the paths that move it by hash alone (a disconnect's
    //! flush); SetBestBlock(hash, height) sets it too. Distinct from the change height, which a reorg
    //! holds at the fork while the best block moves.
    void SetTierHeight(int height)
    {
        // Below the block the database holds, or is being brought to: what the disconnect changed is tagged
        // with the fork height, and no run may stop at a bound until a final one has carried it.
        Assert(!m_run_in_flight);
        if (height < m_written_height) m_fell_below_written = true;
        m_tier_height = height;
    }

    //! One of a block's changes, for ApplyConfirmed(): an unspent coin is the outpoint's creation, a spent one
    //! its spend. The outpoint and the coin stay where the caller has them, for the length of the call: a
    //! change is no copy of either.
    struct Change {
        const COutPoint* outpoint;
        Coin* coin;
        //! For a create: a live coin under it is a BIP30 overwrite, allowed only with this, as AddCoin().
        bool possible_overwrite{false};
        //! For a spend: `coin`, spent, has the height and the coinbase flag of the coin the block validated
        //! the spend against, and another coin met at the apply is a conflict.
        bool known_coin{false};
        //! For a create: the coin's script, where `coin` holds all but it.
        const CScript* script{nullptr};
    };
    /**
     * The shard of an outpoint, by the leading bytes of its txid as the database's key begins with them:
     * the shards are consecutive ranges of the database's keys in the order of their index
     * (SetBaseWrites()), and a commit groups a block's changes by it. A transaction's outputs share a shard.
     * Whoever makes a transaction can pick its shard by grinding, which buys contention on that shard and a
     * larger share of a forced flush's scratch memory (WipeShard() stages a whole shard's changes), and
     * nothing else: an entry's place in the shard's map is by the salted hash.
     */
    size_t ShardIndex(const COutPoint& outpoint) const { return (uint64_t{ReadBE32(outpoint.hash.ToUint256().data())} * m_shards.size()) >> 32; }
    //! What ApplyConfirmed() tells of a call, for the caller that takes its changes back.
    struct ConfirmedReport {
        //! The changes that were not made, by the address of their outpoint.
        std::vector<const COutPoint*> conflicted;
        //! The coins that creates wrote over (Change::possible_overwrite), by the address of the create's outpoint.
        std::vector<std::pair<const COutPoint*, Coin>> overwritten;
        //! The creates that may overwrite and found no entry: what they wrote over, if anything, is the
        //! database's coin (BaseCoin()).
        std::vector<const COutPoint*> unread;
    };
    //! The changes of the block at `height`, which validated, by several threads on disjoint shards. Returns
    //! the conflicts: a spend whose coin is not there, or is another, and a create over a coin it may not
    //! overwrite. A change that is a conflict is not made, the others are; `report` names them, so that the
    //! caller can take the others back (with the inverse changes, as a disconnected block's).
    //! A spend of a coin the tier does not hold takes the coin from `base_coin` where it gives one: the coin
    //! the database holds under the outpoint, as the block read it, which nothing has changed since (a block
    //! connected alone, from its undo data); else from a read of the database.
    uint32_t ApplyConfirmed(std::vector<Change>&& changes, int height, ConfirmedReport* report = nullptr, const std::function<const Coin*(const COutPoint&)>& base_coin = {});
    size_t ShardCount() const { return m_shards.size(); }
    //! The database's coin under `outpoint`, whatever the tier holds.
    std::optional<Coin> BaseCoin(const COutPoint& outpoint) const { return base->GetCoin(outpoint); }
    //! The coins the tier holds among the outpoints at the positions `which`, read with one hold of each shard's
    //! lock: each coin found into `coins` at its position and `found` set there; what is missing or spent is not
    //! found.
    void FetchInto(std::span<const COutPoint> outpoints, std::span<const uint32_t> which, std::vector<Coin>& coins, std::vector<uint8_t>& found) const;
    //! For a check that none of `outpoints` has a coin (BIP30), with one hold of each shard's lock. An
    //! outpoint with `may` unset has none, as GetCoin() and PeekCoin() would answer: the tier has no entry
    //! for it and the base no coin. One with `may` set is theirs to answer.
    void MayHave(std::span<const COutPoint> outpoints, std::vector<uint8_t>& may) const;

    //! The lowest height bound whose write-out frees at least `bytes` of dirty entries, or the tip height if
    //! all dirty entries together do not.
    int BoundForBytes(size_t bytes, int tip_height) const;

    //! Drop clean entries until the tier's usage is at most `target_bytes`, oldest first in each shard; an
    //! entry read since it was last passed over gets a second chance.
    size_t EvictClean(size_t target_bytes);

    /**
     * Open a run that carries every dirty entry changed at or below `bound_height`, under the
     * marker (`new_head`, the settled head of the previous run); `head_height` is new_head's height.
     * `final` carries everything regardless of the bound. A run with nothing to carry still
     * moves the settled head when the bound is above it; nullptr if there is nothing to carry and no
     * head to move.
     *
     * What the marker promises: before the run the database holds the state at old_head; each write sets
     * an outpoint to its value at new_head; so a crash anywhere inside the run is repaired by undoing
     * old_head down to its fork with new_head and re-applying up to new_head (ReplayBlocks), which
     * rewrites every outpoint the two states can differ in. For the promise to hold at the next run, the
     * settled head after this one must be a lower bound for that replay: a block at or below which the
     * database holds every change, which is `settled_height` on new_head's chain. The bound names the
     * oldest change carried, not that block (a reorg's inverse changes are tagged with the fork height and
     * move it to the tier's head). It is a lower bound and not a state the database holds whole: a coin
     * created at or below it and spent above it cancels in the tier and never reaches the database, which
     * any replay forward from it agrees with; a disconnect below the spend restores the coin, and the next
     * run writes it first (KeepWritten()).
     *
     * One kind of write is not the outpoint's value at new_head: a confirmed create above the bound that
     * owes the database an erase is written as that erase (CollectChunk()), also where it went over an
     * unspent coin the database never had. The database then holds less than the state below the create,
     * never a coin that state does not have, and the replay re-applies the create like every block above
     * old_head.
     */
    std::unique_ptr<WriteOutRun> BeginWriteOut(int bound_height, int head_height, const uint256& new_head, bool final);
    /**
     * Write the run through the base a chunk at a time, a chunk being one database batch of about
     * `batch_bytes` (the database's batch size): collect a chunk of dirty entries from the shards'
     * maps, each shard under its lock and the chunk filling across shards, write
     * it with no shard locked, then settle the chunk's entries shard by shard through the node pointers
     * kept (an entry changed since stays dirty and is collected again; a spent one is erased). Memory
     * beyond the tier is one chunk's coins, whatever the size of the run; a run smaller than a chunk is one
     * write. A collected entry is not FRESH: its value is on its way to the database. At most one chunk
     * is outstanding, and a collected entry is never erased before its chunk settles, which is what keeps
     * the pointers good.
     *
     * The last write closes the marker: a final run replaces it by the best block; any other rewrites
     * it to (new_head, settled head), the pair the next run starts from, so that the marker on disk
     * never names an old head below what the tier counts as written. `block_at` names the block at a
     * height on new_head's chain (not needed for a final run).
     *
     * Takes shard locks and touches nothing else of the tier, so it may run on another thread than the
     * owner's. Throws what the base's WriteRun() throws; the run is then abandoned (AbandonWriteOut()).
     */
    void StreamWriteOut(WriteOutRun& run, const std::function<uint256(int)>& block_at, uint64_t batch_bytes = DEFAULT_DB_CACHE_BATCH);
    //! Coins a disconnect restored that the database may never have held (see DisconnectTip()): the next
    //! run writes them first, before its marker, and a forced flush before anything (TakeRestored()). The
    //! database may hold them from now on, so none of them stays fresh: a later spend is written as an
    //! erase rather than dropped.
    void KeepWritten(std::vector<CoinWrite> restored);
    //! The restored coins not yet handed to a run, for a caller that writes them itself (a forced flush).
    std::vector<CoinWrite> TakeRestored() { return std::exchange(m_restored, {}); }
    const std::vector<CoinWrite>& Restored() const { return m_restored; }
    //! The run has landed: its settled head is the database's state from now on.
    void CommitWriteOut(const WriteOutRun& run);
    //! The run did not land (the node is going down). What it had collected counts as written in the tier and
    //! may be missing in the database, so the tier refuses every later run and flush (FlushFailed()).
    void AbandonWriteOut();
    //! The settled head: the block whose state the database holds, apart from the run in flight (null
    //! before any run).
    const uint256& WrittenHead() const { return m_written_head; }
    //! At start-up, after the database was found or made consistent with `block_hash`: nothing is dirty.
    void InitWrittenHead(const uint256& block_hash, int height)
    {
        m_written_head = block_hash;
        m_written_height = height;
        SetTierHeight(height);
    }
    //! Whether a run is between BeginWriteOut() and CommitWriteOut().
    bool RunInFlight() const { return m_run_in_flight; }
    //! Whether a flush lost changes to a failed write. Nothing may be written after it (Flush()).
    bool FlushFailed() const { return m_flush_failed; }

private:
    //! How an entry is counted in its shard: among the dirty entries, by the span of its change height, or
    //! among the clean ones; LIST_NONE while a change is made to it (Detach()).
    enum List : uint8_t { LIST_NONE, LIST_DIRTY, LIST_CLEAN };
    /**
     * An entry is counted in its shard by its state: a dirty entry in the byte sum of its change height's
     * span, where the write-out's bound is found, a clean one among the entries eviction may take. There
     * are no lists: a write-out run and eviction walk the shard's map.
     *
     * The flags other than `referenced`, with whether the coin is spent, are the entry's state: what
     * each flag means, which states there are and what leads from one to another is said in
     * coins_entry_machine.h, with what each change does to the height the entry is filed under. The flags
     * and the height are written by Transit() and Enter() alone.
     */
    struct Entry {
        StoredCoin coin;
        //! The height the entry is filed under, the write-out's age: the last change's as a rule, and an
        //! earlier one's where that change is owed still, or where a change finds the entry dirty
        //! (coins_entry::Height). A block height, below MAX_FILED_HEIGHT (Enter()). It and the flags share
        //! one word, of one type for all, as a compiler that starts a new unit where the type changes (MSVC)
        //! would not pack them.
        uint32_t height : 25 {0};
        uint32_t dirty : 1 {0};
        uint32_t fresh : 1 {0};
        uint32_t collected : 1 {0};
        //! A lookup found the entry since eviction last passed over it.
        uint32_t referenced : 1 {0};
        //! The coin was made over an erase the database is owed still (coins_entry::OWED).
        uint32_t owed : 1 {0};
        uint32_t list : 2 {LIST_NONE};
    };
    static constexpr int MAX_FILED_HEIGHT{1 << 25};
    //! The huge page size the hint below is made for (x86-64's; elsewhere the hint is as harmless as none).
    static constexpr size_t HUGE_PAGE_BYTES{size_t{2} << 20};
    //! Memory of a multiple of HUGE_PAGE_BYTES, aligned to it, that the system is asked to back with huge
    //! pages where it can (Linux's transparent huge pages, in their "madvise" or "always" mode): a lookup
    //! lands on a random node of a large map, and with small pages misses the TLB for the bucket and again
    //! for the node. Only a hint. Freed with FreeRegion().
    static void* HintedRegion(size_t bytes);
    static inline std::atomic<size_t> s_node_region_bytes{0};
    //! The size of the regions the nodes' memory is asked for in.
    static constexpr size_t NODE_REGION_BYTES{size_t{32} << 20};
    static void FreeRegion(void* region) noexcept { ::operator delete(region, std::align_val_t{HUGE_PAGE_BYTES}); }
    //! Memory of NODE_REGION_BYTES aligned to its size, so that a node's address finds its region's number
    //! (NodeAllocator::Number()), with the huge page hint, mapped from the system with nothing beside it. An
    //! aligned operator new would serve the alignment from a mapping of twice the size and keep all of it
    //! (glibc does), which doubles the address space and, where it is charged, the commit of every region
    //! for the life of the process. Freed with UnmapNodeRegion(). A region the system does not give is a
    //! failed allocation, which ends the process (NodeRegionUnavailable()).
    static void* MapNodeRegion();
    static void UnmapNodeRegion(void* region) noexcept;
    //! The system gave no memory for a node region: logged, then aborted, as any failed allocation ends the process.
    [[noreturn]] static void NodeRegionUnavailable(size_t region_bytes);
    //! The process has used every node-region number (NodeAllocator::Depot): logged, then aborted.
    [[noreturn]] static void NodeNumbersExhausted(size_t regions, size_t region_bytes);
    //! A shard's entries are inserted and erased by whichever thread holds its mutex, so a node is often freed
    //! by another thread than the one that allocated it. A thread keeps the nodes it frees for its own next
    //! insertions: a list of up to MAX_KEPT nodes and one full list beside it. A thread with two full lists
    //! gives one to the depot, and a thread with none takes one from there: a lock for every MAX_KEPT
    //! nodes, held for the exchange of a pointer.
    //!
    //! The depot carves new lists from regions of REGION_BYTES (MapNodeRegion()). A node is never given back: one that is freed waits, with its thread or in the
    //! depot, for the next insertion, and the regions stay with the process.
    //!
    //! A node has a number of 32 bits, which links hold in place of its address (Number(), At()): its
    //! region's number in the high bits and its place in the region in the low ones. A region is aligned
    //! to its size and begins with its number, in a slot of its own, so that no node has the number 0.
    template <typename T>
    struct NodeAllocator {
        using value_type = T;
        static constexpr size_t MAX_KEPT{1024};
        static constexpr size_t REGION_BYTES{NODE_REGION_BYTES};
        static constexpr unsigned SLOT_BITS{static_cast<unsigned>(std::bit_width(REGION_BYTES / sizeof(T)))};
        //! 2^13 regions of 32 MiB for 88-byte nodes, 256 GiB of them.
        static constexpr size_t MAX_REGIONS{size_t{1} << (32 - SLOT_BITS)};
        //! The regions by number, each written once, when the depot takes it. Constant-initialized and never
        //! destroyed (trivially destructible), so a lookup needs no guard and nodes may outlive static objects.
        static inline std::atomic<std::byte*> s_regions[MAX_REGIONS]{};
        static uint32_t Number(const T* node)
        {
            static_assert(std::has_single_bit(REGION_BYTES) && SLOT_BITS < 32);
            const auto address{reinterpret_cast<uintptr_t>(node)};
            const uintptr_t base{address & ~uintptr_t{REGION_BYTES - 1}};
            uint32_t region;
            std::memcpy(&region, reinterpret_cast<const void*>(base), sizeof(region));
            return (region << SLOT_BITS) | static_cast<uint32_t>((address - base) / sizeof(T));
        }
        static T* At(uint32_t number)
        {
            if (number == 0) return nullptr;
            std::byte* const base{s_regions[number >> SLOT_BITS].load(std::memory_order_relaxed)};
            return reinterpret_cast<T*>(base + (number & ((uint32_t{1} << SLOT_BITS) - 1)) * sizeof(T));
        }
        //! A free node's links are node numbers, as the map's are, so that a node needs no pointer's alignment: the
        //! next node of its list in its first word, and in a list's first node the next full list in the depot in
        //! its second.
        static void* Link(const void* node, size_t word)
        {
            uint32_t number;
            std::memcpy(&number, static_cast<const std::byte*>(node) + word * sizeof(number), sizeof(number));
            return At(number);
        }
        static void SetLink(void* node, size_t word, const void* to)
        {
            const uint32_t number{to ? Number(static_cast<const T*>(to)) : 0};
            std::memcpy(static_cast<std::byte*>(node) + word * sizeof(number), &number, sizeof(number));
        }
        static void* Next(const void* node) { return Link(node, 0); }
        static void SetNext(void* node, const void* to) { SetLink(node, 0, to); }
        static void* NextList(const void* list) { return Link(list, 1); }
        static void SetNextList(void* list, const void* to) { SetLink(list, 1, to); }
        //! Full lists, each MAX_KEPT nodes, for whichever thread next runs out, and the region new ones are
        //! carved from. Never destroyed, so that a thread may end at any time.
        struct Depot {
            Mutex mutex;
            void* lists GUARDED_BY(mutex){nullptr};
            //! The nodes of threads that ended, fewer than MAX_KEPT: a full list once there are as many.
            void* loose GUARDED_BY(mutex){nullptr};
            size_t loose_count GUARDED_BY(mutex){0};
            //! What is left of the newest region.
            std::byte* fresh GUARDED_BY(mutex){nullptr};
            size_t fresh_bytes GUARDED_BY(mutex){0};
            uint32_t regions GUARDED_BY(mutex){0};
            //! A region a thread asked for while another refilled the depot: the next refill's.
            std::byte* unused GUARDED_BY(mutex){nullptr};
            void Give(void* list) EXCLUSIVE_LOCKS_REQUIRED(!mutex)
            {
                LOCK(mutex);
                SetNextList(list, lists);
                lists = list;
            }
            void GiveLoose(void* nodes) EXCLUSIVE_LOCKS_REQUIRED(!mutex)
            {
                LOCK(mutex);
                while (nodes) {
                    void* next{Next(nodes)};
                    SetNext(nodes, loose);
                    loose = nodes;
                    if (++loose_count == MAX_KEPT) {
                        SetNextList(loose, lists);
                        lists = std::exchange(loose, nullptr);
                        loose_count = 0;
                    }
                    nodes = next;
                }
            }
            //! A full list: one that was given, else MAX_KEPT nodes no one has used yet.
            void* Take() EXCLUSIVE_LOCKS_REQUIRED(!mutex)
            {
                static constexpr size_t LIST_BYTES{MAX_KEPT * sizeof(T)};
                std::byte* region{nullptr};
                while (true) {
                    void* list{nullptr};
                    std::byte* nodes{nullptr};
                    std::byte* surplus{nullptr};
                    {
                        LOCK(mutex);
                        // A region asked for below waits for the next refill, unless one waits already.
                        if (region) (unused ? surplus : unused) = std::exchange(region, nullptr);
                        if (lists) {
                            list = std::exchange(lists, NextList(lists));
                        } else if (fresh_bytes >= LIST_BYTES || unused) {
                            if (fresh_bytes < LIST_BYTES) {
                                // The remainder, less than a list, is left behind. The region's first slot holds
                                // its number; past the last number (256 GiB of nodes) there is none to give.
                                const uint32_t number{regions++};
                                if (number >= MAX_REGIONS) NodeNumbersExhausted(MAX_REGIONS, REGION_BYTES);
                                std::byte* const next{std::exchange(unused, nullptr)};
                                std::memcpy(next, &number, sizeof(number));
                                s_regions[number].store(next, std::memory_order_relaxed);
                                fresh = next + sizeof(T);
                                fresh_bytes = REGION_BYTES - sizeof(T);
                                s_node_region_bytes.fetch_add(REGION_BYTES, std::memory_order_relaxed);
                            }
                            nodes = fresh;
                            fresh += LIST_BYTES;
                            fresh_bytes -= LIST_BYTES;
                        }
                    }
                    if (surplus) UnmapNodeRegion(surplus);
                    if (list) return list;
                    if (nodes) {
                        // Linked outside the lock: the first touch of the memory is where the system supplies it.
                        for (size_t i{0}; i < MAX_KEPT; ++i) {
                            SetNext(nodes + i * sizeof(T), i + 1 < MAX_KEPT ? nodes + (i + 1) * sizeof(T) : nullptr);
                        }
                        return nodes;
                    }
                    // No list and no room: the region is asked for and its first page, where its number goes,
                    // touched without the mutex, which every thread that takes or gives a list waits for.
                    region = static_cast<std::byte*>(MapNodeRegion());
                    *reinterpret_cast<volatile std::byte*>(region) = std::byte{0};
                }
            }
        };
        static Depot& Shared()
        {
            static Depot* const depot{new Depot};
            return *depot;
        }
        //! Whether this thread's list is destroyed: at the thread's end, and on the main thread before the
        //! static objects, which may still free nodes (a store a static test setup owns). Trivially
        //! destructible, so it can be read after that.
        static bool& KeptGone()
        {
            thread_local bool gone{false};
            return gone;
        }
        struct Kept {
            void* head{nullptr};
            size_t count{0};
            //! A full list, or none.
            void* spare{nullptr};
            ~Kept()
            {
                KeptGone() = true;
                if (spare) Shared().Give(std::exchange(spare, nullptr));
                if (head) Shared().GiveLoose(std::exchange(head, nullptr));
                count = 0;
            }
        };
        static Kept& Mine()
        {
            // A free node holds two node numbers (the links of its lists), and nodes lie back to back from an aligned start.
            static_assert(sizeof(T) >= 2 * sizeof(uint32_t) && sizeof(T) % alignof(uint32_t) == 0 && alignof(T) <= alignof(uint32_t));
            thread_local Kept kept;
            return kept;
        }
        NodeAllocator() = default;
        template <typename U>
        NodeAllocator(const NodeAllocator<U>&) noexcept {}
        T* allocate(size_t n)
        {
            Assert(n == 1);
            if (KeptGone()) {
                // One node of a list of the depot's (which is never destroyed), the rest back to it.
                void* node{Shared().Take()};
                void* rest{Next(node)};
                SetNext(node, nullptr);
                Shared().GiveLoose(rest);
                return static_cast<T*>(node);
            }
            Kept& kept{Mine()};
            if (!kept.head) {
                // The full list beside, else one of the depot's.
                kept.head = kept.spare ? std::exchange(kept.spare, nullptr) : Shared().Take();
                kept.count = MAX_KEPT;
            }
            void* node{kept.head};
            kept.head = Next(node);
            --kept.count;
            return static_cast<T*>(node);
        }
        void deallocate(T* p, size_t n) noexcept
        {
            Assert(n == 1);
            if (KeptGone()) {
                SetNext(p, nullptr);
                Shared().GiveLoose(p);
                return;
            }
            Kept& kept{Mine()};
            if (kept.count == MAX_KEPT) {
                // The full list goes beside, the one that was there to the depot, and a new one begins.
                if (kept.spare) Shared().Give(kept.spare);
                kept.spare = std::exchange(kept.head, nullptr);
                kept.count = 0;
            }
            SetNext(p, kept.head);
            kept.head = p;
            ++kept.count;
        }
        friend bool operator==(const NodeAllocator&, const NodeAllocator&) noexcept { return true; }
    };
    //! The salted hash of an outpoint, one salt for all shards: a batch hashes its keys before it takes a shard.
    static uint64_t KeyHash(const COutPoint& outpoint) noexcept
    {
        static const SaltedCoinsCacheHasher hasher;
        return hasher(outpoint);
    }
    /**
     * A shard's table: chained, with nodes that never move (a run in flight points at them),
     * of the std::unordered_map interface as far as the store uses it. The bucket array is a power of two
     * and a chain ends at a link of zero. A caller with many keys for one shard hashes them first and asks
     * for their buckets and nodes ahead (PrefetchAll()).
     */
    class Map
    {
    public:
        //! The chain's link is the next cell's number (NodeAllocator::Number()), 0 at the chain's end.
        struct Cell {
            uint32_t chain;
            Node node;
        };
        //! The node follows the link without padding, so that the link and the key are a cell's first bytes.
        //! Cells lie back to back in their region (NodeAllocator) and are not a multiple of half a cache line
        //! long: a cell starts anywhere in a line and may lie across three (PrefetchAll()).
        static constexpr size_t NODE_OFFSET{sizeof(uint32_t)};
        static constexpr size_t LINE_BYTES{64};
        static_assert(NODE_OFFSET + sizeof(COutPoint) <= LINE_BYTES && alignof(Node) <= alignof(uint32_t) && sizeof(Cell) == NODE_OFFSET + sizeof(Node));
        static_assert(sizeof(Cell) <= 2 * LINE_BYTES);
        static Cell* CellAt(uint32_t number) { return NodeAllocator<Cell>::At(number); }
        static uint32_t CellNumber(const Cell* cell) { return NodeAllocator<Cell>::Number(cell); }
        //! A bucket is the number of its chain's first cell.
        static constexpr size_t BUCKET_BYTES{sizeof(uint32_t)};
        class iterator
        {
        public:
            iterator() = default;
            iterator(const Map* map, size_t bucket, Cell* cell) : m_map{map}, m_bucket{bucket}, m_cell{cell} {}
            Node& operator*() const { return m_cell->node; }
            Node* operator->() const { return &m_cell->node; }
            bool operator==(const iterator& other) const { return m_cell == other.m_cell; }
            //! The bucket the iterator is in.
            size_t bucket() const { return m_bucket; }
            iterator& operator++()
            {
                m_cell = CellAt(m_cell->chain);
                while (!m_cell && ++m_bucket < m_map->m_bucket_count) m_cell = CellAt(m_map->m_buckets[m_bucket]);
                return *this;
            }

        private:
            friend class Map;
            const Map* m_map{nullptr};
            size_t m_bucket{0};
            Cell* m_cell{nullptr};
        };

        Map() = default;
        Map(const Map&) = delete;
        Map& operator=(const Map&) = delete;
        ~Map()
        {
            for (size_t b{0}; b < m_bucket_count; ++b) {
                for (Cell* cell{CellAt(m_buckets[b])}; cell;) {
                    Cell* next{CellAt(cell->chain)};
                    Free(cell);
                    cell = next;
                }
            }
        }

        size_t size() const { return m_size; }
        size_t bucket_count() const { return m_bucket_count; }
        iterator end() const { return {}; }
        iterator begin() const { return from(0); }
        //! The first node of the first bucket at or after `first` that has one.
        iterator from(size_t first) const
        {
            for (size_t b{first}; b < m_bucket_count; ++b) {
                if (m_buckets[b]) return {this, b, CellAt(m_buckets[b])};
            }
            return {};
        }
        //! Ask for every item's bucket, then every chain's first node, before the first probe waits for one: every
        //! line of the node's cell, its link and key first and the rest of the entry at the end. A line asked for
        //! twice costs nothing.
        void PrefetchAll(std::span<const uint64_t> hashes, std::span<const uint32_t> items) const
        {
            if (m_bucket_count == 0) return;
            for (const uint32_t i : items) Ahead(&m_buckets[hashes[i] & (m_bucket_count - 1)]);
            for (const uint32_t i : items) {
                if (const Cell* cell{CellAt(m_buckets[hashes[i] & (m_bucket_count - 1)])}) {
                    const char* const first{reinterpret_cast<const char*>(cell)};
                    Ahead(first);
                    Ahead(first + LINE_BYTES);
                    Ahead(first + sizeof(Cell) - 1);
                }
            }
        }
        iterator find(const COutPoint& key) const { return find(key, KeyHash(key)); }
        iterator find(const COutPoint& key, uint64_t hash) const
        {
            if (m_bucket_count == 0) return {};
            const size_t bucket{hash & (m_bucket_count - 1)};
            for (Cell* cell{CellAt(m_buckets[bucket])}; cell; cell = CellAt(cell->chain)) {
                if (cell->node.first == key) return {this, bucket, cell};
            }
            return {};
        }
        std::pair<iterator, bool> try_emplace(const COutPoint& key) { return try_emplace(key, KeyHash(key)); }
        std::pair<iterator, bool> try_emplace(const COutPoint& key, uint64_t hash)
        {
            if (const iterator it{find(key, hash)}; it != end()) return {it, false};
            if (m_size == m_bucket_count) Rebucket(m_bucket_count == 0 ? MIN_BUCKETS : m_bucket_count * 2);
            const size_t bucket{hash & (m_bucket_count - 1)};
            Cell* cell{::new (NodeAllocator<Cell>{}.allocate(1)) Cell{{}, Node(std::piecewise_construct, std::forward_as_tuple(key), std::tuple<>())}};
            cell->chain = m_buckets[bucket];
            m_buckets[bucket] = CellNumber(cell);
            ++m_size;
            return {iterator{this, bucket, cell}, true};
        }
        iterator erase(iterator it)
        {
            iterator next{it};
            ++next;
            const uint32_t number{CellNumber(it.m_cell)};
            uint32_t* link{&m_buckets[it.m_bucket]};
            while (*link != number) link = &CellAt(*link)->chain;
            *link = it.m_cell->chain;
            Free(it.m_cell);
            --m_size;
            return next;
        }
        size_t erase(const COutPoint& key)
        {
            const iterator it{find(key)};
            if (it == end()) return 0;
            erase(it);
            return 1;
        }
        //! Only rehash(0) of an empty map is used: the bucket array goes back.
        void rehash(size_t)
        {
            Assume(m_size == 0);
            m_buckets.reset();
            m_bucket_count = 0;
        }

    private:
        static constexpr size_t MIN_BUCKETS{16};
        //! A hint only: without the builtin the probe waits for its memory.
        static void Ahead(const void* p)
        {
#if defined(__GNUC__) || defined(__clang__)
            __builtin_prefetch(p);
#else
            (void)p;
#endif
        }
        static void Free(Cell* cell)
        {
            std::destroy_at(cell);
            NodeAllocator<Cell>{}.deallocate(cell, 1);
        }
        void Rebucket(size_t count)
        {
            Buckets buckets{nullptr, BucketsDeleter{count * BUCKET_BYTES >= HUGE_PAGE_BYTES}};
            // An array of a huge page or more (a power of two, so a multiple of it) asks for huge pages.
            void* array{buckets.get_deleter().hinted ? HintedRegion(count * BUCKET_BYTES) : ::operator new(count * BUCKET_BYTES)};
            buckets.reset(new (array) uint32_t[count]());
            for (size_t b{0}; b < m_bucket_count; ++b) {
                for (Cell* cell{CellAt(m_buckets[b])}; cell;) {
                    Cell* next{CellAt(cell->chain)};
                    const size_t bucket{KeyHash(cell->node.first) & (count - 1)};
                    cell->chain = buckets[bucket];
                    buckets[bucket] = CellNumber(cell);
                    cell = next;
                }
            }
            m_buckets = std::move(buckets);
            m_bucket_count = count;
        }

        struct BucketsDeleter {
            bool hinted;
            void operator()(uint32_t* array) const noexcept
            {
                if (hinted) {
                    FreeRegion(array);
                } else {
                    ::operator delete(array);
                }
            }
        };
        using Buckets = std::unique_ptr<uint32_t[], BucketsDeleter>;
        Buckets m_buckets{nullptr, BucketsDeleter{false}};
        size_t m_bucket_count{0};
        size_t m_size{0};
    };
    //! The dirty entries of one span of change heights in a shard: what writing them out frees, DirtyBytes()
    //! of each.
    struct Bucket {
        int64_t bytes{0};
    };
    using ShardMutex = Mutex;
    //! On cache lines of its own: a shard is written by whichever thread holds it, and its neighbours by others.
    struct alignas(64) Shard {
        mutable ShardMutex mutex;
        Map map GUARDED_BY(mutex);
        //! The shard's share of the tier's totals (see Totals): its dirty entries, and its map's size and
        //! bucket count as last counted (Resized()).
        size_t dirty_count GUARDED_BY(mutex){0};
        size_t entries GUARDED_BY(mutex){0};
        size_t buckets GUARDED_BY(mutex){0};
        //! The dirty entries' byte sums by span of change heights (SpanOf()), what writing each span out frees
        //! (DirtyBytes() of each entry), empty when the shard has none: they find the span a write-out's bound
        //! lies in.
        std::vector<int64_t> dirty_spans GUARDED_BY(mutex);
        //! Where eviction's sweep of the map stands, a bucket: a clean entry found since the last sweep keeps
        //! its place once (`referenced`), as a clock.
        size_t clock GUARDED_BY(mutex){0};
        //! Counts the times a write-out settled entries of this shard, or a flush emptied it: an unlocked
        //! read of the base that started before the count moved may predate what was written.
        uint64_t settles GUARDED_BY(mutex){0};
    };
    /**
     * The tier's totals: its memory usage, its entries and its dirty entries. The owner asks for them
     * for every block it publishes, and every change of an entry moves them. They are kept as sums of
     * deltas in stripes, a thread adding to the stripe it was dealt and each stripe on a cache line of
     * its own, so that a query reads the stripes and neither locks nor reads a shard. One stripe means
     * nothing (what one thread adds another takes away); the sum is the total, exact when no change is
     * under way.
     */
    struct alignas(64) Totals {
        //! As DynamicMemoryUsage() reports it: the maps' nodes and bucket arrays, the shards' arrays of
        //! dirty span sums, and the coins' own memory.
        std::atomic<int64_t> bytes{0};
        std::atomic<int64_t> entries{0};
        std::atomic<int64_t> dirty{0};
    };
    static constexpr size_t TOTALS_STRIPES{64};
    //! The calling thread's stripe.
    Totals& MyTotals() const;
    size_t SumTotals(std::atomic<int64_t> Totals::* field) const;
    //! What an entry's map node costs, as DynamicMemoryUsage() counts it.
    static int64_t NodeBytes();
    /**
     * How many change heights share a list of dirty entries, as a power of two. While nothing is written
     * out the dirty entries are the coins of every height since the last write, a few to a shard for each:
     * a list for every height would be an allocation of its own for nearly every dirty coin. An entry
     * keeps its height, and whoever needs heights apart inside a span reads them there.
     */
    static constexpr int DIRTY_SPAN_BITS{10};
    static size_t SpanOf(int height);
    //! The list for dirty entries of this change height; the shard's array grows to hold it.
    Bucket& DirtyBucket(Shard& shard, int height) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    //! Give the shard's array of dirty span sums back: it has no dirty entry left.
    void FreeDirtySpans(Shard& shard) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    //! The memory of a shard's coins grows or shrinks.
    void AddUsage(Shard& shard, size_t bytes) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    void SubUsage(Shard& shard, size_t bytes) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);

    Shard& ShardFor(const COutPoint& outpoint) const { return m_shards[ShardIndex(outpoint)]; }
    //! What a dirty entry counts for in the tier's usage, as DynamicMemoryUsage() measures it.
    static int64_t DirtyBytes(const Entry& entry);
    //! Take the entry out of its shard's counts. Before any change of its coin or height, which its span's
    //! byte sum depends on.
    void Detach(Shard& shard, Node& node) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    //! Count a detached entry as its state calls for.
    void Attach(Shard& shard, Node& node) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    //! A lookup found the entry.
    static void Reference(Entry& entry) { if (!entry.referenced) entry.referenced = true; }
    using State = coins_entry::State;
    using Event = coins_entry::Event;
    //! The entry's state: its flags, and whether its coin is spent.
    static State StateOf(const Entry& entry)
    {
        using namespace coins_entry;
        return static_cast<State>((entry.dirty ? DIRTY : 0) | (entry.fresh ? FRESH : 0) | (entry.collected ? COLLECTED : 0) |
                                  (entry.owed ? OWED : 0) | (entry.coin.IsSpent() ? SPENT : 0));
    }
    //! A coin put over the entry and filed under `height` owes the database the entry's erase
    //! (Event::CoinOverErase): the entry is dirty, spent and filed lower, an erase the database does not have
    //! yet, which a run bounded below the coin still writes (CollectChunk()).
    static bool OwesErase(const Entry& entry, int height) { return entry.dirty && entry.coin.IsSpent() && entry.height < height; }
    //! Put the entry in the state `to`, which the machine answered and which the entry's coin agrees
    //! with, and file it under `height`, which the machine answered as well. An event the machine refuses
    //! is a defect of the store's logic, and stops the node here. The span sums are by height: an entry
    //! changes its height out of its shard's counts (Detach()).
    static void Enter(Entry& entry, State to, int height)
    {
        using namespace coins_entry;
        Assert(to != REFUSED);
        Assert(static_cast<bool>(to & SPENT) == entry.coin.IsSpent());
        Assert(height == entry.height || entry.list == LIST_NONE);
        Assert(height >= 0 && height < MAX_FILED_HEIGHT);
        entry.height = static_cast<uint32_t>(height);
        entry.dirty = (to & DIRTY) != 0;
        entry.fresh = (to & FRESH) != 0;
        entry.collected = (to & COLLECTED) != 0;
        entry.owed = (to & OWED) != 0;
    }
    //! `event` happened to the entry at `height` (the change's or the coin's, as the event has it). The entry was in `from` before and has its coin as the event leaves it. With the
    //! entry out of its shard's counts where the event changes how it is counted (Detach()).
    static void Transit(Entry& entry, State from, Event event, int height)
    {
        using namespace coins_entry;
        Enter(entry, Next(from, event), NextHeight(HeightRule(from, event), from, entry.height, height));
    }
    //! The same for an event that has no height and leaves the entry where it is filed.
    static void Transit(Entry& entry, State from, Event event)
    {
        Assert(coins_entry::HeightRule(from, event) == coins_entry::Height::Keep);
        Enter(entry, coins_entry::Next(from, event), entry.height);
    }
    //! The same for events that leave the coin spent or unspent as it was.
    static void Transit(Entry& entry, Event event, int height) { Transit(entry, StateOf(entry), event, height); }
    static void Transit(Entry& entry, Event event) { Transit(entry, StateOf(entry), event); }
    //! Collect the shard's dirty entries at or below the run's bound into `writes` and `nodes` until
    //! `size`, about what the collected changes come to in a database batch, reaches `limit`; note the
    //! oldest change height left behind. Of a coin above the bound that was made over an erase the database
    //! is owed (Entry::owed), the erase is written.
    void CollectChunk(Shard& shard, WriteOutRun& run, uint64_t limit, uint64_t& size, std::vector<CoinWrite>& writes, std::vector<Node*>& nodes) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    //! A collected entry's value is in the database: clean (erased when spent), unless changed since.
    void SettleNode(Shard& shard, Node& node) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    void SpendEntry(Shard& shard, Map::iterator it, Coin* moveto) EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    //! What a lookup has of an entry it found, with the shard held: a copy of the coin, if there is one.
    static std::optional<Coin> SeenCoin(Entry& entry);
    /**
     * Visit the shards of a block's items, `shard_of[i]` being item i's: `work(shard, items)` runs once
     * for every shard that has any, with its lock held and the indices of its items in their order. The
     * walk passes over a shard another thread holds, coming back for those at the end: there is no order
     * between shards to keep.
     */
    template <typename Work>
    void VisitShards(const std::vector<uint32_t>& shard_of, Work work) const;
    //! Erase an entry, keeping the shard's counts right.
    void EraseEntry(Shard& shard, Map::iterator it) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    //! The same for an entry reached over a list, erased by its key: a cell has no link back, so the erase
    //! walks the chain from its bucket either way.
    void EraseEntry(Shard& shard, Node& node) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    //! Erase an entry that is out of its shard's counts and whose coin's usage is taken off the books already.
    void EraseDetached(Shard& shard, Map::iterator it) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    struct Wipe;
    struct WipeWorker;
    //! A flush's worker: takes its shards from the highest down, and writes what it took out of them.
    void RunWipeWorker(Wipe& wipe, WipeWorker& worker) NO_THREAD_SAFETY_ANALYSIS;
    //! Take the shard's entries out of it, its changes into the worker's batch in the order of their
    //! keys. Returns what the shard's changes come to as a database batch.
    uint64_t WipeShard(Shard& shard, WipeWorker& worker) EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    //! Write the worker's batch and unlock the shards it emptied: their changes are in the database.
    void WriteWipeBatch(const Wipe& wipe, WipeWorker& worker) NO_THREAD_SAFETY_ANALYSIS;
    //! The map's size or bucket count may have changed: count the difference into the totals.
    void Resized(Shard& shard) const EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    //! ApplyConfirmed()'s create and spend of one entry, at `height`, with the shard held. A create over a coin it
    //! may not overwrite never comes here: ApplyConfirmed() reports it as a conflict instead.
    void CreateLocked(Shard& shard, const COutPoint& outpoint, uint64_t hash, Coin&& coin, const CScript* script, bool possible_overwrite, int height) EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);
    void SpendEntryAt(Shard& shard, Map::iterator it, int height) EXCLUSIVE_LOCKS_REQUIRED(shard.mutex);

    /**
     * The change height an unspent coin arriving one by one is tagged with (AddCoin(), BatchWrite()). A coin
     * restored by a disconnect (the change height is then the fork height) that was created above the
     * settled height is not in the database. It keeps its creation height, below the fork or above it:
     * a run bounded below its creation leaves it and the settled head below its creating block, which a
     * replay then applies again, and a run bounded at or above its creation writes it. Tagged with the
     * fork height, a run bounded between the two would settle the database above its creation without
     * it. Any other coin takes the change height. Where the entry is dirty already, it stays filed under
     * the height of the earlier change if that is lower (coins_entry::Height::Oldest), and where that change
     * is an erase the database is still owed, the coin owes it in turn (OwesErase()).
     */
    int TagFor(const Coin& coin) const
    {
        return static_cast<int>(coin.nHeight) > m_written_height ? static_cast<int>(coin.nHeight) : m_change_height;
    }

    mutable std::vector<Shard> m_shards;
    mutable std::array<Totals, TOTALS_STRIPES> m_totals;
    mutable uint256 m_tier_block_hash;
    int m_change_height{0};
    int m_tier_height{-1};
    uint256 m_written_head;
    int m_written_height{-1};
    bool m_run_in_flight{false};
    //! A flush lost changes to a failed write (Flush()). The run it began never ends.
    bool m_flush_failed{false};
    //! The tier's block has been below the written head since the last final run began (a disconnect):
    //! every run is final, whatever its caller asked for (BeginWriteOut()).
    bool m_fell_below_written{false};
    //! Restored coins for the next run (KeepWritten()); the owner's alone.
    std::vector<CoinWrite> m_restored;

    //! See SetBaseWrites().
    uint64_t m_batch_bytes{DEFAULT_DB_CACHE_BATCH};
    bool m_base_concurrent{false};
    //! See SetFlushHelpers().
    std::shared_ptr<ThreadPool> m_flush_helpers;
};

/**
 * The coins the blocks of a window create before their changes are in the store (the window's outputs
 * overlay, validation.cpp).
 */
class WindowCoinSource
{
public:
    virtual ~WindowCoinSource() = default;
    //! Whether the window decides `outpoint` for a block at `viewer_height`: an unpublished block below it
    //! made its transaction, or the viewer's block did in one of its first `own_limit` transactions. `coin`
    //! is then the output, or empty where the transaction has no such output. An outpoint the window does
    //! not decide is the store's. The window does not know what its blocks spend: the coin may be one that
    //! a lower block of the window spent, which the block's commit finds (CoinsStore::ApplyConfirmed()).
    virtual bool FindCoin(const COutPoint& outpoint, int viewer_height, uint32_t own_limit, std::optional<Coin>& coin) const = 0;
    //! As FindCoin() for each input, into `coins` by position: `decided` is set where the window decides the
    //! input, the coin then the output (also one an earlier transaction of the viewer's block makes) or none.
    virtual void FindInputs(std::span<const COutPoint> outpoints, std::span<const uint32_t> positions, int viewer_height,
                            std::vector<uint8_t>& decided, std::vector<Coin>& coins) const = 0;
    //! Which of the transactions' ids an unpublished block below `viewer_height` may make a transaction
    //! of: `named` is set for those. No output of the others is the window's to decide: FindCoin() would
    //! answer false for each.
    virtual void NamedIds(std::span<const CTransactionRef> txs, int viewer_height, std::vector<uint8_t>& named) const = 0;
};

/**
 * The view a block is connected into: a CoinsViewOverlay (prefetch and the block's own changes for the
 * core's lookups and references) whose changes are committed to the store once the block is valid, as
 * the changes of its height (CoinsStore::ApplyConfirmed()). A block that fails leaves nothing in the store.
 */
class BlockApplyView final : public CoinsViewOverlay
{
public:
    BlockApplyView(CoinsStore& store, util::NotNullSharedPtr<ThreadPool> thread_pool, bool deterministic = false);
    ~BlockApplyView() override;

    //! The view's block is at this height: the view is staged with a block. Cleared by the view's reset.
    void SetHeight(int height) { m_height = height; }
    bool InUse() const { return m_height >= 0; }
    //! Whether a coin the view reads from the database stays in the store, clean (CoinsStore::GetCoin()),
    //! so that the commit of its spend does not read the database a second time. For a window block. A block
    //! at the tip reads without it: one that fails leaves none of its inputs in the cache.
    //! Set before the block's first lookup; the view's reset clears it.
    void SetKeepBaseReads(bool keep) { m_keep_base_reads = keep; }
    //! A window block's reads that the view does not hold look in the window's outputs before the store.
    //! Cleared by the view's reset.
    void SetOverlay(const WindowCoinSource* overlay, int height)
    {
        m_overlay = overlay;
        m_overlay_height = height;
    }

    //! The coin of each of the block's inputs by position (the coinbase has none), from the window's outputs,
    //! the tier or the database; a spent coin where there is none. The view is left as it was.
    void ResolveInputs(const CBlock& block, std::vector<Coin>& coins);
    //! The transaction at `position` is being checked (BlockValidationContext::window_tx): the block's outputs
    //! are not added to the view, and its reads find those of the transactions below in the overlay; a lookup
    //! of one of the transaction's inputs' outpoints, which the checks make with the input's own `prevout`, is
    //! answered from `coins`, the coins of its inputs by position. No transaction once all are checked.
    //! Cleared by the view's reset.
    void SetTransaction(uint32_t position, const CTransaction* tx, std::span<const Coin> coins)
    {
        m_own_limit = position;
        m_inputs_tx = tx;
        m_inputs_coins = coins;
    }
    const Coin& AccessCoin(const COutPoint& outpoint) const override;
    bool HaveCoin(const COutPoint& outpoint) const override;
    //! As the cache's, and the spend left in the view keeps the height and the coinbase flag of its coin.
    bool SpendCoin(const COutPoint& outpoint, Coin* moveto = nullptr) override;
    //! A window block's view, before the block's first transaction, answers for the block at once: the
    //! window's outputs are asked once for each transaction, the store once for each shard. Only an output
    //! that one of them may have is asked as the cache asks.
    bool HaveAnyOutput(std::span<const CTransactionRef> txs) const override;

    //! Give the view back: the block's changes are dropped.
    void Release() { Reset(); }
    CoinsStore& Store() const { return m_store; }
    //! A block connected alone, valid: its changes go into the store as confirmed changes
    //! (CoinsStore::ApplyConfirmed()), a spend of a coin the store does not hold with the coin `undo` has for it.
    //! Requires that nothing changed the store since the block's view read it, so that a coin the store
    //! does not hold now is one it did not hold then.
    void CommitAlone(const CBlock& block, const CBlockUndo& undo);
    //! A commit made the view's changes in the store: the view is emptied.
    void ChangesCommitted();
    //! The block is committed and published: the store's best block moves to it. The view is reset.
    void ConfirmCommitted(int height);
    //! One change of a window block, as its commit needs it once the view is emptied: a spend names the
    //! input's outpoint in the block and the coin spent (`n` its height twice plus the coinbase flag), a
    //! create the transaction in the block and its output `n`, from which the commit makes the coin. The
    //! block outlives the commit.
    struct CommitItem {
        //! CONFLICT: the commit did not make the change (CoinsStore::ConfirmedReport).
        static constexpr uint8_t SPEND{1}, FRESH{2}, OVERWRITE{4}, KNOWN{8}, DROPPED{16}, CONFLICT{32};
        const void* at;
        uint32_t n;
        uint8_t flags;
        bool Spend() const { return flags & SPEND; }
        const CTransaction& Tx() const { return *static_cast<const CTransaction*>(at); }
        COutPoint Outpoint() const { return Spend() ? *static_cast<const COutPoint*>(at) : COutPoint{Tx().GetHash(), n}; }
    };

protected:
    std::optional<Coin> ReadBase(const COutPoint& outpoint) const override;
    void Reset() noexcept override;

private:
    CoinsStore& m_store;
    bool m_keep_base_reads{false};
    const WindowCoinSource* m_overlay{nullptr};
    int m_overlay_height{0};
    uint32_t m_own_limit{0};
    int m_height{-1};
    const CTransaction* m_inputs_tx{nullptr};
    std::span<const Coin> m_inputs_coins{};
    //! The coin SetTransaction() gave for `outpoint` if it is an input's own prevout, by its address.
    const Coin* InputCoin(const COutPoint& outpoint) const;
};

#endif // BITCOIN_COINSSTORE_H
