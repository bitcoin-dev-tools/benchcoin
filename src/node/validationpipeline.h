// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_NODE_VALIDATIONPIPELINE_H
#define BITCOIN_NODE_VALIDATIONPIPELINE_H

#include <consensus/validation.h>
#include <net.h>
#include <primitives/block.h>
#include <uint256.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

class CBlock;
class CBlockIndex;

namespace node {

enum class ValidationBlockSource {
    FULL_BLOCK,
    COMPACT_BLOCK,
};

struct ValidationBlockCandidate {
    NodeId m_peer;
    std::shared_ptr<const CBlock> m_block;
    ValidationBlockSource m_source{ValidationBlockSource::FULL_BLOCK};
    bool m_force_processing{false};
    bool m_min_pow_checked{false};
    bool m_punish_peer_on_invalid{true};
    bool m_queue_if_initial_block_download{false};
};

struct BlockAcceptedToStorage {
    NodeId m_peer;
    uint256 m_block_hash;
    ValidationBlockSource m_source{ValidationBlockSource::FULL_BLOCK};
};

struct BlockRejectedByValidation {
    NodeId m_peer;
    uint256 m_block_hash;
    ValidationBlockSource m_source{ValidationBlockSource::FULL_BLOCK};
    BlockValidationState m_state;
    bool m_punish_peer{true};
};

struct BlockCheckedByValidation {
    NodeId m_peer;
    uint256 m_block_hash;
    ValidationBlockSource m_source{ValidationBlockSource::FULL_BLOCK};
};

struct ValidationBlockChecked {
    std::optional<BlockCheckedByValidation> m_valid;
    std::optional<BlockRejectedByValidation> m_rejected;
};

using ValidationPipelineEvent = std::variant<BlockAcceptedToStorage, BlockRejectedByValidation, BlockCheckedByValidation>;

struct ValidationPipelineEvents {
    std::vector<ValidationPipelineEvent> m_events;
};

struct ValidationBlockResult {
    NodeId m_peer;
    uint256 m_block_hash;
    ValidationBlockSource m_source{ValidationBlockSource::FULL_BLOCK};
    bool m_queued_for_validation{false};
    std::optional<BlockAcceptedToStorage> m_accepted_to_storage;
};

struct BlockHeadersCandidate {
    NodeId m_peer;
    std::vector<CBlockHeader> m_headers;
    bool m_min_pow_checked{false};
};

struct BlockHeadersResult {
    NodeId m_peer;
    bool m_processed{false};
    BlockValidationState m_state;
    const CBlockIndex* m_last_header{nullptr};
};

struct ValidationBacklogSnapshot {
    size_t m_queued_candidates{0};
    size_t m_pending_candidates{0};
    size_t m_active_candidates{0};
    size_t m_max_candidate_slots{0};
    size_t m_available_candidate_slots{0};
    bool m_initial_block_download{false};
};

/**
 * Boundary for block validation admission and result reporting.
 *
 * Callers submit block candidates and consume typed validation results. The
 * implementation may process candidates synchronously or queue them during IBD,
 * but callers should not depend on ChainstateManager's internal scheduling.
 */
class ValidationPipeline
{
public:
    virtual ~ValidationPipeline() = default;

    virtual ValidationBlockResult SubmitBlockCandidate(const ValidationBlockCandidate& candidate) = 0;
    virtual std::vector<ValidationBlockResult> DrainQueuedBlockCandidates(size_t max_count) = 0;
    virtual ValidationPipelineEvents DrainEvents(size_t max_events) = 0;
    virtual BlockHeadersResult ProcessBlockHeaders(const BlockHeadersCandidate& candidate) = 0;
    virtual ValidationBlockChecked BlockChecked(const std::shared_ptr<const CBlock>& block,
                                                const BlockValidationState& state) = 0;
    virtual ValidationBacklogSnapshot GetBacklogSnapshot() const = 0;
};

} // namespace node

#endif // BITCOIN_NODE_VALIDATIONPIPELINE_H
