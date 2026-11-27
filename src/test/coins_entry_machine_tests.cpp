// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <coins_entry_machine.h>

#include <boost/test/unit_test.hpp>

#include <array>
#include <cstddef>
#include <set>
#include <vector>

using namespace coins_entry;

namespace {
constexpr State Go(State from, Event event) { return Next(from, event); }

std::vector<Event> AllEvents()
{
    std::vector<Event> events;
    for (size_t e{0}; e < EVENT_COUNT; ++e) events.push_back(static_cast<Event>(e));
    return events;
}
} // namespace

BOOST_AUTO_TEST_SUITE(coins_entry_machine_tests)

BOOST_AUTO_TEST_CASE(known_states)
{
    // The states an entry may rest in, written out: a change of IsStored() shows here.
    const std::set<State> stored{
        BASE_COPY,
        DIRTY,
        DIRTY | FRESH,
        DIRTY | COLLECTED,
        DIRTY | OWED,
        DIRTY | OWED | COLLECTED,
        DIRTY | SPENT,
        DIRTY | SPENT | COLLECTED,
    };
    for (size_t s{0}; s < 256; ++s) {
        const State state{static_cast<State>(s)};
        BOOST_CHECK_EQUAL(IsStored(state), stored.contains(state));
        BOOST_CHECK_EQUAL(IsKnown(state), stored.contains(state) || state == ABSENT);
    }
    BOOST_CHECK(!IsStored(ABSENT));
    BOOST_CHECK(!IsKnown(REFUSED));
}

BOOST_AUTO_TEST_CASE(table_is_closed_and_refuses_the_unknown)
{
    for (const Event event : AllEvents()) {
        for (size_t s{0}; s < 256; ++s) {
            const State from{static_cast<State>(s)};
            const State to{Go(from, event)};
            if (!IsKnown(from)) {
                BOOST_CHECK_EQUAL(to, REFUSED);
            } else {
                BOOST_CHECK(to == REFUSED || IsKnown(to));
            }
        }
    }
}

BOOST_AUTO_TEST_CASE(every_known_state_is_reached)
{
    // No state is allowed that nothing leads to: the set is what the events make of an inserted entry.
    std::array<bool, 256> seen{};
    std::vector<State> todo{ABSENT};
    seen[ABSENT] = true;
    while (!todo.empty()) {
        const State from{todo.back()};
        todo.pop_back();
        const auto add{[&](State to) {
            if (to == REFUSED || seen[to]) return;
            seen[to] = true;
            todo.push_back(to);
        }};
        for (const Event event : AllEvents()) add(Go(from, event));
    }
    for (size_t s{0}; s < 256; ++s) BOOST_CHECK_EQUAL(seen[s], IsKnown(static_cast<State>(s)));
}

BOOST_AUTO_TEST_CASE(insertions)
{
    // An inserted entry takes exactly the events that bring something in.
    const std::set<Event> inserting{Event::BaseCopy, Event::ConfirmedNew, Event::ConfirmedWrite, Event::ConfirmedSpend};
    for (const Event event : AllEvents()) {
        const State to{Go(ABSENT, event)};
        // What clears a bit an inserted entry does not have leaves it as it is: harmless, and not an insertion.
        if (to == ABSENT) continue;
        BOOST_CHECK_EQUAL(to != REFUSED, inserting.contains(event));
    }
    BOOST_CHECK_EQUAL(Go(ABSENT, Event::BaseCopy), BASE_COPY);
    BOOST_CHECK_EQUAL(Go(ABSENT, Event::ConfirmedNew), DIRTY | FRESH);
    BOOST_CHECK_EQUAL(Go(ABSENT, Event::ConfirmedWrite), DIRTY);
    BOOST_CHECK_EQUAL(Go(ABSENT, Event::ConfirmedSpend), DIRTY | SPENT);
    // And only an inserted entry takes those that say the tier held nothing.
    for (size_t s{0}; s < 256; ++s) {
        const State from{static_cast<State>(s)};
        if (from == ABSENT) continue;
        for (const Event event : {Event::BaseCopy, Event::ConfirmedNew}) {
            BOOST_CHECK_EQUAL(Go(from, event), REFUSED);
        }
    }
}

BOOST_AUTO_TEST_CASE(fresh_means_nothing_to_erase)
{
    // A confirmed spend of a fresh entry is no event: the entry is erased, there being no erase to write.
    BOOST_CHECK_EQUAL(Go(DIRTY | FRESH, Event::ConfirmedSpend), REFUSED);
    // A collected entry is not fresh: a spend while the run is under way must produce an erase.
    BOOST_CHECK_EQUAL(Go(DIRTY | FRESH, Event::RunCollects), DIRTY | COLLECTED);
    BOOST_CHECK_EQUAL(Go(DIRTY | COLLECTED, Event::ConfirmedSpend), DIRTY | SPENT);
    // A child's batch over an entry keeps what is known of the database, a plain overwrite does not.
    BOOST_CHECK_EQUAL(Go(DIRTY | FRESH, Event::BatchOverwrite), DIRTY | FRESH);
    BOOST_CHECK_EQUAL(Go(DIRTY | FRESH, Event::ConfirmedWrite), DIRTY);
    // A coin over a spend not yet written is not fresh: the database may hold the coin that was spent.
    BOOST_CHECK_EQUAL(Go(DIRTY | SPENT, Event::ConfirmedNew), REFUSED);
    BOOST_CHECK_EQUAL(Go(DIRTY | SPENT, Event::ConfirmedWrite), DIRTY);
    // A restored coin a run wrote is in the database.
    BOOST_CHECK_EQUAL(Go(DIRTY | FRESH, Event::KeptWritten), DIRTY);
}

BOOST_AUTO_TEST_CASE(owed_under_a_create)
{
    // A coin made over a spend not yet written owes the erase; no other entry takes the event.
    const State owing{Go(DIRTY | SPENT, Event::CoinOverErase)};
    BOOST_CHECK_EQUAL(owing, DIRTY | OWED);
    BOOST_CHECK_EQUAL(Go(DIRTY | SPENT | COLLECTED, Event::CoinOverErase), owing);
    BOOST_CHECK_EQUAL(Go(DIRTY, Event::CoinOverErase), REFUSED);
    BOOST_CHECK_EQUAL(Go(BASE_COPY, Event::CoinOverErase), REFUSED);
    BOOST_CHECK_EQUAL(Go(ABSENT, Event::CoinOverErase), REFUSED);
    // A run writes what is owed and leaves the coin.
    BOOST_CHECK_EQUAL(Go(owing, Event::RunWritesOwed), DIRTY);
    BOOST_CHECK_EQUAL(Go(DIRTY, Event::RunWritesOwed), REFUSED);
    // A spend of the coin leaves the erase to write, and a write over it a coin the database may differ from.
    BOOST_CHECK_EQUAL(Go(owing, Event::ConfirmedSpend), DIRTY | SPENT);
    BOOST_CHECK_EQUAL(Go(owing, Event::ConfirmedWrite), DIRTY);
    // Within a run's bound the entry is collected with what it owed, and lands clean.
    BOOST_CHECK_EQUAL(Go(owing, Event::RunCollects), owing | COLLECTED);
    BOOST_CHECK_EQUAL(Go(owing | COLLECTED, Event::RunLands), BASE_COPY);
    // An erase that lands leaves nothing, and the entry is erased.
    BOOST_CHECK_EQUAL(Go(DIRTY | SPENT | COLLECTED, Event::RunLands), ABSENT);
}

BOOST_AUTO_TEST_CASE(a_change_withdraws_the_entry_from_its_run)
{
    // Every event clears COLLECTED but those of a run on the entry it collected (which leave the value
    // the run took as it is), and the one that says a restored coin was written.
    const std::set<Event> keeping{Event::RunWritesOwed, Event::KeptWritten};
    for (const Event event : AllEvents()) {
        if (event == Event::RunCollects) continue;
        for (size_t s{0}; s < 256; ++s) {
            const State from{static_cast<State>(s)};
            if (!IsKnown(from) || !(from & COLLECTED)) continue;
            const State to{Go(from, event)};
            if (to == REFUSED) continue;
            BOOST_CHECK_EQUAL(static_cast<bool>(to & COLLECTED), keeping.contains(event));
        }
    }
    // Only a collected entry lands.
    for (size_t s{0}; s < 256; ++s) {
        const State from{static_cast<State>(s)};
        if (!IsKnown(from)) continue;
        BOOST_CHECK_EQUAL(Go(from, Event::RunLands) != REFUSED, static_cast<bool>(from & COLLECTED));
        if (Go(from, Event::RunLands) != REFUSED) BOOST_CHECK(!(Go(from, Event::RunLands) & DIRTY));
    }
}

namespace {
//! An entry as far as the machine knows it: its state and the height it is filed under.
struct Filed {
    State state;
    int height;
};
//! `event` at `height` happens to `entry`.
Filed Step(Filed entry, Event event, int height)
{
    const State to{Next(entry.state, event)};
    BOOST_REQUIRE(to != REFUSED);
    return {to, NextHeight(HeightRule(entry.state, event), entry.state, entry.height, height)};
}
} // namespace

BOOST_AUTO_TEST_CASE(what_each_height_rule_does)
{
    BOOST_CHECK(NextHeight(Height::Keep, DIRTY, 5, 9) == 5);
    BOOST_CHECK(NextHeight(Height::Take, DIRTY, 5, 9) == 9);
    BOOST_CHECK(NextHeight(Height::Take, DIRTY, 9, 5) == 5);
    // Oldest keeps the height of any change a dirty entry has for the database.
    BOOST_CHECK(NextHeight(Height::Oldest, DIRTY | SPENT, 5, 9) == 5);
    BOOST_CHECK(NextHeight(Height::Oldest, DIRTY, 9, 5) == 5);
    BOOST_CHECK(NextHeight(Height::Oldest, BASE_COPY, 5, 9) == 9);
    // A change a run in flight collected is the run's to write: the new one is filed under its own height.
    BOOST_CHECK(NextHeight(Height::Oldest, DIRTY | COLLECTED, 5, 9) == 9);
}

BOOST_AUTO_TEST_CASE(height_rules_follow_the_table)
{
    for (const Event event : AllEvents()) {
        for (size_t s{0}; s < 256; ++s) {
            const State from{static_cast<State>(s)};
            // No rule, no change of height.
            if (Next(from, event) == REFUSED) BOOST_CHECK(HeightRule(from, event) == Height::Keep);
            if (!IsKnown(from) || Next(from, event) == REFUSED || !(from & OWED)) continue;
            // An entry that owes an earlier change leaves the height of that change on one ground only:
            // a run wrote the change.
            const bool takes{HeightRule(from, event) == Height::Take};
            BOOST_CHECK_EQUAL(takes, event == Event::RunWritesOwed);
        }
    }
    // What is inserted is filed under the event's height.
    for (const Event event : {Event::BaseCopy, Event::ConfirmedNew, Event::ConfirmedWrite, Event::ConfirmedSpend}) {
        BOOST_CHECK(NextHeight(HeightRule(ABSENT, event), ABSENT, 0, 7) == 7);
    }
    // A run that collects or lands moves nothing.
    for (const Event event : {Event::RunCollects, Event::RunLands, Event::KeptWritten}) {
        for (size_t s{0}; s < 256; ++s) BOOST_CHECK(HeightRule(static_cast<State>(s), event) == Height::Keep);
    }
}

BOOST_AUTO_TEST_CASE(owed_erase_keeps_its_height)
{
    // A confirmed spend at 2 of a coin the database holds: the erase is owed since 2.
    const Filed erase{Step({BASE_COPY, 1}, Event::ConfirmedSpend, 2)};
    BOOST_CHECK_EQUAL(erase.state, DIRTY | SPENT);
    BOOST_CHECK_EQUAL(erase.height, 2);

    // A block makes the outpoint again at 4: the erase is owed under the coin.
    const Filed created{Step(erase, Event::CoinOverErase, 4)};
    BOOST_CHECK_EQUAL(created.state, DIRTY | OWED);
    BOOST_CHECK_EQUAL(created.height, 2);
    // Later changes leave it there.
    BOOST_CHECK_EQUAL(Step(created, Event::ConfirmedSpend, 6).height, 2);
    BOOST_CHECK_EQUAL(Step(created, Event::ConfirmedWrite, 6).height, 2);
    BOOST_CHECK_EQUAL(Step(created, Event::BatchOverwrite, 6).height, 2);
    // A spend at 6 and a coin over it at 8: the erase is owed since 2 still.
    const Filed again{Step(Step(created, Event::ConfirmedSpend, 6), Event::CoinOverErase, 8)};
    BOOST_CHECK_EQUAL(again.state, DIRTY | OWED);
    BOOST_CHECK_EQUAL(again.height, 2);
    // A run that writes the erase files the entry under its coin.
    const Filed written{Step(created, Event::RunWritesOwed, 4)};
    BOOST_CHECK_EQUAL(written.state, DIRTY);
    BOOST_CHECK_EQUAL(written.height, 4);
    // A run that collects the entry leaves it where it is; a change made meanwhile is filed under its own
    // height, the run writing what it collected.
    const Filed collected{Step(created, Event::RunCollects, 0)};
    BOOST_CHECK_EQUAL(collected.height, 2);
    BOOST_CHECK_EQUAL(Step(collected, Event::ConfirmedSpend, 6).height, 6);
    BOOST_CHECK_EQUAL(Step(collected, Event::RunLands, 0).state, BASE_COPY);
}

BOOST_AUTO_TEST_CASE(an_erase_owed_since_a_spend_keeps_its_height_through_a_disconnect)
{
    // A coinbase output the database holds, spent at 100 by a block that was confirmed: an erase owed since 100.
    const Filed spent{Step({BASE_COPY, 1}, Event::ConfirmedSpend, 100)};
    BOOST_CHECK_EQUAL(spent.height, 100);
    // A disconnect puts back the coin a later repeat of the coinbase made at 105, and the disconnect of 105
    // spends it again at the fork's height: neither comes from a block a crash's replay would connect again,
    // so the entry stays under the height of the erase the database still lacks.
    const Filed restored{Step(spent, Event::BatchOverwrite, 105)};
    BOOST_CHECK_EQUAL(restored.height, 100);
    BOOST_CHECK_EQUAL(Step(restored, Event::ConfirmedSpend, 104).height, 100);
    BOOST_CHECK_EQUAL(Step(restored, Event::ConfirmedWrite, 106).height, 100);
}

BOOST_AUTO_TEST_SUITE_END()
