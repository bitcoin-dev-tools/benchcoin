// Copyright (c) 2026 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/blockcache.h>

#include <core_memusage.h>
#include <primitives/block.h>
#include <sync.h>
#include <uint256.h>

#include <algorithm>
#include <iterator>
#include <limits>
#include <memory>
#include <utility>

namespace node {

size_t BlockCache::Bytes(const CBlock& block)
{
    return sizeof(CBlock) + RecursiveDynamicUsage(block);
}

void BlockCache::DropPassed(const Tips& tips)
{
    const auto drop{[&](int from, int to) EXCLUSIVE_LOCKS_REQUIRED(m_mutex) {
        if (from > to) return;
        const auto last{m_blocks.lower_bound({to + 1, uint256{}})};
        for (auto it{m_blocks.lower_bound({from, uint256{}})}; it != last;) {
            m_account.Sub(it->second.bytes);
            it = m_blocks.erase(it);
            m_full = false;
        }
    }};
    if (tips.historical_end < 0) {
        drop(std::numeric_limits<int>::min(), tips.active);
    } else {
        drop(std::numeric_limits<int>::min(), std::min(tips.historical, tips.historical_end));
        drop(tips.historical_end + 1, tips.active);
    }
}

BlockCache::Blocks::iterator BlockCache::FirstToGo(const Tips& tips)
{
    // The historical chainstate's highest block, while it has one here: the active chain is the one synced to
    // the tip, and the connector takes its blocks first. Then the active chain's highest.
    if (tips.historical_end >= 0) {
        if (const auto end{m_blocks.lower_bound({tips.historical_end + 1, uint256{}})}; end != m_blocks.begin()) return std::prev(end);
    }
    return std::prev(m_blocks.end());
}

void BlockCache::Insert(int height, const std::shared_ptr<const CBlock>& block, const Tips& tips, size_t bytes)
{
    if (m_budget == 0 || tips.Passed(height)) return;
    // With the budget used up by blocks no tip has passed, a block that would be the first to go is neither
    // measured nor kept (a reindex's files, read far ahead of the tip, pass here). Used up means the block's
    // known size does not fit, or, not measured yet, the last insertion had to drop blocks.
    {
        LOCK(m_mutex);
        DropPassed(tips);
        if (!m_blocks.empty() && (bytes > 0 ? !m_account.Admits(bytes) : m_full)) {
            const int first{FirstToGo(tips)->first.first};
            const bool historical{height <= tips.historical_end};
            const bool first_historical{first <= tips.historical_end};
            if (historical == first_historical ? height > first : historical) return;
        }
    }
    if (bytes == 0) bytes = Bytes(*block);
    if (bytes > m_budget) return;
    LOCK(m_mutex);
    DropPassed(tips);
    const auto [it, inserted]{m_blocks.try_emplace({height, block->GetHash()}, Entry{block, bytes})};
    if (!inserted) return;
    m_account.Add(bytes);
    // Over budget: the highest blocks go first, the lowest connect next; the historical chainstate's first.
    while (!m_account.Admits()) {
        const auto last{FirstToGo(tips)};
        m_account.Sub(last->second.bytes);
        m_blocks.erase(last);
        m_full = true;
    }
}

void BlockCache::Drop(int height, const uint256& hash)
{
    if (m_budget == 0) return;
    LOCK(m_mutex);
    const auto it{m_blocks.find({height, hash})};
    if (it == m_blocks.end()) return;
    m_account.Sub(it->second.bytes);
    m_blocks.erase(it);
    m_full = false;
}

std::shared_ptr<const CBlock> BlockCache::Take(int height, const uint256& hash, size_t* bytes)
{
    if (m_budget == 0) return nullptr;
    LOCK(m_mutex);
    const auto it{m_blocks.find({height, hash})};
    if (it == m_blocks.end()) return nullptr;
    std::shared_ptr<const CBlock> block{std::move(it->second.block)};
    if (bytes) *bytes = it->second.bytes;
    m_account.Sub(it->second.bytes);
    m_blocks.erase(it);
    m_full = false;
    return block;
}

} // namespace node
