// Copyright (c) 2026-present The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Every ECDSA signature on this chain ends in a hash type with SIGHASH_FORKID set.
// A PSBT has to carry, compare and verify such signatures.

#include <addresstype.h>
#include <common/types.h>
#include <consensus/amount.h>
#include <key.h>
#include <primitives/transaction.h>
#include <psbt.h>
#include <pubkey.h>
#include <script/interpreter.h>
#include <script/script.h>
#include <script/signingprovider.h>
#include <streams.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <optional>
#include <vector>

using common::PSBTError;

namespace {
const int ALL_FORKID{SIGHASH_ALL | SIGHASH_FORKID};
const int SINGLE_FORKID{SIGHASH_SINGLE | SIGHASH_FORKID};

struct Fixture : BasicTestingSetup {
    CKey key{GenerateRandomKey()};
    FlatSigningProvider provider;
    CTransactionRef funding;

    Fixture()
    {
        const CPubKey pubkey{key.GetPubKey()};
        provider.keys[pubkey.GetID()] = key;
        provider.pubkeys[pubkey.GetID()] = pubkey;

        CMutableTransaction prev;
        prev.vin.emplace_back(COutPoint{Txid::FromUint256(uint256::ONE), 0});
        for (int i{0}; i < 2; ++i) prev.vout.emplace_back(COIN, GetScriptForDestination(PKHash{pubkey}));
        funding = MakeTransactionRef(prev);
    }

    //! Spends `inputs` coins of the key to `outputs` outputs.
    PartiallySignedTransaction Psbt(unsigned int inputs, unsigned int outputs) const
    {
        CMutableTransaction tx;
        for (unsigned int i{0}; i < inputs; ++i) tx.vin.emplace_back(COutPoint{funding->GetHash(), i});
        for (unsigned int i{0}; i < outputs; ++i) tx.vout.emplace_back(COIN / 2, CScript{} << OP_TRUE);
        PartiallySignedTransaction psbt{tx};
        for (PSBTInput& input : psbt.inputs) input.non_witness_utxo = funding;
        return psbt;
    }

    PSBTError Sign(PartiallySignedTransaction& psbt, int index, std::optional<int> sighash, bool finalize) const
    {
        const PrecomputedTransactionData txdata{PrecomputePSBTData(psbt)};
        return SignPSBTInput(provider, psbt, index, &txdata, sighash, nullptr, finalize);
    }
};

PartiallySignedTransaction RoundTrip(const PartiallySignedTransaction& psbt)
{
    DataStream stream;
    stream << psbt;
    PartiallySignedTransaction decoded;
    stream >> decoded;
    return decoded;
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(psbt_forkid_tests, Fixture)

// The second signer of a multisig reads the first one's partial signature.
BOOST_AUTO_TEST_CASE(a_partial_signature_survives_a_round_trip)
{
    PartiallySignedTransaction psbt{Psbt(1, 1)};
    BOOST_CHECK(Sign(psbt, 0, std::nullopt, /*finalize=*/false) == PSBTError::OK);
    BOOST_REQUIRE_EQUAL(psbt.inputs[0].partial_sigs.size(), 1U);
    BOOST_CHECK_EQUAL(psbt.inputs[0].partial_sigs.begin()->second.second.back(), ALL_FORKID);
    // The default type is not written to the field
    BOOST_CHECK(!psbt.inputs[0].sighash_type);

    PartiallySignedTransaction decoded;
    BOOST_REQUIRE_NO_THROW(decoded = RoundTrip(psbt));
    BOOST_REQUIRE_EQUAL(decoded.inputs[0].partial_sigs.size(), 1U);

    // Asked for nothing in particular, the next signer agrees with what is there
    BOOST_CHECK(Sign(decoded, 0, std::nullopt, /*finalize=*/false) == PSBTError::OK);
    BOOST_CHECK(Sign(decoded, 0, SIGHASH_ALL, /*finalize=*/false) == PSBTError::OK);
    BOOST_CHECK(Sign(decoded, 0, SINGLE_FORKID, /*finalize=*/false) == PSBTError::SIGHASH_MISMATCH);

    // A signature without the fork id is one the chain refuses
    PartiallySignedTransaction legacy{psbt};
    legacy.inputs[0].partial_sigs.begin()->second.second.back() = SIGHASH_ALL;
    BOOST_CHECK_THROW(RoundTrip(legacy), std::ios_base::failure);
}

BOOST_AUTO_TEST_CASE(the_sighash_field_is_compared_with_the_fork_id)
{
    for (const int field : {ALL_FORKID, int{SIGHASH_ALL}}) {
        PartiallySignedTransaction psbt{Psbt(1, 1)};
        psbt.inputs[0].sighash_type = field;
        BOOST_CHECK(Sign(psbt, 0, std::nullopt, /*finalize=*/false) == PSBTError::OK);
        BOOST_CHECK(Sign(psbt, 0, ALL_FORKID, /*finalize=*/false) == PSBTError::OK);
    }

    // Only ALL and ALL|FORKID are the same request. Any other type in the field has to
    // be asked for.
    for (const int field : {SINGLE_FORKID, int{SIGHASH_SINGLE}, int{SIGHASH_NONE}, SIGHASH_NONE | SIGHASH_FORKID,
                            ALL_FORKID | SIGHASH_ANYONECANPAY, SIGHASH_ALL | SIGHASH_ANYONECANPAY}) {
        PartiallySignedTransaction psbt{Psbt(1, 1)};
        psbt.inputs[0].sighash_type = field;
        BOOST_CHECK(Sign(psbt, 0, std::nullopt, /*finalize=*/false) == PSBTError::SIGHASH_MISMATCH);
        BOOST_CHECK(Sign(psbt, 0, ALL_FORKID, /*finalize=*/false) == PSBTError::SIGHASH_MISMATCH);
        BOOST_CHECK(psbt.inputs[0].partial_sigs.empty());
    }

    // Another type is written to the field, with the fork id
    PartiallySignedTransaction other{Psbt(1, 1)};
    BOOST_CHECK(Sign(other, 0, SIGHASH_SINGLE, /*finalize=*/false) == PSBTError::OK);
    BOOST_REQUIRE(other.inputs[0].sighash_type);
    BOOST_CHECK_EQUAL(*other.inputs[0].sighash_type, SINGLE_FORKID);
    BOOST_CHECK_EQUAL(other.inputs[0].partial_sigs.begin()->second.second.back(), SINGLE_FORKID);
}

BOOST_AUTO_TEST_CASE(a_finalized_input_verifies)
{
    PartiallySignedTransaction psbt{Psbt(1, 1)};
    BOOST_CHECK(Sign(psbt, 0, std::nullopt, /*finalize=*/true) == PSBTError::OK);
    BOOST_REQUIRE(!psbt.inputs[0].final_script_sig.empty());
    const PrecomputedTransactionData txdata{PrecomputePSBTData(psbt)};
    BOOST_CHECK(PSBTInputSignedAndVerified(psbt, 0, &txdata));

    // And one whose signature is not for this transaction does not
    psbt.tx->vout[0].nValue -= 1;
    const PrecomputedTransactionData changed{PrecomputePSBTData(psbt)};
    BOOST_CHECK(!PSBTInputSignedAndVerified(psbt, 0, &changed));
}

// SIGHASH_SINGLE with no output at the input's index signs the constant 1 for a
// non-witness input. That signature would spend any other coin of the key.
BOOST_AUTO_TEST_CASE(single_is_not_signed_without_a_matching_output)
{
    PartiallySignedTransaction psbt{Psbt(2, 1)};
    BOOST_CHECK(Sign(psbt, 1, SINGLE_FORKID, /*finalize=*/true) == PSBTError::INCOMPLETE);
    BOOST_CHECK(psbt.inputs[1].partial_sigs.empty());
    BOOST_CHECK(psbt.inputs[1].final_script_sig.empty());
    // Its field is left alone, so the input can still be signed with another type
    BOOST_CHECK(!psbt.inputs[1].sighash_type);
    BOOST_CHECK(Sign(psbt, 1, std::nullopt, /*finalize=*/false) == PSBTError::OK);
    BOOST_CHECK_EQUAL(psbt.inputs[1].partial_sigs.size(), 1U);

    BOOST_CHECK(Sign(psbt, 0, SINGLE_FORKID, /*finalize=*/false) == PSBTError::OK);
    BOOST_CHECK_EQUAL(psbt.inputs[0].partial_sigs.size(), 1U);
}

// The guard is on making a signature. One that is already in the PSBT, made by
// whoever holds the key, is put in place by the finalizer like any other.
BOOST_AUTO_TEST_CASE(a_single_signature_that_is_there_is_still_finalized)
{
    PartiallySignedTransaction psbt{Psbt(2, 1)};
    std::vector<unsigned char> signature;
    BOOST_REQUIRE(key.Sign(uint256::ONE, signature));
    signature.push_back(SINGLE_FORKID);
    const CPubKey pubkey{key.GetPubKey()};
    psbt.inputs[1].partial_sigs[pubkey.GetID()] = {pubkey, signature};
    psbt.inputs[1].sighash_type = SINGLE_FORKID;

    FlatSigningProvider public_only;
    public_only.pubkeys[pubkey.GetID()] = pubkey;
    const PrecomputedTransactionData txdata{PrecomputePSBTData(psbt)};
    BOOST_CHECK(SignPSBTInput(public_only, psbt, 1, &txdata, SINGLE_FORKID, nullptr, /*finalize=*/true) == PSBTError::OK);
    BOOST_CHECK(!psbt.inputs[1].final_script_sig.empty());
    BOOST_CHECK(PSBTInputSignedAndVerified(psbt, 1, &txdata));
}

BOOST_AUTO_TEST_SUITE_END()
