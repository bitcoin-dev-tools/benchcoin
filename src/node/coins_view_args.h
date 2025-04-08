// Copyright (c) 2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_NODE_COINS_VIEW_ARGS_H
#define BITCOIN_NODE_COINS_VIEW_ARGS_H

#include <node/caches.h>

class ArgsManager;
struct CoinsViewOptions;

namespace node
{
    static constexpr size_t MIN_DB_CACHE_BATCH{16_MiB};
    static constexpr size_t DEFAULT_DB_CACHE_BATCH{16_MiB}; // The batch size of DEFAULT_KERNEL_CACHE
    static constexpr size_t MAX_DB_CACHE_BATCH{256_MiB};

    static constexpr size_t GetDbBatchSize(const size_t dbcache_bytes)
    {
        const size_t target{(dbcache_bytes / DEFAULT_KERNEL_CACHE) * DEFAULT_DB_CACHE_BATCH};
        return std::max<size_t>(MIN_DB_CACHE_BATCH, std::min<size_t>(MAX_DB_CACHE_BATCH, target));
    }

    void ReadCoinsViewArgs(const ArgsManager& args, CoinsViewOptions& options);
} // namespace node

#endif // BITCOIN_NODE_COINS_VIEW_ARGS_H
