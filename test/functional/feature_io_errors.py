#!/usr/bin/env python3
# Copyright (c) The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
"""Test handling of I/O errors."""

from contextlib import closing
import http.client
import urllib.parse

from test_framework.messages import (
    BlockTransactionsRequest,
    msg_getblocktxn,
)
from test_framework.p2p import P2PInterface
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
)
from test_framework.wallet import MiniWallet


class IOErrorTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.uses_wallet = None

    def test_blk_dat_io_error(self):
        node = self.nodes[0]
        self.restart_node(0, ["-txindex", "-rest"])
        wallet = MiniWallet(node)

        tx, tx2 = wallet.send_self_transfer_chain(from_node=node, chain_length=2)
        txid = tx["txid"]
        self.generate(node, 1)
        psbt = node.createpsbt(inputs=[{"txid": txid, "vout": 0}], outputs=[])

        blk_dat = node.blocks_path / "blk00000.dat"
        blk_dat_moved = node.blocks_path / "blk00000.dat.moved"
        blk_dat.rename(blk_dat_moved)

        self.log.info("Check tx RPCs throw on I/O errors")
        msg = "Can't read block from disk"
        assert_raises_rpc_error(-32603, msg, node.gettxoutproof, [tx2["txid"]])
        msg = "txindex: Failed to open block file"
        assert_raises_rpc_error(-1, msg, node.gettxoutproof, [txid])
        assert_raises_rpc_error(-1, msg, node.getrawtransaction, txid)
        assert_raises_rpc_error(-1, msg, node.utxoupdatepsbt, psbt)

        self.log.info("Check REST /tx/ on I/O errors")
        url = urllib.parse.urlparse(node.url)
        with closing(http.client.HTTPConnection(url.hostname, url.port)) as conn:
            conn.request("GET", f"/rest/tx/{txid}.json")
            response = conn.getresponse()
            assert_equal(response.status, 500)
            assert_equal(response.read().decode("utf-8")[:len(msg)], msg)

        blk_dat_moved.rename(blk_dat)

    def test_getblocktxn_io_error(self):
        node = self.nodes[0]
        # Two blocks more, so that the one asked for is near the tip but not the block kept in memory (the last).
        self.generate(node, 2)
        block_hash = node.getblockhash(node.getblockcount() - 1)
        request = msg_getblocktxn()
        request.block_txn_request = BlockTransactionsRequest(int(block_hash, 16), [0])

        blk_dat = node.blocks_path / "blk00000.dat"
        blk_dat_moved = node.blocks_path / "blk00000.dat.moved"
        blk_dat.rename(blk_dat_moved)

        self.log.info("Check a getblocktxn for a block that cannot be read disconnects the requester")
        peer = node.add_p2p_connection(P2PInterface())
        with node.assert_debug_log(["Cannot load block from disk"]):
            peer.send_without_ping(request)
            peer.wait_for_disconnect()
        assert_equal(peer.message_count["blocktxn"], 0)
        assert_equal(node.getbestblockhash(), node.getblockhash(node.getblockcount()))

        blk_dat_moved.rename(blk_dat)
        self.log.info("Check a getblocktxn is answered once the block can be read again")
        peer = node.add_p2p_connection(P2PInterface())
        peer.send_without_ping(request)
        peer.wait_until(lambda: peer.message_count["blocktxn"] == 1)
        node.disconnect_p2ps()

    def run_test(self):
        self.test_blk_dat_io_error()
        self.test_getblocktxn_io_error()


if __name__ == "__main__":
    IOErrorTest(__file__).main()
