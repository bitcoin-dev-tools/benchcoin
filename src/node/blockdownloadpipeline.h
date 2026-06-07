// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_NODE_BLOCKDOWNLOADPIPELINE_H
#define BITCOIN_NODE_BLOCKDOWNLOADPIPELINE_H

#include <arith_uint256.h>
#include <blockencodings.h>
#include <consensus/validation.h>
#include <net.h>
#include <uint256.h>

#include <chrono>
#include <memory>
#include <optional>
#include <vector>

class CBlock;
class CBlockIndex;

namespace node {

/** Maximum number of outstanding CMPCTBLOCK requests for the same block. */
static constexpr unsigned int MAX_CMPCTBLOCKS_INFLIGHT_PER_BLOCK{3};

enum class BlockFetchType {
    FULL_BLOCK,
    COMPACT_BLOCK,
};

enum class BlockDataSource {
    FULL_BLOCK,
    COMPACT_BLOCK,
};

struct BlockDownloadPeerInfo {
    bool m_preferred_download{false};
    bool m_can_serve_blocks{false};
    bool m_limited{false};
    bool m_inbound{false};
    bool m_manual_connection{false};
    bool m_noban_permission{false};
    bool m_witness_relay{false};
    bool m_provides_cmpctblocks{false};
};

struct BlockDownloadSnapshot {
    const CBlockIndex* m_active_tip{nullptr};
    arith_uint256 m_minimum_chain_work;
    const CBlockIndex* m_snapshot_base{nullptr};
    const CBlockIndex* m_historical_start{nullptr};
    const CBlockIndex* m_historical_target{nullptr};
    bool m_assumeutxo_unvalidated{false};
    bool m_initial_block_download{false};
    bool m_direct_fetch{false};
};

struct BlockRequest {
    NodeId m_peer;
    const CBlockIndex* m_block{nullptr};
    BlockFetchType m_fetch_type{BlockFetchType::FULL_BLOCK};
    bool m_include_witness{false};
};

struct BlockTransactionsRequestCommand {
    NodeId m_peer;
    BlockTransactionsRequest m_request;
};

struct BlockCandidate {
    NodeId m_peer;
    std::shared_ptr<const CBlock> m_block;
    BlockDataSource m_source{BlockDataSource::FULL_BLOCK};
    bool m_force_processing{false};
    bool m_min_pow_checked{false};
};

struct BlockDownloadCommands {
    std::vector<BlockRequest> m_block_requests;
    std::vector<BlockTransactionsRequestCommand> m_block_txn_requests;
    std::vector<BlockCandidate> m_validation_candidates;
    std::vector<NodeId> m_disconnect_peers;
    std::optional<NodeId> m_stalling_peer;
};

struct BlockRequestContext {
    NodeId m_peer;
    std::chrono::microseconds m_current_time{0};
    int m_request_capacity{0};
    BlockDownloadSnapshot m_chain;
};

struct BlockTimeoutContext {
    NodeId m_peer;
    std::chrono::microseconds m_current_time{0};
    std::chrono::seconds m_block_interval{0};
    std::chrono::seconds m_stalling_timeout{0};
    std::chrono::seconds m_max_stalling_timeout{0};
};

enum class BlockTimeoutReason {
    STALLING,
    BLOCK_DOWNLOAD,
};

struct BlockDownloadTimeout {
    NodeId m_peer{0};
    BlockTimeoutReason m_reason{BlockTimeoutReason::STALLING};
    uint256 m_block_hash;
};

struct BlockTimeoutDecision {
    std::optional<BlockDownloadTimeout> m_disconnect;
    std::optional<std::chrono::seconds> m_next_stalling_timeout;
};

struct BlockBodyReceivedEvent {
    NodeId m_peer;
    std::shared_ptr<const CBlock> m_block;
    bool m_force_processing{false};
    bool m_min_pow_checked{false};
};

struct CompactBlockReceivedEvent {
    NodeId m_peer;
    const CBlockHeaderAndShortTxIDs* m_block{nullptr};
};

struct BlockTransactionsReceivedEvent {
    NodeId m_peer;
    const BlockTransactions* m_transactions{nullptr};
};

struct BlockValidationResult {
    NodeId m_peer;
    uint256 m_block_hash;
    BlockDataSource m_source{BlockDataSource::FULL_BLOCK};
    BlockValidationState m_state;
};

struct BlockDownloadPeerStats {
    int m_synced_headers_height{-1};
    int m_common_block_height{-1};
    std::vector<int> m_inflight_heights;
    std::optional<std::chrono::microseconds> m_stalling_since;
};

struct BlockInFlightRegistration {
    bool m_already_in_flight_from_peer{false};
    bool m_had_partial_block{false};
    PartiallyDownloadedBlock* m_partial_block{nullptr};
};

struct CompactBlockTxnRequestState {
    bool m_requested_from_peer{false};
    bool m_first_in_flight{false};
    PartiallyDownloadedBlock* m_partial_block{nullptr};
};

struct CompactBlockRequestState {
    bool m_already_in_flight{false};
    bool m_requested_from_peer{false};
    bool m_first_in_flight{false};
    bool m_can_request_from_peer{false};
    bool m_should_request_block_transactions_from_peer{false};
};

/**
 * Boundary for block download policy and accounting.
 *
 * Callers report peer, block availability, block data, and validation events.
 * The pipeline returns commands for the caller to translate into net messages,
 * validation submissions, or disconnect execution.
 */
class BlockDownloadPipeline
{
public:
    virtual ~BlockDownloadPipeline() = default;

    virtual void ConnectedPeer(NodeId peer, const BlockDownloadPeerInfo& info) = 0;
    virtual void DisconnectedPeer(NodeId peer) = 0;

    virtual void PeerAnnouncedBlock(NodeId peer, const uint256& hash) = 0;
    virtual void PeerHeadersAccepted(NodeId peer, const CBlockIndex& last_header, bool may_have_more_headers) = 0;

    virtual bool IsBlockRequested(const uint256& hash) const = 0;
    virtual bool IsOnlyBlockInFlight(const uint256& hash) const = 0;
    virtual bool HasBlockRequests() const = 0;
    virtual std::optional<NodeId> GetBlockRequestStaller(const uint256& hash) const = 0;
    virtual int GetRequestCapacity(NodeId peer, int max_peer_requests) const = 0;
    virtual bool PeerHasBlockRequests(NodeId peer) const = 0;
    virtual BlockInFlightRegistration BlockRequested(NodeId peer, const CBlockIndex& block, bool use_compact_block, std::chrono::microseconds now) = 0;
    virtual void RemoveBlockRequest(const uint256& hash, std::optional<NodeId> from_peer, std::chrono::microseconds now) = 0;
    virtual void StartStalling(NodeId peer, std::chrono::microseconds now) = 0;
    virtual CompactBlockRequestState GetCompactBlockRequestState(NodeId peer, const uint256& hash, int max_peer_requests) const = 0;
    virtual CompactBlockTxnRequestState GetCompactBlockTxnRequestState(NodeId peer, const uint256& hash) = 0;

    virtual BlockDownloadCommands BuildRequestsForPeer(const BlockRequestContext& context) = 0;
    virtual BlockDownloadCommands BlockBodyReceived(const BlockBodyReceivedEvent& block) = 0;
    virtual BlockDownloadCommands CompactBlockReceived(const CompactBlockReceivedEvent& compact_block) = 0;
    virtual BlockDownloadCommands BlockTransactionsReceived(const BlockTransactionsReceivedEvent& block_transactions) = 0;

    virtual void ValidationAcceptedBlock(const uint256& hash) = 0;
    virtual BlockDownloadCommands ValidationRejectedBlock(const BlockValidationResult& result) = 0;

    virtual BlockTimeoutDecision CheckTimeouts(const BlockTimeoutContext& context) = 0;
    virtual BlockDownloadPeerStats GetPeerStats(NodeId peer) const = 0;
};

} // namespace node

#endif // BITCOIN_NODE_BLOCKDOWNLOADPIPELINE_H
