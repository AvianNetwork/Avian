#!/usr/bin/env python3
# Copyright (c) 2017 The Bitcoin Core developers
# Copyright (c) 2017-2020 The Raven Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

"""
Test message sending before handshake completion.

A node should never send anything other than VERSION/VERACK/REJECT until it's
received a VERACK.

This test connects to a node and sends it a few messages, trying to entice it
into sending us something it shouldn't.

Also test that nodes that send unsupported service bits to aviand are disconnected
and don't receive a VERACK. Unsupported service bits are currently 1 << 5 and
1 << 7 (until August 1st 2018).

UPDATE: Avian RIP-2 uses bit 1 << 5.  Currently there are no unsupported service bits.
"""

from test_framework.mininode import NodeConnCB, NodeConn, MsgVerack, MsgPing, MsgGetAddr, NetworkThread, mininode_lock
from test_framework.test_framework import AvianTestFramework
from test_framework.util import logger, p2p_port, wait_until, time

banscore = 10


# noinspection PyMethodOverriding
class CLazyNode(NodeConnCB):
    def __init__(self):
        super().__init__()
        self.unexpected_msg = False
        self.ever_connected = False

    def bad_message(self, message):
        self.unexpected_msg = True
        logger.info("should not have received message: %s" % message.command)

    def on_open(self, conn):
        self.connected = True
        self.ever_connected = True

    def on_version(self, conn, message): self.bad_message(message)
    def on_verack(self, conn, message): self.bad_message(message)
    def on_reject(self, conn, message): self.bad_message(message)
    def on_inv(self, conn, message): self.bad_message(message)
    def on_addr(self, conn, message): self.bad_message(message)
    def on_alert(self, conn, message): self.bad_message(message)
    def on_getdata(self, conn, message): self.bad_message(message)
    def on_getblocks(self, conn, message): self.bad_message(message)
    def on_tx(self, conn, message): self.bad_message(message)
    def on_block(self, conn, message): self.bad_message(message)
    def on_getaddr(self, conn, message): self.bad_message(message)
    def on_headers(self, conn, message): self.bad_message(message)
    def on_getheaders(self, conn, message): self.bad_message(message)
    def on_ping(self, conn, message): self.bad_message(message)
    def on_mempool(self, conn, message): self.bad_message(message)
    def on_pong(self, conn, message): self.bad_message(message)
    def on_feefilter(self, conn, message): self.bad_message(message)
    def on_sendheaders(self, conn, message): self.bad_message(message)
    def on_sendcmpct(self, conn, message): self.bad_message(message)
    def on_cmpctblock(self, conn, message): self.bad_message(message)
    def on_getblocktxn(self, conn, message): self.bad_message(message)
    def on_blocktxn(self, conn, message): self.bad_message(message)

# Node that never sends a version. We'll use this to send a bunch of messages
# anyway, and eventually get disconnected.
class CNodeNoVersionBan(CLazyNode):
    # send a bunch of veracks without sending a message. This should get us disconnected.
    # NOTE: implementation-specific check here. Remove if aviand ban behavior changes
    def on_open(self, conn):
        super().on_open(conn)
        for _ in range(banscore):
            self.send_message(MsgVerack())

    def on_reject(self, conn, message): pass

# Node that never sends a version. This one just sits idle and hopes to receive
# any message (it shouldn't!)
class CNodeNoVersionIdle(CLazyNode):
    def __init__(self):
        super().__init__()

# Node that sends a version but not a verack.
class CNodeNoVerackIdle(CLazyNode):
    def __init__(self):
        self.version_received = False
        super().__init__()

    def on_reject(self, conn, message): pass
    def on_verack(self, conn, message): pass
    # When version is received, don't reply with a verack. Instead, see if the
    # node will give us a message that it shouldn't. This is not an exhaustive
    # list!
    def on_version(self, conn, message):
        self.version_received = True
        conn.send_message(MsgPing())
        conn.send_message(MsgGetAddr())

class P2PLeakTest(AvianTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.extra_args = [['-banscore='+str(banscore)]]

    def run_test(self):
        no_version_bannode = CNodeNoVersionBan()
        no_version_idlenode = CNodeNoVersionIdle()
        no_verack_idlenode = CNodeNoVerackIdle()
        CLazyNode()
        CLazyNode()

        connections = [NodeConn('127.0.0.1', p2p_port(0), self.nodes[0], no_version_bannode, send_version=False),
                       NodeConn('127.0.0.1', p2p_port(0), self.nodes[0], no_version_idlenode, send_version=False),
                       NodeConn('127.0.0.1', p2p_port(0), self.nodes[0], no_verack_idlenode)]
        no_version_bannode.add_connection(connections[0])
        no_version_idlenode.add_connection(connections[1])
        no_verack_idlenode.add_connection(connections[2])

        NetworkThread().start()  # Start up network handling in another thread

        wait_until(lambda: no_version_bannode.ever_connected, err_msg="Wait for no Version BanNode", timeout=10, lock=mininode_lock)
        wait_until(lambda: no_version_idlenode.ever_connected, err_msg="Wait for no Version IdleNode", timeout=10, lock=mininode_lock)
        wait_until(lambda: no_verack_idlenode.version_received, err_msg="Wait for VerAck IdleNode", timeout=10, lock=mininode_lock)

        # Mine a block and make sure that it's not sent to the connected nodes
        self.nodes[0].generate(1)

        #Give the node enough time to possibly leak out a message
        time.sleep(5)

        #This node should have been banned
        assert not no_version_bannode.connected

        [conn.disconnect_node() for conn in connections]

        # Wait until all connections are closed
        wait_until(lambda: len(self.nodes[0].getpeerinfo()) == 0, err_msg="Wait for Connection Close")

        # Make sure no unexpected messages came in
        assert(no_version_bannode.unexpected_msg == False)
        assert(no_version_idlenode.unexpected_msg == False)
        assert(no_verack_idlenode.unexpected_msg == False)

        NetworkThread().start()  # Network thread stopped when all previous NodeConnCBs disconnected. Restart it

if __name__ == '__main__':
    P2PLeakTest().main()
