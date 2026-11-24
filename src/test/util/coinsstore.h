// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_TEST_UTIL_COINSSTORE_H
#define BITCOIN_TEST_UTIL_COINSSTORE_H

#include <coins.h>
#include <coinsstore.h>
#include <primitives/transaction.h>

#include <cstdint>
#include <utility>
#include <vector>

//! A change that owns its outpoint and its coin, as a test writes one down (CoinsStore::Change points at
//! a view's entry).
struct OwnedChange {
    COutPoint outpoint;
    Coin coin;
    bool possible_overwrite{false};
    bool known_coin{false};
};

//! CoinsStore::ApplyConfirmed() of `changes` as the changes of the block at `height`: the number of
//! conflicts. A report names the outpoints in `changes`.
inline uint32_t ApplyConfirmed(CoinsStore& store, int height, std::vector<OwnedChange>& changes, CoinsStore::ConfirmedReport* report = nullptr)
{
    std::vector<CoinsStore::Change> refs;
    refs.reserve(changes.size());
    for (OwnedChange& change : changes) {
        refs.push_back({.outpoint = &change.outpoint, .coin = &change.coin, .possible_overwrite = change.possible_overwrite, .known_coin = change.known_coin});
    }
    return store.ApplyConfirmed(std::move(refs), height, report);
}

#endif // BITCOIN_TEST_UTIL_COINSSTORE_H
