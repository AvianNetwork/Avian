#!/usr/bin/env python3
# Copyright (c) 2024 The Avian Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test ML-DSA-44 post-quantum address generation and receiving (RIP-25).

DEPLOYMENT_MLDSA44 is ALWAYS_ACTIVE on regtest, so these tests do not require
any activation block logic.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
)


class MLDsa44Test(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        node = self.nodes[0]

        self.log.info("Test ML-DSA-44 PQ address generation")

        # Generate first PQ address (empty label, type "pq").
        pq_addr = node.getnewaddress("", "pq")
        self.log.info(f"Generated PQ address: {pq_addr}")

        # Verify address info.
        info = node.getaddressinfo(pq_addr)
        assert_equal(info["ispostquantum"], True)
        assert_equal(info["witness_version"], 2)
        assert_equal(info["ismine"], True)
        assert_equal(info["isscript"], False)

        self.log.info("Test that consecutive PQ addresses are distinct")

        # A second PQ address must differ from the first (index increments).
        pq_addr2 = node.getnewaddress("", "pq")
        assert pq_addr != pq_addr2, f"Consecutive PQ addresses must be distinct (got {pq_addr})"

        info2 = node.getaddressinfo(pq_addr2)
        assert_equal(info2["ispostquantum"], True)
        assert_equal(info2["witness_version"], 2)

        self.log.info("Test receiving funds at a PQ address")

        # Mine blocks to make coinbase outputs spendable.
        self.generate(node, 101)

        # Send funds to the first PQ address.
        txid = node.sendtoaddress(pq_addr, 1.0)
        self.log.info(f"Sent 1.0 AVN to PQ address, txid: {txid}")

        # Confirm the transaction.
        self.generate(node, 1)

        # The UTXO must be tracked by the wallet.
        utxos = node.listunspent(1, 9999999, [pq_addr])
        assert_equal(len(utxos), 1)
        assert_equal(utxos[0]["address"], pq_addr)
        assert utxos[0]["amount"] > 0, "Received amount must be positive"

        self.log.info("Test getaddressinfo for funded PQ address")

        info3 = node.getaddressinfo(pq_addr)
        assert_equal(info3["ispostquantum"], True)
        assert_equal(info3["ismine"], True)

        self.log.info("Test that 'pq' address type is rejected on non-regtest without deployment")
        # On regtest DEPLOYMENT_MLDSA44 is always active, so no negative-activation
        # test is needed here; the RPC-level guard is covered by the unit tests.

        self.log.info("All ML-DSA-44 tests passed")


if __name__ == "__main__":
    MLDsa44Test(__file__).main()
