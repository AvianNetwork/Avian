// Copyright (c) 2026 ALENOC <https://github.com/ALENOC> (Ravencoin RIP-25)
// Copyright (c) 2024-present The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

// RIP-25: Unit tests for ML-DSA-44 (FIPS 204) post-quantum keys.
// Ported from Ravencoin RIP-25 <https://github.com/RavenProject/Ravencoin/pull/1281>

#include <crypto/mldsa.h>
#include <crypto/sha256.h>
#include <pqkey.h>
#include <uint256.h>
#include <util/strencodings.h>

#include <boost/test/unit_test.hpp>

#include <array>
#include <cstdint>
#include <vector>

BOOST_AUTO_TEST_SUITE(pqkey_tests)

// ML-DSA-44 keys are backed by liboqs. When the build is configured with
// WITH_LIBOQS=OFF, mldsa.cpp falls back to stubs and CPQKey cannot generate or
// sign, so these tests are compiled out (matching descriptor_mldsa44_test).
#ifdef HAVE_LIBOQS

BOOST_AUTO_TEST_CASE(keygen_roundtrip)
{
    // Generate a key pair
    CPQKey key;
    BOOST_REQUIRE(key.MakeNewKey());
    BOOST_CHECK(key.IsValid());

    CPQPubKey pubkey = key.GetPubKey();
    BOOST_CHECK(pubkey.IsValid());

    // GetData returns the right sizes
    BOOST_CHECK_EQUAL(key.GetData().size(), mldsa::SECRETKEY_SIZE);
    BOOST_CHECK_EQUAL(pubkey.GetData().size(), mldsa::PUBKEY_SIZE);

    // Witness program is SHA256(pubkey), 32 bytes
    uint256 program = pubkey.GetWitnessProgram();
    BOOST_CHECK(!program.IsNull());
}

BOOST_AUTO_TEST_CASE(deterministic_keygen_from_seed)
{
    std::array<uint8_t, mldsa::SEED_SIZE> seed{};
    // Fill seed with known bytes
    for (size_t i = 0; i < seed.size(); ++i) seed[i] = static_cast<uint8_t>(i);

    CPQKey key1, key2;
    BOOST_REQUIRE(key1.SetSeed(std::span<const uint8_t, mldsa::SEED_SIZE>(seed)));
    BOOST_REQUIRE(key2.SetSeed(std::span<const uint8_t, mldsa::SEED_SIZE>(seed)));

    // Same seed → same pubkey
    BOOST_CHECK(key1.GetPubKey() == key2.GetPubKey());
    // Same seed → same witness program
    BOOST_CHECK(key1.GetPubKey().GetWitnessProgram() == key2.GetPubKey().GetWitnessProgram());
}

BOOST_AUTO_TEST_CASE(derivation_known_answer_vector)
{
    // RIP-25 key-derivation known-answer vector.  This pins the exact mapping
    //   seed -> xi = SHA256("AVN/ML-DSA-44/keygen/v1" || seed)
    //        -> (pk, sk) = ML-DSA.KeyGen_internal(xi)
    // so the derivation can never silently drift (liboqs upgrade, struct-ABI
    // change, DST change, or expansion change all break these values).  If this
    // test fails after a dependency bump, the derivation changed and every
    // previously derived PQ address/key would move: treat it as a consensus /
    // wallet-recovery event, not a value to blindly update.
    //
    // Seed = 0x00,0x01,...,0x1f.
    std::array<uint8_t, mldsa::SEED_SIZE> seed{};
    for (size_t i = 0; i < seed.size(); ++i) seed[i] = static_cast<uint8_t>(i);

    CPQKey key;
    BOOST_REQUIRE(key.SetSeed(std::span<const uint8_t, mldsa::SEED_SIZE>(seed)));

    // Witness program is SHA256(pubkey); this is the on-chain commitment.
    const uint256 wp = key.GetPubKey().GetWitnessProgram();
    BOOST_CHECK_EQUAL(HexStr(wp),
        "a7da36522f995f7a2ccba0e8d10f6b7d2dcd7882486a38462176ba720823a0fb");

    // Sanity: the witness program equals SHA256 over the raw public key bytes.
    auto pk = key.GetPubKey().GetData();
    uint256 pkh;
    CSHA256().Write(pk.data(), pk.size()).Finalize(pkh.begin());
    BOOST_CHECK_EQUAL(HexStr(pkh), HexStr(wp));

    // Pin the secret key too (SHA256 of the 2560-byte packed sk).
    auto sk = key.GetData();
    uint256 skh;
    CSHA256().Write(sk.data(), sk.size()).Finalize(skh.begin());
    BOOST_CHECK_EQUAL(HexStr(skh),
        "f59fd75b78cb6727f9c626dc82898ce6b5f8794418708330e686a1e1a5fc4099");

    // And the derived keypair must be internally consistent: a signature made
    // with sk verifies under pk.  This proves the packed sk matches the pk.
    std::array<uint8_t, 32> msg{};
    for (size_t i = 0; i < msg.size(); ++i) msg[i] = static_cast<uint8_t>(0x40 + i);
    std::vector<uint8_t> sig;
    BOOST_REQUIRE(key.Sign(sig, std::span<const uint8_t>(msg)));
    BOOST_CHECK(key.GetPubKey().Verify(std::span<const uint8_t>(sig), std::span<const uint8_t>(msg)));
}

BOOST_AUTO_TEST_CASE(sign_and_verify)
{
    CPQKey key;
    BOOST_REQUIRE(key.MakeNewKey());
    CPQPubKey pubkey = key.GetPubKey();

    std::array<uint8_t, 32> msg{};
    for (size_t i = 0; i < msg.size(); ++i) msg[i] = static_cast<uint8_t>(i + 1);

    std::vector<uint8_t> sig;
    BOOST_REQUIRE(key.Sign(sig, std::span<const uint8_t>(msg)));

    // Correct signature verifies
    BOOST_CHECK(pubkey.Verify(std::span<const uint8_t>(sig), std::span<const uint8_t>(msg)));
}

BOOST_AUTO_TEST_CASE(verify_wrong_message_fails)
{
    CPQKey key;
    BOOST_REQUIRE(key.MakeNewKey());
    CPQPubKey pubkey = key.GetPubKey();

    std::array<uint8_t, 32> msg{};
    std::fill(msg.begin(), msg.end(), 0xAB);

    std::vector<uint8_t> sig;
    BOOST_REQUIRE(key.Sign(sig, std::span<const uint8_t>(msg)));

    // Tamper with message
    std::array<uint8_t, 32> bad_msg{};
    std::fill(bad_msg.begin(), bad_msg.end(), 0xCD);
    BOOST_CHECK(!pubkey.Verify(std::span<const uint8_t>(sig), std::span<const uint8_t>(bad_msg)));
}

BOOST_AUTO_TEST_CASE(verify_wrong_key_fails)
{
    CPQKey key1, key2;
    BOOST_REQUIRE(key1.MakeNewKey());
    BOOST_REQUIRE(key2.MakeNewKey());

    CPQPubKey pubkey2 = key2.GetPubKey();

    std::array<uint8_t, 32> msg{};
    std::fill(msg.begin(), msg.end(), 0x77);

    std::vector<uint8_t> sig;
    BOOST_REQUIRE(key1.Sign(sig, std::span<const uint8_t>(msg)));

    // Signature from key1 should not verify under key2's pubkey
    BOOST_CHECK(!pubkey2.Verify(std::span<const uint8_t>(sig), std::span<const uint8_t>(msg)));
}

BOOST_AUTO_TEST_CASE(verify_tampered_signature_fails)
{
    CPQKey key;
    BOOST_REQUIRE(key.MakeNewKey());
    CPQPubKey pubkey = key.GetPubKey();

    std::array<uint8_t, 32> msg{};
    std::fill(msg.begin(), msg.end(), 0x55);

    std::vector<uint8_t> sig;
    BOOST_REQUIRE(key.Sign(sig, std::span<const uint8_t>(msg)));

    // Flip a byte in the signature
    sig[42] ^= 0xFF;
    BOOST_CHECK(!pubkey.Verify(std::span<const uint8_t>(sig), std::span<const uint8_t>(msg)));
}

BOOST_AUTO_TEST_CASE(setkey_data_roundtrip)
{
    // Generate key, extract data, reconstruct via SetKeyData, verify signing still works
    CPQKey key;
    BOOST_REQUIRE(key.MakeNewKey());
    CPQPubKey pubkey = key.GetPubKey();

    // Copy raw key data
    auto sk_span = key.GetData();
    std::vector<uint8_t> sk_bytes(sk_span.begin(), sk_span.end());

    CPQKey key2;
    key2.SetKeyData(std::span<const uint8_t, CPQKey::SIZE>(sk_bytes.data(), CPQKey::SIZE), pubkey);

    std::array<uint8_t, 32> msg{};
    std::fill(msg.begin(), msg.end(), 0xDE);

    std::vector<uint8_t> sig;
    BOOST_REQUIRE(key2.Sign(sig, std::span<const uint8_t>(msg)));

    BOOST_CHECK(pubkey.Verify(std::span<const uint8_t>(sig), std::span<const uint8_t>(msg)));
}

#else // HAVE_LIBOQS not defined

BOOST_AUTO_TEST_CASE(pqkey_tests_require_liboqs)
{
    // Built with WITH_LIBOQS=OFF: post-quantum keys are unavailable, so there is
    // nothing to exercise here. The suite exists so the run does not report a
    // hard failure for an intentionally-disabled optional feature.
    BOOST_TEST_MESSAGE("pqkey_tests skipped: built without liboqs (WITH_LIBOQS=OFF)");
    BOOST_CHECK(true);
}

#endif // HAVE_LIBOQS

BOOST_AUTO_TEST_SUITE_END()
