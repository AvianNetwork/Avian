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

    const unsigned int flags = SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_WITNESS | SCRIPT_VERIFY_MLDSA44;

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
    BOOST_REQUIRE(key.Sign(sig, std::span<const uint8_t>(sighash.begin(), 32),
                           GetMLDsa44DomainContext()));
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
    BOOST_REQUIRE(key.Sign(sig_wrong_type, std::span<const uint8_t>(sighash_no_forkid.begin(), 32),
                           GetMLDsa44DomainContext()));
    BOOST_REQUIRE_EQUAL(sig_wrong_type.size(), mldsa::SIG_SIZE);
    err = SCRIPT_ERR_OK;
    BOOST_CHECK(!verify(sig_wrong_type, pk_bytes, err));
    BOOST_CHECK_EQUAL(err, SCRIPT_ERR_PQ_SIGNATURE_VERIFY_FAILED);
}

BOOST_AUTO_TEST_CASE(domain_separation_binds_the_network)
{
    // A signature made under a different ML-DSA-44 context (another network's,
    // or none) must not verify under this node's context, even though the
    // sighash, program, and key are identical.
    std::array<uint8_t, mldsa::SEED_SIZE> seed{};
    for (size_t i = 0; i < seed.size(); ++i) seed[i] = static_cast<uint8_t>(0x80 + i);

    CPQKey key;
    if (!key.SetSeed(std::span<const uint8_t, mldsa::SEED_SIZE>(seed))) {
        BOOST_WARN_MESSAGE(false, "liboqs unavailable; skipping ML-DSA-44 domain separation test");
        return;
    }
    const CPQPubKey pub = key.GetPubKey();
    const uint256 program = pub.GetWitnessProgram();
    const std::vector<uint8_t> program_bytes(program.begin(), program.end());
    const std::vector<uint8_t> pk_bytes(pub.GetData().begin(), pub.GetData().end());

    const CScript spk = CScript() << OP_2 << program_bytes;
    const CAmount amount = 100'000'000;
    const CTransaction txCredit{BuildCreditingTransaction(spk, amount)};
    const CScript scriptCode = CScript() << OP_2 << program_bytes;
    const CMutableTransaction txSpend = BuildSpendingTransaction(CScript(), CScriptWitness(), txCredit);
    const uint256 sighash = SignatureHash(scriptCode, txSpend, 0,
                                          SIGHASH_ALL | SIGHASH_FORKID, amount,
                                          SigVersion::WITNESS_V0);

    const unsigned int flags = SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_WITNESS | SCRIPT_VERIFY_MLDSA44;
    auto verify = [&](const std::vector<uint8_t>& sig, ScriptError& err) {
        CScriptWitness wit;
        wit.stack.push_back(sig);
        wit.stack.push_back(pk_bytes);
        const CMutableTransaction spend = BuildSpendingTransaction(CScript(), wit, txCredit);
        const CTransaction tx(spend);
        return VerifyScript(CScript(), spk, &tx.vin[0].scriptWitness, flags,
                            TransactionSignatureChecker(&tx, 0, amount, MissingDataBehavior::ASSERT_FAIL),
                            &err);
    };

    // The active context must be non-empty (SelectParams set it in the fixture),
    // otherwise this test would not be exercising separation at all.
    const auto node_ctx = GetMLDsa44DomainContext();
    BOOST_REQUIRE(!node_ctx.empty());

    // Sign under a foreign context (one byte flipped) instead of the node's.
    std::vector<unsigned char> foreign_ctx(node_ctx.begin(), node_ctx.end());
    foreign_ctx.back() ^= 0x01;
    std::vector<uint8_t> sig_foreign;
    BOOST_REQUIRE(key.Sign(sig_foreign, std::span<const uint8_t>(sighash.begin(), 32),
                           std::span<const uint8_t>(foreign_ctx)));
    ScriptError err = SCRIPT_ERR_OK;
    BOOST_CHECK(!verify(sig_foreign, err));
    BOOST_CHECK_EQUAL(err, SCRIPT_ERR_PQ_SIGNATURE_VERIFY_FAILED);

    // Signing under the node's real context DOES verify: proves the failure
    // above is the context binding, not something else.
    std::vector<uint8_t> sig_ok;
    BOOST_REQUIRE(key.Sign(sig_ok, std::span<const uint8_t>(sighash.begin(), 32), node_ctx));
    err = SCRIPT_ERR_OK;
    BOOST_CHECK(verify(sig_ok, err));
    BOOST_CHECK_EQUAL(err, SCRIPT_ERR_OK);
}

BOOST_AUTO_TEST_CASE(pq_verify_sigop_cost_gated_on_activation)
{
    // A witness-v2 (ML-DSA-44) spend counts MLDSA44_SIGOP_COST sigops once the PQ
    // rule is active, and 0 before it, so pre-activation cost accounting is
    // unchanged while active blocks/txs are bounded by the existing sigop limits.
    // Shape-based (no real verify), so it runs with or without liboqs.
    std::vector<unsigned char> program(32, 0xAB);
    const CScript spk = CScript() << OP_2 << program;
    CScriptWitness wit;
    wit.stack.emplace_back(mldsa::SIG_SIZE, 0x11);
    wit.stack.emplace_back(mldsa::PUBKEY_SIZE, 0x22);

    const unsigned int base = SCRIPT_VERIFY_WITNESS | SCRIPT_VERIFY_P2SH;
    // Pre-activation: witness v2 still contributes nothing.
    BOOST_CHECK_EQUAL(CountWitnessSigOps(CScript(), spk, &wit, base), 0U);
    // Active: one PQ verify is charged the fixed sigop cost.
    BOOST_CHECK_EQUAL(CountWitnessSigOps(CScript(), spk, &wit, base | SCRIPT_VERIFY_MLDSA44),
                      static_cast<size_t>(MLDSA44_SIGOP_COST));
}

BOOST_AUTO_TEST_SUITE_END()
