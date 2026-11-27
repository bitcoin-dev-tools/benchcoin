// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/handoff_machine.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

using node::ConnectorHandOffs;
using node::HandOffTicket;

BOOST_FIXTURE_TEST_SUITE(handoff_machine_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(one_pass)
{
    ConnectorHandOffs handoffs;
    BOOST_CHECK(!handoffs.AnyQueued() && !handoffs.AnyActiveUnattempted());
    const HandOffTicket first{handoffs.Hand(/*for_active=*/true)};
    BOOST_CHECK(handoffs.AnyQueued());
    handoffs.Take();
    BOOST_CHECK(!handoffs.AnyQueued() && handoffs.AnyActiveUnattempted());
    BOOST_CHECK(!handoffs.ActiveChainAttempted(first));
    // A block handed over during the pass waits for the next one.
    const HandOffTicket second{handoffs.Hand(/*for_active=*/true)};
    handoffs.ActiveAttempted();
    BOOST_CHECK(handoffs.ActiveChainAttempted(first));
    BOOST_CHECK(handoffs.AnyQueued());
    BOOST_CHECK(!handoffs.ActiveChainAttempted(second));
}

BOOST_AUTO_TEST_CASE(a_block_handed_over_during_a_pass_is_taken_by_the_next)
{
    ConnectorHandOffs handoffs;
    const HandOffTicket first{handoffs.Hand(/*for_active=*/true)};
    BOOST_CHECK(handoffs.AnyActiveQueued());
    handoffs.Take();
    BOOST_CHECK(!handoffs.AnyActiveQueued());
    handoffs.ActiveAttempted();
    // A block for the historical chainstate is nothing to give way to.
    handoffs.Hand(/*for_active=*/false);
    BOOST_CHECK(handoffs.AnyQueued() && !handoffs.AnyActiveQueued());
    const HandOffTicket second{handoffs.Hand(/*for_active=*/true)};
    BOOST_CHECK(handoffs.AnyActiveQueued());
    handoffs.Take();
    BOOST_CHECK(handoffs.ActiveChainAttempted(first));
    BOOST_CHECK(!handoffs.AnyQueued());
    BOOST_CHECK(!handoffs.ActiveChainAttempted(second));
    handoffs.ActiveAttempted();
    BOOST_CHECK(handoffs.ActiveChainAttempted(first) && handoffs.ActiveChainAttempted(second));
}

BOOST_AUTO_TEST_CASE(a_block_of_the_historical_chain_is_not_waited_for_by_the_active_one)
{
    ConnectorHandOffs handoffs;
    handoffs.Hand(/*for_active=*/true);
    handoffs.Take();
    handoffs.ActiveAttempted();
    handoffs.Hand(/*for_active=*/false);
    BOOST_CHECK(handoffs.AnyQueued() && !handoffs.AnyActiveQueued());
    // A transaction or a ping that follows it waits for nothing.
    BOOST_CHECK(handoffs.ActiveChainAttempted(handoffs.Latest()));
    // Nor does the stalling verdict.
    BOOST_CHECK(!handoffs.AnyActiveUnattempted());
}

BOOST_AUTO_TEST_CASE(stopped_stays_stopped)
{
    ConnectorHandOffs handoffs;
    const HandOffTicket done{handoffs.Hand(/*for_active=*/true)};
    handoffs.Take();
    handoffs.ActiveAttempted();
    const HandOffTicket taken{handoffs.Hand(/*for_active=*/true)};
    handoffs.Take();
    const HandOffTicket queued{handoffs.Hand(/*for_active=*/true)};
    handoffs.Stop();
    // The pass that was running when the connector was stopped ends, and changes nothing any more.
    handoffs.ActiveAttempted();
    handoffs.Take();
    const HandOffTicket late{handoffs.Hand(/*for_active=*/true)};
    BOOST_CHECK(handoffs.Stopped());
    BOOST_CHECK(handoffs.ActiveChainAttempted(done));
    BOOST_CHECK(!handoffs.ActiveChainAttempted(taken));
    BOOST_CHECK(!handoffs.ActiveChainAttempted(queued));
    BOOST_CHECK(!handoffs.ActiveChainAttempted(late));
}

BOOST_AUTO_TEST_SUITE_END()
