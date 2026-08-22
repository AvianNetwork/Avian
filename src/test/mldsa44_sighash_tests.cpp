// Copyright (c) 2024-present The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

// RIP-25: locks the ML-DSA-44 (witness v2) sighash policy.
//
// The rule (doc/rip-25.md) is: every ML-DSA-44 spend commits to exactly
// SIGHASH_ALL | SIGHASH_FORKID, and the witness carries NO sighash-type byte --
// the signature element is the bare 2420-byte ML-DSA signature. These tests
// exercise the real consensus verifier (VerifyScript) to prove both halves:
//   1. a correctly signed spend verifies;
//   2. appending any sighash-type byte makes the signature the wrong length and
//      is rejected (SCRIPT_ERR_PQ_SIGNATURE_SIZE);
//   3. a signature over a digest built with any other hashtype (e.g. plain
//      SIGHASH_ALL, no FORKID) is rejected (SCRIPT_ERR_PQ_SIGNATURE_VERIFY_FAILED).

#include <crypto/mldsa.h>
#include <pqkey.h>
#include <primitives/transaction.h>
#include <script/interpreter.h>
#include <script/script.h>
#include <script/script_error.h>
#include <test/util/transaction_utils.h>
#include <uint256.h>

#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

BOOST_FIXTURE_TEST_SUITE(mldsa44_sighash_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(sighash_policy_is_fixed_and_carries_no_type_byte)
{
    // Deterministic ML-DSA-44 key from a fixed seed.
    std::array<uint8_t, mldsa::SEED_SIZE> seed{};
    for (size_t i = 0; i < seed.size(); ++i) seed[i] = static_cast<uint8_t>(i);

    CPQKey key;
    if (!key.SetSeed(std::span<const uint8_t, mldsa::SEED_SIZE>(seed))) {
        // Built without liboqs: the post-quantum path is a stub. Nothing to lock.
        BOOST_WARN_MESSAGE(false, "liboqs unavailable; skipping ML-DSA-44 sighash policy test");
        return;
    }
    const CPQPubKey pub = key.GetPubKey();
    const uint256 program = pub.GetWitnessProgram();
    const std::vector<uint8_t> program_bytes(program.begin(), program.end());
    const std::vector<uint8_t> pk_bytes(pub.GetData().begin(), pub.GetData().end());

    // Native witness v2 output: OP_2 <32-byte SHA256(pubkey)>.
    const CScript spk = CScript() << OP_2 << program_bytes;
    const CAmount amount = 100'000'000;
    const CTransaction txCredit{BuildCreditingTransaction(spk, amount)};

    // scriptCode used for the sighash, identical to both signer and verifier.
    const CScript scriptCode = CScript() << OP_2 << program_bytes;
    const CMutableTransaction txSpend = BuildSpendingTransaction(CScript(), CScriptWitness(), txCredit);

    const unsigned int flags = SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_WITNESS | SCRIPT_VERIFY_PQ_HYBRID;

    // Run the real consensus verifier with an arbitrary witness stack. The BIP143
    // sighash does not depend on the witness, so a checker built from any spend of
    // this credit is correct for every case below.
    auto verify = [&](const std::vector<uint8_t>& sig, const std::vector<uint8_t>& pk, ScriptError& err) {
        CScriptWitness wit;
        wit.stack.push_back(sig);  // stack[0] = signature
        wit.stack.push_back(pk);   // stack[1] = public key
        const CMutableTransaction spend = BuildSpendingTransaction(CScript(), wit, txCredit);
        const CTransaction tx(spend);
        return VerifyScript(CScript(), spk, &tx.vin[0].scriptWitness, flags,
                            TransactionSignatureChecker(&tx, 0, amount, MissingDataBehavior::ASSERT_FAIL),
                            &err);
    };

    // (1) Canonical spend: sign SIGHASH_ALL|SIGHASH_FORKID, bare 2420-byte sig.
    const uint256 sighash = SignatureHash(scriptCode, txSpend, 0,
                                          SIGHASH_ALL | SIGHASH_FORKID, amount,
                                          SigVersion::WITNESS_V0);
    std::vector<uint8_t> sig;
    BOOST_REQUIRE(key.Sign(sig, std::span<const uint8_t>(sighash.begin(), 32)));
    // The signer emits the raw signature with no appended hashtype byte.
    BOOST_REQUIRE_EQUAL(sig.size(), mldsa::SIG_SIZE);

    ScriptError err = SCRIPT_ERR_OK;
    BOOST_CHECK(verify(sig, pk_bytes, err));
    BOOST_CHECK_EQUAL(err, SCRIPT_ERR_OK);

    // (2) Any carried sighash-type byte makes the element the wrong length.
    for (uint8_t type_byte : {uint8_t{0x41}, uint8_t{0x01}, uint8_t{0x00}}) {
        std::vector<uint8_t> sig_with_byte = sig;
        sig_with_byte.push_back(type_byte);
        err = SCRIPT_ERR_OK;
        BOOST_CHECK(!verify(sig_with_byte, pk_bytes, err));
        BOOST_CHECK_EQUAL(err, SCRIPT_ERR_PQ_SIGNATURE_SIZE);
    }

    // (3) A signature over a different hashtype's digest must not verify: the
    // verifier commits to SIGHASH_ALL|SIGHASH_FORKID only. Here we sign the
    // SIGHASH_ALL (no FORKID) digest but present a correctly-sized 2420-byte sig.
    const uint256 sighash_no_forkid = SignatureHash(scriptCode, txSpend, 0,
                                                    SIGHASH_ALL, amount,
                                                    SigVersion::WITNESS_V0);
    BOOST_REQUIRE(sighash_no_forkid != sighash);
    std::vector<uint8_t> sig_wrong_type;
    BOOST_REQUIRE(key.Sign(sig_wrong_type, std::span<const uint8_t>(sighash_no_forkid.begin(), 32)));
    BOOST_REQUIRE_EQUAL(sig_wrong_type.size(), mldsa::SIG_SIZE);
    err = SCRIPT_ERR_OK;
    BOOST_CHECK(!verify(sig_wrong_type, pk_bytes, err));
    BOOST_CHECK_EQUAL(err, SCRIPT_ERR_PQ_SIGNATURE_VERIFY_FAILED);
}

BOOST_AUTO_TEST_SUITE_END()
