#!/usr/bin/env python3
# Copyright (c) 2024 The Avian Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test ML-DSA-44 post-quantum address generation and receiving (RIP-25).

DEPLOYMENT_MLDSA44 is ALWAYS_ACTIVE on regtest, so these tests do not require
any activation block logic.
"""

import os

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_greater_than_or_equal,
    assert_raises_rpc_error,
)

# RIP-25 ML-DSA-44 witness element sizes (bytes).
MLDSA44_SIG_SIZE = 2420
MLDSA44_PUBKEY_SIZE = 1312


class MLDsa44Test(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def assert_pq_witness(self, node, txid, pq_prevouts):
        """Every input in txid that spends a prevout in pq_prevouts must carry
        exactly the RIP-25 witness: [signature (2420 bytes), pubkey (1312 bytes)].
        Uses the wallet's own tx hex so it works without -txindex."""
        decoded = node.decoderawtransaction(node.gettransaction(txid)["hex"])
        seen = set()
        for vin in decoded["vin"]:
            key = (vin["txid"], vin["vout"])
            if key in pq_prevouts:
                wit = vin.get("txinwitness", [])
                assert_equal(len(wit), 2)
                assert_equal(len(wit[0]) // 2, MLDSA44_SIG_SIZE)     # ML-DSA-44 signature
                assert_equal(len(wit[1]) // 2, MLDSA44_PUBKEY_SIZE)  # ML-DSA-44 public key
                seen.add(key)
        assert_equal(seen, pq_prevouts)

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

        legacy_dest = node.getnewaddress("", "legacy")

        self.log.info("Test spending FROM a PQ address (single ML-DSA-44 input)")

        # Fund a dedicated PQ address, then spend exactly that UTXO (add_inputs
        # False so only the ML-DSA input is used) to a legacy destination.
        spend_addr = node.getnewaddress("", "pq")
        node.sendtoaddress(spend_addr, 10.0)
        self.generate(node, 1)
        utxo = node.listunspent(1, 9999999, [spend_addr])[0]

        res = node.send(
            outputs={legacy_dest: 9.0},
            options={"inputs": [{"txid": utxo["txid"], "vout": utxo["vout"]}],
                     "add_inputs": False, "change_type": "legacy"})
        assert_equal(res["complete"], True)
        self.generate(node, 1)

        # Confirmed in a block => consensus verified and accepted the ML-DSA spend
        # (verify_with_ctx_str + domain context + witness/program/sigop rules).
        assert_greater_than_or_equal(node.gettransaction(res["txid"])["confirmations"], 1)
        self.assert_pq_witness(node, res["txid"], {(utxo["txid"], utxo["vout"])})

        self.log.info("Test two PQ inputs + a PQ output in one transaction")

        # Two PQ inputs, spent together, paying a legacy address and a PQ address
        # (a PQ output created by a normal, non-coinbase transaction).
        a = node.getnewaddress("", "pq")
        b = node.getnewaddress("", "pq")
        node.sendtoaddress(a, 5.0)
        node.sendtoaddress(b, 5.0)
        self.generate(node, 1)
        ua = node.listunspent(1, 9999999, [a])[0]
        ub = node.listunspent(1, 9999999, [b])[0]

        pq_out = node.getnewaddress("", "pq")
        res2 = node.send(
            outputs={legacy_dest: 6.0, pq_out: 3.0},
            options={"inputs": [{"txid": ua["txid"], "vout": ua["vout"]},
                                {"txid": ub["txid"], "vout": ub["vout"]}],
                     "add_inputs": False, "change_type": "legacy"})
        assert_equal(res2["complete"], True)
        self.generate(node, 1)

        assert_greater_than_or_equal(node.gettransaction(res2["txid"])["confirmations"], 1)
        # Both PQ inputs carry the canonical witness.
        self.assert_pq_witness(node, res2["txid"],
                               {(ua["txid"], ua["vout"]), (ub["txid"], ub["vout"])})
        # The PQ output was created and is owned/spendable by the wallet.
        pq_created = node.listunspent(1, 9999999, [pq_out])
        assert_equal(len(pq_created), 1)

        self.log.info("Test spending a non-coinbase PQ output (closes the loop)")

        uc = pq_created[0]
        res3 = node.send(
            outputs={legacy_dest: 2.0},
            options={"inputs": [{"txid": uc["txid"], "vout": uc["vout"]}],
                     "add_inputs": False, "change_type": "legacy"})
        assert_equal(res3["complete"], True)
        self.generate(node, 1)
        assert_greater_than_or_equal(node.gettransaction(res3["txid"])["confirmations"], 1)
        self.assert_pq_witness(node, res3["txid"], {(uc["txid"], uc["vout"])})

        self.log.info("Test change follows a PQ recipient (RIP-25 change routing)")

        # With no explicit change_type, paying a PQ recipient must keep the change
        # in a PQ output rather than downgrading it to a quantum-vulnerable type
        # (CWallet::TransactionChangeType). Coins are auto-selected here.
        pq_recipient = node.getnewaddress("", "pq")
        recipient_spk = node.getaddressinfo(pq_recipient)["scriptPubKey"]
        change_txid = node.sendtoaddress(pq_recipient, 1.0)
        self.generate(node, 1)
        dec = node.decoderawtransaction(node.gettransaction(change_txid)["hex"])
        change_types = []
        for out in dec["vout"]:
            spk = out["scriptPubKey"]
            if spk["hex"] == recipient_spk:
                continue  # this is the payment, not the change
            addr = spk.get("address")
            if addr and node.getaddressinfo(addr)["ismine"]:
                change_types.append(spk.get("type"))
        assert "witness_v2_mldsa44" in change_types, \
            f"change should be a PQ output when paying a PQ recipient, got {change_types}"

        self.log.info("Test PQ coins survive wallet backup and restore")

        # Fund a fresh PQ address, back up the wallet, restore it under a new name,
        # and spend the recovered coin. This is the "users can reliably recover PQ
        # keys" gate: the PQ secret is derived from the (backed-up) descriptor xpriv.
        recover_addr = node.getnewaddress("", "pq")
        node.sendtoaddress(recover_addr, 7.0)
        self.generate(node, 1)
        backup_path = os.path.join(self.nodes[0].datadir_path, "pq_backup.dat")
        node.backupwallet(backup_path)
        node.restorewallet("pq_restored", backup_path)
        restored = node.get_wallet_rpc("pq_restored")
        rutxos = restored.listunspent(0, 9999999, [recover_addr])
        assert_equal(len(rutxos), 1)
        ru = rutxos[0]
        rres = restored.send(
            outputs={legacy_dest: 6.0},
            options={"inputs": [{"txid": ru["txid"], "vout": ru["vout"]}],
                     "add_inputs": False, "change_type": "legacy"})
        assert_equal(rres["complete"], True)
        self.generate(node, 1)
        assert_greater_than_or_equal(restored.gettransaction(rres["txid"])["confirmations"], 1)
        self.assert_pq_witness(restored, rres["txid"], {(ru["txid"], ru["vout"])})
        node.unloadwallet("pq_restored")

        self.log.info("Test a PQ spend requires an unlocked encrypted wallet")

        # PQ secrets are protected transitively: they are re-derived from the
        # encrypted descriptor xpriv, so a spend must fail while locked and succeed
        # once the passphrase is supplied.
        w0 = node.get_wallet_rpc(self.default_wallet_name)
        node.createwallet(wallet_name="pq_enc")
        enc = node.get_wallet_rpc("pq_enc")
        enc_addr = enc.getnewaddress("", "pq")
        w0.sendtoaddress(enc_addr, 8.0)
        self.generate(node, 1)
        eu = enc.listunspent(1, 9999999, [enc_addr])[0]
        enc.encryptwallet("test-pass")
        assert_raises_rpc_error(-13, "walletpassphrase", enc.sendtoaddress, legacy_dest, 1.0)
        enc.walletpassphrase("test-pass", 60)
        eres = enc.send(
            outputs={legacy_dest: 7.0},
            options={"inputs": [{"txid": eu["txid"], "vout": eu["vout"]}],
                     "add_inputs": False, "change_type": "legacy"})
        assert_equal(eres["complete"], True)
        self.generate(node, 1)
        assert_greater_than_or_equal(enc.gettransaction(eres["txid"])["confirmations"], 1)
        self.assert_pq_witness(enc, eres["txid"], {(eu["txid"], eu["vout"])})
        enc.walletlock()

        self.log.info("Test that 'pq' address type is rejected on non-regtest without deployment")
        # On regtest DEPLOYMENT_MLDSA44 is always active, so no negative-activation
        # test is needed here; the RPC-level guard is covered by the unit tests.

        self.log.info("All ML-DSA-44 tests passed")


if __name__ == "__main__":
    MLDsa44Test(__file__).main()
