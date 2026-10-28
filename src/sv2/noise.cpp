// Copyright (c) 2026 The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <sv2/noise.h>

#include <crypto/hmac_sha256.h>
#include <crypto/sha256.h>
#include <hash.h>
#include <random.h>
#include <span.h>
#include <streams.h>
#include <support/cleanse.h>

#include <array>
#include <cassert>

namespace sv2 {

namespace {
//! ElligatorSwift-encode a key with fresh entropy. The encoding is randomized,
//! so the same key yields a different 64-byte encoding each time.
EllSwiftPubKey EllSwiftEncode(const CKey& key)
{
    std::array<unsigned char, 32> entropy;
    GetRandBytes(entropy);
    return key.EllSwiftCreate(std::as_bytes(std::span{entropy}));
}
} // namespace

void Hkdf2(std::span<const std::byte> salt, std::span<const std::byte> ikm, uint256& out1, uint256& out2)
{
    // Extract: temp = HMAC(salt, ikm). ikm is empty for the final split, and
    // an empty span's data() may be null, so skip the write in that case.
    unsigned char temp[CSHA256::OUTPUT_SIZE];
    {
        CHMAC_SHA256 extract{UCharCast(salt.data()), salt.size()};
        if (!ikm.empty()) extract.Write(UCharCast(ikm.data()), ikm.size());
        extract.Finalize(temp);
    }

    // Expand with an empty info field, so the counter byte is the whole input
    // for out1 and out1 || counter for out2.
    const unsigned char one{1};
    CHMAC_SHA256{temp, sizeof(temp)}.Write(&one, 1).Finalize(out1.data());

    const unsigned char two{2};
    CHMAC_SHA256{temp, sizeof(temp)}.Write(out1.data(), out1.size()).Write(&two, 1).Finalize(out2.data());

    memory_cleanse(temp, sizeof(temp));
}

uint256 SignatureNoiseMessage::Digest(const XOnlyPubKey& static_key) const
{
    // m = SHA256(version || valid_from || not_valid_after || server_public_key),
    // with the integers little-endian as everywhere else in Sv2. Note this is a
    // single SHA256, not Bitcoin's usual double hash.
    HashWriter writer{};
    writer << version << valid_from << not_valid_after << static_key;
    return writer.GetSHA256();
}

bool SignatureNoiseMessage::Sign(const CKey& authority, const XOnlyPubKey& static_key)
{
    return authority.SignSchnorr(Digest(static_key), signature, /*merkle_root=*/nullptr, GetRandHash());
}

bool SignatureNoiseMessage::Verify(const XOnlyPubKey& static_key, const XOnlyPubKey& authority, int64_t now) const
{
    // Reject a version we do not understand rather than assuming the fields
    // mean what they mean today.
    if (version != 0) return false;
    if (now < static_cast<int64_t>(valid_from)) return false;
    if (now > static_cast<int64_t>(not_valid_after)) return false;
    return authority.VerifySchnorr(Digest(static_key), signature);
}

void CipherState::SetKey(const uint256& key)
{
    m_aead.emplace(std::as_bytes(std::span{key.data(), key.size()}));
    m_nonce = 0;
}

void CipherState::Encrypt(std::span<const std::byte> plain, std::span<const std::byte> aad, std::span<std::byte> cipher)
{
    assert(m_aead.has_value());
    assert(cipher.size() == plain.size() + MAC_SIZE);
    // Four zero bytes then the counter little-endian, which is what
    // Nonce96{0, counter} serializes to.
    m_aead->Encrypt(plain, aad, AEADChaCha20Poly1305::Nonce96{0, m_nonce}, cipher);
    ++m_nonce;
}

bool CipherState::Decrypt(std::span<const std::byte> cipher, std::span<const std::byte> aad, std::span<std::byte> plain)
{
    assert(m_aead.has_value());
    assert(cipher.size() == plain.size() + MAC_SIZE);
    const bool ok{m_aead->Decrypt(cipher, aad, AEADChaCha20Poly1305::Nonce96{0, m_nonce}, plain)};
    // Advance even on failure. A forged frame consumes its nonce, so a peer
    // cannot probe the same counter repeatedly.
    ++m_nonce;
    return ok;
}

SymmetricState::SymmetricState()
{
    // The protocol name is longer than 32 bytes, so h starts as its hash
    // rather than the zero-padded name.
    CSHA256 sha{};
    sha.Write(reinterpret_cast<const unsigned char*>(NOISE_PROTOCOL_NAME.data()), NOISE_PROTOCOL_NAME.size());
    sha.Finalize(m_h.data());

    m_ck = m_h;

    // MixHash over an empty prologue, which reduces to hashing h again.
    MixHash({});
}

void SymmetricState::MixHash(std::span<const std::byte> data)
{
    CSHA256 sha{};
    sha.Write(m_h.data(), m_h.size());
    if (!data.empty()) sha.Write(UCharCast(data.data()), data.size());
    sha.Finalize(m_h.data());
}

void SymmetricState::MixKey(std::span<const std::byte> ikm)
{
    uint256 new_ck;
    uint256 temp_k;
    Hkdf2(std::as_bytes(std::span{m_ck.data(), m_ck.size()}), ikm, new_ck, temp_k);
    m_ck = new_ck;
    m_cipher.SetKey(temp_k);
}

void SymmetricState::EncryptAndHash(std::span<const std::byte> plain, std::span<std::byte> cipher)
{
    if (!m_cipher.HasKey()) {
        // Noise degrades EncryptAndHash to MixHash before a key exists. No Sv2
        // handshake message needs this, but keeping the rule here means the
        // state machine stays correct if one is added.
        assert(cipher.size() == plain.size());
        std::copy(plain.begin(), plain.end(), cipher.begin());
        MixHash(cipher);
        return;
    }
    m_cipher.Encrypt(plain, std::as_bytes(std::span{m_h.data(), m_h.size()}), cipher);
    MixHash(cipher);
}

bool SymmetricState::DecryptAndHash(std::span<const std::byte> cipher, std::span<std::byte> plain)
{
    if (!m_cipher.HasKey()) {
        assert(cipher.size() == plain.size());
        std::copy(cipher.begin(), cipher.end(), plain.begin());
        MixHash(cipher);
        return true;
    }
    // h is the associated data, so it must be captured before the ciphertext
    // is mixed in.
    const uint256 aad{m_h};
    const bool ok{m_cipher.Decrypt(cipher, std::as_bytes(std::span{aad.data(), aad.size()}), plain)};
    MixHash(cipher);
    return ok;
}

void SymmetricState::Split(CipherState& c1, CipherState& c2)
{
    uint256 k1;
    uint256 k2;
    Hkdf2(std::as_bytes(std::span{m_ck.data(), m_ck.size()}), {}, k1, k2);
    c1.SetKey(k1);
    c2.SetKey(k2);
}

void GenerateEphemeral(CKey& key, EllSwiftPubKey& pubkey)
{
    key.MakeNewKey(/*fCompressedIn=*/true);
    pubkey = EllSwiftEncode(key);
}

HandshakeState HandshakeState::Responder(const CKey& static_key, const SignatureNoiseMessage& cert)
{
    HandshakeState state;
    state.m_initiator = false;
    state.m_static = static_key;
    state.m_static_pub = EllSwiftEncode(static_key);
    state.m_cert = cert;
    return state;
}

HandshakeState HandshakeState::Initiator(const std::optional<XOnlyPubKey>& authority)
{
    HandshakeState state;
    state.m_initiator = true;
    state.m_authority = authority;
    return state;
}

void HandshakeState::WriteMsg1(std::span<std::byte> out)
{
    assert(m_initiator);
    assert(out.size() == HANDSHAKE_MSG1_SIZE);

    GenerateEphemeral(m_ephemeral, m_ephemeral_pub);
    std::copy(m_ephemeral_pub.begin(), m_ephemeral_pub.end(), out.begin());
    m_state.MixHash(out);

    // Act 1 step 4: EncryptAndHash over an empty payload. No key exists yet,
    // so it writes nothing to the wire and degrades to MixHash on empty data,
    // which is why message 1 is still 64 bytes. It is still part of the
    // transcript: a peer that skips it derives an h one SHA256 iteration
    // apart, and since h is the associated data for message 2, the only
    // symptom is a MAC failure with no diagnostic.
    m_state.EncryptAndHash({}, {});
}

bool HandshakeState::ReadMsg1(std::span<const std::byte> in)
{
    assert(!m_initiator);
    if (in.size() != HANDSHAKE_MSG1_SIZE) return false;

    m_remote_ephemeral_pub = EllSwiftPubKey{in};
    m_state.MixHash(in);
    // The counterpart of act 1 step 4, over the empty remainder of the
    // message. Cannot fail without a key, but kept in this form so the
    // transcript reads as the specification states it.
    if (!m_state.DecryptAndHash({}, {})) return false;
    return true;
}

bool HandshakeState::WriteMsg2(std::span<std::byte> out)
{
    assert(!m_initiator);
    if (out.size() != HANDSHAKE_MSG2_SIZE) return false;

    // e: our ephemeral key, in the clear.
    GenerateEphemeral(m_ephemeral, m_ephemeral_pub);
    auto cursor{out};
    std::copy(m_ephemeral_pub.begin(), m_ephemeral_pub.end(), cursor.begin());
    m_state.MixHash(cursor.first(ELLSWIFT_SIZE));
    cursor = cursor.subspan(ELLSWIFT_SIZE);

    // ee: both ephemerals. The role flag normalizes the key order, so each
    // side derives the same secret.
    const auto ee{m_ephemeral.ComputeBIP324ECDHSecret(m_remote_ephemeral_pub, m_ephemeral_pub, /*initiating=*/false)};
    m_state.MixKey(ee);

    // s: our static key, encrypted under the key just derived.
    m_state.EncryptAndHash(std::as_bytes(std::span{m_static_pub.data(), m_static_pub.size()}),
                           cursor.first(ELLSWIFT_SIZE + MAC_SIZE));
    cursor = cursor.subspan(ELLSWIFT_SIZE + MAC_SIZE);

    // es: our static key with their ephemeral.
    const auto es{m_static.ComputeBIP324ECDHSecret(m_remote_ephemeral_pub, m_static_pub, /*initiating=*/false)};
    m_state.MixKey(es);

    // The certificate travels as this message's payload.
    DataStream cert_bytes{};
    cert_bytes << m_cert;
    if (cert_bytes.size() != CERT_SIZE) return false;
    m_state.EncryptAndHash(cert_bytes, cursor.first(CERT_SIZE + MAC_SIZE));
    return true;
}

bool HandshakeState::ReadMsg2(std::span<const std::byte> in, int64_t now)
{
    assert(m_initiator);
    if (in.size() != HANDSHAKE_MSG2_SIZE) return false;

    auto cursor{in};
    const EllSwiftPubKey their_ephemeral{cursor.first(ELLSWIFT_SIZE)};
    m_state.MixHash(cursor.first(ELLSWIFT_SIZE));
    cursor = cursor.subspan(ELLSWIFT_SIZE);

    const auto ee{m_ephemeral.ComputeBIP324ECDHSecret(their_ephemeral, m_ephemeral_pub, /*initiating=*/true)};
    m_state.MixKey(ee);

    std::array<std::byte, ELLSWIFT_SIZE> their_static_bytes;
    if (!m_state.DecryptAndHash(cursor.first(ELLSWIFT_SIZE + MAC_SIZE), their_static_bytes)) return false;
    cursor = cursor.subspan(ELLSWIFT_SIZE + MAC_SIZE);
    const EllSwiftPubKey their_static{their_static_bytes};

    const auto es{m_ephemeral.ComputeBIP324ECDHSecret(their_static, m_ephemeral_pub, /*initiating=*/true)};
    m_state.MixKey(es);

    std::array<std::byte, CERT_SIZE> cert_bytes;
    if (!m_state.DecryptAndHash(cursor.first(CERT_SIZE + MAC_SIZE), cert_bytes)) return false;

    SignatureNoiseMessage cert;
    try {
        SpanReader reader{std::span<const std::byte>{cert_bytes}};
        reader >> cert;
        if (!reader.empty()) return false;
    } catch (const std::ios_base::failure&) {
        return false;
    }

    // The certificate signs the x-only form of the static key, so recover it
    // from the ElligatorSwift encoding we just decrypted.
    const CPubKey decoded{their_static.Decode()};
    if (!decoded.IsValid()) return false;
    const XOnlyPubKey their_static_xonly{decoded};

    // Without an authority key this is trust on first use: the server is
    // authenticated only by the handshake, not by the pool. The caller decides
    // whether that is acceptable.
    if (m_authority.has_value() && !cert.Verify(their_static_xonly, *m_authority, now)) return false;

    m_remote_static = their_static_xonly;
    return true;
}

void HandshakeState::Finalize(CipherState& send, CipherState& recv)
{
    CipherState c1;
    CipherState c2;
    m_state.Split(c1, c2);
    // c1 carries initiator to responder traffic, c2 the reverse.
    if (m_initiator) {
        send = c1;
        recv = c2;
    } else {
        send = c2;
        recv = c1;
    }
}

} // namespace sv2
