// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chain.h>
#include <consensus/validation.h>
#include <node/block_facts.h>
#include <node/tip_snapshot.h>
#include <script/script.h>
#include <test/util/setup_common.h>
#include <test/util/validation.h>
#include <validation.h>
#include <validationinterface.h>

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <thread>
#include <vector>

BOOST_FIXTURE_TEST_SUITE(tip_snapshot_tests, TestChain100Setup)

//! The published value and the chain agree whenever cs_main is free.
static void CheckMatchesChain(ChainstateManager& chainman)
{
    // Nothing is left unpublished, so the value read is the published one.
    BOOST_CHECK(!chainman.TipUnpublished());
    LOCK(::cs_main);
    const node::TipSnapshot published{chainman.PublishedTip()};
    const CBlockIndex* tip{chainman.ActiveTip()};
    BOOST_REQUIRE(tip);
    BOOST_CHECK(published.HasTip());
    BOOST_CHECK(published.hash == tip->GetBlockHash());
    BOOST_CHECK_EQUAL(published.height, tip->nHeight);
    BOOST_CHECK(published.chain_work == tip->nChainWork);
    BOOST_CHECK_EQUAL(published.time, tip->GetBlockTime());
    BOOST_CHECK_EQUAL(published.bits, tip->nBits);
    BOOST_CHECK_EQUAL(published.initial_block_download, chainman.IsInitialBlockDownload());
}

BOOST_AUTO_TEST_CASE(published_tip_follows_the_chain)
{
    ChainstateManager& chainman{*Assert(m_node.chainman)};
    const CScript script{CScript{} << OP_TRUE};

    CheckMatchesChain(chainman);
    const node::TipSnapshot at_100{chainman.PublishedTip()};
    BOOST_CHECK_EQUAL(at_100.height, 100);
    BOOST_CHECK_GT(at_100.sequence, 0U);

    // Connecting a block moves the tip once and publishes once.
    CreateAndProcessBlock({}, script);
    CheckMatchesChain(chainman);
    const node::TipSnapshot at_101{chainman.PublishedTip()};
    BOOST_CHECK_EQUAL(at_101.height, 101);
    BOOST_CHECK_EQUAL(at_101.sequence, at_100.sequence + 1);

    // A copy taken earlier is unchanged: it is a value, not a view.
    BOOST_CHECK_EQUAL(at_100.height, 100);

    // Disconnecting publishes too, and the sequence number never repeats
    // even though the tip does.
    BlockValidationState state;
    CBlockIndex* tip{WITH_LOCK(::cs_main, return chainman.ActiveTip())};
    BOOST_REQUIRE(chainman.ActiveChainstate().InvalidateBlock(state, tip));
    CheckMatchesChain(chainman);
    const node::TipSnapshot back_at_100{chainman.PublishedTip()};
    BOOST_CHECK(back_at_100.hash == at_100.hash);
    BOOST_CHECK_GT(back_at_100.sequence, at_101.sequence);
}

BOOST_AUTO_TEST_CASE(published_tip_follows_the_ibd_latch)
{
    auto& chainman{static_cast<TestChainstateManager&>(*Assert(m_node.chainman))};
    if (!chainman.IsInitialBlockDownload()) chainman.ResetIbd();
    const node::TipSnapshot during{chainman.PublishedTip()};
    BOOST_CHECK(during.initial_block_download);
    CheckMatchesChain(chainman);

    // The end of initial block download moves no tip and is published
    // under a number of its own.
    chainman.JumpOutOfIbd();
    const node::TipSnapshot after{chainman.PublishedTip()};
    BOOST_CHECK(!after.initial_block_download);
    BOOST_CHECK(after.hash == during.hash);
    BOOST_CHECK_EQUAL(after.sequence, during.sequence + 1);
    CheckMatchesChain(chainman);
}

BOOST_AUTO_TEST_CASE(published_tip_follows_a_block_that_ends_ibd)
{
    auto& chainman{static_cast<TestChainstateManager&>(*Assert(m_node.chainman))};
    if (!chainman.IsInitialBlockDownload()) chainman.ResetIbd();
    const node::TipSnapshot during{chainman.PublishedTip()};
    BOOST_REQUIRE(during.initial_block_download);

    // A recent block ends initial block download in the hold that connects
    // it: the published value has the new tip and the new answer.
    CreateAndProcessBlock({}, CScript{} << OP_TRUE);
    BOOST_REQUIRE(!chainman.IsInitialBlockDownload());
    const node::TipSnapshot after{chainman.PublishedTip()};
    BOOST_CHECK(!after.initial_block_download);
    BOOST_CHECK_EQUAL(after.height, during.height + 1);
    BOOST_CHECK_GT(after.sequence, during.sequence);
    CheckMatchesChain(chainman);
}

//! Reads the tip from inside the cs_main hold that moved it.
struct InsideTheHold final : public CValidationInterface {
    ChainstateManager& m_chainman;
    std::vector<bool> m_unpublished;
    std::vector<int> m_heights;
    explicit InsideTheHold(ChainstateManager& chainman) : m_chainman{chainman} {}
    void ActiveTipChange(const CBlockIndex&, bool) override
    {
        m_unpublished.push_back(m_chainman.TipUnpublished());
        m_heights.push_back(m_chainman.PublishedTip().height);
    }
};

BOOST_AUTO_TEST_CASE(tip_is_published_at_the_end_of_the_hold)
{
    ChainstateManager& chainman{*Assert(m_node.chainman)};
    ValidationSignals& signals{*Assert(m_node.validation_signals)};
    InsideTheHold inside{chainman};
    signals.RegisterValidationInterface(&inside);

    const node::TipSnapshot before{chainman.PublishedTip()};
    CreateAndProcessBlock({}, CScript{} << OP_TRUE);

    // While the hold was open nothing was published, and a reader holding
    // cs_main saw the tip of that moment, as it would reading the chain.
    BOOST_REQUIRE_EQUAL(inside.m_unpublished.size(), 1U);
    BOOST_CHECK(inside.m_unpublished[0]);
    BOOST_CHECK_EQUAL(inside.m_heights[0], before.height + 1);
    // The hold published before it released cs_main.
    CheckMatchesChain(chainman);
    BOOST_CHECK_EQUAL(chainman.PublishedTip().sequence, before.sequence + 1);

    // A tip moved outside the holds that publish leaves readers on
    // cs_main until the next publication.
    {
        LOCK(::cs_main);
        chainman.MarkTipChanged();
        BOOST_CHECK(chainman.TipUnpublished());
        BOOST_CHECK(chainman.PublishedTip().hash == chainman.ActiveTip()->GetBlockHash());
        chainman.PublishTip();
        BOOST_CHECK(!chainman.TipUnpublished());
    }
    BOOST_CHECK_EQUAL(chainman.PublishedTip().sequence, before.sequence + 2);

    signals.UnregisterValidationInterface(&inside);
}

// What the published value is for: a reader on another thread gets the tip while cs_main is held elsewhere and
// the tip is not moving, without waiting for the lock.
BOOST_AUTO_TEST_CASE(published_tip_is_read_without_cs_main)
{
    ChainstateManager& chainman{*Assert(m_node.chainman)};
    BOOST_REQUIRE(!chainman.TipUnpublished());
    const node::TipSnapshot before{chainman.PublishedTip()};
    std::future<node::TipSnapshot> read;
    std::future_status status;
    {
        LOCK(::cs_main);
        read = std::async(std::launch::async, [&chainman] { return chainman.PublishedTip(); });
        // Generous: it can only expire if the read waits for the lock this thread holds, which is released
        // before the future is waited for again, so a failure cannot hang.
        status = read.wait_for(std::chrono::seconds{60});
    }
    BOOST_REQUIRE(status == std::future_status::ready);
    const node::TipSnapshot seen{read.get()};
    BOOST_CHECK(seen.hash == before.hash);
    BOOST_CHECK_EQUAL(seen.sequence, before.sequence);
}

// A reader on another thread during a reorganisation of several blocks: what it reads while the hold is open it
// reads under cs_main, and what it reads otherwise is the published value. Either way it sees the tip before the
// reorganisation or the tip after it, never one of the blocks the reorganisation disconnects or connects on its way.
BOOST_AUTO_TEST_CASE(outside_reader_sees_no_tip_a_reorganisation_passes_through)
{
    ChainstateManager& chainman{*Assert(m_node.chainman)};
    Chainstate& chainstate{chainman.ActiveChainstate()};
    // Branch A of three blocks on the fixture's tip, set aside; branch B of two blocks in its place.
    std::vector<uint256> branch_a;
    for (int i{0}; i < 3; ++i) branch_a.push_back(CreateAndProcessBlock({}, CScript{} << OP_2).GetHash());
    CBlockIndex* const a1{WITH_LOCK(::cs_main, return chainman.m_blockman.LookupBlockIndex(branch_a[0]))};
    BlockValidationState state;
    BOOST_REQUIRE(chainstate.InvalidateBlock(state, a1));
    std::vector<uint256> branch_b;
    for (int i{0}; i < 2; ++i) branch_b.push_back(CreateAndProcessBlock({}, CScript{} << OP_3).GetHash());
    BOOST_REQUIRE(chainman.PublishedTip().hash == branch_b.back());

    // Reconsidered, branch A has more work: one ActivateBestChain() disconnects B's two blocks and connects A's
    // three. Master's step lets go of cs_main only once the chain has more work than the old tip, which here is
    // with A's last block, so no hold leaves a tip between the two.
    std::atomic<bool> stop{false};
    std::vector<uint256> seen;
    std::thread reader{[&] {
        do {
            seen.push_back(chainman.PublishedTip().hash);
        } while (!stop.load());
    }};
    {
        LOCK(::cs_main);
        chainstate.ResetBlockFailureFlags(a1);
        chainman.RecalculateBestHeader();
    }
    BOOST_CHECK(chainstate.ActivateBestChain(state));
    stop = true;
    reader.join();

    BOOST_REQUIRE(chainman.PublishedTip().hash == branch_a.back());
    BOOST_CHECK(!seen.empty());
    for (const uint256& hash : seen) {
        BOOST_CHECK_MESSAGE(hash == branch_b.back() || hash == branch_a.back(),
                            "the reader saw " << hash.ToString() << ", a tip the reorganisation passed through");
    }
    CheckMatchesChain(chainman);
}

BOOST_AUTO_TEST_CASE(block_facts_are_one_copy)
{
    ChainstateManager& chainman{*Assert(m_node.chainman)};
    LOCK(::cs_main);
    const CBlockIndex* tip{chainman.ActiveTip()};
    BOOST_REQUIRE(tip);

    BOOST_CHECK(!chainman.m_blockman.LookupBlockFacts(uint256::ONE));

    const auto facts{chainman.m_blockman.LookupBlockFacts(tip->GetBlockHash())};
    BOOST_REQUIRE(facts);
    BOOST_CHECK_EQUAL(facts->height, tip->nHeight);
    BOOST_CHECK_EQUAL(facts->status, tip->nStatus);
    BOOST_CHECK_EQUAL(facts->tx_count, tip->nTx);
    BOOST_CHECK(facts->HaveData());
    BOOST_CHECK(facts->data_pos == tip->GetBlockPos());
}

BOOST_AUTO_TEST_SUITE_END()
