#!/usr/bin/env python3
# Copyright (c) 2026-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A block that spends one outpoint in two of its transactions fails in the connect window.

A window block's inputs are resolved before it is checked, each by its position in the block,
and the block keeps the outpoints it has spent in a set of its own: the second spend of an
outpoint finds it there. The block is run in two forms: two transactions spend an output that
an earlier transaction of the same block makes (such spends never reach the store), and two
transactions spend a coin of the chain. Each fork block below the invalid one stays connected,
the invalid block fails with the reason of a missing input, in the window and again alone, and
nothing of it or of the blocks above it is connected.
"""

import re
import time

from decimal import Decimal

from test_framework.blocktools import (
    COINBASE_MATURITY,
    add_witness_commitment,
    create_block,
    create_coinbase,
)
from test_framework.messages import (
    CBlock,
    CBlockHeader,
    from_hex,
    msg_headers,
)
from test_framework.p2p import P2PDataStore
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal
from test_framework.wallet import MiniWallet

FORK_LENGTH = 6
INVALID_AT = 3  # the invalid block's position in the fork, 1-based


class ConnectWindowDoubleSpendTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 3
        # The framework gives every node one connect thread; the window needs several to overlap.
        self.extra_args = [
            [],
            ["-connectwindow=4", "-prevoutfetchthreads=4"],
            ["-connectwindow=4", "-prevoutfetchthreads=4"],
        ]

    def setup_network(self):
        # The syncing nodes are served the blocks by a test peer, in initial block download.
        self.setup_nodes()

    def build_source_chain(self):
        node = self.nodes[0]
        # Blocks dated two days back keep the syncing nodes in initial block download for the whole chain.
        node.setmocktime(int(time.time()) - 2 * 24 * 3600)
        self.wallet = MiniWallet(node)
        self.generate(self.wallet, 101 + 2 * FORK_LENGTH, sync_fun=self.no_op)

    def retried_alone(self, i):
        """Stop the node and return the number of blocks its shutdown summary reports as retried alone."""
        with self.nodes[i].assert_debug_log(expected_msgs=["Connect window:"]):
            self.stop_node(i)
        with open(self.nodes[i].debug_log_path, encoding="utf-8") as log:
            summary = [line for line in log if "Connect window:" in line][-1]
        return int(re.search(r"(\d+) blocks retried alone", summary).group(1))

    def test_double_spend_in_block(self, i, *, own):
        node0, node = self.nodes[0], self.nodes[i]
        height = node0.getblockcount()
        self.log.info(f"A block spending {'an output an earlier transaction of it makes' if own else 'a coin of the chain'} twice, inside the window")
        main_blocks = [from_hex(CBlock(), node0.getblock(node0.getblockhash(h), 0)) for h in range(1, height + 1)]
        fork_base_height = height - FORK_LENGTH // 2
        fork_base = main_blocks[fork_base_height - 1]
        utxo = self.wallet.get_utxo()
        assert utxo["height"] + COINBASE_MATURITY <= fork_base_height + 1
        if own:
            maker = self.wallet.create_self_transfer(utxo_to_spend=utxo)
            spends = [maker["tx"],
                      self.wallet.create_self_transfer(utxo_to_spend=maker["new_utxo"])["tx"],
                      self.wallet.create_self_transfer(utxo_to_spend=maker["new_utxo"], fee_rate=Decimal("0.005"))["tx"]]
        else:
            spends = [self.wallet.create_self_transfer(utxo_to_spend=utxo)["tx"],
                      self.wallet.create_self_transfer(utxo_to_spend=utxo, fee_rate=Decimal("0.005"))["tx"]]
        assert spends[-1].txid_hex != spends[-2].txid_hex
        fork_blocks = []
        prev_hash, prev_time = fork_base.hash_int, fork_base.nTime
        for n in range(1, FORK_LENGTH + 1):
            block = create_block(prev_hash, create_coinbase(fork_base_height + n), ntime=prev_time + 1, txlist=spends if n == INVALID_AT else [])
            if n == INVALID_AT:
                add_witness_commitment(block)
            block.solve()
            fork_blocks.append(block)
            prev_hash, prev_time = block.hash_int, block.nTime

        peer = node.add_p2p_connection(P2PDataStore())
        for block in main_blocks + fork_blocks:
            peer.block_store[block.hash_int] = block
        peer.last_block_hash = fork_blocks[-1].hash_int
        # Both branches' headers: the fork has more work and is validated first, up to its invalid block.
        with node.assert_debug_log(["bad-txns-inputs-missingorspent"]):
            peer.send_without_ping(msg_headers(headers=[CBlockHeader(b) for b in main_blocks]))
            peer.send_without_ping(msg_headers(headers=[CBlockHeader(b) for b in fork_blocks]))
            self.wait_until(lambda: any(t["status"] == "invalid" for t in node.getchaintips()), timeout=120)
        tips = {t["hash"]: t["status"] for t in node.getchaintips()}
        assert_equal(tips[fork_blocks[-1].hash_hex], "invalid")
        assert_equal(node.getbestblockhash(), fork_blocks[INVALID_AT - 2].hash_hex)
        node.disconnect_p2ps()
        # The block failed in the window, which decides nothing, and was connected again alone.
        assert self.retried_alone(i) >= 1

    def run_test(self):
        self.build_source_chain()
        self.test_double_spend_in_block(1, own=True)
        self.test_double_spend_in_block(2, own=False)


if __name__ == '__main__':
    ConnectWindowDoubleSpendTest(__file__).main()
