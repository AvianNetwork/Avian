#!/usr/bin/env python3
# Copyright (c) 2014-2016 The Bitcoin Core developers
# Copyright (c) 2017-2018 The Raven Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test mempool limiting together/eviction with the wallet."""

from test_framework.test_framework import RavenTestFramework
from test_framework.util import *

class MempoolLimitTest(RavenTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.extra_args = [["-maxmempool=9", "-spendzeroconfchange=0"]]
        self.thirtyTransactions = 9  #tx are created in groups of 30.  Value here will be multiplied by thirty for the number of tx.

    def run_test(self):
        txouts = gen_return_txouts()
        relayfee = self.nodes[0].getnetworkinfo()['relayfee']

        txids = []
        utxos = create_confirmed_utxos(relayfee, self.nodes[0], self.thirtyTransactions*30)

        #create a mempool tx that will be evicted
        us0 = utxos.pop()
        inputs = [{ "txid" : us0["txid"], "vout" : us0["vout"]}]
        outputs = {self.nodes[0].getnewaddress() : 0.0001}
        tx = self.nodes[0].createrawtransaction(inputs, outputs)

        # Any fee calc method should work as longs as base_fee is set proportionally...

        #1
        txF = self.nodes[0].fundrawtransaction(tx)
        base_fee = satoshi_round(0.00075*100)  # DEFAULT_FALLBACK_FEE (settxfee(0) is default and falls through to this)

        #2
        # self.nodes[0].settxfee(relayfee)  # specifically fund this tx with low fee (this is too low and will be bumped to MINFEE)
        # txF = self.nodes[0].fundrawtransaction(tx)
        # base_fee = satoshi_round(0.0005*100)  # DEFAULT_TRANSACTION_MINFEE
        # self.nodes[0].settxfee(0) # return to automatic fee selection

        #3
        # txF = self.nodes[0].fundrawtransaction(tx, {"feeRate": relayfee})
        # relayfee = self.nodes[0].getnetworkinfo()['relayfee']
        # base_fee = relayfee*100

        txFS = self.nodes[0].signrawtransaction(txF['hex'])
        txid = self.nodes[0].sendrawtransaction(txFS['hex'])

        for i in range (self.thirtyTransactions):
            txids.append([])
            txids[i] = create_lots_of_big_transactions(self.nodes[0], txouts, utxos[30*i:30*i+30], 30, (i+1)*base_fee)

        # by now, the tx should be evicted, check confirmation state
        assert(txid not in self.nodes[0].getrawmempool())
        txdata = self.nodes[0].gettransaction(txid)
        assert(txdata['confirmations'] ==  0) #confirmation should still be 0

if __name__ == '__main__':
    MempoolLimitTest().main()
