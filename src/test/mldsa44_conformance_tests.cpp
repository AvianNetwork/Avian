// Copyright (c) 2024-present The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

// RIP-25: ML-DSA-44 verification conformance and backend agreement.
//
// ML-DSA-44 verification is a consensus rule, and liboqs picks its backend at
// runtime (AVX2 on x86-64 with AVX2+BMI2+POPCNT, NEON on aarch64, portable C
// otherwise), so two nodes can run different verify code. Every case here is
// checked against both mldsa::Verify (dispatched) and mldsa::VerifyReference
// (portable C), and both must return the published expected result. The C
// backend is the same code everywhere, so a platform that passes this suite
// agrees with every other platform that passes it.
//
// Vectors, used unmodified apart from the ACVP subsetting described in that
// file's "source" member:
//   data/wycheproof_mldsa_44_verify_test.json
//     C2SP/wycheproof @ 20b256949ea8f21a11375619b1825027ca0668bd,
//     testvectors_v1/mldsa_44_verify_test.json
//     sha256 f70759c95146ff3057ae9708438f6e8d25ba822e3af11d2e87bb20d9a9c39d0e
//   data/acvp_mldsa_44_sigver.json
//     usnistgov/ACVP-Server @ a7f283cdc87d2d6dd93c1bac59e5622c5f9f8324,
//     gen-val/json-files/ML-DSA-sigVer-FIPS204/internalProjection.json

#include <crypto/mldsa.h>

#include <boost/test/unit_test.hpp>

#ifdef HAVE_LIBOQS

#include <test/data/acvp_mldsa_44_sigver.json.h>
#include <test/data/wycheproof_mldsa_44_verify_test.json.h>

#include <pqkey.h>
#include <univalue.h>
#include <util/strencodings.h>

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#endif // HAVE_LIBOQS

BOOST_AUTO_TEST_SUITE(mldsa44_conformance_tests)

#ifdef HAVE_LIBOQS

namespace {

using Bytes = std::vector<unsigned char>;

struct Result {
    bool dispatched;
    bool reference;
};

// Verify one case the way consensus does: the interpreter rejects any public
// key or signature that is not exactly PUBKEY_SIZE / SIG_SIZE before liboqs is
// called, so wrong-length inputs are rejections here too.
Result VerifyBoth(const Bytes& sig, const Bytes& msg, const Bytes& ctx, const Bytes& pk)
{
    if (pk.size() != mldsa::PUBKEY_SIZE || sig.size() != mldsa::SIG_SIZE) return {false, false};
    const std::span<const uint8_t, mldsa::SIG_SIZE> s{sig.data(), mldsa::SIG_SIZE};
    const std::span<const uint8_t, mldsa::PUBKEY_SIZE> p{pk.data(), mldsa::PUBKEY_SIZE};
    return {mldsa::Verify(s, msg, ctx, p), mldsa::VerifyReference(s, msg, ctx, p)};
}

Bytes HexMember(const UniValue& obj, const std::string& key)
{
    if (!obj.exists(key)) return {};
    const std::string& hex = obj[key].get_str();
    BOOST_REQUIRE_MESSAGE(IsHex(hex) || hex.empty(), key << " is not hex");
    return ParseHex(hex);
}

} // namespace

BOOST_AUTO_TEST_CASE(wycheproof_verify)
{
    UniValue root;
    BOOST_REQUIRE(root.read(json_tests::wycheproof_mldsa_44_verify_test));
    BOOST_REQUIRE_EQUAL(root["algorithm"].get_str(), "ML-DSA-44");

    size_t total{0}, valid{0}, invalid{0}, wrong_length{0};
    const UniValue& groups = root["testGroups"].get_array();
    for (size_t g = 0; g < groups.size(); ++g) {
        const Bytes pk = HexMember(groups[g], "publicKey");
        const UniValue& tests = groups[g]["tests"].get_array();
        for (size_t t = 0; t < tests.size(); ++t) {
            const UniValue& tc = tests[t];
            const int id = tc["tcId"].getInt<int>();
            const std::string& expected = tc["result"].get_str();
            const Bytes sig = HexMember(tc, "sig");
            const Result r = VerifyBoth(sig, HexMember(tc, "msg"), HexMember(tc, "ctx"), pk);

            BOOST_CHECK_MESSAGE(r.dispatched == r.reference,
                                "backends disagree on tcId " << id << " (" << tc["comment"].get_str() << ")");
            BOOST_REQUIRE_MESSAGE(expected == "valid" || expected == "invalid", "unexpected result class on tcId " << id);
            BOOST_CHECK_MESSAGE(r.dispatched == (expected == "valid"),
                                "tcId " << id << " (" << tc["comment"].get_str() << ") expected " << expected);
            BOOST_CHECK_MESSAGE(r.reference == (expected == "valid"),
                                "reference backend: tcId " << id << " expected " << expected);

            ++total;
            ++(expected == "valid" ? valid : invalid);
            if (pk.size() != mldsa::PUBKEY_SIZE || sig.size() != mldsa::SIG_SIZE) ++wrong_length;
        }
    }

    // Guard against a truncated or silently filtered vector file.
    BOOST_CHECK_EQUAL(total, root["numberOfTests"].getInt<size_t>());
    BOOST_CHECK_EQUAL(total, 180U);
    BOOST_CHECK_EQUAL(valid, 77U);
    BOOST_CHECK_EQUAL(invalid, 103U);
    BOOST_CHECK_EQUAL(wrong_length, 7U);
}

BOOST_AUTO_TEST_CASE(acvp_sigver)
{
    UniValue root;
    BOOST_REQUIRE(root.read(json_tests::acvp_mldsa_44_sigver));
    BOOST_REQUIRE_EQUAL(root["mode"].get_str(), "sigVer");
    BOOST_REQUIRE_EQUAL(root["revision"].get_str(), "FIPS204");

    size_t total{0}, valid{0};
    const UniValue& groups = root["testGroups"].get_array();
    for (size_t g = 0; g < groups.size(); ++g) {
        // Only the interface consensus uses: external, pure (no pre-hash).
        BOOST_REQUIRE_EQUAL(groups[g]["parameterSet"].get_str(), "ML-DSA-44");
        BOOST_REQUIRE_EQUAL(groups[g]["signatureInterface"].get_str(), "external");
        BOOST_REQUIRE_EQUAL(groups[g]["preHash"].get_str(), "pure");
        BOOST_REQUIRE(!groups[g]["externalMu"].get_bool());

        const UniValue& tests = groups[g]["tests"].get_array();
        for (size_t t = 0; t < tests.size(); ++t) {
            const UniValue& tc = tests[t];
            const int id = tc["tcId"].getInt<int>();
            const bool expected = tc["testPassed"].get_bool();
            const Result r = VerifyBoth(HexMember(tc, "signature"), HexMember(tc, "message"),
                                        HexMember(tc, "context"), HexMember(tc, "pk"));

            BOOST_CHECK_MESSAGE(r.dispatched == expected, "tcId " << id << " (" << tc["reason"].get_str() << ")");
            BOOST_CHECK_MESSAGE(r.reference == expected, "reference backend: tcId " << id);
            ++total;
            if (expected) ++valid;
        }
    }
    BOOST_CHECK_EQUAL(total, 15U);
    BOOST_CHECK_EQUAL(valid, 3U);
}

// Every one-bit change to a valid (signature, public key, message, context)
// must be rejected by both backends. ML-DSA is strongly unforgeable, so an
// accepted change would be either a malleable encoding or a backend bug.
BOOST_AUTO_TEST_CASE(single_bit_changes_rejected_by_both_backends)
{
    std::array<uint8_t, mldsa::SEED_SIZE> seed{};
    for (size_t i = 0; i < seed.size(); ++i) seed[i] = static_cast<uint8_t>(0xA0 + i);
    CPQKey key;
    BOOST_REQUIRE(key.SetSeed(seed));
    const Bytes pk(key.GetPubKey().GetData().begin(), key.GetPubKey().GetData().end());

    Bytes msg(32);
    for (size_t i = 0; i < msg.size(); ++i) msg[i] = static_cast<uint8_t>(i * 7 + 1);
    const std::string ctx_str{"AVN/ML-DSA-44/conformance-test"};
    const Bytes ctx(ctx_str.begin(), ctx_str.end());

    Bytes sig;
    BOOST_REQUIRE(key.Sign(sig, msg, ctx));
    BOOST_REQUIRE_EQUAL(sig.size(), mldsa::SIG_SIZE);
    {
        const Result r = VerifyBoth(sig, msg, ctx, pk);
        BOOST_REQUIRE(r.dispatched);
        BOOST_REQUIRE(r.reference);
    }

    size_t accepted{0}, disagreed{0}, checked{0};
    auto expect_reject = [&](const Bytes& s, const Bytes& m, const Bytes& c, const Bytes& p) {
        const Result r = VerifyBoth(s, m, c, p);
        if (r.dispatched || r.reference) ++accepted;
        if (r.dispatched != r.reference) ++disagreed;
        ++checked;
    };
    auto flip = [](Bytes v, size_t bit) {
        v[bit / 8] ^= static_cast<unsigned char>(1U << (bit % 8));
        return v;
    };

    // Signature layout (FIPS 204, ML-DSA-44): c_tilde (32) || z (4 * 576) ||
    // hints (80 + 4). Flip every bit of c_tilde and of the hint encoding, and one
    // bit (rotating position) of every byte of z.
    static constexpr size_t CTILDE_BYTES{32};
    static constexpr size_t HINT_BYTES{84};
    static constexpr size_t HINT_START{mldsa::SIG_SIZE - HINT_BYTES};
    for (size_t bit = 0; bit < CTILDE_BYTES * 8; ++bit) expect_reject(flip(sig, bit), msg, ctx, pk);
    for (size_t byte = CTILDE_BYTES; byte < HINT_START; ++byte) expect_reject(flip(sig, byte * 8 + byte % 8), msg, ctx, pk);
    for (size_t bit = HINT_START * 8; bit < mldsa::SIG_SIZE * 8; ++bit) expect_reject(flip(sig, bit), msg, ctx, pk);

    for (size_t byte = 0; byte < pk.size(); ++byte) expect_reject(sig, msg, ctx, flip(pk, byte * 8 + byte % 8));
    for (size_t bit = 0; bit < msg.size() * 8; ++bit) expect_reject(sig, flip(msg, bit), ctx, pk);
    for (size_t bit = 0; bit < ctx.size() * 8; ++bit) expect_reject(sig, msg, flip(ctx, bit), pk);

    // Length changes to the message and context, and an empty context.
    Bytes longer_msg{msg};
    longer_msg.push_back(0);
    expect_reject(sig, longer_msg, ctx, pk);
    expect_reject(sig, Bytes(msg.begin(), msg.end() - 1), ctx, pk);
    Bytes longer_ctx{ctx};
    longer_ctx.push_back(0);
    expect_reject(sig, msg, longer_ctx, pk);
    expect_reject(sig, msg, Bytes(ctx.begin(), ctx.end() - 1), pk);
    expect_reject(sig, msg, Bytes{}, pk);

    BOOST_CHECK_MESSAGE(accepted == 0 && disagreed == 0,
                        accepted << " accepted, " << disagreed << " backend disagreements out of " << checked
                                 << "; signature " << HexStr(sig));
    BOOST_CHECK_GT(checked, 4000U);
}

// The FIPS 204 context string is at most 255 bytes. Both entry points must
// refuse a longer one rather than pass a truncated length to liboqs.
BOOST_AUTO_TEST_CASE(oversized_context_rejected)
{
    std::array<uint8_t, mldsa::SEED_SIZE> seed{};
    CPQKey key;
    BOOST_REQUIRE(key.SetSeed(seed));
    const Bytes pk(key.GetPubKey().GetData().begin(), key.GetPubKey().GetData().end());
    const Bytes msg(32, 0x11);

    Bytes sig;
    BOOST_CHECK(!key.Sign(sig, msg, Bytes(256, 0x22)));

    const Bytes max_ctx(255, 0x22);
    BOOST_REQUIRE(key.Sign(sig, msg, max_ctx));
    Result r = VerifyBoth(sig, msg, max_ctx, pk);
    BOOST_CHECK(r.dispatched && r.reference);
    r = VerifyBoth(sig, msg, Bytes(256, 0x22), pk);
    BOOST_CHECK(!r.dispatched && !r.reference);
}

#else // HAVE_LIBOQS not defined

BOOST_AUTO_TEST_CASE(mldsa44_conformance_tests_require_liboqs)
{
    BOOST_TEST_MESSAGE("mldsa44_conformance_tests skipped: built without liboqs (WITH_LIBOQS=OFF)");
    BOOST_CHECK(!mldsa::IsAvailable());
}

#endif // HAVE_LIBOQS

BOOST_AUTO_TEST_SUITE_END()
