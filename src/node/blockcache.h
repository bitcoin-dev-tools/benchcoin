// Copyright (c) 2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_NODE_BLOCKCACHE_H
#define BITCOIN_NODE_BLOCKCACHE_H

#include <primitives/block.h>
#include <sync.h>
#include <uint256.h>
#include <util/byte_account.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <utility>

namespace node {

/**
 * Accepted blocks kept in memory until they are connected, so that the connect loop takes the object that was
 * received and checked instead of reading the block back from disk and deserialising and hashing it again. During
 * initial block download blocks arrive out of order inside the download window, so the block being connected is
 * rarely the one that just arrived.
 *
 * Bounded by a byte budget: when it is exceeded the highest blocks are dropped first, since the lowest connect
 * next, and with a snapshot the historical chainstate's before the active one's, which the connector takes
 * first; blocks a tip has passed are dropped on insertion (Tips: with a snapshot, a block at or below its base
 * is the historical chainstate's). An object taken from here still carries CBlock::fChecked, so ConnectBlock()
 * does not run CheckBlock() on it again. A budget of zero disables it.
 */
class BlockCache
{
    mutable Mutex m_mutex;
    const size_t m_budget;
    //! What the kept blocks count, changed under m_mutex, with the budget as its limit.
    util::ByteAccount m_account;
    struct Entry {
        std::shared_ptr<const CBlock> block;
        size_t bytes;
    };
    //! Ordered by height, then hash.
    using Blocks = std::map<std::pair<int, uint256>, Entry>;
    Blocks m_blocks GUARDED_BY(m_mutex);
    //! The last insertion dropped blocks to stay within the budget, and nothing has left since: the budget is
    //! used up, and a block above every one kept would be the first to go.
    bool m_full GUARDED_BY(m_mutex){false};

public:
    explicit BlockCache(size_t budget)
        : m_budget{budget}
    {
        m_account.SetLimit(budget);
    }

    //! What a block counts against the budget.
    static size_t Bytes(const CBlock& block);
    //! The tips a block is measured against: one at or below `historical_end` (a snapshot's base) is the
    //! historical chainstate's, passed once its tip `historical` reaches it; any other is passed once the
    //! active tip does. Without a historical chainstate `historical_end` is -1.
    struct Tips {
        int active;
        int historical{-1};
        int historical_end{-1};
        bool Passed(int height) const { return height <= historical_end ? height <= historical : height <= active; }
    };

private:
    //! Drop the blocks a tip has passed: they will never be taken.
    void DropPassed(const Tips& tips) EXCLUSIVE_LOCKS_REQUIRED(m_mutex);
    //! The block that goes first when the budget is exceeded; the cache is not empty.
    Blocks::iterator FirstToGo(const Tips& tips) EXCLUSIVE_LOCKS_REQUIRED(m_mutex);

public:
    //! Keep `block`, accepted at `height`, when the budget allows, after dropping the blocks a tip has passed.
    //! `bytes` is Bytes(*block) if the caller measured it already (with no lock held), else 0.
    void Insert(int height, const std::shared_ptr<const CBlock>& block, const Tips& tips, size_t bytes = 0) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    //! The same with the active tip alone.
    void Insert(int height, const std::shared_ptr<const CBlock>& block, int tip_height, size_t bytes = 0) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex)
    {
        Insert(height, block, Tips{.active = tip_height}, bytes);
    }
    //! The block `hash` at `height`, removed from the cache; null when it is not here.
    //! `bytes`, if given, receives the taken block's Bytes() as measured at its insertion.
    std::shared_ptr<const CBlock> Take(int height, const uint256& hash, size_t* bytes = nullptr) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    //! Drop the block `hash` at `height` if it is here, as a block connected from another copy (the one passed
    //! in) will never be taken.
    void Drop(int height, const uint256& hash) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    //! Whether the block `hash` at `height` is here.
    bool Has(int height, const uint256& hash) const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex) { return WITH_LOCK(m_mutex, return m_blocks.contains({height, hash})); }

    size_t Budget() const { return m_budget; }
    size_t Bytes() const { return m_account.Bytes(); }
    size_t Count() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex) { return WITH_LOCK(m_mutex, return m_blocks.size()); }
};

} // namespace node

#endif // BITCOIN_NODE_BLOCKCACHE_H
