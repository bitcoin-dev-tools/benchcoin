// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_UTIL_BYTE_ACCOUNT_H
#define BITCOIN_UTIL_BYTE_ACCOUNT_H

#include <atomic>
#include <cstddef>

namespace util {

/**
 * A count of the bytes a holder keeps in memory, with the holder's limit. The
 * holder changes the count under whatever lock orders its own work, and asks
 * Admits() before it takes more: where the answer is no it waits or leaves the
 * work for later, it never fails. A limit of zero is no limit. Safe to read
 * from any thread.
 */
class ByteAccount
{
    std::atomic<size_t> m_bytes{0};
    std::atomic<size_t> m_limit{0};

public:
    void Add(size_t bytes) { m_bytes.fetch_add(bytes, std::memory_order_relaxed); }
    void Sub(size_t bytes) { m_bytes.fetch_sub(bytes, std::memory_order_relaxed); }
    //! For a holder that works its count out anew each time.
    void Set(size_t bytes) { m_bytes.store(bytes, std::memory_order_relaxed); }
    //! Zero: the account has no limit in bytes.
    void SetLimit(size_t limit) { m_limit.store(limit, std::memory_order_relaxed); }

    size_t Bytes() const { return m_bytes.load(std::memory_order_relaxed); }
    size_t Limit() const { return m_limit.load(std::memory_order_relaxed); }
    //! Whether the account stays within its limit with `more` bytes added. An account without a limit admits
    //! everything. The holder decides what it does with one that is empty and still does not admit.
    bool Admits(size_t more = 0) const
    {
        const size_t limit{Limit()};
        return limit == 0 || Bytes() + more <= limit;
    }
};
} // namespace util

#endif // BITCOIN_UTIL_BYTE_ACCOUNT_H
