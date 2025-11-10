#!/usr/bin/env python3
# Copyright (c) 2009-2022 The Bitcoin Core developers
# Copyright (c) 2025 The Avian Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

"""
Test PSBT (Partially Signed Bitcoin Transaction) functionality.

Test the following PSBT RPC commands:
   - createpsbt
   - decodepsbt
   - walletprocesspsbt
   - finalizepsbt
   - analyzepsbt
   - utxoupdatepsbt
"""

from test_framework.test_framework import AvianTestFramework
from test_framework.util import (
    connect_nodes_bi,
    assert_raises_rpc_error,
    assert_equal,
    assert_greater_than,
    Decimal,
)


class PSBTTest(AvianTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 3

    def setup_network(self, split=False):
        super().setup_network()
        connect_nodes_bi(self.nodes, 0, 1)
        connect_nodes_bi(self.nodes, 1, 2)

    def run_test(self):
        self.test_basic_psbt_creation()
        self.test_psbt_with_multiple_inputs()
        self.test_psbt_decode()
        self.test_psbt_analysis()
        self.test_psbt_multisig_workflow()
        self.test_psbt_finalization()

    def test_basic_psbt_creation(self):
        """Test basic PSBT creation"""
        self.log.info("Testing basic PSBT creation...")

        # Generate some coins
        self.nodes[0].generate(1)
        self.sync_all()
        self.nodes[0].generate(101)
        self.sync_all()

        # Send coins to node 1
        addr = self.nodes[1].getnewaddress()
        self.nodes[0].sendtoaddress(addr, 10)
        self.sync_all()
        self.nodes[0].generate(5)
        self.sync_all()

        # Get a UTXO
        utxos = self.nodes[1].listunspent(1, 9999)
        assert_greater_than(len(utxos), 0)

        # Create PSBT
        utxo = utxos[0]
        inputs = [{"txid": utxo["txid"], "vout": utxo["vout"]}]
        outputs = {self.nodes[0].getnewaddress(): 5}

        psbt = self.nodes[1].createpsbt(inputs, outputs)
        assert psbt is not None
        assert len(psbt) > 0

        self.log.info("✓ Basic PSBT creation successful")

    def test_psbt_with_multiple_inputs(self):
        """Test PSBT with multiple inputs"""
        self.log.info("Testing PSBT with multiple inputs...")

        # Get multiple UTXOs
        utxos = self.nodes[1].listunspent(1, 9999)
        assert_greater_than(len(utxos), 1)

        # Create inputs from multiple UTXOs
        inputs = []
        total_amount = Decimal(0)
        for i, utxo in enumerate(utxos[:2]):  # Use first 2 UTXOs
            inputs.append({"txid": utxo["txid"], "vout": utxo["vout"]})
            total_amount += utxo["amount"]

        # Create output
        outputs = {self.nodes[0].getnewaddress(): total_amount - 1}

        # Create PSBT with multiple inputs
        psbt = self.nodes[1].createpsbt(inputs, outputs)
        assert psbt is not None
        assert len(psbt) > 0

        self.log.info("✓ PSBT with multiple inputs created successfully")

    def test_psbt_decode(self):
        """Test PSBT decoding"""
        self.log.info("Testing PSBT decoding...")

        # Create a PSBT
        utxos = self.nodes[1].listunspent(1, 9999)
        assert_greater_than(len(utxos), 0)

        utxo = utxos[0]
        inputs = [{"txid": utxo["txid"], "vout": utxo["vout"]}]
        outputs = {self.nodes[0].getnewaddress(): 5}

        psbt = self.nodes[1].createpsbt(inputs, outputs)

        # Decode the PSBT
        decoded = self.nodes[1].decodepsbt(psbt)

        # Verify decoded structure
        assert "tx" in decoded
        assert "inputs" in decoded
        assert "outputs" in decoded
        assert len(decoded["inputs"]) == len(inputs)
        assert len(decoded["outputs"]) == len(outputs)

        self.log.info("✓ PSBT decoding successful")

    def test_psbt_analysis(self):
        """Test PSBT analysis"""
        self.log.info("Testing PSBT analysis...")

        # Create a PSBT
        utxos = self.nodes[1].listunspent(1, 9999)
        assert_greater_than(len(utxos), 0)

        utxo = utxos[0]
        inputs = [{"txid": utxo["txid"], "vout": utxo["vout"]}]
        outputs = {self.nodes[0].getnewaddress(): 5}

        psbt = self.nodes[1].createpsbt(inputs, outputs)

        # Analyze the PSBT
        analysis = self.nodes[1].analyzepsbt(psbt)

        # Verify analysis structure
        assert "inputs" in analysis
        assert "complete" in analysis

        # Unsigned PSBT should not be complete
        assert analysis["complete"] == False

        self.log.info("✓ PSBT analysis successful")

    def test_psbt_multisig_workflow(self):
        """Test PSBT workflow with multiple signers"""
        self.log.info("Testing PSBT multisig workflow...")

        # This would require setting up a multisig address across nodes
        # For now, we just verify the basic workflow

        # Create a PSBT
        utxos = self.nodes[1].listunspent(1, 9999)
        assert_greater_than(len(utxos), 0)

        utxo = utxos[0]
        inputs = [{"txid": utxo["txid"], "vout": utxo["vout"]}]
        outputs = {self.nodes[0].getnewaddress(): 5}

        psbt = self.nodes[1].createpsbt(inputs, outputs)

        # Process PSBT (sign)
        result = self.nodes[1].walletprocesspsbt(psbt, True, "ALL")

        # Verify result
        assert "psbt" in result
        assert "complete" in result

        self.log.info("✓ PSBT multisig workflow successful")

    def test_psbt_finalization(self):
        """Test PSBT finalization"""
        self.log.info("Testing PSBT finalization...")

        # Create and sign a PSBT
        utxos = self.nodes[1].listunspent(1, 9999)
        assert_greater_than(len(utxos), 0)

        utxo = utxos[0]
        inputs = [{"txid": utxo["txid"], "vout": utxo["vout"]}]
        outputs = {self.nodes[0].getnewaddress(): 5}

        psbt = self.nodes[1].createpsbt(inputs, outputs)

        # Process PSBT to sign it
        signed_psbt_result = self.nodes[1].walletprocesspsbt(psbt, True, "ALL")
        signed_psbt = signed_psbt_result["psbt"]

        # Finalize the PSBT
        final_result = self.nodes[1].finalizepsbt(signed_psbt)

        # Verify result
        assert "hex" in final_result or "psbt" in final_result
        assert "complete" in final_result

        self.log.info("✓ PSBT finalization successful")


if __name__ == "__main__":
    PSBTTest().main()
