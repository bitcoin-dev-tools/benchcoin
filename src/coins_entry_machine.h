// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_COINS_ENTRY_MACHINE_H
#define BITCOIN_COINS_ENTRY_MACHINE_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

/**
 * The state of one coin entry in the coins store (CoinsStore::Entry), and
 * every way it changes.
 *
 * The state is a set of bits: four flags the entry stores in one byte, and
 * whether its coin is spent, which the coin says. Few of the 256 sets mean
 * anything: IsStored() names those an entry of the store may rest in, and
 * ABSENT is the entry that was just inserted and holds nothing yet.
 *
 * An event is something the store does to an entry: a change, or a write-out
 * run collecting the entry or landing. RULES lists, per event, the states it
 * applies to and what it clears and sets. TABLES is that list as a table from
 * (event, state) to the next state, built during compilation, which refuses
 * to build if
 *  - two rules of an event apply to one state, or
 *  - a rule leads from a state an entry may be in to one it may not be in.
 * The set of states is therefore closed under the events, by construction.
 * ABSENT is one of them, and a rule may lead there (a run that lands on an
 * entry with nothing left to say): that such an entry leaves the map is the
 * store's to do (SettleNode()), not the table's.
 * An event in a state that none of its rules names is refused (REFUSED): the
 * store's logic went wrong before, and the store stops there.
 *
 * An entry is also filed under a height (CoinsStore::Entry::height), which
 * decides which write-out run carries it. Each rule says what its event does
 * to that height (Height), and NextHeight() is the one place the answer is
 * worked out. What the rules keep, between them:
 *
 *   An entry that owes the database an erase is filed at or below the height
 *   the erase is owed since, so that a run bounded at that height or above
 *   writes it.
 *
 * The machine holds no entry. The store changes an entry's flags and its
 * height through CoinsStore::Transit(), which asks Next() and NextHeight(), and
 * CoinsStore::Enter(), with what they gave; erasing an entry is the one change
 * that is not an event, since nothing is left to have a state.
 */
namespace coins_entry {
using State = uint8_t;

//! The entry differs from the database: it is counted among its shard's dirty entries, for a write-out run to find.
inline constexpr State DIRTY{1 << 0};
//! The database is known not to hold the outpoint, so a spend leaves nothing to erase.
inline constexpr State FRESH{1 << 1};
//! A run collected the entry's value and has not settled it; any change clears the bit, and the entry
//! then stays dirty when the run's chunk settles.
inline constexpr State COLLECTED{1 << 2};
//! A coin made over an erase the database is still owed, the entry filed under the height of that erase.
//! A run bounded below the coin writes the erase and files the entry under the coin's height.
inline constexpr State OWED{1 << 3};
//! The coin is spent. Not stored with the flags: the coin says it.
inline constexpr State SPENT{1 << 7};

//! The flags an entry stores.
inline constexpr State FLAGS{DIRTY | FRESH | COLLECTED | OWED};
//! An entry just inserted: no coin and nothing to say. Every insertion is followed by an event.
inline constexpr State ABSENT{SPENT};
//! A clean, unspent copy of what the database holds.
inline constexpr State BASE_COPY{0};
//! No state: what Next() answers for an event the machine refuses.
inline constexpr State REFUSED{0xff};

//! Whether an entry of the store may rest in `s`.
constexpr bool IsStored(State s)
{
    // A bit that is neither a flag nor SPENT means nothing.
    if (s & ~(FLAGS | SPENT)) return false;
    const bool dirty(s & DIRTY), fresh(s & FRESH), collected(s & COLLECTED), owed(s & OWED), spent(s & SPENT);
    // A clean entry is an unspent copy of the database's coin and nothing else: a spent entry is only
    // ever kept to record an erase, and what a run left on an entry goes when it is written.
    if (!dirty) return s == BASE_COPY;
    // A fresh entry is a coin, neither on its way to the database nor over something the database is
    // owed.
    if (fresh && (spent || collected || owed)) return false;
    // An erase is owed under a coin only.
    if (owed && spent) return false;
    return true;
}

//! Whether the machine knows `s`: a state an entry may rest in, or the one it is inserted in.
constexpr bool IsKnown(State s) { return s == ABSENT || IsStored(s); }

enum class Event : uint8_t {
    //! A lookup brings in the database's coin.
    BaseCopy,
    //! A confirmed coin the database is known not to hold.
    ConfirmedNew,
    //! A confirmed coin written over whatever was there, of which the database may hold something.
    ConfirmedWrite,
    //! As ConfirmedWrite, from a child view's batch over an entry that was there: one the database is
    //! known not to hold stays so.
    BatchOverwrite,
    //! A confirmed spend of an entry the database may hold: the erase is to be written.
    ConfirmedSpend,
    //! A run wrote a coin a disconnect restored: the database holds the outpoint now.
    KeptWritten,
    //! A confirmed coin made over an erase the database is still owed: the erase stays owed under it.
    CoinOverErase,
    //! A run wrote the erase owed under a coin.
    RunWritesOwed,
    //! A run collects the entry's value for the database.
    RunCollects,
    //! The run's chunk is in the database and the entry was not changed since.
    RunLands,
};
inline constexpr size_t EVENT_COUNT{static_cast<size_t>(Event::RunLands) + 1};

/** What an event does to the height an entry is filed under. The event comes with a height of its own. */
enum class Height : uint8_t {
    //! The entry stays where it is filed; the event's height, if it has one, is not looked at.
    Keep,
    //! The entry is filed under the event's height: the change's or the coin's.
    Take,
    //! As Take, unless the entry is dirty, not collected by a run in flight, and filed lower: it stays there,
    //! since a change the database does not have, owed or not, is still to be written as of its height (a
    //! collected change is the run's to write, and the new one is all that is left). A change a disconnect
    //! makes comes from no block on the chain, and a crash's replay, which makes again only what the blocks
    //! above the settled head do, would not make it.
    Oldest,
};

/**
 * In a state that has every bit of `all_of` and none of `none_of`, `event` clears the bits of `clear`
 * and then sets those of `set`, and does `height` to the height the entry is filed under. A rule states
 * SPENT like any other bit: what the coin must be before, and what the store makes of it with the event.
 */
struct Rule {
    Event event;
    State all_of;
    State none_of;
    State clear;
    State set;
    Height height;
};

inline constexpr State EVERYTHING{0xff};

// clang-format off
inline constexpr Rule RULES[]{
    // event                          all_of                        none_of        clear                set                         height
    {Event::BaseCopy,                 SPENT,                        FLAGS,         EVERYTHING,          BASE_COPY,                  Height::Take},
    {Event::ConfirmedNew,             SPENT,                        FLAGS,         EVERYTHING,          DIRTY | FRESH,              Height::Take},
    {Event::ConfirmedWrite,           0,                            0,             EVERYTHING,          DIRTY,                      Height::Oldest},
    {Event::BatchOverwrite,           DIRTY,                        0,             EVERYTHING & ~FRESH, DIRTY,                      Height::Oldest},
    {Event::BatchOverwrite,           0,                            DIRTY | SPENT, EVERYTHING,          DIRTY,                      Height::Oldest},
    {Event::ConfirmedSpend,           0,                            FRESH,         EVERYTHING,          DIRTY | SPENT,              Height::Oldest},
    {Event::KeptWritten,              0,                            0,             FRESH,               0,                          Height::Keep},
    {Event::CoinOverErase,            DIRTY | SPENT,                0,             EVERYTHING,          DIRTY | OWED,               Height::Keep},
    {Event::RunWritesOwed,            OWED,                         0,             OWED,                0,                          Height::Take},
    {Event::RunCollects,              DIRTY,                        0,             FRESH,               COLLECTED,                  Height::Keep},
    {Event::RunLands,                 DIRTY | COLLECTED,            0,             DIRTY | COLLECTED | OWED, 0,                     Height::Keep},
};
// clang-format on

//! By event and state: the state the event leads to, or REFUSED, and what it does to the height the entry is
//! filed under, Keep (the first) where it is refused.
struct Tables {
    std::array<std::array<State, 256>, EVENT_COUNT> next;
    std::array<std::array<Height, 256>, EVENT_COUNT> height;
};

consteval Tables MakeTables()
{
    Tables tables{};
    for (auto& row : tables.next) row.fill(REFUSED);
    for (const Rule& rule : RULES) {
        const size_t e{static_cast<size_t>(rule.event)};
        for (size_t s{0}; s < 256; ++s) {
            const State from{static_cast<State>(s)};
            if (!IsKnown(from)) continue;
            if ((from & rule.all_of) != rule.all_of || (from & rule.none_of) != 0) continue;
            if (tables.next[e][s] != REFUSED) throw "two rules of one event apply to one state";
            const State to{static_cast<State>((from & ~rule.clear) | rule.set)};
            if (!IsKnown(to)) throw "a rule leads out of the states an entry may be in";
            tables.next[e][s] = to;
            tables.height[e][s] = rule.height;
        }
    }
    return tables;
}

inline constexpr Tables TABLES{MakeTables()};
static_assert(!IsKnown(REFUSED));

//! The state `event` leads to from `from`, or REFUSED.
constexpr State Next(State from, Event event) { return TABLES.next[static_cast<size_t>(event)][from]; }
//! What `event` does in `from` to the height the entry is filed under. Keep where Next() refuses.
constexpr Height HeightRule(State from, Event event) { return TABLES.height[static_cast<size_t>(event)][from]; }

/**
 * The height an entry is filed under after an event: `rule` is what the event does (HeightRule()), `from`
 * the state before it, `filed` the height the entry is filed under now (which means nothing for an entry
 * just inserted) and `event_height` the event's.
 */
constexpr int NextHeight(Height rule, State from, int filed, int event_height)
{
    switch (rule) {
    case Height::Keep: return filed;
    case Height::Take: return event_height;
    case Height::Oldest: return (from & DIRTY) && !(from & COLLECTED) ? std::min(filed, event_height) : event_height;
    } // no default case, so the compiler can warn about missing cases
    return filed;
}

} // namespace coins_entry

#endif // BITCOIN_COINS_ENTRY_MACHINE_H
