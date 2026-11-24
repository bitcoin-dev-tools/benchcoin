// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_NODE_CONNECT_MACHINE_H
#define BITCOIN_NODE_CONNECT_MACHINE_H

#include <util/machine.h>

#include <cstdint>

namespace node {
/**
 * A block of the connect window between its staging and its publication, as the
 * connector thread and the pool's workers see it of each other:
 *
 *   Running --Fail---> Unreadable
 *      |
 *      +----Index----> Indexed --Claim--> Validating --Finish--> Finished
 *
 * Running: the block's worker has it, and nobody else reads what it holds.
 * Unreadable: the block could not be read or parsed; its outcome is complete.
 * Indexed: its transactions are in the window's outputs; nobody has it.
 * Validating: a worker (any block's) validates it. Finished: the outcome is
 * complete.
 *
 * The state is written under the window's mutex. Whoever moves a block out of
 * Running or Validating has written its outcome before, so a thread that reads
 * the state under the mutex may read what the state says exists.
 */
enum class WindowBlockState : uint8_t {
    Running,
    Unreadable,
    Indexed,
    Validating,
    Finished,
};

enum class WindowBlockEvent : uint8_t {
    Fail,
    Index,
    Claim,
    Finish,
};

inline constexpr auto WINDOW_BLOCK_TABLE{util::MakeTransitionTable<WindowBlockState, WindowBlockEvent>({
    {WindowBlockState::Running, WindowBlockEvent::Fail, WindowBlockState::Unreadable},
    {WindowBlockState::Running, WindowBlockEvent::Index, WindowBlockState::Indexed},
    {WindowBlockState::Indexed, WindowBlockEvent::Claim, WindowBlockState::Validating},
    {WindowBlockState::Validating, WindowBlockEvent::Finish, WindowBlockState::Finished},
})};

//! The block's transactions are in the window's outputs, or it could not be read: the blocks above it
//! need not wait for it.
constexpr bool IsIndexed(WindowBlockState state) { return state != WindowBlockState::Running; }
//! The block's outcome is complete: the connector may publish it.
constexpr bool IsComplete(WindowBlockState state)
{
    return state == WindowBlockState::Unreadable || state == WindowBlockState::Finished;
}
//! A validation is running on the block. (Whether its own worker still runs is its task's future.)
constexpr bool IsValidating(WindowBlockState state) { return state == WindowBlockState::Validating; }
} // namespace node

#endif // BITCOIN_NODE_CONNECT_MACHINE_H
