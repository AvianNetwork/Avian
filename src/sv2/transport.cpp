// Copyright (c) 2026 The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <sv2/transport.h>

#include <streams.h>

#include <algorithm>
#include <cassert>

namespace sv2 {

Transport::Transport(HandshakeState handshake, bool initiator)
    : m_initiator{initiator}, m_handshake{std::move(handshake)}
{
}

Transport Transport::Responder(const CKey& static_key, const SignatureNoiseMessage& cert)
{
    return Transport{HandshakeState::Responder(static_key, cert), /*initiator=*/false};
}

Transport Transport::Initiator(const std::optional<XOnlyPubKey>& authority)
{
    Transport transport{HandshakeState::Initiator(authority), /*initiator=*/true};
    // The initiator speaks first, so message 1 is queued immediately rather
    // than waiting for an Advance() that has nothing to read.
    transport.m_send.resize(HANDSHAKE_MSG1_SIZE);
    transport.m_handshake.WriteMsg1(transport.m_send);
    return transport;
}

void Transport::Received(std::span<const std::byte> bytes)
{
    m_recv.insert(m_recv.end(), bytes.begin(), bytes.end());
}

bool Transport::Advance(int64_t now)
{
    if (m_failed) return false;

    for (;;) {
        // Drop bytes already consumed so the buffer does not grow without
        // bound over a long-lived connection.
        if (m_recv_pos > 0) {
            m_recv.erase(m_recv.begin(), m_recv.begin() + m_recv_pos);
            m_recv_pos = 0;
        }

        if (m_phase == Phase::HANDSHAKE) {
            const size_t want{m_initiator ? HANDSHAKE_MSG2_SIZE : HANDSHAKE_MSG1_SIZE};
            if (m_recv.size() < want) return true;

            if (m_initiator) {
                if (!m_handshake.ReadMsg2(std::span{m_recv}.first(want), now)) {
                    m_failed = true;
                    return false;
                }
            } else {
                if (!m_handshake.ReadMsg1(std::span{m_recv}.first(want))) {
                    m_failed = true;
                    return false;
                }
                // The reply is the responder's only handshake output.
                const size_t at{m_send.size()};
                m_send.resize(at + HANDSHAKE_MSG2_SIZE);
                if (!m_handshake.WriteMsg2(std::span{m_send}.subspan(at, HANDSHAKE_MSG2_SIZE))) {
                    m_failed = true;
                    return false;
                }
            }
            m_recv_pos = want;
            m_handshake.Finalize(m_send_cipher, m_recv_cipher);
            m_phase = Phase::TRANSPORT;
            continue;
        }

        bool progress{false};
        if (!ReadFrame(progress)) {
            m_failed = true;
            return false;
        }
        if (!progress) return true;
    }
}

bool Transport::ReadFrame(bool& progress)
{
    progress = false;

    // Decrypt the header, unless an earlier pass already did and was left
    // waiting for the body.
    if (!m_pending.has_value()) {
        if (m_recv.size() - m_recv_pos < ENCRYPTED_HEADER_SIZE) return true;

        std::byte header_plain[HEADER_SIZE];
        if (!m_recv_cipher.Decrypt(std::span{m_recv}.subspan(m_recv_pos, ENCRYPTED_HEADER_SIZE), {}, header_plain)) {
            return false;
        }
        m_recv_pos += ENCRYPTED_HEADER_SIZE;

        NetHeader header;
        try {
            SpanReader reader{std::span<const std::byte>{header_plain, HEADER_SIZE}};
            reader >> header;
        } catch (const std::ios_base::failure&) {
            return false;
        }
        if (header.msg_length > MAX_PAYLOAD) return false;

        m_pending = PendingFrame{header, CiphertextLen(header.msg_length)};
    }

    if (m_recv.size() - m_recv_pos < m_pending->cipher_len) return true;

    ReceivedMsg msg;
    msg.header = m_pending->header;
    msg.payload.resize(msg.header.msg_length);

    size_t offset{m_recv_pos};
    size_t written{0};
    while (written < msg.header.msg_length) {
        const size_t chunk{std::min<size_t>(msg.header.msg_length - written, MAX_PLAINTEXT_CHUNK)};
        auto out{std::span{msg.payload}.subspan(written, chunk)};
        if (!m_recv_cipher.Decrypt(std::span{m_recv}.subspan(offset, chunk + MAC_SIZE), {},
                                   std::as_writable_bytes(out))) {
            return false;
        }
        offset += chunk + MAC_SIZE;
        written += chunk;
    }

    m_recv_pos = offset;
    m_pending.reset();
    m_ready.push_back(std::move(msg));
    progress = true;
    return true;
}

std::optional<ReceivedMsg> Transport::Next()
{
    if (m_ready_pos >= m_ready.size()) {
        m_ready.clear();
        m_ready_pos = 0;
        return std::nullopt;
    }
    return std::move(m_ready[m_ready_pos++]);
}

bool Transport::Send(const NetHeader& header, std::span<const uint8_t> payload)
{
    if (m_failed || m_phase != Phase::TRANSPORT) return false;
    if (payload.size() > MAX_PAYLOAD) return false;

    NetHeader out_header{header};
    out_header.msg_length = static_cast<uint32_t>(payload.size());

    DataStream header_bytes{};
    header_bytes << out_header;
    assert(header_bytes.size() == HEADER_SIZE);

    const size_t at{m_send.size()};
    m_send.resize(at + ENCRYPTED_HEADER_SIZE + CiphertextLen(payload.size()));

    size_t offset{at};
    m_send_cipher.Encrypt(std::span{header_bytes.data(), header_bytes.size()}, {},
                          std::span{m_send}.subspan(offset, ENCRYPTED_HEADER_SIZE));
    offset += ENCRYPTED_HEADER_SIZE;

    size_t sent{0};
    while (sent < payload.size()) {
        const size_t chunk{std::min<size_t>(payload.size() - sent, MAX_PLAINTEXT_CHUNK)};
        m_send_cipher.Encrypt(std::as_bytes(payload.subspan(sent, chunk)), {},
                              std::span{m_send}.subspan(offset, chunk + MAC_SIZE));
        offset += chunk + MAC_SIZE;
        sent += chunk;
    }
    assert(offset == m_send.size());
    return true;
}

std::span<const std::byte> Transport::SendBuffer() const
{
    return std::span{m_send}.subspan(m_send_pos);
}

void Transport::Sent(size_t count)
{
    assert(count <= m_send.size() - m_send_pos);
    m_send_pos += count;
    if (m_send_pos >= m_send.size()) {
        m_send.clear();
        m_send_pos = 0;
    } else if (m_send_pos > MAX_CIPHERTEXT_CHUNK) {
        m_send.erase(m_send.begin(), m_send.begin() + m_send_pos);
        m_send_pos = 0;
    }
}

} // namespace sv2
