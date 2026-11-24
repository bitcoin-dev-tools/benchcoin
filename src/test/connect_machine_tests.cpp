// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/connect_machine.h>

#include <boost/test/unit_test.hpp>

using node::WINDOW_BLOCK_TABLE;
using S = node::WindowBlockState;
using E = node::WindowBlockEvent;

BOOST_AUTO_TEST_SUITE(connect_machine_tests)

BOOST_AUTO_TEST_CASE(window_block_readings)
{
    // A block is published from a state nothing leads out of, so its outcome cannot change under the
    // publisher; and a validation is claimed once: no event leads into Validating but Claim.
    for (const S state : {S::Running, S::Unreadable, S::Indexed, S::Validating, S::Finished}) {
        bool leaves{false};
        for (const E event : {E::Fail, E::Index, E::Claim, E::Finish}) {
            const auto next{WINDOW_BLOCK_TABLE.Next(state, event)};
            if (!next) continue;
            leaves = true;
            BOOST_CHECK(node::IsIndexed(*next));
            BOOST_CHECK_EQUAL(node::IsValidating(*next), event == E::Claim);
        }
        BOOST_CHECK_EQUAL(node::IsComplete(state), !leaves);
    }
    BOOST_CHECK(!node::IsIndexed(S::Running));
    // The blocks above wait for an indexed block no longer, the publisher still does.
    BOOST_CHECK(node::IsIndexed(S::Indexed) && !node::IsComplete(S::Indexed));
    BOOST_CHECK(node::IsIndexed(S::Validating) && !node::IsComplete(S::Validating));
}

BOOST_AUTO_TEST_SUITE_END()
