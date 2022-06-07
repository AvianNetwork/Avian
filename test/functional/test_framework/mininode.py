#!/usr/bin/env python3
# Copyright (c) 2010 ArtForz -- public domain half-a-node
# Copyright (c) 2012 Jeff Garzik
# Copyright (c) 2010-2016 The Bitcoin Core developers
# Copyright (c) 2017-2020 The Raven Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

"""
Avian P2P network half-a-node.

This python code was modified from ArtForz' public domain  half-a-node, as
found in the mini-node branch of http://github.com/jgarzik/pynode.

NodeConn: an object which manages p2p connectivity to a avian node

NodeConnCB: a base class that describes the interface for receiving
            callbacks with network messages from a NodeConn
"""

import asyncore
from collections import defaultdict
# from io import BytesIO
import logging
# import struct
import sys
from threading import RLock, Thread
from test_framework.messages import *
from test_framework.util import wait_until

logger = logging.getLogger("TestFramework.mininode")

# Keep our own socket map for asyncore, so that we can track disconnects
# ourselves (to workaround an issue with closing an asyncore socket when
# using select)
mininode_socket_map = dict()

# One lock for synchronizing all data access between the networking thread (see
# NetworkThread below) and the thread running the test logic.  For simplicity,
# NodeConn acquires this lock whenever delivering a message to a NodeConnCB,
# and whenever adding anything to the send buffer (in send_message()).  This
# lock should be acquired in the thread running the test logic to synchronize
# access to any data shared with the NodeConnCB or NodeConn.
mininode_lock = RLock()


class NodeConnCB:
    """Callback and helper functions for P2P connection to a aviand node.

    Individual test cases should subclass this and override the on_* methods
    if they want to alter message handling behaviour.
    """

    def __init__(self):
        # Track whether we have a P2P connection open to the node
        self.connected = False
        self.connection = None

        # Track number of messages of each type received and the most recent
        # message of each type
        self.message_count = defaultdict(int)
        self.last_message = {}

        # A count of the number of ping messages we've sent to the node
        self.ping_counter = 1

        # deliver_sleep_time is helpful for debugging race conditions in p2p
        # tests; it causes message delivery to sleep for the specified time
        # before acquiring the global lock and delivering the next message.
        self.deliver_sleep_time = None

    # Message receiving methods

    def deliver(self, conn, message):
        """Receive message and dispatch message to appropriate callback.

        We keep a count of how many of each message type has been received
        and the most recent message of each type.

        Optionally waits for deliver_sleep_time before dispatching message.
        """

        deliver_sleep = self.get_deliver_sleep_time()
        if deliver_sleep is not None:
            time.sleep(deliver_sleep)
        with mininode_lock:
            try:
                command = message.command.decode('ascii')
                self.message_count[command] += 1
                self.last_message[command] = message
                getattr(self, 'on_' + command)(conn, message)
            except Exception:
                print("ERROR delivering %s (%s)" % (repr(message), sys.exc_info()[0]))
                raise

    def get_deliver_sleep_time(self):
        with mininode_lock:
            return self.deliver_sleep_time

    # Callback methods. Can be overridden by subclasses in individual test
    # cases to provide custom message handling behaviour.
    def on_open(self, conn):
        self.connected = True

    def on_close(self, conn):
        self.connected = False
        self.connection = None

    def on_addr(self, conn, message):
        pass

    def on_alert(self, conn, message):
        pass

    def on_block(self, conn, message):
        pass

    def on_blocktxn(self, conn, message):
        pass

    def on_cmpctblock(self, conn, message):
        pass

    def on_feefilter(self, conn, message):
        pass

    def on_getaddr(self, conn, message):
        pass

    def on_getblocks(self, conn, message):
        pass

    def on_getblocktxn(self, conn, message):
        pass

    def on_getdata(self, conn, message):
        pass

    def on_getheaders(self, conn, message):
        pass

    def on_headers(self, conn, message):
        pass

    def on_mempool(self, conn):
        pass

    def on_notfound(self, conn, message):
        pass

    def on_pong(self, conn, message):
        pass

    def on_reject(self, conn, message):
        pass

    def on_sendcmpct(self, conn, message):
        pass

    def on_sendheaders(self, conn, message):
        pass

    def on_tx(self, conn, message):
        pass

    def on_inv(self, conn, message):
        want = MsgGetdata()
        for i in message.inv:
            if i.type != 0:
                want.inv.append(i)
        if len(want.inv):
            conn.send_message(want)

    def on_ping(self, conn, message):
        if conn.ver_send > BIP0031_VERSION:
            conn.send_message(MsgPong(message.nonce))

    def on_verack(self, conn, message):
        conn.ver_recv = conn.ver_send
        self.verack_received = True

    def on_version(self, conn, message):
        if message.nVersion >= 209:
            conn.send_message(MsgVerack())
        conn.ver_send = min(MY_VERSION, message.nVersion)
        if message.nVersion < 209:
            conn.ver_recv = conn.ver_send
        conn.nServices = message.nServices

    # Connection helper methods

    def add_connection(self, conn):
        self.connection = conn

    def wait_for_disconnect(self, timeout=60):
        test_function = lambda: not self.connected
        wait_until(test_function, err_msg="Wait for Disconnect", timeout=timeout, lock=mininode_lock)

    # Message receiving helper methods

    def wait_for_block(self, blockhash, timeout=60):
        test_function = lambda: self.last_message.get("block") and self.last_message[
            "block"].block.rehash() == blockhash
        wait_until(test_function, err_msg="Wait for Block", timeout=timeout, lock=mininode_lock)

    def wait_for_getdata(self, timeout=60):
        test_function = lambda: self.last_message.get("getdata")
        wait_until(test_function, err_msg="Wait for GetData", timeout=timeout, lock=mininode_lock)

    def wait_for_getheaders(self, timeout=60):
        test_function = lambda: self.last_message.get("getheaders")
        wait_until(test_function, err_msg="Wait for GetHeaders", timeout=timeout, lock=mininode_lock)

    def wait_for_inv(self, expected_inv, timeout=60):
        """Waits for an INV message and checks that the first inv object in the message was as expected."""
        if len(expected_inv) > 1:
            raise NotImplementedError("wait_for_inv() will only verify the first inv object")
        test_function = lambda: self.last_message.get("inv") and \
                                self.last_message["inv"].inv[0].type == expected_inv[0].type \
                                and self.last_message["inv"].inv[0].hash == expected_inv[0].hash
        wait_until(test_function, timeout=timeout, lock=mininode_lock, err_msg="wait_for_inv")

    def wait_for_verack(self, timeout=60):
        test_function = lambda: self.message_count["verack"]
        wait_until(test_function, err_msg="Wait for VerAck", timeout=timeout, lock=mininode_lock)

    # Message sending helper functions

    def send_message(self, message):
        if self.connection:
            self.connection.send_message(message)
        else:
            logger.error("Cannot send message. No connection to node!")

    def send_and_ping(self, message):
        self.send_message(message)
        self.sync_with_ping()

    # Sync up with the node
    def sync_with_ping(self, timeout=60):
        self.send_message(MsgPing(nonce=self.ping_counter))
        test_function = lambda: self.last_message.get("pong") and self.last_message["pong"].nonce == self.ping_counter
        wait_until(test_function, err_msg="Sync with Ping", timeout=timeout, lock=mininode_lock)
        self.ping_counter += 1


# The actual NodeConn class
# This class provides an interface for a p2p connection to a specified node
class NodeConn(asyncore.dispatcher):
    messagemap = {
        b"version": MsgVersion,
        b"verack": MsgVerack,
        b"addr": MsgAddr,
        b"alert": MsgAlert,
        b"inv": MsgInv,
        b"getdata": MsgGetdata,
        b"getblocks": MsgGetBlocks,
        b"tx": MsgTx,
        b"block": MsgBlock,
        b"getaddr": MsgGetAddr,
        b"ping": MsgPing,
        b"pong": MsgPong,
        b"headers": MsgHeaders,
        b"getheaders": MsgGetHeaders,
        b"reject": MsgReject,
        b"mempool": MsgMempool,
        b"notfound": MsgNotFound,
        b"feefilter": MsgFeeFilter,
        b"sendheaders": MsgSendHeaders,
        b"sendcmpct": MsgSendCmpct,
        b"cmpctblock": MsgCmpctBlock,
        b"getblocktxn": MsgGetBlockTxn,
        b"blocktxn": MsgBlockTxn
    }

    MAGIC_BYTES = {
        "mainnet": b"\x52\x41\x56\x4e",  # mainnet
        "testnet3": b"\x45\x50\x4f\x45",  # testnet3
        "regtest": b"\x43\x52\x4f\x57",  # regtest
    }

    def __init__(self, dstaddr, dstport, rpc, callback, net="regtest", services=NODE_NETWORK, send_version=True):
        asyncore.dispatcher.__init__(self, map=mininode_socket_map)
        self.dstaddr = dstaddr
        self.dstport = dstport
        self.create_socket(socket.AF_INET, socket.SOCK_STREAM)
        self.socket.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.sendbuf = b""
        self.recvbuf = b""
        self.ver_send = 209
        self.ver_recv = 209
        self.last_sent = 0
        self.state = "connecting"
        self.network = net
        self.cb = callback
        self.disconnect = False
        self.nServices = 0

        if send_version:
            # stuff version msg into sendbuf
            vt = MsgVersion()
            vt.nServices = services
            vt.addrTo.ip = self.dstaddr
            vt.addrTo.port = self.dstport
            vt.addrFrom.ip = "0.0.0.0"
            vt.addrFrom.port = 0
            self.send_message(vt, True)

        logger.info('Connecting to Avian Node: %s:%d' % (self.dstaddr, self.dstport))

        try:
            self.connect((dstaddr, dstport))
        except OSError:
            self.handle_close()
        self.rpc = rpc

    def handle_connect(self):
        if self.state != "connected":
            logger.debug("Connected & Listening: %s:%d" % (self.dstaddr, self.dstport))
            self.state = "connected"
            self.cb.on_open(self)

    def handle_close(self):
        logger.debug("Closing connection to: %s:%d" % (self.dstaddr, self.dstport))
        self.state = "closed"
        self.recvbuf = b""
        self.sendbuf = b""
        try:
            self.close()
        except OSError:
            pass
        self.cb.on_close(self)

    def handle_read(self):
        t = self.recv(8192)
        if len(t) > 0:
            self.recvbuf += t
            self.got_data()

    def readable(self):
        return True

    def writable(self):
        with mininode_lock:
            pre_connection = self.state == "connecting"
            length = len(self.sendbuf)
        return length > 0 or pre_connection

    def handle_write(self):
        with mininode_lock:
            # asyncore does not expose socket connection, only the first read/write
            # event, thus we must check connection manually here to know when we
            # actually connect
            if self.state == "connecting":
                self.handle_connect()
            if not self.writable():
                return

            try:
                sent = self.send(self.sendbuf)
            except OSError:
                self.handle_close()
                return
            self.sendbuf = self.sendbuf[sent:]

    def got_data(self):
        try:
            while True:
                if len(self.recvbuf) < 4:
                    return
                if self.recvbuf[:4] != self.MAGIC_BYTES[self.network]:
                    raise ValueError("got garbage %s" % repr(self.recvbuf))
                if self.ver_recv < 209:
                    if len(self.recvbuf) < 4 + 12 + 4:
                        return
                    command = self.recvbuf[4:4 + 12].split(b"\x00", 1)[0]
                    msglen = struct.unpack("<i", self.recvbuf[4 + 12:4 + 12 + 4])[0]
                    if len(self.recvbuf) < 4 + 12 + 4 + msglen:
                        return
                    msg = self.recvbuf[4 + 12 + 4:4 + 12 + 4 + msglen]
                    self.recvbuf = self.recvbuf[4 + 12 + 4 + msglen:]
                else:
                    if len(self.recvbuf) < 4 + 12 + 4 + 4:
                        return
                    command = self.recvbuf[4:4 + 12].split(b"\x00", 1)[0]
                    msglen = struct.unpack("<i", self.recvbuf[4 + 12:4 + 12 + 4])[0]
                    checksum = self.recvbuf[4 + 12 + 4:4 + 12 + 4 + 4]
                    if len(self.recvbuf) < 4 + 12 + 4 + 4 + msglen:
                        return
                    msg = self.recvbuf[4 + 12 + 4 + 4:4 + 12 + 4 + 4 + msglen]
                    th = sha256(msg)
                    h = sha256(th)
                    if checksum != h[:4]:
                        raise ValueError("got bad checksum " + repr(self.recvbuf))
                    self.recvbuf = self.recvbuf[4 + 12 + 4 + 4 + msglen:]
                if command in self.messagemap:
                    f = BytesIO(msg)
                    t = self.messagemap[command]()
                    t.deserialize(f)
                    self.got_message(t)
                else:
                    logger.warning("Received unknown command from %s:%d: '%s' %s" % (
                        self.dstaddr, self.dstport, command, repr(msg)))
                    raise ValueError(f"Unknown command: '{command}'")
        except Exception as e:
            logger.exception('got_data: %s', repr(e))
            raise

    def send_message(self, message, pushbuf=False):
        if self.state != "connected" and not pushbuf:
            raise IOError('Not connected, no pushbuf')
        self._log_message("send", message)
        command = message.command
        data = message.serialize()
        tmsg = self.MAGIC_BYTES[self.network]
        tmsg += command
        tmsg += b"\x00" * (12 - len(command))
        tmsg += struct.pack("<I", len(data))
        if self.ver_send >= 209:
            th = sha256(data)
            h = sha256(th)
            tmsg += h[:4]
        tmsg += data
        with mininode_lock:
            if len(self.sendbuf) == 0 and not pushbuf:
                try:
                    sent = self.send(tmsg)
                    self.sendbuf = tmsg[sent:]
                except BlockingIOError:
                    self.sendbuf = tmsg
            else:
                self.sendbuf += tmsg
            self.last_sent = time.time()

    def got_message(self, message):
        if message.command == b"version":
            if message.nVersion <= BIP0031_VERSION:
                self.messagemap[b'ping'] = MsgPingPreBip31
        if self.last_sent + 30 * 60 < time.time():
            self.send_message(self.messagemap[b'ping']())
        self._log_message("receive", message)
        self.cb.deliver(self, message)

    def _log_message(self, direction, msg):
        log_message = "Log message missing direction"
        if direction == "send":
            log_message = "Send message to "
        elif direction == "receive":
            log_message = "Received message from "
        log_message += "%s:%d: %s" % (self.dstaddr, self.dstport, repr(msg)[:500])
        if len(log_message) > 500:
            log_message += "... (msg truncated)"
        logger.debug(log_message)

    def disconnect_node(self):
        self.disconnect = True


class NetworkThread(Thread):
    def run(self):
        while mininode_socket_map:
            # We check for whether to disconnect outside of the asyncore
            # loop to workaround the behavior of asyncore when using
            # select
            disconnected = []
            for _, obj in mininode_socket_map.items():
                if obj.disconnect:
                    disconnected.append(obj)
            [obj.handle_close() for obj in disconnected]
            asyncore.loop(0.1, use_poll=True, map=mininode_socket_map, count=1)
        logger.debug("Network thread closing")


# An exception we can raise if we detect a potential disconnect
# (p2p or rpc) before the test is complete
class EarlyDisconnectError(Exception):
    def __init__(self, value):
        self.value = value

    def __str__(self):
        return repr(self.value)
