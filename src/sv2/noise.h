// Copyright (c) 2026 The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_SV2_NOISE_H
#define BITCOIN_SV2_NOISE_H

#include <crypto/chacha20poly1305.h>
#include <key.h>
#include <pubkey.h>
#include <sv2/messages.h>
#include <sv2/types.h>
#include <uint256.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

//! The Sv2 Noise handshake and encrypted transport (sv2-spec 4).
//!
//! Sv2 uses Noise_NX: the client does not authenticate, and the server proves
//! its identity with a certificate signed by a pool authority key. The key
//! exchange is X-only ElligatorSwift ECDH on secp256k1, which is the same
//! v2_ecdh that BIP324 defines, down to the tag and the role-dependent key
//! ordering. CKey::ComputeBIP324ECDHSecret is therefore reusable verbatim and
//! this file does no elliptic curve arithmetic of its own.
//!
//! What is *not* shared with BIP324 is everything above the ECDH. BIP324
//! derives its keys with its own labels and rekeys with FSChaCha20Poly1305;
//! Noise mixes a chaining key and a transcript hash and uses a plain AEAD with
//! a message counter. So src/bip324.cpp is the structural model here, not a
//! component to call.
namespace sv2 {

//! Cipher suite name, hashed into the initial handshake state.
//!
//! Every byte of this string, capitalization included, is part of the wire
//! contract: one wrong character produces a different transcript hash and the
//! handshake fails with no useful diagnostic. It is 45 bytes, which is why the
//! spec initializes h as HASH(name) instead of zero-padding it.
inline constexpr std::string_view NOISE_PROTOCOL_NAME{"Noise_NX_Secp256k1+EllSwift_ChaChaPoly_SHA256"};
static_assert(NOISE_PROTOCOL_NAME.size() == 45, "protocol name must exceed 32 bytes, so h = HASH(name)");

static constexpr size_t MAC_SIZE{16};
static constexpr size_t ELLSWIFT_SIZE{64};
static constexpr size_t XONLY_SIZE{32};
static constexpr size_t SIGNATURE_SIZE{64};

//! SIGNATURE_NOISE_MESSAGE: version, valid_from, not_valid_after, signature.
static constexpr size_t CERT_SIZE{2 + 4 + 4 + SIGNATURE_SIZE};
static_assert(CERT_SIZE == 74);

//! Handshake message sizes (sv2-spec 4.5.2). Message 1 is a bare ephemeral
//! key; message 2 adds the encrypted static key and the encrypted certificate.
static constexpr size_t HANDSHAKE_MSG1_SIZE{ELLSWIFT_SIZE};
static constexpr size_t HANDSHAKE_MSG2_SIZE{ELLSWIFT_SIZE + (ELLSWIFT_SIZE + MAC_SIZE) + (CERT_SIZE + MAC_SIZE)};
static_assert(HANDSHAKE_MSG1_SIZE == 64);
static_assert(HANDSHAKE_MSG2_SIZE == 234);

//! After the handshake the six-byte frame header is encrypted, so it occupies
//! 22 bytes on the wire.
static constexpr size_t ENCRYPTED_HEADER_SIZE{HEADER_SIZE + MAC_SIZE};
static_assert(ENCRYPTED_HEADER_SIZE == 22);

//! Payloads are split into chunks of at most this many plaintext bytes; each
//! gains a MAC, so no ciphertext chunk exceeds the 65535-byte Noise limit.
static constexpr size_t MAX_PLAINTEXT_CHUNK{65519};
static constexpr size_t MAX_CIPHERTEXT_CHUNK{MAX_PLAINTEXT_CHUNK + MAC_SIZE};
static_assert(MAX_CIPHERTEXT_CHUNK == 65535);

//! RFC5869 HKDF-SHA256 with an empty info field, yielding the two 32-byte
//! outputs Noise asks for (sv2-spec 4.5.1).
//!
//! Written out rather than using CHKDF_HMAC_SHA256_L32, which produces only
//! one output and takes its salt as a std::string, neither of which fits a
//! binary chaining key.
void Hkdf2(std::span<const std::byte> salt, std::span<const std::byte> ikm, uint256& out1, uint256& out2);

//! The payload of handshake message 2: a certificate attesting that the
//! server's static key is endorsed by the pool authority key.
struct SignatureNoiseMessage {
    uint16_t version{0};
    uint32_t valid_from{0};
    uint32_t not_valid_after{0};
    Signature signature{};

    SERIALIZE_METHODS(SignatureNoiseMessage, obj)
    {
        READWRITE(obj.version, obj.valid_from, obj.not_valid_after, obj.signature);
    }

    //! The 32-byte digest the authority key signs. The server's static key is
    //! covered by the signature but is not carried in this message, because
    //! the peer already learned it from the handshake.
    uint256 Digest(const XOnlyPubKey& static_key) const;

    //! Sign this certificate with the pool authority key.
    [[nodiscard]] bool Sign(const CKey& authority, const XOnlyPubKey& static_key);

    //! Verify the signature and that `now` falls inside the validity window.
    [[nodiscard]] bool Verify(const XOnlyPubKey& static_key, const XOnlyPubKey& authority, int64_t now) const;
};

//! Noise CipherState: a key plus a message counter.
//!
//! The nonce is four zero bytes followed by the counter little-endian, which
//! is exactly what Nonce96{0, counter} produces.
class CipherState
{
public:
    CipherState() = default;

    void SetKey(const uint256& key);
    bool HasKey() const { return m_aead.has_value(); }

    //! Encrypt with the current nonce, then advance it. cipher must be
    //! plain.size() + MAC_SIZE bytes.
    void Encrypt(std::span<const std::byte> plain, std::span<const std::byte> aad, std::span<std::byte> cipher);

    //! Decrypt and advance the nonce. Returns false on a MAC failure, in which
    //! case the nonce is still advanced: a forged frame must not be retryable.
    [[nodiscard]] bool Decrypt(std::span<const std::byte> cipher, std::span<const std::byte> aad, std::span<std::byte> plain);

private:
    std::optional<AEADChaCha20Poly1305> m_aead;
    uint64_t m_nonce{0};
};

//! Noise SymmetricState: the transcript hash h, the chaining key ck, and the
//! handshake cipher.
class SymmetricState
{
public:
    SymmetricState();

    void MixHash(std::span<const std::byte> data);
    void MixKey(std::span<const std::byte> ikm);

    //! Encrypt with h as associated data, then mix the ciphertext into h.
    //! Before any key is established this degrades to MixHash, as Noise
    //! specifies.
    void EncryptAndHash(std::span<const std::byte> plain, std::span<std::byte> cipher);
    [[nodiscard]] bool DecryptAndHash(std::span<const std::byte> cipher, std::span<std::byte> plain);

    //! Derive the two transport ciphers. c1 carries initiator to responder
    //! traffic and c2 the reverse.
    void Split(CipherState& c1, CipherState& c2);

    const uint256& Hash() const { return m_h; }

private:
    uint256 m_h;
    uint256 m_ck;
    CipherState m_cipher;
};

//! Drives the NX handshake for either role.
//!
//!   -> e                 (64 bytes)
//!   <- e, ee, s, es      (234 bytes)
//!
//! Both roles are implemented, even though a Template Provider is only ever
//! the responder, so that a unit test can run a complete handshake against
//! itself and so the transport can be exercised without a network.
class HandshakeState
{
public:
    //! Responder. `static_key` is the server's long-term key and `cert` the
    //! authority-signed certificate for it.
    static HandshakeState Responder(const CKey& static_key, const SignatureNoiseMessage& cert);

    //! Initiator. `authority` is the pool authority key the certificate is
    //! checked against. If absent, the certificate signature is not verified,
    //! which is trust-on-first-use and must be an explicit choice.
    static HandshakeState Initiator(const std::optional<XOnlyPubKey>& authority);

    //! Initiator: write message 1. Output must be HANDSHAKE_MSG1_SIZE bytes.
    void WriteMsg1(std::span<std::byte> out);

    //! Responder: read message 1.
    [[nodiscard]] bool ReadMsg1(std::span<const std::byte> in);

    //! Responder: write message 2. Output must be HANDSHAKE_MSG2_SIZE bytes.
    [[nodiscard]] bool WriteMsg2(std::span<std::byte> out);

    //! Initiator: read message 2, which also verifies the server certificate
    //! against the authority key and `now`.
    [[nodiscard]] bool ReadMsg2(std::span<const std::byte> in, int64_t now);

    //! Derive the transport ciphers for this role: send is what we encrypt
    //! with, recv what we decrypt with.
    void Finalize(CipherState& send, CipherState& recv);

    //! The server static key, as learned by the initiator from message 2.
    const std::optional<XOnlyPubKey>& RemoteStaticKey() const { return m_remote_static; }

private:
    HandshakeState() = default;

    bool m_initiator{false};
    SymmetricState m_state;

    CKey m_ephemeral;
    EllSwiftPubKey m_ephemeral_pub;

    //! Responder only.
    CKey m_static;
    EllSwiftPubKey m_static_pub;
    SignatureNoiseMessage m_cert;

    //! Initiator only.
    std::optional<XOnlyPubKey> m_authority;
    std::optional<XOnlyPubKey> m_remote_static;
    EllSwiftPubKey m_remote_ephemeral_pub;
};

//! Generate a fresh ephemeral key and its ElligatorSwift encoding.
void GenerateEphemeral(CKey& key, EllSwiftPubKey& pubkey);

} // namespace sv2

#endif // BITCOIN_SV2_NOISE_H
