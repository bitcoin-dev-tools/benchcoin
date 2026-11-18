// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_NODE_DURABILITY_MACHINE_H
#define BITCOIN_NODE_DURABILITY_MACHINE_H

#include <util/machine.h>

namespace node {
/**
 * The order of one write-out of the node's state (FlushStateToDisk()). Each
 * step may rely on what the steps before it made durable, and on nothing
 * else:
 *
 *   Start --SyncFiles--> FilesSynced --WriteIndex--> IndexWritten --WriteCoins--> CoinsWritten
 *                                                      |                              ^
 *                                                      +--Unlink--> FilesUnlinked ----+
 *                                                                      WriteCoins
 *
 * - the block index is written only after the files it refers to are synced;
 * - pruned files are unlinked only after the index stopped referring to them;
 * - the coins are written only after the index holds the block they are at.
 *
 * A step that fails ends the write-out where it stands; there is no edge out
 * of a failure, and the next write-out starts again from Start.
 */
enum class FlushPhase {
    Start,
    FilesSynced,
    IndexWritten,
    FilesUnlinked,
    CoinsWritten,
};

enum class FlushStep {
    SyncFiles,
    WriteIndex,
    Unlink,
    WriteCoins,
};

inline constexpr auto FLUSH_TABLE{util::MakeTransitionTable<FlushPhase, FlushStep>({
    {FlushPhase::Start, FlushStep::SyncFiles, FlushPhase::FilesSynced},
    {FlushPhase::FilesSynced, FlushStep::WriteIndex, FlushPhase::IndexWritten},
    {FlushPhase::IndexWritten, FlushStep::Unlink, FlushPhase::FilesUnlinked},
    {FlushPhase::IndexWritten, FlushStep::WriteCoins, FlushPhase::CoinsWritten},
    {FlushPhase::FilesUnlinked, FlushStep::WriteCoins, FlushPhase::CoinsWritten},
})};
} // namespace node

#endif // BITCOIN_NODE_DURABILITY_MACHINE_H
