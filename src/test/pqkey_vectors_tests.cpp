// Copyright (c) 2024-present The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

// RIP-25 known-answer vectors (src/test/data/rip25_vectors.json).
//
// This suite LOCKS the ML-DSA-44 (witness v2) derivation and BIP143 sighash so
// they can never silently drift (a liboqs upgrade, ABI change, DST change, or
// address/encoding change all break these values). If a check here fails after a
// dependency bump, the derivation changed and every previously derived PQ
// address/key would move: treat it as a consensus / wallet-recovery event, not a
// value to blindly update.
//
// Determinism: ML-DSA-44 signing is randomized (FIPS-204 hedged), so the
// committed `signature`/`final_tx`/`wtxid` are a captured VALID instance -- the
// reader asserts the committed signature VERIFIES and that the (witness-
// independent) `txid` matches, rather than re-deriving the signature. The
// deterministic fields (seed -> pubkey_sha256/witness_program -> address ->
// unsigned_tx -> sighash) are re-derived from the seed and byte-compared.
//
// To regenerate the JSON after an intentional change, run with GEN_RIP25_VECTORS
// set and replace src/test/data/rip25_vectors.json with the emitted block:
//   GEN_RIP25_VECTORS=1 test_avian --run_test=pqkey_vectors/generate

#include <test/data/rip25_vectors.json.h>

#include <core_io.h>
#include <crypto/mldsa.h>
#include <key_io.h>
#include <pqkey.h>
#include <primitives/transaction.h>
#include <script/interpreter.h>
#include <script/script.h>
#include <script/script_error.h>
#include <test/util/transaction_utils.h>
#include <uint256.h>
#include <univalue.h>
#include <util/strencodings.h>

#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <vector>

BOOST_FIXTURE_TEST_SUITE(pqkey_vectors, BasicTestingSetup)

namespace {

bool LibOqsAvailable()
{
    std::array<uint8_t, mldsa::SEED_SIZE> probe{};
    CPQKey k;
    return k.SetSeed(std::span<const uint8_t, mldsa::SEED_SIZE>(probe));
}

// Re-derive the deterministic half of a vector from its seed and build the
// witness-v2 spend, so callers can byte-compare against the committed fields.
struct Derived {
    CPQKey key;
    CPQPubKey pub;
    uint256 program;
    CScript spk;
    CMutableTransaction txCredit;
    CMutableTransaction txSpendUnsigned;
    uint256 sighash;
    CAmount amount;
    Derived(const std::array<uint8_t, mldsa::SEED_SIZE>& seed, CAmount amt)
        : amount(amt)
    {
        BOOST_REQUIRE(key.SetSeed(std::span<const uint8_t, mldsa::SEED_SIZE>(seed)));
        pub = key.GetPubKey();
        program = pub.GetWitnessProgram();
        const std::vector<uint8_t> pbytes(program.begin(), program.end());
        spk = CScript() << OP_2 << pbytes;
        txCredit = BuildCreditingTransaction(spk, amount);
        txSpendUnsigned = BuildSpendingTransaction(CScript(), CScriptWitness(), CTransaction(txCredit));
        const CScript scriptCode = CScript() << OP_2 << pbytes;
        sighash = SignatureHash(scriptCode, txSpendUnsigned, 0,
                                SIGHASH_ALL | SIGHASH_FORKID, amount,
                                SigVersion::WITNESS_V0);
    }
    // Run the real consensus verifier with a given witness [sig, pubkey].
    bool VerifyWith(const std::vector<uint8_t>& sig, const std::vector<uint8_t>& pk, ScriptError& err) const
    {
        CScriptWitness wit;
        wit.stack.push_back(sig);
        wit.stack.push_back(pk);
        const CMutableTransaction spend = BuildSpendingTransaction(CScript(), wit, CTransaction(txCredit));
        const CTransaction tx(spend);
        const unsigned int flags = SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_WITNESS | SCRIPT_VERIFY_MLDSA44;
        return VerifyScript(CScript(), spk, &tx.vin[0].scriptWitness, flags,
                            TransactionSignatureChecker(&tx, 0, amount, MissingDataBehavior::ASSERT_FAIL),
                            &err);
    }
};

std::array<uint8_t, mldsa::SEED_SIZE> SeedFromHex(const std::string& hex)
{
    const std::vector<unsigned char> b = ParseHex(hex);
    BOOST_REQUIRE_EQUAL(b.size(), mldsa::SEED_SIZE);
    std::array<uint8_t, mldsa::SEED_SIZE> s{};
    std::copy(b.begin(), b.end(), s.begin());
    return s;
}

} // namespace

BOOST_AUTO_TEST_CASE(vectors_match_committed_file)
{
    if (!LibOqsAvailable()) {
        BOOST_WARN_MESSAGE(false, "liboqs unavailable; skipping RIP-25 vector validation");
        return;
    }

    UniValue root;
    BOOST_REQUIRE(root.read(json_tests::rip25_vectors));
    BOOST_REQUIRE(root.isObject());

    // The vectors are network-scoped (address HRP + domain context). They were
    // generated under the same network BasicTestingSetup selects (regtest), so
    // the active domain context must equal the committed one.
    BOOST_CHECK_EQUAL(root["network"].get_str(), "regtest");
    BOOST_CHECK_EQUAL(root["domain_context"].get_str(), HexStr(GetMLDsa44DomainContext()));

    const UniValue& vectors = root["vectors"].get_array();
    BOOST_REQUIRE(!vectors.empty());

    for (size_t i = 0; i < vectors.size(); ++i) {
        const UniValue& v = vectors[i];
        const CAmount amount = v["amount"].getInt<int64_t>();
        const Derived d(SeedFromHex(v["seed"].get_str()), amount);

        // --- Deterministic fields: re-derived == committed (the drift lock). ---
        BOOST_CHECK_EQUAL(HexStr(d.program), v["witness_program"].get_str());
        BOOST_CHECK_EQUAL(HexStr(d.program), v["pubkey_sha256"].get_str());
        BOOST_CHECK_EQUAL(EncodeDestination(WitnessV2MLDsa44(d.program)), v["address"].get_str());
        BOOST_CHECK_EQUAL(HexStr(d.spk), v["script_pubkey"].get_str());
        BOOST_CHECK_EQUAL(EncodeHexTx(CTransaction(d.txSpendUnsigned)), v["unsigned_tx"].get_str());
        BOOST_CHECK_EQUAL(HexStr(d.sighash), v["sighash"].get_str());

        // --- Reference instance: the committed signature must VERIFY. ---
        const std::vector<unsigned char> sig = ParseHex(v["signature"].get_str());
        const std::vector<unsigned char> pk(d.pub.GetData().begin(), d.pub.GetData().end());
        BOOST_CHECK_EQUAL(sig.size(), mldsa::SIG_SIZE);
        ScriptError err = SCRIPT_ERR_OK;
        BOOST_CHECK_MESSAGE(d.VerifyWith(sig, pk, err), "committed signature must verify (vector " << i << ")");
        BOOST_CHECK_EQUAL(err, SCRIPT_ERR_OK);
        BOOST_CHECK(v["verify"].get_bool());

        // The committed final_tx must parse and its txid (witness-independent)
        // must match; wtxid is instance-specific so it is not re-derived here.
        CMutableTransaction finalTx;
        BOOST_REQUIRE(DecodeHexTx(finalTx, v["final_tx"].get_str()));
        BOOST_CHECK_EQUAL(CTransaction(finalTx).GetHash().GetHex(), v["txid"].get_str());

        // --- Negative: tampered signature must NOT verify. ---
        std::vector<unsigned char> bad_sig = sig;
        bad_sig[bad_sig.size() / 2] ^= 0x01;
        err = SCRIPT_ERR_OK;
        BOOST_CHECK(!d.VerifyWith(bad_sig, pk, err));
        BOOST_CHECK_EQUAL(err, SCRIPT_ERR_PQ_SIGNATURE_VERIFY_FAILED);

        // A carried sighash-type byte makes the element the wrong length.
        std::vector<unsigned char> sig_plus = sig;
        sig_plus.push_back(0x41);
        err = SCRIPT_ERR_OK;
        BOOST_CHECK(!d.VerifyWith(sig_plus, pk, err));
        BOOST_CHECK_EQUAL(err, SCRIPT_ERR_PQ_SIGNATURE_SIZE);
    }
}

// Emit the vectors as JSON for capture into src/test/data/rip25_vectors.json.
// Only runs when GEN_RIP25_VECTORS is set; a no-op otherwise.
BOOST_AUTO_TEST_CASE(generate)
{
    if (!std::getenv("GEN_RIP25_VECTORS")) return;
    if (!LibOqsAvailable()) return;

    auto make = [&](uint8_t seed_base, CAmount amount) {
        std::array<uint8_t, mldsa::SEED_SIZE> seed{};
        for (size_t i = 0; i < seed.size(); ++i) seed[i] = static_cast<uint8_t>(seed_base + i);
        Derived d(seed, amount);
        std::vector<uint8_t> sig;
        BOOST_REQUIRE(d.key.Sign(sig, std::span<const uint8_t>(d.sighash.begin(), 32), GetMLDsa44DomainContext()));
        const std::vector<uint8_t> pk(d.pub.GetData().begin(), d.pub.GetData().end());
        ScriptError err = SCRIPT_ERR_OK;
        BOOST_REQUIRE(d.VerifyWith(sig, pk, err));
        CScriptWitness wit; wit.stack.push_back(sig); wit.stack.push_back(pk);
        const CTransaction finalTx(BuildSpendingTransaction(CScript(), wit, CTransaction(d.txCredit)));

        UniValue o(UniValue::VOBJ);
        o.pushKV("seed", HexStr(seed));
        o.pushKV("pubkey_sha256", HexStr(d.program));
        o.pushKV("witness_program", HexStr(d.program));
        o.pushKV("address", EncodeDestination(WitnessV2MLDsa44(d.program)));
        o.pushKV("amount", amount);
        o.pushKV("script_pubkey", HexStr(d.spk));
        o.pushKV("unsigned_tx", EncodeHexTx(CTransaction(d.txSpendUnsigned)));
        o.pushKV("sighash", HexStr(d.sighash));
        o.pushKV("signature", HexStr(sig));
        o.pushKV("final_tx", EncodeHexTx(finalTx));
        o.pushKV("txid", finalTx.GetHash().GetHex());
        o.pushKV("wtxid", finalTx.GetWitnessHash().GetHex());
        o.pushKV("verify", true);
        return o;
    };

    UniValue arr(UniValue::VARR);
    arr.push_back(make(0x00, 100'000'000));
    arr.push_back(make(0x80, 250'000'000));
    UniValue root(UniValue::VOBJ);
    root.pushKV("comment",
        "RIP-25 ML-DSA-44 (witness v2) known-answer vectors. Deterministic fields "
        "(seed, pubkey_sha256/witness_program, address, unsigned_tx, sighash) are "
        "byte-pinned and lock the derivation + BIP143 sighash. ML-DSA signing is "
        "randomized (FIPS-204 hedged), so signature/final_tx/wtxid are a captured "
        "VALID instance asserted to verify; txid is witness-independent. Generated "
        "under regtest (domain context + address HRP are that network's).");
    root.pushKV("network", "regtest");
    root.pushKV("domain_context", HexStr(GetMLDsa44DomainContext()));
    root.pushKV("vectors", arr);
    std::printf("RIP25_VECTORS_BEGIN\n%s\nRIP25_VECTORS_END\n", root.write(2).c_str());
}

BOOST_AUTO_TEST_SUITE_END()
