#!/usr/bin/env python3
# Copyright (c) 2026 The Avian Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the Stratum v2 Template Provider.

The unit tests in src/test/sv2_*_tests.cpp drive the same code paths directly.
What this adds is an independent reader of the wire format and the end to end
claim: a block mined over Stratum v2, by each of Avian's two algorithms, that
the node accepts and reports under the algorithm that was negotiated.
"""

import time

from test_framework.key import compute_xonly_pubkey
from test_framework.messages import hash256
from test_framework.sv2 import (
    AVIAN_EXTENSION_ID,
    POW_TYPE_MINOTAURX,
    POW_TYPE_X16RT,
    PROTOCOL_MINING,
    PROTOCOL_VERSION,
    TIME_MASK,
    Sv2Client,
    build_coinbase,
    merkle_root,
    template_outputs,
)
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_greater_than, sv2_port
from test_framework.wallet import MiniWallet

# Avian's proof of work has no cheap pre-filter, so the only way to search for a
# nonce is to submit and watch the tip. On regtest the target is the algorithm's
# own floor, so roughly half of all nonces meet it and this lands at once; the
# bound only exists so a broken build fails instead of looping.
MAX_NONCE_ATTEMPTS = 40

# The node allows a few submissions per ten seconds. Stay inside that.
SUBMIT_INTERVAL = 4

ALGO_NAMES = {POW_TYPE_X16RT: 'x16rt', POW_TYPE_MINOTAURX: 'minotaurx'}

# MAX_SOLUTIONS in src/sv2/template_provider.h. Kept here so the test that
# exceeds it says why that number.
sv2_MAX_SOLUTIONS = 4


class Sv2TemplateProviderTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [[
            '-sv2',
            '-sv2bind=127.0.0.1:{}'.format(sv2_port(0)),
            '-sv2powalgo=x16rt',
            '-sv2powalgo=minotaurx',
            # Every failed proof of work counts as a refusal, and the nonce
            # search below expects to make several.
            '-sv2maxrejects=1000',
            '-debug=sv2',
        ]]

    def connect(self, verify_authority=True):
        """A client that has completed the Noise handshake.

        The node writes its authority private key into the datadir on first
        run, so the test can derive the public key and check the certificate
        rather than trusting the server on first use.
        """
        authority = None
        if verify_authority:
            with open(self.nodes[0].chain_path / 'sv2_authority_key', 'rb') as f:
                privkey = f.read()
            assert_equal(len(privkey), 32)
            authority, _negated = compute_xonly_pubkey(privkey)
        return Sv2Client('127.0.0.1', sv2_port(0), authority)

    def run_test(self):
        self.wallet = MiniWallet(self.nodes[0])
        # Past coinbase maturity, so the wallet can build a mempool
        # transaction, and with a recent tip so the node is not in initial
        # block download and will build templates.
        self.generate(self.wallet, 101, sync_fun=self.no_op)

        self.test_handshake_and_certificate()
        self.test_setup_connection()
        self.test_extension_is_required()
        self.test_template_fields()
        self.test_time_constraints()
        self.test_transaction_data()
        self.test_tip_change_reissues_work()
        self.test_refusals()
        self.test_mines_a_block(POW_TYPE_X16RT)
        self.test_mines_a_block(POW_TYPE_MINOTAURX)
        self.test_mines_a_block_on_superseded_work()
        self.test_finding_blocks_quickly_is_not_rate_limited()
        self.test_ntime_window_opens_without_min_difficulty()

    def test_handshake_and_certificate(self):
        self.log.info("Noise handshake, with the certificate checked against the authority key")
        with self.connect() as client:
            # read_msg2 raised nothing, so the transcript matched and the
            # certificate verified. Confirm the validity window is a real one
            # rather than two zeroes that happen to bracket now.
            cert = client.transport.cert
            assert_equal(cert['version'], 0)
            assert_greater_than(cert['not_valid_after'], cert['valid_from'])
            assert_equal(len(client.transport.remote_static), 32)

        self.log.info("A certificate signed by some other key is refused")
        wrong, _negated = compute_xonly_pubkey(bytes([1] + [0] * 31))
        try:
            Sv2Client('127.0.0.1', sv2_port(0), wrong)
            raise AssertionError("a certificate signed by another key was accepted")
        except AssertionError:
            raise
        except Exception as e:
            assert 'certificate is not signed by the authority key' in str(e), str(e)

    def test_setup_connection(self):
        self.log.info("SetupConnection")
        with self.connect() as client:
            assert_equal(client.setup_connection()['used_version'], PROTOCOL_VERSION)

        self.log.info("A protocol this node does not serve is refused with an explanation")
        with self.connect() as client:
            assert_equal(client.setup_connection(protocol=PROTOCOL_MINING)['error_code'],
                         'unsupported-protocol')
            # The explanation has to reach the client before the socket shuts,
            # which is why the node drains its send buffer before closing.
            assert client.disconnected()

        self.log.info("A version range that does not overlap ours is refused")
        with self.connect() as client:
            assert_equal(client.setup_connection(min_version=99, max_version=99)['error_code'],
                         'protocol-version-mismatch')

    def test_extension_is_required(self):
        self.log.info("The Avian extension is required by default")
        with self.connect() as client:
            client.setup_connection()
            # Ask only for something the node does not have, so nothing is
            # negotiated.
            reply = client.request_extensions(wanted=[0x7fff])
            assert not reply['accepted']
            assert_equal(reply['unsupported'], [0x7fff])
            # The refusal names what this node needs, so a client that asked
            # for the wrong thing can correct itself.
            assert_equal(reply['required'], [AVIAN_EXTENSION_ID])

            # Without the extension the node cannot tell this client its ntime
            # bounds, so it must not serve templates to it.
            client.coinbase_output_constraints()
            assert client.disconnected()

        self.log.info("Negotiating it succeeds and reports it back")
        with self.connect() as client:
            client.setup_connection()
            reply = client.request_extensions()
            assert reply['accepted']
            assert_equal(reply['extensions'], [AVIAN_EXTENSION_ID])

        self.log.info("Version rolling is not offered")
        with self.connect() as client:
            client.setup_connection()
            client.request_extensions()
            selected = client.select_algorithm(POW_TYPE_X16RT)
            assert_equal(selected['pow_type'], POW_TYPE_X16RT)
            # A non-zero mask would let a miner change the algorithm bits, and
            # with them the proof-of-work limit the share is judged against.
            assert_equal(selected['version_rolling_mask'], 0)

    def test_template_fields(self):
        self.log.info("NewTemplate describes the algorithm that was selected")
        for pow_type in (POW_TYPE_X16RT, POW_TYPE_MINOTAURX):
            with self.connect() as client:
                work = client.get_work(pow_type)
                template = work['template']
                assert_equal((template['version'] >> 16) & 0xff, pow_type)
                assert work['prev_hash'] is not None
                assert_equal(work['prev_hash']['template_id'], template['template_id'])
                assert_equal(work['prev_hash']['prev_hash'][::-1].hex(),
                             self.nodes[0].getbestblockhash())

        self.log.info("The required outputs end with the witness commitment")
        with self.connect() as client:
            work = client.get_work()
            template = work['template']
            outputs = template_outputs(template)
            assert_greater_than(len(outputs), 0)
            # The commitment has to be the last required output, because
            # consensus reads the highest-indexed one.
            assert_equal(outputs[-1].scriptPubKey[:6].hex(), '6a24aa21a9ed')

            # An empty block holds one transaction, so the path is empty and
            # the root is the coinbase txid itself.
            assert_equal(template['merkle_path'], [])
            coinbase = build_coinbase(template)
            assert_equal(merkle_root(coinbase, template['merkle_path'])[::-1].hex(),
                         coinbase.txid_hex)

    def test_time_constraints(self):
        self.log.info("AvianTimeConstraints pins ntime shut on a min-difficulty chain")
        for pow_type in (POW_TYPE_X16RT, POW_TYPE_MINOTAURX):
            with self.connect() as client:
                work = client.get_work(pow_type)
                bounds = work['time_constraints']
                assert_equal(bounds['template_id'], work['template']['template_id'])
                assert_equal(bounds['ntime_min'], work['prev_hash']['ntime_start'])

                # Regtest and testnet allow minimum-difficulty blocks, which
                # makes nBits a function of nTime, so rolling ntime at all can
                # produce bad-diff even though the template's nBits was right
                # when it was issued. The window is therefore pinned, not just
                # narrowed. What the node does once it opens is
                # test_ntime_window_opens_without_min_difficulty below.
                assert_equal(bounds['ntime_min'], bounds['ntime_max'])
                assert_equal(bounds['ntime_min'] & TIME_MASK, bounds['ntime_max'] & TIME_MASK)

    def test_transaction_data(self):
        self.log.info("RequestTransactionData returns the block's transactions")
        txid = self.wallet.send_self_transfer(from_node=self.nodes[0])['txid']

        with self.connect() as client:
            work = client.get_work()
            template = work['template']
            data = client.request_transaction_data(template['template_id'])
            assert_equal(data['template_id'], template['template_id'])
            assert_equal(len(data['transaction_list']), 1)
            assert_equal(self.nodes[0].decoderawtransaction(
                data['transaction_list'][0].hex())['txid'], txid)

            # With one other transaction the path is that transaction's txid,
            # which pins both the ordering and the orientation: the coinbase is
            # leftmost, so the sibling is the right-hand one.
            assert_equal(len(template['merkle_path']), 1)
            assert_equal(template['merkle_path'][0][::-1].hex(), txid)
            coinbase = build_coinbase(template)
            assert_equal(merkle_root(coinbase, template['merkle_path']),
                         hash256(bytes.fromhex(coinbase.txid_hex)[::-1]
                                 + template['merkle_path'][0]))

        self.log.info("An unknown template id is an error, not a disconnect")
        with self.connect() as client:
            work = client.get_work()
            reply = client.request_transaction_data(work['template']['template_id'] + 1000)
            assert_equal(reply['error_code'], 'template-id-not-found')

    def test_tip_change_reissues_work(self):
        self.log.info("A new tip pushes fresh work")
        with self.connect() as client:
            first = client.get_work()
            self.generate(self.wallet, 1, sync_fun=self.no_op)

            second = client.read_work()
            assert second['prev_hash'] is not None
            assert_equal(second['prev_hash']['prev_hash'][::-1].hex(),
                         self.nodes[0].getbestblockhash())
            assert second['template']['template_id'] != first['template']['template_id']

    def test_refusals(self):
        self.log.info("Submissions that break the rules are refused without moving the tip")
        before = self.nodes[0].getblockcount()
        with self.connect() as client:
            work = client.get_work()
            template = work['template']
            ntime = work['time_constraints']['ntime_min']

            # A rolled version bit, which the zero mask forbids.
            client.submit_solution(template['template_id'], template['version'] ^ 0x1,
                                   ntime, 0, build_coinbase(template))

            # The founder payment and the commitment dropped. Consensus only
            # notices this in ConnectBlock, which is why the node checks it
            # here instead of relaying a block that cannot connect.
            stripped = build_coinbase(template)
            del stripped.vout[1:]
            client.submit_solution(template['template_id'], template['version'],
                                   ntime, 0, stripped)

            # An ntime past the advertised window.
            client.submit_solution(template['template_id'], template['version'],
                                   work['time_constraints']['ntime_max'] + 1, 0,
                                   build_coinbase(template))

            # Still answered, so none of those closed the connection.
            reply = client.request_transaction_data(template['template_id'])
            assert_equal(reply['template_id'], template['template_id'])

        assert_equal(self.nodes[0].getblockcount(), before)

    def test_mines_a_block(self, pow_type):
        self.log.info("Mining a {} block over Stratum v2".format(ALGO_NAMES[pow_type]))
        before = self.nodes[0].getblockcount()

        with self.connect() as client:
            work = client.get_work(pow_type)
            template = work['template']
            coinbase = build_coinbase(template)
            ntime = work['time_constraints']['ntime_min']

            for nonce in range(MAX_NONCE_ATTEMPTS):
                client.submit_solution(template['template_id'], template['version'],
                                       ntime, nonce, coinbase)
                if self.nodes[0].getblockcount() > before:
                    break
                time.sleep(SUBMIT_INTERVAL)
            else:
                raise AssertionError("no nonce in {} attempts met the {} target".format(
                    MAX_NONCE_ATTEMPTS, ALGO_NAMES[pow_type]))

        assert_equal(self.nodes[0].getblockcount(), before + 1)
        tip = self.nodes[0].getblock(self.nodes[0].getbestblockhash())
        assert_equal(tip['powtype'], ALGO_NAMES[pow_type])
        # The block carries the coinbase this test built, not one the node made.
        assert_equal(tip['tx'][0], coinbase.txid_hex)


    def test_mines_a_block_on_superseded_work(self):
        """A block found on work the node has already replaced is still taken.

        The node refreshes templates on the same cadence as the chain produces
        blocks, so a pool always has jobs in flight when a refresh lands. If
        the node dropped the superseded template, every one of those jobs would
        be worthless the instant it was replaced, and a block found on one would
        be thrown away. This is the test that distinguishes retaining the
        template from merely still having its id.
        """
        self.log.info("A block found on superseded work is still accepted")
        # Two settings, for two different reasons. A short refresh interval is
        # how the test gets a template superseded promptly. The raised
        # retention compensates for that same short interval: the nonce search
        # below is metered and takes tens of seconds, during which a one or two
        # second refresh cadence would evict the template long before
        # production's thirty second cadence would. The default of three
        # retains ninety seconds of work, which is well past any pool's
        # job to share round trip.
        self.restart_node(0, extra_args=self.extra_args[0]
                          + ['-sv2interval=2', '-sv2maxtemplates=60'])
        before = self.nodes[0].getblockcount()

        with self.connect() as client:
            work = client.get_work()
            template = work['template']
            coinbase = build_coinbase(template)
            ntime = work['time_constraints']['ntime_min']

            # Wait for the refresh that supersedes it.
            for _ in range(20):
                nxt = client.read_work()
                if nxt['template']['template_id'] != template['template_id']:
                    break
            else:
                raise AssertionError("no refreshed template arrived")

            for nonce in range(MAX_NONCE_ATTEMPTS):
                client.submit_solution(template['template_id'], template['version'],
                                       ntime, nonce, coinbase)
                if self.nodes[0].getblockcount() > before:
                    break
                time.sleep(SUBMIT_INTERVAL)
            else:
                raise AssertionError("a block on superseded work was never accepted")

        assert_equal(self.nodes[0].getblockcount(), before + 1)
        assert_equal(self.nodes[0].getblock(self.nodes[0].getbestblockhash())['tx'][0],
                     coinbase.txid_hex)
        self.restart_node(0, extra_args=self.extra_args[0])


    def test_ntime_window_opens_without_min_difficulty(self):
        """What the node advertises once nBits stops following the candidate's nTime.

        Every chain this suite can run on allows minimum-difficulty blocks,
        which makes nBits a function of the candidate's own timestamp. The
        node therefore pins ntime shut, and the entire window computation in
        BuildTemplate never executes: not the -sv2ntimeforward ceiling, not
        the consensus future-time ceiling, and not the X16RT epoch ceiling
        that is the whole reason the window exists. Mainnet would be the
        first place that code ran, which is the wrong place to find out.

        -test=no_min_difficulty is regtest-only and turns that rule off.
        fPowNoRetargeting still holds nBits constant, so the chain stays
        trivially mineable while timestamps stop steering difficulty.
        """
        self.log.info("Without min-difficulty blocks the ntime window opens, bounded by X16RT's epoch")
        before = self.nodes[0].getblockcount()
        self.restart_node(0, extra_args=self.extra_args[0] + ['-test=no_min_difficulty'])

        with self.connect() as client:
            bounds = client.get_work(POW_TYPE_MINOTAURX)['time_constraints']
            # MinotaurX has no epoch, so -sv2ntimeforward is the binding
            # ceiling: a fresh chain is nowhere near the consensus future-time
            # limit, which sits 1290 seconds out.
            assert_equal(bounds['ntime_max'], bounds['ntime_min'] + 600)

        with self.connect() as client:
            bounds = client.get_work(POW_TYPE_X16RT)['time_constraints']
            # X16RT rederives its hash chain from nTime & TIME_MASK, so the
            # window stops at that boundary however much room the other two
            # ceilings leave. Since the boundary is at most 127 seconds out
            # and -sv2ntimeforward is 600, the epoch always wins and the value
            # is exact rather than merely bounded.
            assert_equal(bounds['ntime_max'], (bounds['ntime_min'] & TIME_MASK) + 127)
            assert_equal(bounds['ntime_min'] & TIME_MASK, bounds['ntime_max'] & TIME_MASK)
            # Deliberately not asserting the window is non-empty: a template
            # built in the last second of an epoch legitimately has
            # ntime_max == ntime_min, which is a 1-in-128 flake rather than a
            # fault. The MinotaurX case above carries that claim.

        # And the window is usable. Pinning has meant this suite could never
        # show a block accepted at a timestamp other than the template's own.
        with self.connect() as client:
            work = client.get_work(POW_TYPE_MINOTAURX)
            template = work['template']
            bounds = work['time_constraints']
            coinbase = build_coinbase(template)
            ntime = bounds['ntime_min'] + 30
            assert_greater_than(bounds['ntime_max'], ntime)

            for nonce in range(MAX_NONCE_ATTEMPTS):
                client.submit_solution(template['template_id'], template['version'],
                                       ntime, nonce, coinbase)
                if self.nodes[0].getblockcount() > before:
                    break
                time.sleep(SUBMIT_INTERVAL)
            else:
                raise AssertionError(
                    "no nonce in {} attempts met the target at a rolled ntime".format(
                        MAX_NONCE_ATTEMPTS))

        assert_equal(self.nodes[0].getblockcount(), before + 1)
        tip = self.nodes[0].getblock(self.nodes[0].getbestblockhash())
        # The rolled timestamp is the one that got mined, so nBits really did
        # hold still while it moved.
        assert_equal(tip['time'], ntime)
        assert_greater_than(ntime, bounds['ntime_min'])

        # An ntime past the ceiling is still refused, which on a pinned chain
        # was indistinguishable from refusing every roll.
        with self.connect() as client:
            work = client.get_work(POW_TYPE_X16RT)
            template = work['template']
            held = self.nodes[0].getblockcount()
            client.submit_solution(template['template_id'], template['version'],
                                   work['time_constraints']['ntime_max'] + 1, 0,
                                   build_coinbase(template))
            reply = client.request_transaction_data(template['template_id'])
            assert_equal(reply['template_id'], template['template_id'])
        assert_equal(self.nodes[0].getblockcount(), held)

        self.restart_node(0, extra_args=self.extra_args[0])

    def test_finding_blocks_quickly_is_not_rate_limited(self):
        """A pool that keeps finding blocks must not be disconnected for it.

        The submission allowance exists to stop cheap bytes buying expensive
        hashes, so it has to meter submissions that waste one. A submission
        whose proof of work verifies is not that, and cannot be forged, so it
        is refunded and the client is not charged for succeeding.

        The distinction is the whole point, so the test has to respect it:
        failed submissions are paced, because those legitimately consume the
        allowance, and successful ones are not paced at all. Anything that
        trips the limit here is therefore a success being charged for.

        Without the refund this measured the wrong thing on every chain
        anyone can test with. Where the share target is the network target,
        every share a miner finds is a block candidate, so a working pool
        forwarding them as it must was disconnected by its own node for
        succeeding.
        """
        self.log.info("Finding blocks faster than the allowance is not punished")
        before = self.nodes[0].getblockcount()
        blocks_wanted = sv2_MAX_SOLUTIONS + 3

        with self.connect() as client:
            work = client.get_work()
            for _ in range(blocks_wanted):
                template = work['template']
                coinbase = build_coinbase(template)
                ntime = work['time_constraints']['ntime_min']
                height = self.nodes[0].getblockcount()

                for nonce in range(MAX_NONCE_ATTEMPTS):
                    client.submit_solution(template['template_id'], template['version'],
                                           ntime, nonce, coinbase)
                    if self.nodes[0].getblockcount() > height:
                        break
                    # A refused submission did consume the allowance, by
                    # design, so do not let those accumulate. There is no
                    # way to pick a winning nonce in advance from here.
                    time.sleep(SUBMIT_INTERVAL)
                else:
                    raise AssertionError("no nonce met the target")

                # The tip moved, so fresh work arrives unprompted. Reading it
                # also proves the connection survived the submission, which
                # before the refund it would not have past the fourth block.
                work = client.read_work()

        assert_equal(self.nodes[0].getblockcount(), before + blocks_wanted)


if __name__ == '__main__':
    Sv2TemplateProviderTest(__file__).main()
