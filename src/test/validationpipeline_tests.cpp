// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <chainparams.h>
#include <consensus/validation.h>
#include <node/validationpipeline.h>
#include <node/validationpipeline_impl.h>
#include <pow.h>
#include <test/util/mining.h>
#include <test/util/setup_common.h>
#include <test/util/validation.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(validationpipeline_tests, RegTestingSetup)

namespace {
std::shared_ptr<CBlock> PrepareValidBlock(const node::NodeContext& node)
{
    std::shared_ptr<CBlock> block{PrepareBlock(node, node.mining_args)};
    while (!CheckProofOfWork(block->GetHash(), block->nBits, Params().GetConsensus())) {
        ++block->nNonce;
    }
    return block;
}
} // namespace

BOOST_AUTO_TEST_CASE(backlog_snapshot_tracks_candidate_capacity)
{
    auto pipeline{node::MakeValidationPipeline(*m_node.chainman)};
    std::shared_ptr<CBlock> block{PrepareValidBlock(m_node)};

    const node::ValidationBacklogSnapshot initial_snapshot{pipeline->GetBacklogSnapshot()};
    BOOST_CHECK_EQUAL(initial_snapshot.m_queued_candidates, 0);
    BOOST_CHECK_EQUAL(initial_snapshot.m_pending_candidates, 0);
    BOOST_CHECK_EQUAL(initial_snapshot.m_active_candidates, 0);
    BOOST_CHECK_GT(initial_snapshot.m_max_candidate_slots, 0);
    BOOST_CHECK_EQUAL(initial_snapshot.m_available_candidate_slots, initial_snapshot.m_max_candidate_slots);
    BOOST_CHECK_EQUAL(initial_snapshot.m_initial_block_download, m_node.chainman->IsInitialBlockDownload());

    const NodeId peer{42};
    const node::ValidationBlockResult result{pipeline->SubmitBlockCandidate({
        .m_peer = peer,
        .m_block = block,
        .m_source = node::ValidationBlockSource::FULL_BLOCK,
        .m_force_processing = true,
        .m_min_pow_checked = true,
        .m_punish_peer_on_invalid = true,
    })};
    BOOST_REQUIRE(result.m_accepted_to_storage);
    BOOST_CHECK(!result.m_queued_for_validation);
    BOOST_CHECK_EQUAL(result.m_accepted_to_storage->m_peer, peer);
    BOOST_CHECK(result.m_accepted_to_storage->m_block_hash == block->GetHash());

    const node::ValidationBacklogSnapshot pending_snapshot{pipeline->GetBacklogSnapshot()};
    BOOST_CHECK_EQUAL(pending_snapshot.m_queued_candidates, 0);
    BOOST_CHECK_EQUAL(pending_snapshot.m_pending_candidates, 1);
    BOOST_CHECK_EQUAL(pending_snapshot.m_active_candidates, 0);
    BOOST_CHECK_EQUAL(pending_snapshot.m_available_candidate_slots, pending_snapshot.m_max_candidate_slots - 1);

    node::ValidationPipelineEvents events{pipeline->DrainEvents(/*max_events=*/1)};
    BOOST_REQUIRE_EQUAL(events.m_events.size(), 1);
    const auto* accepted{std::get_if<node::BlockAcceptedToStorage>(&events.m_events.front())};
    BOOST_REQUIRE(accepted);
    BOOST_CHECK_EQUAL(accepted->m_peer, peer);
    BOOST_CHECK(accepted->m_block_hash == block->GetHash());

    BlockValidationState valid_state;
    const node::ValidationBlockChecked checked{pipeline->BlockChecked(block, valid_state)};
    BOOST_REQUIRE(checked.m_valid);
    BOOST_CHECK_EQUAL(checked.m_valid->m_peer, peer);
    BOOST_CHECK(checked.m_valid->m_block_hash == block->GetHash());

    events = pipeline->DrainEvents(/*max_events=*/1);
    BOOST_REQUIRE_EQUAL(events.m_events.size(), 1);
    const auto* valid{std::get_if<node::BlockCheckedByValidation>(&events.m_events.front())};
    BOOST_REQUIRE(valid);
    BOOST_CHECK_EQUAL(valid->m_peer, peer);
    BOOST_CHECK(valid->m_block_hash == block->GetHash());

    const node::ValidationBacklogSnapshot drained_snapshot{pipeline->GetBacklogSnapshot()};
    BOOST_CHECK_EQUAL(drained_snapshot.m_queued_candidates, 0);
    BOOST_CHECK_EQUAL(drained_snapshot.m_pending_candidates, 0);
    BOOST_CHECK_EQUAL(drained_snapshot.m_active_candidates, 0);
    BOOST_CHECK_EQUAL(drained_snapshot.m_available_candidate_slots, drained_snapshot.m_max_candidate_slots);
}

BOOST_AUTO_TEST_CASE(ibd_queue_drains_to_pending_candidate)
{
    static_cast<TestChainstateManager&>(*m_node.chainman).ResetIbd();

    auto pipeline{node::MakeValidationPipeline(*m_node.chainman)};
    std::shared_ptr<CBlock> block{PrepareValidBlock(m_node)};
    const NodeId peer{42};

    const node::ValidationBlockResult queued{pipeline->SubmitBlockCandidate({
        .m_peer = peer,
        .m_block = block,
        .m_source = node::ValidationBlockSource::FULL_BLOCK,
        .m_force_processing = true,
        .m_min_pow_checked = true,
        .m_punish_peer_on_invalid = true,
        .m_queue_if_initial_block_download = true,
    })};
    BOOST_CHECK(queued.m_queued_for_validation);
    BOOST_CHECK(!queued.m_accepted_to_storage);

    const node::ValidationBacklogSnapshot queued_snapshot{pipeline->GetBacklogSnapshot()};
    BOOST_CHECK_EQUAL(queued_snapshot.m_queued_candidates, 1);
    BOOST_CHECK_EQUAL(queued_snapshot.m_pending_candidates, 0);
    BOOST_CHECK_EQUAL(queued_snapshot.m_active_candidates, 0);
    BOOST_CHECK_EQUAL(queued_snapshot.m_available_candidate_slots, queued_snapshot.m_max_candidate_slots - 1);

    std::vector<node::ValidationBlockResult> results{pipeline->DrainQueuedBlockCandidates(/*max_count=*/1)};
    BOOST_REQUIRE_EQUAL(results.size(), 1);
    BOOST_CHECK(!results.front().m_queued_for_validation);
    BOOST_REQUIRE(results.front().m_accepted_to_storage);
    BOOST_CHECK_EQUAL(results.front().m_accepted_to_storage->m_peer, peer);
    BOOST_CHECK(results.front().m_accepted_to_storage->m_block_hash == block->GetHash());

    node::ValidationPipelineEvents events{pipeline->DrainEvents(/*max_events=*/1)};
    BOOST_REQUIRE_EQUAL(events.m_events.size(), 1);
    const auto* accepted{std::get_if<node::BlockAcceptedToStorage>(&events.m_events.front())};
    BOOST_REQUIRE(accepted);
    BOOST_CHECK_EQUAL(accepted->m_peer, peer);
    BOOST_CHECK(accepted->m_block_hash == block->GetHash());

    const node::ValidationBacklogSnapshot pending_snapshot{pipeline->GetBacklogSnapshot()};
    BOOST_CHECK_EQUAL(pending_snapshot.m_queued_candidates, 0);
    BOOST_CHECK_EQUAL(pending_snapshot.m_pending_candidates, 1);
    BOOST_CHECK_EQUAL(pending_snapshot.m_active_candidates, 0);
    BOOST_CHECK_EQUAL(pending_snapshot.m_available_candidate_slots, pending_snapshot.m_max_candidate_slots - 1);

    BlockValidationState valid_state;
    const node::ValidationBlockChecked checked{pipeline->BlockChecked(block, valid_state)};
    BOOST_REQUIRE(checked.m_valid);
    BOOST_CHECK_EQUAL(checked.m_valid->m_peer, peer);

    events = pipeline->DrainEvents(/*max_events=*/1);
    BOOST_REQUIRE_EQUAL(events.m_events.size(), 1);
    const auto* valid{std::get_if<node::BlockCheckedByValidation>(&events.m_events.front())};
    BOOST_REQUIRE(valid);
    BOOST_CHECK_EQUAL(valid->m_peer, peer);
    BOOST_CHECK(valid->m_block_hash == block->GetHash());

    const node::ValidationBacklogSnapshot drained_snapshot{pipeline->GetBacklogSnapshot()};
    BOOST_CHECK_EQUAL(drained_snapshot.m_queued_candidates, 0);
    BOOST_CHECK_EQUAL(drained_snapshot.m_pending_candidates, 0);
    BOOST_CHECK_EQUAL(drained_snapshot.m_active_candidates, 0);
    BOOST_CHECK_EQUAL(drained_snapshot.m_available_candidate_slots, drained_snapshot.m_max_candidate_slots);
}

BOOST_AUTO_TEST_SUITE_END()
