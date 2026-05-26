// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_NODE_BLOCKDOWNLOADPIPELINE_IMPL_H
#define BITCOIN_NODE_BLOCKDOWNLOADPIPELINE_IMPL_H

#include <node/blockdownloadpipeline.h>

#include <memory>

class CTxMemPool;

namespace node {

class BlockDownloadPipelineDelegate
{
public:
    virtual ~BlockDownloadPipelineDelegate() = default;
    virtual BlockDownloadCommands BuildRequestsForPeer(const BlockRequestContext& context,
                                                       const BlockDownloadPeerInfo& peer_info,
                                                       const CBlockIndex* best_known_block,
                                                       const CBlockIndex*& last_common_block) = 0;
    virtual const CBlockIndex* LookupBlockIndex(const uint256& hash) const = 0;
};

std::unique_ptr<BlockDownloadPipeline> MakeBlockDownloadPipeline(BlockDownloadPipelineDelegate& delegate, CTxMemPool& mempool);

} // namespace node

#endif // BITCOIN_NODE_BLOCKDOWNLOADPIPELINE_IMPL_H
