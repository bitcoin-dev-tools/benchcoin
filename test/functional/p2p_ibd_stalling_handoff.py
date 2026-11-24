#!/usr/bin/env python3
# Copyright (c) 2026-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""
Test stalling logic during IBD with received blocks processed on the receive pool.

The functional tests run with one connect pool worker, where a received block is processed by the message
handler thread as it always was. With several workers a block the node asked for leaves that thread, and
the stalling logic is to count the same blocks as received and in flight: the scenario of
p2p_ibd_stalling.py, unchanged, on a node with four workers.
"""

from p2p_ibd_stalling import P2PIBDStallingTest


class P2PIBDStallingHandoffTest(P2PIBDStallingTest):
    def set_test_params(self):
        super().set_test_params()
        self.extra_args = [["-prevoutfetchthreads=4"]]

    def run_test(self):
        super().run_test()
        # What makes this a test of the hand-off: the blocks of the scenario left the message handler thread.
        assert "goes to the receive pool" in self.nodes[0].debug_log_path.read_text(encoding="utf-8")


if __name__ == '__main__':
    P2PIBDStallingHandoffTest(__file__).main()
