// Copyright (c) 2026 The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <crypto/sha256.h>
#include <key.h>
#include <pubkey.h>
#include <sv2/noise.h>
#include <test/util/setup_common.h>
#include <uint256.h>

#include <array>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <boost/test/unit_test.hpp>

//! Sv2 uses Noise_NX: the client does not authenticate, and the server proves
//! its identity with a certificate signed by a pool authority key.
//!
//! The key exchange is the same v2_ecdh that BIP324 defines, so
//! CKey::ComputeBIP324ECDHSecret is reused directly rather than reimplemented.
//! These tests exist to prove that reuse is actually correct: that both roles
//! derive the same transport keys, and that the transcript hash matches what a
//! peer computing it independently would get.
BOOST_FIXTURE_TEST_SUITE(sv2_noise_tests, BasicTestingSetup)

namespace {
//! A server static key plus a certificate for it, signed by `authority`.
struct TestServer {
    CKey static_key;
    XOnlyPubKey static_pub;
    sv2::SignatureNoiseMessage cert;
};

TestServer MakeServer(const CKey& authority, uint32_t valid_from = 1000, uint32_t not_valid_after = 2000,
                      uint16_t version = 0)
{
    TestServer server;
    server.static_key.MakeNewKey(/*fCompressedIn=*/true);
    server.static_pub = XOnlyPubKey{server.static_key.GetPubKey()};
    server.cert.version = version;
    server.cert.valid_from = valid_from;
    server.cert.not_valid_after = not_valid_after;
    BOOST_REQUIRE(server.cert.Sign(authority, server.static_pub));
    return server;
}

//! Run a complete handshake, returning false if either side rejects it.
bool Handshake(sv2::HandshakeState& initiator, sv2::HandshakeState& responder, int64_t now)
{
    std::vector<std::byte> msg1(sv2::HANDSHAKE_MSG1_SIZE);
    initiator.WriteMsg1(msg1);
    if (!responder.ReadMsg1(msg1)) return false;

    std::vector<std::byte> msg2(sv2::HANDSHAKE_MSG2_SIZE);
    if (!responder.WriteMsg2(msg2)) return false;
    return initiator.ReadMsg2(msg2, now);
}

std::vector<std::byte> ToBytes(const std::string& s)
{
    std::vector<std::byte> out(s.size());
    if (!s.empty()) std::memcpy(out.data(), s.data(), s.size());
    return out;
}
} // namespace

BOOST_AUTO_TEST_CASE(protocol_name_is_pinned)
{
    // The cipher suite name is hashed into the initial state, so every byte of
    // it, capitalization included, is part of the wire contract. One wrong
    // character yields a different transcript hash and the handshake fails
    // with no useful diagnostic, so pin it by its hash rather than trusting
    // the literal to stay correct through edits.
    BOOST_CHECK_EQUAL(sv2::NOISE_PROTOCOL_NAME, "Noise_NX_Secp256k1+EllSwift_ChaChaPoly_SHA256");
    BOOST_CHECK_EQUAL(sv2::NOISE_PROTOCOL_NAME.size(), 45u);

    uint256 name_hash;
    CSHA256 sha{};
    sha.Write(reinterpret_cast<const unsigned char*>(sv2::NOISE_PROTOCOL_NAME.data()), sv2::NOISE_PROTOCOL_NAME.size());
    sha.Finalize(name_hash.data());
    // The initial chaining key is SHA256(name).
    BOOST_CHECK_EQUAL(name_hash.GetHex(), "10bf293ec84bdc30e83f500d0988eaa90ee76ec6679f661fee9e8e208178b42e");

    // h starts as SHA256(name) and is then mixed with an empty prologue, which
    // reduces to hashing it again.
    const sv2::SymmetricState state{};
    BOOST_CHECK_EQUAL(state.Hash().GetHex(), "0461dc33d43dd2f27c3bbd3de1b78dd9e39c89d7bea3ca590d7c484f2ea32f92");
}

BOOST_AUTO_TEST_CASE(message_sizes_match_spec)
{
    BOOST_CHECK_EQUAL(sv2::HANDSHAKE_MSG1_SIZE, 64u);
    BOOST_CHECK_EQUAL(sv2::HANDSHAKE_MSG2_SIZE, 234u);
    BOOST_CHECK_EQUAL(sv2::CERT_SIZE, 74u);
    BOOST_CHECK_EQUAL(sv2::ENCRYPTED_HEADER_SIZE, 22u);
    BOOST_CHECK_EQUAL(sv2::MAX_PLAINTEXT_CHUNK, 65519u);
    BOOST_CHECK_EQUAL(sv2::MAX_CIPHERTEXT_CHUNK, 65535u);
}

BOOST_AUTO_TEST_CASE(certificate_sign_and_verify)
{
    CKey authority;
    authority.MakeNewKey(/*fCompressedIn=*/true);
    const XOnlyPubKey authority_pub{authority.GetPubKey()};
    const auto server{MakeServer(authority)};

    BOOST_CHECK(server.cert.Verify(server.static_pub, authority_pub, 1500));
    // Validity window is inclusive at both ends.
    BOOST_CHECK(server.cert.Verify(server.static_pub, authority_pub, 1000));
    BOOST_CHECK(server.cert.Verify(server.static_pub, authority_pub, 2000));
    BOOST_CHECK(!server.cert.Verify(server.static_pub, authority_pub, 999));
    BOOST_CHECK(!server.cert.Verify(server.static_pub, authority_pub, 2001));

    // A different authority key must not validate.
    CKey other;
    other.MakeNewKey(/*fCompressedIn=*/true);
    BOOST_CHECK(!server.cert.Verify(server.static_pub, XOnlyPubKey{other.GetPubKey()}, 1500));

    // The signature covers the static key, so it must not validate for another.
    CKey other_static;
    other_static.MakeNewKey(/*fCompressedIn=*/true);
    BOOST_CHECK(!server.cert.Verify(XOnlyPubKey{other_static.GetPubKey()}, authority_pub, 1500));

    // An unknown certificate version is rejected rather than interpreted.
    const auto future{MakeServer(authority, 1000, 2000, /*version=*/1)};
    BOOST_CHECK(!future.cert.Verify(future.static_pub, authority_pub, 1500));
}

BOOST_AUTO_TEST_CASE(handshake_derives_matching_transport_keys)
{
    CKey authority;
    authority.MakeNewKey(/*fCompressedIn=*/true);
    const XOnlyPubKey authority_pub{authority.GetPubKey()};
    const auto server{MakeServer(authority)};

    auto initiator{sv2::HandshakeState::Initiator(authority_pub)};
    auto responder{sv2::HandshakeState::Responder(server.static_key, server.cert)};
    BOOST_REQUIRE(Handshake(initiator, responder, 1500));

    // The initiator learns the server's static key from the handshake, and it
    // must be exactly the key the certificate was signed over.
    BOOST_REQUIRE(initiator.RemoteStaticKey().has_value());
    BOOST_CHECK(*initiator.RemoteStaticKey() == server.static_pub);

    sv2::CipherState i_send, i_recv, r_send, r_recv;
    initiator.Finalize(i_send, i_recv);
    responder.Finalize(r_send, r_recv);
    BOOST_REQUIRE(i_send.HasKey());
    BOOST_REQUIRE(r_recv.HasKey());

    // Several messages in each direction, to exercise nonce sequencing rather
    // than just the first frame.
    for (int i = 0; i < 3; ++i) {
        const auto plain{ToBytes("initiator frame " + std::to_string(i))};
        std::vector<std::byte> cipher(plain.size() + sv2::MAC_SIZE);
        i_send.Encrypt(plain, {}, cipher);
        std::vector<std::byte> out(plain.size());
        BOOST_REQUIRE(r_recv.Decrypt(cipher, {}, out));
        BOOST_CHECK(out == plain);
    }

    for (int i = 0; i < 3; ++i) {
        const auto plain{ToBytes("responder frame " + std::to_string(i))};
        std::vector<std::byte> cipher(plain.size() + sv2::MAC_SIZE);
        r_send.Encrypt(plain, {}, cipher);
        std::vector<std::byte> out(plain.size());
        BOOST_REQUIRE(i_recv.Decrypt(cipher, {}, out));
        BOOST_CHECK(out == plain);
    }
}

BOOST_AUTO_TEST_CASE(transport_directions_are_separate)
{
    // c1 carries initiator to responder traffic and c2 the reverse, so a frame
    // sent by the initiator must not decrypt with the initiator's own receive
    // cipher.
    CKey authority;
    authority.MakeNewKey(/*fCompressedIn=*/true);
    const auto server{MakeServer(authority)};

    auto initiator{sv2::HandshakeState::Initiator(XOnlyPubKey{authority.GetPubKey()})};
    auto responder{sv2::HandshakeState::Responder(server.static_key, server.cert)};
    BOOST_REQUIRE(Handshake(initiator, responder, 1500));

    sv2::CipherState i_send, i_recv, r_send, r_recv;
    initiator.Finalize(i_send, i_recv);
    responder.Finalize(r_send, r_recv);

    const auto plain{ToBytes("one way only")};
    std::vector<std::byte> cipher(plain.size() + sv2::MAC_SIZE);
    i_send.Encrypt(plain, {}, cipher);
    std::vector<std::byte> out(plain.size());
    BOOST_CHECK(!i_recv.Decrypt(cipher, {}, out));
}

BOOST_AUTO_TEST_CASE(tampered_frame_is_rejected)
{
    CKey authority;
    authority.MakeNewKey(/*fCompressedIn=*/true);
    const auto server{MakeServer(authority)};

    auto initiator{sv2::HandshakeState::Initiator(XOnlyPubKey{authority.GetPubKey()})};
    auto responder{sv2::HandshakeState::Responder(server.static_key, server.cert)};
    BOOST_REQUIRE(Handshake(initiator, responder, 1500));

    sv2::CipherState i_send, i_recv, r_send, r_recv;
    initiator.Finalize(i_send, i_recv);
    responder.Finalize(r_send, r_recv);

    const auto plain{ToBytes("authentic")};
    std::vector<std::byte> cipher(plain.size() + sv2::MAC_SIZE);
    i_send.Encrypt(plain, {}, cipher);

    // Flipping a ciphertext bit must fail the MAC.
    auto corrupt_body{cipher};
    corrupt_body[0] ^= std::byte{0x01};
    std::vector<std::byte> out(plain.size());
    BOOST_CHECK(!r_recv.Decrypt(corrupt_body, {}, out));

    // So must flipping a bit of the tag. Note the receiver's nonce advanced on
    // the failure above, so this uses a fresh pair rather than the same one.
    auto initiator2{sv2::HandshakeState::Initiator(XOnlyPubKey{authority.GetPubKey()})};
    auto responder2{sv2::HandshakeState::Responder(server.static_key, server.cert)};
    BOOST_REQUIRE(Handshake(initiator2, responder2, 1500));
    sv2::CipherState i2_send, i2_recv, r2_send, r2_recv;
    initiator2.Finalize(i2_send, i2_recv);
    responder2.Finalize(r2_send, r2_recv);

    std::vector<std::byte> cipher2(plain.size() + sv2::MAC_SIZE);
    i2_send.Encrypt(plain, {}, cipher2);
    cipher2.back() ^= std::byte{0x80};
    BOOST_CHECK(!r2_recv.Decrypt(cipher2, {}, out));
}

BOOST_AUTO_TEST_CASE(handshake_rejects_bad_certificate)
{
    CKey authority;
    authority.MakeNewKey(/*fCompressedIn=*/true);
    const XOnlyPubKey authority_pub{authority.GetPubKey()};

    // Signed by a key that is not the configured authority.
    CKey impostor;
    impostor.MakeNewKey(/*fCompressedIn=*/true);
    const auto rogue{MakeServer(impostor)};

    auto initiator{sv2::HandshakeState::Initiator(authority_pub)};
    auto responder{sv2::HandshakeState::Responder(rogue.static_key, rogue.cert)};
    BOOST_CHECK(!Handshake(initiator, responder, 1500));

    // A genuine certificate presented outside its validity window.
    const auto server{MakeServer(authority)};
    auto initiator2{sv2::HandshakeState::Initiator(authority_pub)};
    auto responder2{sv2::HandshakeState::Responder(server.static_key, server.cert)};
    BOOST_CHECK(!Handshake(initiator2, responder2, /*now=*/5000));
}

BOOST_AUTO_TEST_CASE(trust_on_first_use_skips_certificate_check)
{
    // Without a configured authority key the server is authenticated only by
    // the handshake, not by the pool. That is a deliberate mode, so it must
    // work, but it accepts a certificate no authority ever endorsed.
    CKey impostor;
    impostor.MakeNewKey(/*fCompressedIn=*/true);
    const auto server{MakeServer(impostor)};

    auto initiator{sv2::HandshakeState::Initiator(std::nullopt)};
    auto responder{sv2::HandshakeState::Responder(server.static_key, server.cert)};
    BOOST_CHECK(Handshake(initiator, responder, 1500));
    BOOST_REQUIRE(initiator.RemoteStaticKey().has_value());
    BOOST_CHECK(*initiator.RemoteStaticKey() == server.static_pub);
}

BOOST_AUTO_TEST_CASE(malformed_handshake_messages_rejected)
{
    CKey authority;
    authority.MakeNewKey(/*fCompressedIn=*/true);
    const auto server{MakeServer(authority)};
    auto responder{sv2::HandshakeState::Responder(server.static_key, server.cert)};

    // Wrong-length message 1 must be refused rather than read short.
    std::vector<std::byte> short_msg(sv2::HANDSHAKE_MSG1_SIZE - 1);
    BOOST_CHECK(!responder.ReadMsg1(short_msg));
    std::vector<std::byte> long_msg(sv2::HANDSHAKE_MSG1_SIZE + 1);
    BOOST_CHECK(!responder.ReadMsg1(long_msg));

    // And likewise for message 2 on the initiator side.
    auto initiator{sv2::HandshakeState::Initiator(XOnlyPubKey{authority.GetPubKey()})};
    std::vector<std::byte> msg1(sv2::HANDSHAKE_MSG1_SIZE);
    initiator.WriteMsg1(msg1);
    std::vector<std::byte> bad2(sv2::HANDSHAKE_MSG2_SIZE - 1);
    BOOST_CHECK(!initiator.ReadMsg2(bad2, 1500));
}

BOOST_AUTO_TEST_CASE(hkdf_two_outputs_differ)
{
    // The two outputs come from the same PRK but different counter bytes, so
    // they must not collide; out2 chains on out1.
    const std::array<std::byte, 32> salt{};
    const std::array<std::byte, 4> ikm{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    uint256 a, b;
    sv2::Hkdf2(salt, ikm, a, b);
    BOOST_CHECK(a != b);

    // Deterministic for the same inputs.
    uint256 a2, b2;
    sv2::Hkdf2(salt, ikm, a2, b2);
    BOOST_CHECK_EQUAL(a.GetHex(), a2.GetHex());
    BOOST_CHECK_EQUAL(b.GetHex(), b2.GetHex());

    // An empty ikm is legal; it is what the final split uses.
    uint256 c, d;
    sv2::Hkdf2(salt, {}, c, d);
    BOOST_CHECK(c != d);
    BOOST_CHECK(c != a);
}

BOOST_AUTO_TEST_SUITE_END()
