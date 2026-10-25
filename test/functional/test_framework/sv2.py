#!/usr/bin/env python3
# Copyright (c) 2026 The Avian Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A Stratum v2 Template Distribution client.

This is a port of src/sv2, not an independent implementation. It exists so the
functional tests can drive aviand's Template Provider without an external
Stratum v2 stack, and so that the wire format has a second reader.

The Noise NX handshake reuses the framework's BIP324 primitives, because
Stratum v2's key agreement is BIP324's v2_ecdh down to the tag and the
role-dependent key ordering. Everything above the ECDH is Noise, and is
implemented here.
"""

import hashlib
import hmac
import socket
import time
from io import BytesIO

from .crypto.bip324_cipher import aead_chacha20_poly1305_decrypt, aead_chacha20_poly1305_encrypt
from .crypto.ellswift import ellswift_create, ellswift_ecdh_xonly, xswiftec
from .crypto.secp256k1 import FE
from .key import TaggedHash, verify_schnorr
from .messages import COutPoint, CTransaction, CTxIn, CTxInWitness, CTxOut, hash256
from .script import CScript, OP_TRUE

# Noise and framing sizes, from src/sv2/noise.h.
MAC_SIZE = 16
ELLSWIFT_SIZE = 64
CERT_SIZE = 74
HEADER_SIZE = 6
ENCRYPTED_HEADER_SIZE = HEADER_SIZE + MAC_SIZE
MAX_PLAINTEXT_CHUNK = 65519
HANDSHAKE_MSG1_SIZE = ELLSWIFT_SIZE
HANDSHAKE_MSG2_SIZE = ELLSWIFT_SIZE + (ELLSWIFT_SIZE + MAC_SIZE) + (CERT_SIZE + MAC_SIZE)

# Every byte of this string, capitalization included, is part of the wire
# contract: one wrong character gives a different transcript hash and the
# handshake fails with no useful diagnostic.
NOISE_PROTOCOL_NAME = b"Noise_NX_Secp256k1+EllSwift_ChaChaPoly_SHA256"

PROTOCOL_VERSION = 2

PROTOCOL_MINING = 0
PROTOCOL_JOB_DECLARATION = 1
PROTOCOL_TEMPLATE_DISTRIBUTION = 2

EXT_CORE = 0x0000
EXT_NEGOTIATION = 0x0001
AVIAN_EXTENSION_ID = 0x4001
CHANNEL_MSG_BIT = 0x8000

# Message types are namespaced by extension, so 0x00 is SetupConnection under
# EXT_CORE and RequestExtensions under EXT_NEGOTIATION.
SETUP_CONNECTION = 0x00
SETUP_CONNECTION_SUCCESS = 0x01
SETUP_CONNECTION_ERROR = 0x02
COINBASE_OUTPUT_CONSTRAINTS = 0x70
NEW_TEMPLATE = 0x71
SET_NEW_PREV_HASH = 0x72
REQUEST_TRANSACTION_DATA = 0x73
REQUEST_TRANSACTION_DATA_SUCCESS = 0x74
REQUEST_TRANSACTION_DATA_ERROR = 0x75
SUBMIT_SOLUTION = 0x76

REQUEST_EXTENSIONS = 0x00
REQUEST_EXTENSIONS_SUCCESS = 0x01
REQUEST_EXTENSIONS_ERROR = 0x02

SELECT_ALGORITHM = 0x00
SELECT_ALGORITHM_SUCCESS = 0x01
SELECT_ALGORITHM_ERROR = 0x02
TIME_CONSTRAINTS = 0x03

POW_TYPE_X16RT = 0
POW_TYPE_MINOTAURX = 1

# X16RT rederives its hash chain every 128 seconds.
TIME_MASK = 0xffffff80


class Sv2Error(Exception):
    """A protocol violation by either side."""


def ser_uint(value, size):
    return int(value).to_bytes(size, 'little')


def ser_str(text):
    raw = text.encode()
    assert len(raw) < 256
    return bytes([len(raw)]) + raw


def ser_blob(data, prefix):
    return len(data).to_bytes(prefix, 'little') + data


def ser_seq(items, prefix):
    return len(items).to_bytes(prefix, 'little') + b''.join(items)


class Reader:
    """A cursor over a payload.

    Every field is taken through here so that a short or over-long message
    raises instead of quietly decoding as something else.
    """

    def __init__(self, data):
        self.data = data
        self.pos = 0

    def take(self, n):
        if self.pos + n > len(self.data):
            raise Sv2Error("payload is {} bytes short".format(self.pos + n - len(self.data)))
        out = self.data[self.pos:self.pos + n]
        self.pos += n
        return out

    def uint(self, size):
        return int.from_bytes(self.take(size), 'little')

    def u8(self):
        return self.uint(1)

    def u16(self):
        return self.uint(2)

    def u24(self):
        return self.uint(3)

    def u32(self):
        return self.uint(4)

    def u64(self):
        return self.uint(8)

    def boolean(self):
        # Only the low bit carries meaning; the rest of the byte is reserved.
        return bool(self.u8() & 1)

    def hash(self):
        return self.take(32)

    def blob(self, prefix):
        return self.take(self.uint(prefix))

    def text(self):
        return self.blob(1).decode()

    def seq(self, prefix, item):
        return [item(self) for _ in range(self.uint(prefix))]

    def done(self):
        if self.pos != len(self.data):
            raise Sv2Error("{} trailing bytes in payload".format(len(self.data) - self.pos))


def hkdf2(salt, ikm):
    """Noise's HKDF with an empty info field, which yields two outputs."""
    temp = hmac.new(salt, ikm, hashlib.sha256).digest()
    out1 = hmac.new(temp, b'\x01', hashlib.sha256).digest()
    out2 = hmac.new(temp, out1 + b'\x02', hashlib.sha256).digest()
    return out1, out2


def ellswift_to_xonly(ellswift):
    """The x-only public key an ElligatorSwift encoding decodes to."""
    u = FE(int.from_bytes(ellswift[:32], 'big'))
    t = FE(int.from_bytes(ellswift[32:], 'big'))
    return xswiftec(u, t).to_bytes()


class CipherState:
    """One Noise cipher: a key and a message counter."""

    def __init__(self):
        self.key = None
        self.nonce = 0

    def set_key(self, key):
        self.key = key
        self.nonce = 0

    def _nonce96(self):
        # Four zero bytes then the counter little-endian.
        return b'\x00\x00\x00\x00' + self.nonce.to_bytes(8, 'little')

    def encrypt(self, plain, aad=b''):
        out = aead_chacha20_poly1305_encrypt(self.key, self._nonce96(), aad, plain)
        self.nonce += 1
        return out

    def decrypt(self, cipher, aad=b''):
        out = aead_chacha20_poly1305_decrypt(self.key, self._nonce96(), aad, cipher)
        # Advance even on failure, as the node does: a forged frame consumes
        # its nonce, so a peer cannot probe the same counter repeatedly.
        self.nonce += 1
        if out is None:
            raise Sv2Error("MAC check failed")
        return out


class SymmetricState:
    """The Noise chaining key and transcript hash."""

    def __init__(self):
        # The protocol name is longer than 32 bytes, so h starts as its hash
        # rather than the zero-padded name.
        self.h = hashlib.sha256(NOISE_PROTOCOL_NAME).digest()
        self.ck = self.h
        self.cipher = CipherState()
        # MixHash over an empty prologue, which reduces to hashing h again.
        self.mix_hash(b'')

    def mix_hash(self, data):
        self.h = hashlib.sha256(self.h + data).digest()

    def mix_key(self, ikm):
        self.ck, temp_k = hkdf2(self.ck, ikm)
        self.cipher.set_key(temp_k)

    def decrypt_and_hash(self, cipher):
        # h is the associated data, so it is captured before the ciphertext is
        # mixed in.
        aad = self.h
        plain = self.cipher.decrypt(cipher, aad)
        self.mix_hash(cipher)
        return plain

    def split(self):
        k1, k2 = hkdf2(self.ck, b'')
        c1, c2 = CipherState(), CipherState()
        c1.set_key(k1)
        c2.set_key(k2)
        return c1, c2


def decode_certificate(raw):
    r = Reader(raw)
    cert = {
        'version': r.u16(),
        'valid_from': r.u32(),
        'not_valid_after': r.u32(),
        'signature': r.take(64),
    }
    r.done()
    return cert


def certificate_digest(cert, static_xonly):
    """SHA256(version || valid_from || not_valid_after || static_key).

    A single SHA256, not Bitcoin's usual double hash.
    """
    return hashlib.sha256(
        ser_uint(cert['version'], 2)
        + ser_uint(cert['valid_from'], 4)
        + ser_uint(cert['not_valid_after'], 4)
        + static_xonly
    ).digest()


def verify_certificate(cert, static_xonly, authority_xonly, now):
    if cert['version'] != 0:
        raise Sv2Error("unknown certificate version {}".format(cert['version']))
    if not cert['valid_from'] <= now <= cert['not_valid_after']:
        raise Sv2Error("certificate is not valid at {}".format(now))
    if not verify_schnorr(authority_xonly, cert['signature'], certificate_digest(cert, static_xonly)):
        raise Sv2Error("certificate is not signed by the authority key")


class Sv2Transport:
    """The initiator half of the Noise NX handshake, and the framing above it."""

    def __init__(self, authority_pubkey=None):
        self.authority_pubkey = authority_pubkey
        self.state = SymmetricState()
        self.eph_priv, self.eph_pub = ellswift_create()
        self.send_cipher = None
        self.recv_cipher = None
        self.remote_static = None
        self.cert = None

    def msg1(self):
        self.state.mix_hash(self.eph_pub)
        return self.eph_pub

    def _ecdh(self, theirs):
        # We are always the initiator, so our key is always first.
        x = ellswift_ecdh_xonly(theirs, self.eph_priv)
        return TaggedHash("bip324_ellswift_xonly_ecdh", self.eph_pub + theirs + x)

    def read_msg2(self, data, now=None):
        if len(data) != HANDSHAKE_MSG2_SIZE:
            raise Sv2Error("handshake message 2 is {} bytes, expected {}".format(
                len(data), HANDSHAKE_MSG2_SIZE))

        their_ephemeral = data[:ELLSWIFT_SIZE]
        self.state.mix_hash(their_ephemeral)
        self.state.mix_key(self._ecdh(their_ephemeral))

        at = ELLSWIFT_SIZE
        their_static = self.state.decrypt_and_hash(data[at:at + ELLSWIFT_SIZE + MAC_SIZE])
        at += ELLSWIFT_SIZE + MAC_SIZE
        self.state.mix_key(self._ecdh(their_static))

        self.cert = decode_certificate(self.state.decrypt_and_hash(data[at:at + CERT_SIZE + MAC_SIZE]))
        self.remote_static = ellswift_to_xonly(their_static)
        if self.authority_pubkey is not None:
            verify_certificate(self.cert, self.remote_static, self.authority_pubkey,
                               int(time.time()) if now is None else now)

        # c1 carries initiator to responder traffic, c2 the reverse.
        self.send_cipher, self.recv_cipher = self.state.split()

    def encode(self, extension_type, msg_type, payload):
        header = ser_uint(extension_type, 2) + ser_uint(msg_type, 1) + ser_uint(len(payload), 3)
        # The header and each chunk are separate AEAD operations on the same
        # cipher, so they consume consecutive nonces and must stay in order.
        out = self.send_cipher.encrypt(header)
        at = 0
        while at < len(payload):
            chunk = payload[at:at + MAX_PLAINTEXT_CHUNK]
            out += self.send_cipher.encrypt(chunk)
            at += len(chunk)
        return out

    def read_frame(self, recv_exact):
        r = Reader(self.recv_cipher.decrypt(recv_exact(ENCRYPTED_HEADER_SIZE)))
        extension_type = r.u16()
        msg_type = r.u8()
        length = r.u24()
        r.done()

        payload = b''
        while len(payload) < length:
            chunk = min(length - len(payload), MAX_PLAINTEXT_CHUNK)
            payload += self.recv_cipher.decrypt(recv_exact(chunk + MAC_SIZE))
        return extension_type, msg_type, payload


def decode_new_template(payload):
    r = Reader(payload)
    template = {
        'template_id': r.u64(),
        'future_template': r.boolean(),
        'version': r.u32(),
        'coinbase_tx_version': r.u32(),
        'coinbase_prefix': r.blob(1),
        'coinbase_tx_input_sequence': r.u32(),
        'coinbase_tx_value_remaining': r.u64(),
        'coinbase_tx_outputs_count': r.u32(),
        'coinbase_tx_outputs': r.blob(2),
        'coinbase_tx_locktime': r.u32(),
        'merkle_path': r.seq(1, Reader.hash),
    }
    r.done()
    return template


def decode_set_new_prev_hash(payload):
    r = Reader(payload)
    out = {
        'template_id': r.u64(),
        'prev_hash': r.hash(),
        'ntime_start': r.u32(),
        'nbits': r.u32(),
        'target': r.hash(),
    }
    r.done()
    return out


def decode_time_constraints(payload):
    r = Reader(payload)
    out = {'template_id': r.u64(), 'ntime_min': r.u32(), 'ntime_max': r.u32()}
    r.done()
    return out


def decode_transaction_data_success(payload):
    r = Reader(payload)
    out = {
        'template_id': r.u64(),
        'excess_data': r.blob(2),
        'transaction_list': r.seq(2, lambda rr: rr.blob(3)),
    }
    r.done()
    return out


def template_outputs(template):
    """The outputs a client must reproduce verbatim.

    NewTemplate.coinbase_tx_outputs carries them concatenated with no count
    prefix of its own, which is why the count is a separate field.
    """
    stream = BytesIO(template['coinbase_tx_outputs'])
    outputs = []
    for _ in range(template['coinbase_tx_outputs_count']):
        out = CTxOut()
        out.deserialize(stream)
        outputs.append(out)
    assert stream.read() == b''
    return outputs


def build_coinbase(template, payout_script=None, extranonce=b''):
    """A coinbase built the way the Template Provider says to build one.

    The node checks all of this before hashing: the advertised version,
    locktime and sequence exactly, a scriptSig starting with the BIP34 height
    push, the BIP141 witness reserved value, and every required output present
    unaltered with the commitment last.
    """
    coinbase = CTransaction()
    coinbase.version = template['coinbase_tx_version']
    coinbase.nLockTime = template['coinbase_tx_locktime']

    coinbase.vin = [CTxIn(COutPoint(0, 0xffffffff),
                          template['coinbase_prefix'] + extranonce,
                          template['coinbase_tx_input_sequence'])]
    coinbase.wit.vtxinwit = [CTxInWitness()]
    coinbase.wit.vtxinwit[0].scriptWitness.stack = [b'\x00' * 32]

    if payout_script is None:
        payout_script = CScript([OP_TRUE])
    coinbase.vout = [CTxOut(template['coinbase_tx_value_remaining'], payout_script)]
    coinbase.vout.extend(template_outputs(template))
    return coinbase


def merkle_root(coinbase, merkle_path):
    """Fold the path onto the coinbase txid.

    The coinbase is always leftmost, so every sibling is a right-hand one.
    """
    current = hash256(coinbase.serialize_without_witness())
    for sibling in merkle_path:
        current = hash256(current + sibling)
    return current


class Sv2Client:
    """A Template Distribution client over a real socket.

    Blocking by design: a functional test wants "send this, now tell me what
    came back", and an exception at the line that cared.
    """

    def __init__(self, host, port, authority_pubkey=None, timeout=60):
        self.sock = socket.create_connection((host, port), timeout)
        self.sock.settimeout(timeout)
        self.transport = Sv2Transport(authority_pubkey)
        self.sock.sendall(self.transport.msg1())
        self.transport.read_msg2(self.recv_exact(HANDSHAKE_MSG2_SIZE))

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass

    def recv_exact(self, n):
        buf = b''
        while len(buf) < n:
            chunk = self.sock.recv(n - len(buf))
            if not chunk:
                raise ConnectionResetError("the node closed the Stratum v2 connection")
            buf += chunk
        return buf

    def send(self, extension_type, msg_type, payload=b''):
        self.sock.sendall(self.transport.encode(extension_type, msg_type, payload))

    def recv(self):
        return self.transport.read_frame(self.recv_exact)

    def recv_expect(self, extension_type, msg_type):
        got_ext, got_type, payload = self.recv()
        if (got_ext, got_type) != (extension_type, msg_type):
            raise Sv2Error(
                "expected extension 0x{:04x} message 0x{:02x}, got extension 0x{:04x} message 0x{:02x}".format(
                    extension_type, msg_type, got_ext, got_type))
        return payload

    def disconnected(self):
        """True if the node closed the connection rather than answering."""
        try:
            self.recv()
        except (ConnectionResetError, ConnectionAbortedError, TimeoutError, OSError):
            return True
        return False

    # Session setup.

    def setup_connection(self, protocol=PROTOCOL_TEMPLATE_DISTRIBUTION, flags=0,
                         min_version=PROTOCOL_VERSION, max_version=PROTOCOL_VERSION):
        self.send(EXT_CORE, SETUP_CONNECTION,
                  ser_uint(protocol, 1) + ser_uint(min_version, 2) + ser_uint(max_version, 2)
                  + ser_uint(flags, 4) + ser_str("127.0.0.1") + ser_uint(0, 2)
                  + ser_str("avian-functional-test") + ser_str("") + ser_str("") + ser_str(""))
        got_ext, got_type, payload = self.recv()
        assert got_ext == EXT_CORE, got_ext
        r = Reader(payload)
        if got_type == SETUP_CONNECTION_ERROR:
            out = {'error_code': None, 'flags': r.u32()}
            out['error_code'] = r.text()
        elif got_type == SETUP_CONNECTION_SUCCESS:
            out = {'used_version': r.u16(), 'flags': r.u32()}
        else:
            raise Sv2Error("unexpected reply 0x{:02x} to SetupConnection".format(got_type))
        r.done()
        return out

    def request_extensions(self, wanted=(AVIAN_EXTENSION_ID,), request_id=1):
        self.send(EXT_NEGOTIATION, REQUEST_EXTENSIONS,
                  ser_uint(request_id, 2) + ser_seq([ser_uint(e, 2) for e in wanted], 2))
        got_ext, got_type, payload = self.recv()
        assert got_ext == EXT_NEGOTIATION, got_ext
        r = Reader(payload)
        out = {'request_id': r.u16(), 'accepted': got_type == REQUEST_EXTENSIONS_SUCCESS}
        if out['accepted']:
            out['extensions'] = r.seq(2, Reader.u16)
        else:
            # The error names what it could not provide and what it needs, so
            # a client that asked for the wrong thing learns which extension
            # this node requires.
            out['unsupported'] = r.seq(2, Reader.u16)
            out['required'] = r.seq(2, Reader.u16)
        r.done()
        return out

    def select_algorithm(self, pow_type):
        self.send(AVIAN_EXTENSION_ID, SELECT_ALGORITHM, ser_uint(pow_type, 1))
        got_ext, got_type, payload = self.recv()
        assert got_ext == AVIAN_EXTENSION_ID, got_ext
        r = Reader(payload)
        if got_type == SELECT_ALGORITHM_ERROR:
            out = {'error_code': r.text()}
        else:
            out = {'pow_type': r.u8(), 'version_rolling_mask': r.u32()}
        r.done()
        return out

    def coinbase_output_constraints(self, size=1000, sigops=10):
        self.send(EXT_CORE, COINBASE_OUTPUT_CONSTRAINTS, ser_uint(size, 4) + ser_uint(sigops, 2))

    # Template distribution.

    def read_work(self):
        """Read one round of work: a template, then whatever follows it.

        SetNewPrevHash only accompanies a template built on a tip the client
        has not seen, while AvianTimeConstraints follows every template, so a
        caller cannot assume a fixed number of messages.
        """
        work = {'template': decode_new_template(self.recv_expect(EXT_CORE, NEW_TEMPLATE)),
                'prev_hash': None,
                'time_constraints': None}
        while work['time_constraints'] is None:
            got_ext, got_type, payload = self.recv()
            if (got_ext, got_type) == (EXT_CORE, SET_NEW_PREV_HASH):
                work['prev_hash'] = decode_set_new_prev_hash(payload)
            elif (got_ext, got_type) == (AVIAN_EXTENSION_ID, TIME_CONSTRAINTS):
                work['time_constraints'] = decode_time_constraints(payload)
            else:
                raise Sv2Error("unexpected extension 0x{:04x} message 0x{:02x} alongside a template".format(
                    got_ext, got_type))
        return work

    def get_work(self, pow_type=POW_TYPE_X16RT, size=1000, sigops=10):
        """Walk a fresh connection all the way to a template."""
        assert self.setup_connection()['used_version'] == PROTOCOL_VERSION
        assert self.request_extensions()['accepted']
        assert self.select_algorithm(pow_type)['pow_type'] == pow_type
        self.coinbase_output_constraints(size, sigops)
        return self.read_work()

    def request_transaction_data(self, template_id):
        self.send(EXT_CORE, REQUEST_TRANSACTION_DATA, ser_uint(template_id, 8))
        got_ext, got_type, payload = self.recv()
        assert got_ext == EXT_CORE, got_ext
        if got_type == REQUEST_TRANSACTION_DATA_ERROR:
            r = Reader(payload)
            out = {'template_id': r.u64(), 'error_code': r.text()}
            r.done()
            return out
        return decode_transaction_data_success(payload)

    def submit_solution(self, template_id, version, ntime, nonce, coinbase):
        self.send(EXT_CORE, SUBMIT_SOLUTION,
                  ser_uint(template_id, 8) + ser_uint(version, 4) + ser_uint(ntime, 4)
                  + ser_uint(nonce, 4) + ser_blob(coinbase.serialize_with_witness(), 2))
