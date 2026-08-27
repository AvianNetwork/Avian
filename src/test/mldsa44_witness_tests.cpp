// Copyright (c) 2024-present The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

// RIP-25 Phase 2: locks the ML-DSA-44 (witness v2) structural robustness rules of
// the consensus verifier (VerifyScript / VerifyWitnessProgram). These reject every
// malformed witness shape BEFORE the expensive ML-DSA verification, so a crafted
// input cannot reach liboqs with the wrong size or count, and the rejection paths
// cannot silently regress. The checks are shape-based (no real signature), so this
// suite runs with or without liboqs.
//
// The verifier's rule (doc/rip-25.md): witness v2 with a 32-byte program, once
// SCRIPT_VERIFY_MLDSA44 is active, requires the witness to be exactly
// [signature (2420 bytes), pubkey (1312 bytes)], the pubkey to hash (SHA256) to the
// program, and the signature to verify. Before activation the program is an unknown
// upgrade (anyone-can-spend), optionally discouraged.

#include <coins.h>
#include <crypto/mldsa.h>
#include <crypto/sha256.h>
#include <policy/policy.h>
#include <pqkey.h>
#include <primitives/transaction.h>
#include <script/interpreter.h>
#include <script/script.h>
#include <script/script_error.h>
#include <test/util/transaction_utils.h>
#include <uint256.h>

#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <vector>

BOOST_FIXTURE_TEST_SUITE(mldsa44_witness_tests, BasicTestingSetup)

namespace {

// SHA256(pubkey) — the witness program committed to a public key.
std::vector<unsigned char> ProgramFor(const std::vector<unsigned char>& pubkey)
{
    uint256 h;
    CSHA256().Write(pubkey.data(), pubkey.size()).Finalize(h.begin());
    return std::vector<unsigned char>(h.begin(), h.end());
}

// Run the real consensus verifier over an OP_2 <program> output spent by the given
// witness stack, and return the resulting ScriptError.
ScriptError VerifyV2(const std::vector<unsigned char>& program,
                     const std::vector<std::vector<unsigned char>>& witness_stack,
                     unsigned int flags)
{
    const CScript spk = CScript() << OP_2 << program;
    const CAmount amount = 100'000'000;
    const CTransaction txCredit{BuildCreditingTransaction(spk, amount)};
    CScriptWitness wit;
    wit.stack = witness_stack;
    const CTransaction tx{BuildSpendingTransaction(CScript(), wit, txCredit)};
    ScriptError err = SCRIPT_ERR_OK;
    VerifyScript(CScript(), spk, &tx.vin[0].scriptWitness, flags,
                 TransactionSignatureChecker(&tx, 0, amount, MissingDataBehavior::ASSERT_FAIL),
                 &err);
    return err;
}

} // namespace

BOOST_AUTO_TEST_CASE(witness_structure_rejections)
{
    const unsigned int flags = SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_WITNESS | SCRIPT_VERIFY_MLDSA44;
    const std::vector<unsigned char> pubkey(mldsa::PUBKEY_SIZE, 0xAB);
    const std::vector<unsigned char> sig(mldsa::SIG_SIZE, 0xCD);
    const std::vector<unsigned char> program = ProgramFor(pubkey);

    // Canonical shape passes every structural check and reaches verification; the
    // garbage signature then fails to verify (same result with or without liboqs).
    BOOST_CHECK_EQUAL(VerifyV2(program, {sig, pubkey}, flags), SCRIPT_ERR_PQ_SIGNATURE_VERIFY_FAILED);

    // Wrong number of witness elements — no trailing/extra or missing stuffing.
    BOOST_CHECK_EQUAL(VerifyV2(program, {sig, pubkey, {0x00}}, flags), SCRIPT_ERR_WITNESS_PROGRAM_MISMATCH);
    BOOST_CHECK_EQUAL(VerifyV2(program, {sig}, flags), SCRIPT_ERR_WITNESS_PROGRAM_MISMATCH);
    BOOST_CHECK_EQUAL(VerifyV2(program, {}, flags), SCRIPT_ERR_WITNESS_PROGRAM_MISMATCH);

    // Pubkey the wrong length (±1 byte) — no over/under-read into liboqs.
    {
        std::vector<unsigned char> pk = pubkey; pk.push_back(0x00);
        BOOST_CHECK_EQUAL(VerifyV2(program, {sig, pk}, flags), SCRIPT_ERR_PQ_PUBKEY_SIZE);
        pk = pubkey; pk.pop_back();
        BOOST_CHECK_EQUAL(VerifyV2(program, {sig, pk}, flags), SCRIPT_ERR_PQ_PUBKEY_SIZE);
    }

    // Signature the wrong length (±1 byte), incl. an appended sighash-type byte.
    {
        std::vector<unsigned char> s = sig; s.pop_back();
        BOOST_CHECK_EQUAL(VerifyV2(ProgramFor(pubkey), {s, pubkey}, flags), SCRIPT_ERR_PQ_SIGNATURE_SIZE);
        s = sig; s.push_back(0x41); // trailing SIGHASH_ALL|FORKID byte
        BOOST_CHECK_EQUAL(VerifyV2(ProgramFor(pubkey), {s, pubkey}, flags), SCRIPT_ERR_PQ_SIGNATURE_SIZE);
    }

    // Correctly-sized pubkey that does not hash to the program.
    {
        const std::vector<unsigned char> other(mldsa::PUBKEY_SIZE, 0x11);
        BOOST_CHECK_EQUAL(VerifyV2(program, {sig, other}, flags), SCRIPT_ERR_PQ_WITNESS_PROGRAM_MISMATCH);
    }
}

BOOST_AUTO_TEST_CASE(pre_activation_is_upgradable_anyonecanspend)
{
    const std::vector<unsigned char> pubkey(mldsa::PUBKEY_SIZE, 0xAB);
    const std::vector<unsigned char> sig(mldsa::SIG_SIZE, 0xCD);
    const std::vector<unsigned char> program = ProgramFor(pubkey);

    // Without SCRIPT_VERIFY_MLDSA44 the v2 program is an unknown witness upgrade:
    // it must succeed (anyone-can-spend / softfork-upgradable), even for a witness
    // that would be rejected once the rule is active.
    const unsigned int base = SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_WITNESS;
    BOOST_CHECK_EQUAL(VerifyV2(program, {sig, pubkey}, base), SCRIPT_ERR_OK);
    BOOST_CHECK_EQUAL(VerifyV2(program, {sig, pubkey, {0x00}}, base), SCRIPT_ERR_OK);

    // With DISCOURAGE_UPGRADABLE_WITNESS_PROGRAM (relay policy) it is flagged.
    BOOST_CHECK_EQUAL(VerifyV2(program, {sig, pubkey}, base | SCRIPT_VERIFY_DISCOURAGE_UPGRADABLE_WITNESS_PROGRAM),
                      SCRIPT_ERR_DISCOURAGE_UPGRADABLE_WITNESS_PROGRAM);
}

BOOST_AUTO_TEST_CASE(witness_standardness_relay_rule)
{
    // IsWitnessStandard (policy) must accept only the canonical v2 witness so a
    // malformed or padded v2 spend does not relay / bloat the mempool ahead of the
    // expensive verification. This mirrors the consensus structural rule above.
    CCoinsView dummy;
    CCoinsViewCache coins(&dummy);

    const std::vector<unsigned char> pubkey(mldsa::PUBKEY_SIZE, 0xAB);
    const std::vector<unsigned char> sig(mldsa::SIG_SIZE, 0xCD);
    const std::vector<unsigned char> program = ProgramFor(pubkey);
    const CScript spk = CScript() << OP_2 << program;
    const CTransaction txCredit{BuildCreditingTransaction(spk, 100'000'000)};
    AddCoins(coins, txCredit, /*nHeight=*/0);

    const auto spend = [&](const std::vector<std::vector<unsigned char>>& stack) {
        CScriptWitness wit;
        wit.stack = stack;
        return CTransaction{BuildSpendingTransaction(CScript(), wit, txCredit)};
    };

    // Canonical [sig, pubkey] is standard.
    BOOST_CHECK(IsWitnessStandard(spend({sig, pubkey}), coins));

    // Every malformed shape is non-standard.
    BOOST_CHECK(!IsWitnessStandard(spend({sig, pubkey, {0x00}}), coins)); // extra element
    BOOST_CHECK(!IsWitnessStandard(spend({sig}), coins));                 // too few elements
    {
        std::vector<unsigned char> pk = pubkey; pk.push_back(0x00);
        BOOST_CHECK(!IsWitnessStandard(spend({sig, pk}), coins));         // pubkey too long
    }
    {
        std::vector<unsigned char> s = sig; s.push_back(0x41);
        BOOST_CHECK(!IsWitnessStandard(spend({s, pubkey}), coins));       // signature too long
    }
}

BOOST_AUTO_TEST_SUITE_END()
