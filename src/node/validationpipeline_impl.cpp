// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <node/validationpipeline_impl.h>

#include <primitives/block.h>
#include <validation.h>

#include <algorithm>
#include <deque>
#include <map>
#include <set>

namespace node {
namespace {

/** Maximum block candidates allowed in validation backlog during IBD. */
static constexpr size_t MAX_VALIDATION_BLOCK_BACKLOG{1024};

class ValidationPipelineImpl final : public ValidationPipeline
{
    ChainstateManager& m_chainman;

    enum class CandidateState {
        ACTIVE,
        PENDING,
    };

    struct CandidateMetadata {
        NodeId m_peer;
        ValidationBlockSource m_source{ValidationBlockSource::FULL_BLOCK};
        bool m_punish_peer_on_invalid{true};
        CandidateState m_state{CandidateState::ACTIVE};
    };

    std::map<uint256, CandidateMetadata> m_candidates;
    std::deque<ValidationBlockCandidate> m_queued_candidates;
    std::set<uint256> m_queued_candidate_hashes;
    std::deque<ValidationPipelineEvent> m_events;

public:
    explicit ValidationPipelineImpl(ChainstateManager& chainman) : m_chainman{chainman} {}

    ValidationBlockResult SubmitBlockCandidate(const ValidationBlockCandidate& candidate) override
    {
        ValidationBlockResult result;
        result.m_peer = candidate.m_peer;
        result.m_block_hash = candidate.m_block->GetHash();
        result.m_source = candidate.m_source;

        if (ShouldQueueCandidate(candidate, result.m_block_hash)) {
            m_queued_candidates.push_back(candidate);
            m_queued_candidate_hashes.insert(result.m_block_hash);
            result.m_queued_for_validation = true;
            return result;
        }

        return ProcessCandidate(candidate);
    }

    std::vector<ValidationBlockResult> DrainQueuedBlockCandidates(size_t max_count) override
    {
        std::vector<ValidationBlockResult> results;
        results.reserve(std::min(max_count, m_queued_candidates.size()));
        while (max_count > 0 && !m_queued_candidates.empty()) {
            ValidationBlockCandidate candidate{std::move(m_queued_candidates.front())};
            m_queued_candidates.pop_front();
            m_queued_candidate_hashes.erase(candidate.m_block->GetHash());
            results.push_back(ProcessCandidate(candidate));
            --max_count;
        }
        return results;
    }

    ValidationBlockResult ProcessCandidate(const ValidationBlockCandidate& candidate)
    {
        ValidationBlockResult result;
        result.m_peer = candidate.m_peer;
        result.m_block_hash = candidate.m_block->GetHash();
        result.m_source = candidate.m_source;
        const auto [_, inserted]{m_candidates.try_emplace(result.m_block_hash, CandidateMetadata{
            .m_peer = candidate.m_peer,
            .m_source = candidate.m_source,
            .m_punish_peer_on_invalid = candidate.m_punish_peer_on_invalid,
            .m_state = CandidateState::ACTIVE,
        })};

        bool new_block{false};
        m_chainman.ProcessNewBlock(candidate.m_block,
                                   candidate.m_force_processing,
                                   candidate.m_min_pow_checked,
                                   &new_block);
        if (new_block) {
            result.m_accepted_to_storage = BlockAcceptedToStorage{
                .m_peer = candidate.m_peer,
                .m_block_hash = result.m_block_hash,
                .m_source = candidate.m_source,
            };
            m_events.push_back(*result.m_accepted_to_storage);
        }
        if (!inserted) return result;

        if (!result.m_accepted_to_storage) {
            m_candidates.erase(result.m_block_hash);
        } else if (auto it{m_candidates.find(result.m_block_hash)}; it != m_candidates.end()) {
            it->second.m_state = CandidateState::PENDING;
        }
        return result;
    }

    ValidationPipelineEvents DrainEvents(size_t max_events) override
    {
        ValidationPipelineEvents events;
        events.m_events.reserve(std::min(max_events, m_events.size()));
        while (max_events > 0 && !m_events.empty()) {
            events.m_events.push_back(std::move(m_events.front()));
            m_events.pop_front();
            --max_events;
        }
        return events;
    }

    BlockHeadersResult ProcessBlockHeaders(const BlockHeadersCandidate& candidate) override
    {
        BlockHeadersResult result;
        result.m_peer = candidate.m_peer;
        result.m_processed = m_chainman.ProcessNewBlockHeaders(candidate.m_headers,
                                                               candidate.m_min_pow_checked,
                                                               result.m_state,
                                                               &result.m_last_header);
        return result;
    }

    ValidationBlockChecked BlockChecked(const std::shared_ptr<const CBlock>& block,
                                        const BlockValidationState& state) override
    {
        const uint256 block_hash{block->GetHash()};
        const auto candidate{m_candidates.find(block_hash)};
        if (candidate == m_candidates.end()) return {};

        CandidateMetadata metadata{candidate->second};
        m_candidates.erase(candidate);

        if (state.IsValid()) {
            ValidationBlockChecked checked;
            checked.m_valid = BlockCheckedByValidation{
                .m_peer = metadata.m_peer,
                .m_block_hash = block_hash,
                .m_source = metadata.m_source,
            };
            m_events.push_back(*checked.m_valid);
            return checked;
        }

        if (!state.IsInvalid()) return {};

        ValidationBlockChecked checked;
        checked.m_rejected = BlockRejectedByValidation{
            .m_peer = metadata.m_peer,
            .m_block_hash = block_hash,
            .m_source = metadata.m_source,
            .m_state = state,
            .m_punish_peer = metadata.m_punish_peer_on_invalid,
        };
        m_events.push_back(*checked.m_rejected);
        return checked;
    }

    ValidationBacklogSnapshot GetBacklogSnapshot() const override
    {
        size_t active_candidates{0};
        for (const auto& candidate : m_candidates) {
            active_candidates += candidate.second.m_state == CandidateState::ACTIVE;
        }
        const size_t candidate_count{m_queued_candidates.size() + m_candidates.size()};
        const size_t pending_candidates{m_candidates.size() - active_candidates};
        return {
            .m_queued_candidates = m_queued_candidates.size(),
            .m_pending_candidates = pending_candidates,
            .m_active_candidates = active_candidates,
            .m_max_candidate_slots = MAX_VALIDATION_BLOCK_BACKLOG,
            .m_available_candidate_slots = candidate_count < MAX_VALIDATION_BLOCK_BACKLOG ?
                MAX_VALIDATION_BLOCK_BACKLOG - candidate_count : 0,
            .m_initial_block_download = m_chainman.IsInitialBlockDownload(),
        };
    }

private:
    bool ShouldQueueCandidate(const ValidationBlockCandidate& candidate, const uint256& hash) const
    {
        return candidate.m_queue_if_initial_block_download &&
               m_chainman.IsInitialBlockDownload() &&
               candidate.m_source == ValidationBlockSource::FULL_BLOCK &&
               candidate.m_force_processing &&
               m_queued_candidates.size() + m_candidates.size() < MAX_VALIDATION_BLOCK_BACKLOG &&
               !m_queued_candidate_hashes.contains(hash) &&
               !m_candidates.contains(hash);
    }
};

} // namespace

std::unique_ptr<ValidationPipeline> MakeValidationPipeline(ChainstateManager& chainman)
{
    return std::make_unique<ValidationPipelineImpl>(chainman);
}

} // namespace node
