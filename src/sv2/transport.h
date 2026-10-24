// Copyright (c) 2026 The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_SV2_TRANSPORT_H
#define BITCOIN_SV2_TRANSPORT_H

#include <sv2/messages.h>
#include <sv2/noise.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace sv2 {

//! Ciphertext length for a payload of `len` plaintext bytes.
//!
//! Each chunk of at most MAX_PLAINTEXT_CHUNK bytes gains its own MAC, so the
//! expansion is not a constant. A zero-length payload occupies no chunks.
constexpr size_t CiphertextLen(size_t len)
{
    const size_t chunks{(len + MAX_PLAINTEXT_CHUNK - 1) / MAX_PLAINTEXT_CHUNK};
    return len + chunks * MAC_SIZE;
}

//! A decoded inbound message.
struct ReceivedMsg {
    NetHeader header;
    std::vector<uint8_t> payload;
};

//! Byte-level framing for one Sv2 connection, with no knowledge of sockets.
//!
//! The caller appends whatever it read, drains whatever is ready, and writes
//! out whatever is queued. Keeping the socket out of this class is what makes
//! the whole wire format testable without a network, and fuzzable.
//!
//! Two phases. During the handshake, bytes are the plaintext Noise messages:
//! 64 bytes from the initiator and 234 back. Afterwards every frame is a
//! 22-byte encrypted header followed by the payload in MAC-tagged chunks. The
//! header and each chunk are separate AEAD operations on the same cipher, so
//! they consume consecutive nonces and must be processed strictly in order.
class Transport
{
public:
    //! Server side: completes the handshake by proving `static_key` is
    //! endorsed by `cert`.
    static Transport Responder(const CKey& static_key, const SignatureNoiseMessage& cert);

    //! Client side. An absent `authority` accepts the server's certificate
    //! without checking it, which is trust on first use.
    static Transport Initiator(const std::optional<XOnlyPubKey>& authority);

    bool HandshakeComplete() const { return m_phase == Phase::TRANSPORT; }

    //! Append bytes read from the peer.
    void Received(std::span<const std::byte> bytes);

    //! Advance as far as the buffered bytes allow. Returns false if the peer
    //! violated the protocol or a MAC failed, after which the caller must drop
    //! the connection rather than resynchronize: an authenticated stream has
    //! no safe way to skip a bad frame.
    //!
    //! `now` is the current time, used for certificate validity.
    [[nodiscard]] bool Advance(int64_t now);

    //! Take the next decoded message, if one is ready.
    std::optional<ReceivedMsg> Next();

    //! Queue a message. Returns false if the payload exceeds what msg_length
    //! can describe, or if the handshake has not finished.
    [[nodiscard]] bool Send(const NetHeader& header, std::span<const uint8_t> payload);

    //! Bytes waiting to go to the peer. Call Sent() with however many were
    //! actually written.
    std::span<const std::byte> SendBuffer() const;
    void Sent(size_t count);

    //! True once there is nothing left to write.
    bool SendBufferEmpty() const { return m_send_pos >= m_send.size(); }

    //! The server static key the initiator learned during the handshake.
    const std::optional<XOnlyPubKey>& RemoteStaticKey() const { return m_handshake.RemoteStaticKey(); }

private:
    enum class Phase {
        //! Responder waiting for message 1, or initiator waiting for message 2.
        HANDSHAKE,
        TRANSPORT,
    };

    explicit Transport(HandshakeState handshake, bool initiator);

    //! Try to decode one complete frame. Returns false on a fatal error, and
    //! sets `progress` when a frame was consumed.
    [[nodiscard]] bool ReadFrame(bool& progress);

    //! A frame whose header has been decrypted while its body is still in
    //! flight. The header cannot simply be re-read when the rest arrives: each
    //! AEAD operation consumes a nonce, so decrypting it twice would
    //! desynchronize the stream. Keep the result instead.
    struct PendingFrame {
        NetHeader header;
        size_t cipher_len;
    };
    std::optional<PendingFrame> m_pending;

    bool m_initiator;
    Phase m_phase{Phase::HANDSHAKE};
    HandshakeState m_handshake;
    CipherState m_send_cipher;
    CipherState m_recv_cipher;

    std::vector<std::byte> m_recv;
    size_t m_recv_pos{0};
    std::vector<std::byte> m_send;
    size_t m_send_pos{0};

    std::vector<ReceivedMsg> m_ready;
    size_t m_ready_pos{0};

    //! Set once a fatal error has been reported, so a caller that keeps going
    //! cannot coax the state machine into reinterpreting the stream.
    bool m_failed{false};
};

} // namespace sv2

#endif // BITCOIN_SV2_TRANSPORT_H
