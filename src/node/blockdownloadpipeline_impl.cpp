// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <node/blockdownloadpipeline_impl.h>

#include <chain.h>
#include <txmempool.h>
#include <util/time.h>

#include <algorithm>
#include <list>
#include <map>

namespace node {
namespace {

/** Block download timeout base, expressed in multiples of the block interval. */
static constexpr double BLOCK_DOWNLOAD_TIMEOUT_BASE{1};
/** Additional block download timeout per parallel downloading peer. */
static constexpr double BLOCK_DOWNLOAD_TIMEOUT_PER_PEER{0.5};

class BlockDownloadPipelineImpl final : public BlockDownloadPipeline
{
    BlockDownloadPipelineDelegate& m_delegate;
    CTxMemPool& m_mempool;

public:
    explicit BlockDownloadPipelineImpl(BlockDownloadPipelineDelegate& delegate, CTxMemPool& mempool)
        : m_delegate{delegate}, m_mempool{mempool}
    {
    }

    void ConnectedPeer(NodeId peer, const BlockDownloadPeerInfo& info) override
    {
        BlockDownloadPeerState state;
        state.m_info = info;
        m_peer_state.insert_or_assign(peer, std::move(state));
    }
    void DisconnectedPeer(NodeId peer) override
    {
        if (auto peer_state{m_peer_state.find(peer)}; peer_state != m_peer_state.end()) {
            for (const BlockInFlight& in_flight : peer_state->second.m_blocks_in_flight) {
                auto range{m_blocks_in_flight.equal_range(in_flight.m_block->GetBlockHash())};
                while (range.first != range.second) {
                    if (range.first->second.first == peer) {
                        range.first = m_blocks_in_flight.erase(range.first);
                    } else {
                        ++range.first;
                    }
                }
            }
            m_downloading_peer_count -= !peer_state->second.m_blocks_in_flight.empty();
            m_peer_state.erase(peer_state);
        }
    }

    void PeerAnnouncedBlock(NodeId peer, const uint256& hash) override
    {
        if (auto peer_state{m_peer_state.find(peer)}; peer_state != m_peer_state.end()) {
            UpdateBlockAvailability(peer_state->second, hash);
        }
    }

    void PeerHeadersAccepted(NodeId peer, const CBlockIndex& last_header, bool may_have_more_headers) override
    {
        if (auto peer_state{m_peer_state.find(peer)}; peer_state != m_peer_state.end()) {
            UpdateBlockAvailability(peer_state->second, last_header.GetBlockHash());
            peer_state->second.m_last_accepted_header = &last_header;
            peer_state->second.m_may_have_more_headers = may_have_more_headers;
        }
    }

    BlockDownloadCommands BuildRequestsForPeer(const BlockRequestContext& context) override
    {
        const auto peer_state{m_peer_state.find(context.m_peer)};
        if (peer_state == m_peer_state.end()) return {};
        ResolveLastUnknownBlock(peer_state->second);
        return m_delegate.BuildRequestsForPeer(context, peer_state->second.m_info, peer_state->second.m_best_known_block, peer_state->second.m_last_common_block);
    }
    BlockDownloadCommands BlockBodyReceived(const BlockBodyReceivedEvent& block) override { return {}; }
    BlockDownloadCommands CompactBlockReceived(const CompactBlockReceivedEvent& compact_block) override { return {}; }
    BlockDownloadCommands BlockTransactionsReceived(const BlockTransactionsReceivedEvent& block_transactions) override { return {}; }

    void ValidationAcceptedBlock(const uint256& hash) override
    {
        RemoveBlockRequest(hash, std::nullopt, GetTime<std::chrono::microseconds>());
    }
    BlockDownloadCommands ValidationRejectedBlock(const BlockValidationResult& result) override
    {
        RemoveBlockRequest(result.m_block_hash, std::nullopt, GetTime<std::chrono::microseconds>());
        return {};
    }

    BlockTimeoutDecision CheckTimeouts(const BlockTimeoutContext& context) override
    {
        const auto peer_state{m_peer_state.find(context.m_peer)};
        if (peer_state == m_peer_state.end()) return {};

        if (peer_state->second.m_stalling_since.count() &&
            peer_state->second.m_stalling_since < context.m_current_time - context.m_stalling_timeout) {
            return {
                .m_disconnect = BlockDownloadTimeout{
                    .m_peer = context.m_peer,
                    .m_reason = BlockTimeoutReason::STALLING,
                    .m_block_hash = {},
                },
                .m_next_stalling_timeout = std::min(2 * context.m_stalling_timeout, context.m_max_stalling_timeout),
            };
        }

        if (!peer_state->second.m_blocks_in_flight.empty()) {
            const int other_downloading_peers{m_downloading_peer_count - 1};
            const auto timeout{std::chrono::duration_cast<std::chrono::microseconds>(
                context.m_block_interval * (BLOCK_DOWNLOAD_TIMEOUT_BASE + BLOCK_DOWNLOAD_TIMEOUT_PER_PEER * other_downloading_peers))};
            if (context.m_current_time > peer_state->second.m_downloading_since + timeout) {
                return {
                    .m_disconnect = BlockDownloadTimeout{
                        .m_peer = context.m_peer,
                        .m_reason = BlockTimeoutReason::BLOCK_DOWNLOAD,
                        .m_block_hash = peer_state->second.m_blocks_in_flight.front().m_block->GetBlockHash(),
                    },
                    .m_next_stalling_timeout = std::nullopt,
                };
            }
        }

        return {};
    }
    BlockDownloadPeerStats GetPeerStats(NodeId peer) const override
    {
        BlockDownloadPeerStats stats;
        const auto peer_state{m_peer_state.find(peer)};
        if (peer_state == m_peer_state.end()) return stats;

        stats.m_common_block_height = peer_state->second.m_last_common_block ? peer_state->second.m_last_common_block->nHeight : -1;
        stats.m_stalling_since = peer_state->second.m_stalling_since == 0us ? std::nullopt : std::make_optional(peer_state->second.m_stalling_since);
        for (const BlockInFlight& in_flight : peer_state->second.m_blocks_in_flight) {
            stats.m_inflight_heights.push_back(in_flight.m_block->nHeight);
        }
        return stats;
    }

    bool IsBlockRequested(const uint256& hash) const override { return m_blocks_in_flight.contains(hash); }

    bool IsOnlyBlockInFlight(const uint256& hash) const override
    {
        return m_blocks_in_flight.count(hash) == m_blocks_in_flight.size();
    }

    bool HasBlockRequests() const override { return !m_blocks_in_flight.empty(); }
    int GetRequestCapacity(NodeId peer, int max_peer_requests) const override
    {
        return std::max(0, max_peer_requests - static_cast<int>(CountPeerRequests(peer)));
    }
    bool PeerHasBlockRequests(NodeId peer) const override { return CountPeerRequests(peer) != 0; }

    std::optional<NodeId> GetBlockRequestStaller(const uint256& hash) const override
    {
        const auto range{m_blocks_in_flight.equal_range(hash)};
        if (range.first == range.second) return std::nullopt;
        return range.first->second.first;
    }

    BlockInFlightRegistration BlockRequested(NodeId peer, const CBlockIndex& block, bool use_compact_block, std::chrono::microseconds now) override
    {
        const uint256& hash{block.GetBlockHash()};
        Assume(m_blocks_in_flight.count(hash) <= MAX_CMPCTBLOCKS_INFLIGHT_PER_BLOCK);

        auto peer_state{m_peer_state.find(peer)};
        if (peer_state == m_peer_state.end()) return {};

        for (auto range{m_blocks_in_flight.equal_range(hash)}; range.first != range.second; ++range.first) {
            if (range.first->second.first == peer) {
                BlockInFlight& existing{*range.first->second.second};
                const bool had_partial_block{existing.m_partial_block != nullptr};
                if (use_compact_block && !existing.m_partial_block) {
                    existing.m_partial_block = std::make_unique<PartiallyDownloadedBlock>(&m_mempool);
                }
                return {
                    .m_already_in_flight_from_peer = true,
                    .m_had_partial_block = had_partial_block,
                    .m_partial_block = existing.m_partial_block.get(),
                };
            }
        }

        RemoveBlockRequest(hash, peer, now);

        peer_state = m_peer_state.find(peer);
        if (peer_state == m_peer_state.end()) return {};

        auto it{peer_state->second.m_blocks_in_flight.insert(peer_state->second.m_blocks_in_flight.end(), BlockInFlight{
            .m_block = &block,
            .m_partial_block = use_compact_block ? std::make_unique<PartiallyDownloadedBlock>(&m_mempool) : nullptr,
        })};
        if (peer_state->second.m_blocks_in_flight.size() == 1) {
            peer_state->second.m_downloading_since = now;
            ++m_downloading_peer_count;
        }
        auto in_flight{m_blocks_in_flight.insert({hash, {peer, it}})};
        return {
            .m_already_in_flight_from_peer = false,
            .m_had_partial_block = false,
            .m_partial_block = in_flight->second.second->m_partial_block.get(),
        };
    }

    void RemoveBlockRequest(const uint256& hash, std::optional<NodeId> from_peer, std::chrono::microseconds now) override
    {
        auto range{m_blocks_in_flight.equal_range(hash)};
        if (range.first == range.second) return;

        Assume(m_blocks_in_flight.count(hash) <= MAX_CMPCTBLOCKS_INFLIGHT_PER_BLOCK);

        while (range.first != range.second) {
            const auto& [peer, block_it]{range.first->second};
            if (from_peer && *from_peer != peer) {
                ++range.first;
                continue;
            }

            auto peer_state{m_peer_state.find(peer)};
            if (peer_state != m_peer_state.end()) {
                if (peer_state->second.m_blocks_in_flight.begin() == block_it) {
                    peer_state->second.m_downloading_since = std::max(peer_state->second.m_downloading_since, now);
                }
                peer_state->second.m_blocks_in_flight.erase(block_it);
                if (peer_state->second.m_blocks_in_flight.empty()) {
                    --m_downloading_peer_count;
                }
                peer_state->second.m_stalling_since = 0us;
            }

            range.first = m_blocks_in_flight.erase(range.first);
        }
    }

    void StartStalling(NodeId peer, std::chrono::microseconds now) override
    {
        const auto peer_state{m_peer_state.find(peer)};
        if (peer_state != m_peer_state.end() && peer_state->second.m_stalling_since == 0us) {
            peer_state->second.m_stalling_since = now;
        }
    }

    CompactBlockRequestState GetCompactBlockRequestState(NodeId peer, const uint256& hash, int max_peer_requests) const override
    {
        auto range{m_blocks_in_flight.equal_range(hash)};
        const size_t already_in_flight{static_cast<size_t>(std::distance(range.first, range.second))};
        CompactBlockRequestState state{
            .m_already_in_flight = already_in_flight != 0,
            .m_requested_from_peer = false,
            .m_first_in_flight = already_in_flight == 0 || range.first->second.first == peer,
            .m_can_request_from_peer = already_in_flight < MAX_CMPCTBLOCKS_INFLIGHT_PER_BLOCK && GetRequestCapacity(peer, max_peer_requests) > 0,
            .m_should_request_block_transactions_from_peer = false,
        };

        for (; range.first != range.second; ++range.first) {
            const NodeId request_peer{range.first->second.first};
            if (request_peer == peer) {
                state.m_requested_from_peer = true;
            }
            const auto peer_state{m_peer_state.find(request_peer)};
            if (peer_state != m_peer_state.end() && !peer_state->second.m_info.m_inbound) {
                state.m_should_request_block_transactions_from_peer = true;
            }
        }

        const auto peer_state{m_peer_state.find(peer)};
        if (peer_state != m_peer_state.end() && !peer_state->second.m_info.m_inbound) {
            state.m_should_request_block_transactions_from_peer = true;
        }
        state.m_should_request_block_transactions_from_peer |= already_in_flight < MAX_CMPCTBLOCKS_INFLIGHT_PER_BLOCK - 1;
        return state;
    }

    CompactBlockTxnRequestState GetCompactBlockTxnRequestState(NodeId peer, const uint256& hash) override
    {
        auto range{m_blocks_in_flight.equal_range(hash)};
        const size_t already_in_flight{static_cast<size_t>(std::distance(range.first, range.second))};
        CompactBlockTxnRequestState state{
            .m_first_in_flight = already_in_flight == 0 || range.first->second.first == peer,
        };

        while (range.first != range.second) {
            const auto& [node_id, block_it]{range.first->second};
            if (node_id == peer && block_it->m_partial_block) {
                state.m_requested_from_peer = true;
                state.m_partial_block = block_it->m_partial_block.get();
                break;
            }
            ++range.first;
        }
        return state;
    }

private:
    struct BlockInFlight {
        const CBlockIndex* m_block{nullptr};
        std::unique_ptr<PartiallyDownloadedBlock> m_partial_block;
    };

    struct BlockDownloadPeerState {
        BlockDownloadPeerInfo m_info;
        const CBlockIndex* m_best_known_block{nullptr};
        uint256 m_last_unknown_block;
        const CBlockIndex* m_last_common_block{nullptr};
        const CBlockIndex* m_last_accepted_header{nullptr};
        bool m_may_have_more_headers{false};
        std::chrono::microseconds m_stalling_since{0us};
        std::list<BlockInFlight> m_blocks_in_flight;
        std::chrono::microseconds m_downloading_since{0us};
    };

    using BlockDownloadMap = std::multimap<uint256, std::pair<NodeId, std::list<BlockInFlight>::iterator>>;

    std::map<NodeId, BlockDownloadPeerState> m_peer_state;
    BlockDownloadMap m_blocks_in_flight;
    int m_downloading_peer_count{0};

    size_t CountPeerRequests(NodeId peer) const
    {
        const auto peer_state{m_peer_state.find(peer)};
        return peer_state == m_peer_state.end() ? 0 : peer_state->second.m_blocks_in_flight.size();
    }

    void ResolveLastUnknownBlock(BlockDownloadPeerState& peer_state)
    {
        if (peer_state.m_last_unknown_block.IsNull()) return;

        const CBlockIndex* index{m_delegate.LookupBlockIndex(peer_state.m_last_unknown_block)};
        if (index && index->nChainWork > 0) {
            if (peer_state.m_best_known_block == nullptr ||
                index->nChainWork >= peer_state.m_best_known_block->nChainWork) {
                peer_state.m_best_known_block = index;
            }
            peer_state.m_last_unknown_block.SetNull();
        }
    }

    void UpdateBlockAvailability(BlockDownloadPeerState& peer_state, const uint256& hash)
    {
        ResolveLastUnknownBlock(peer_state);

        const CBlockIndex* index{m_delegate.LookupBlockIndex(hash)};
        if (index && index->nChainWork > 0) {
            if (peer_state.m_best_known_block == nullptr ||
                index->nChainWork >= peer_state.m_best_known_block->nChainWork) {
                peer_state.m_best_known_block = index;
            }
        } else {
            peer_state.m_last_unknown_block = hash;
        }
    }
};

} // namespace

std::unique_ptr<BlockDownloadPipeline> MakeBlockDownloadPipeline(BlockDownloadPipelineDelegate& delegate, CTxMemPool& mempool)
{
    return std::make_unique<BlockDownloadPipelineImpl>(delegate, mempool);
}

} // namespace node
