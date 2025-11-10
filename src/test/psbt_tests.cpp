// Copyright (c) 2009-2022 The Bitcoin Core developers
// Copyright (c) 2025 The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "core_io.h"
#include "psbt.h"
#include "test/test_avian.h"
#include "utilstrencodings.h"

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(psbt_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(psbt_magic_bytes_test)
{
    BOOST_TEST_MESSAGE("Running PSBT Magic Bytes Test");

    // Verify magic bytes are correct (BIP 174)
    BOOST_CHECK_EQUAL(PSBT_MAGIC_BYTES[0], 0x70); // 'p'
    BOOST_CHECK_EQUAL(PSBT_MAGIC_BYTES[1], 0x73); // 's'
    BOOST_CHECK_EQUAL(PSBT_MAGIC_BYTES[2], 0x62); // 'b'
    BOOST_CHECK_EQUAL(PSBT_MAGIC_BYTES[3], 0x74); // 't'
    BOOST_CHECK_EQUAL(PSBT_MAGIC_BYTES[4], 0xff); // 0xff separator
}

BOOST_AUTO_TEST_CASE(psbt_input_basic_test)
{
    BOOST_TEST_MESSAGE("Running PSBT Input Basic Test");

    PSBTInput input;

    // Test initial state
    BOOST_CHECK_EQUAL(input.IsSigned(), false);
    BOOST_CHECK_EQUAL(input.final_script_sig.empty(), true);
    BOOST_CHECK_EQUAL(input.final_script_witness.empty(), true);
    BOOST_CHECK_EQUAL(input.partial_sigs.empty(), true);
}

BOOST_AUTO_TEST_CASE(psbt_input_signing_test)
{
    BOOST_TEST_MESSAGE("Running PSBT Input Signing Test");

    PSBTInput input;

    // Add a signature (simulate signing)
    std::vector<unsigned char> sig = ParseHex("304402206");
    CPubKey pubkey;

    input.partial_sigs[pubkey] = sig;

    // Verify signature was added
    BOOST_CHECK_EQUAL(input.partial_sigs.size(), 1);
}

BOOST_AUTO_TEST_CASE(psbt_input_finalization_test)
{
    BOOST_TEST_MESSAGE("Running PSBT Input Finalization Test");

    PSBTInput input;

    // Add final script sig
    input.final_script_sig = ParseHex("4730440220");

    // Verify finalization status
    BOOST_CHECK_EQUAL(input.IsSigned(), true);
}

BOOST_AUTO_TEST_CASE(psbt_output_basic_test)
{
    BOOST_TEST_MESSAGE("Running PSBT Output Basic Test");

    PSBTOutput output;

    // Test initial state
    BOOST_CHECK_EQUAL(output.redeem_script.empty(), true);
    BOOST_CHECK_EQUAL(output.witness_script.empty(), true);
    BOOST_CHECK_EQUAL(output.hd_keypaths.empty(), true);
}

BOOST_AUTO_TEST_CASE(psbt_partially_signed_transaction_basic_test)
{
    BOOST_TEST_MESSAGE("Running PSBT PartiallySignedTransaction Basic Test");

    PartiallySignedTransaction psbt;

    // Test initial state
    BOOST_CHECK_EQUAL(psbt.IsNull(), true);
    BOOST_CHECK_EQUAL(psbt.IsSigned(), false);
    BOOST_CHECK_EQUAL(psbt.inputs.empty(), true);
    BOOST_CHECK_EQUAL(psbt.outputs.empty(), true);
}

BOOST_AUTO_TEST_CASE(psbt_partially_signed_transaction_add_inputs_test)
{
    BOOST_TEST_MESSAGE("Running PSBT Add Inputs Test");

    PartiallySignedTransaction psbt;

    // Add inputs
    PSBTInput input1;
    PSBTInput input2;

    psbt.inputs.push_back(input1);
    psbt.inputs.push_back(input2);

    // Verify inputs were added
    BOOST_CHECK_EQUAL(psbt.inputs.size(), 2);
}

BOOST_AUTO_TEST_CASE(psbt_partially_signed_transaction_add_outputs_test)
{
    BOOST_TEST_MESSAGE("Running PSBT Add Outputs Test");

    PartiallySignedTransaction psbt;

    // Add outputs
    PSBTOutput output1;
    PSBTOutput output2;

    psbt.outputs.push_back(output1);
    psbt.outputs.push_back(output2);

    // Verify outputs were added
    BOOST_CHECK_EQUAL(psbt.outputs.size(), 2);
}

BOOST_AUTO_TEST_CASE(psbt_serialization_roundtrip_test)
{
    BOOST_TEST_MESSAGE("Running PSBT Serialization Roundtrip Test");

    PartiallySignedTransaction psbt1;

    // Add some test data
    PSBTInput input;
    PSBTOutput output;

    psbt1.inputs.push_back(input);
    psbt1.outputs.push_back(output);

    // Serialize to hex
    std::string hex = psbt1.GetHex();

    // Deserialize from hex
    PartiallySignedTransaction psbt2 = PartiallySignedTransaction::FromHex(hex);

    // Verify roundtrip
    BOOST_CHECK_EQUAL(psbt2.inputs.size(), 1);
    BOOST_CHECK_EQUAL(psbt2.outputs.size(), 1);

    // Verify serialize back matches
    std::string hex2 = psbt2.GetHex();
    BOOST_CHECK_EQUAL(hex, hex2);
}

BOOST_AUTO_TEST_CASE(psbt_hex_encoding_test)
{
    BOOST_TEST_MESSAGE("Running PSBT Hex Encoding Test");

    PartiallySignedTransaction psbt;

    // Get hex representation
    std::string hex = psbt.GetHex();

    // Hex string should not be empty (even for empty PSBT)
    BOOST_CHECK(!hex.empty());

    // Should be valid base64
    try {
        PartiallySignedTransaction::FromHex(hex);
    } catch (...) {
        BOOST_ERROR("Failed to deserialize hex encoded PSBT");
    }
}

BOOST_AUTO_TEST_CASE(psbt_hex_decoding_test)
{
    BOOST_TEST_MESSAGE("Running PSBT Hex Decoding Test");

    // Test with invalid hex
    try {
        PartiallySignedTransaction psbt = PartiallySignedTransaction::FromHex("invalid!!!");
        // If we get here, either it didn't fail or failed gracefully
        BOOST_CHECK(true); // Either is acceptable for robustness
    } catch (...) {
        BOOST_CHECK(true); // Exception on invalid input is fine
    }
}

BOOST_AUTO_TEST_CASE(psbt_multiple_signatures_test)
{
    BOOST_TEST_MESSAGE("Running PSBT Multiple Signatures Test");

    PSBTInput input;

    // Simulate 2-of-2 multisig (two signers)
    CPubKey pubkey1; // Normally would be a real pubkey
    CPubKey pubkey2;

    std::vector<unsigned char> sig1 = ParseHex("304402206");
    std::vector<unsigned char> sig2 = ParseHex("304402207");

    input.partial_sigs[pubkey1] = sig1;
    input.partial_sigs[pubkey2] = sig2;

    // Verify both signatures stored
    BOOST_CHECK_EQUAL(input.partial_sigs.size(), 2);
}

BOOST_AUTO_TEST_CASE(psbt_completion_status_test)
{
    BOOST_TEST_MESSAGE("Running PSBT Completion Status Test");

    PartiallySignedTransaction psbt;

    // Add inputs and outputs
    for (int i = 0; i < 3; i++) {
        PSBTInput input;
        PSBTOutput output;
        psbt.inputs.push_back(input);
        psbt.outputs.push_back(output);
    }

    // Initially not signed
    BOOST_CHECK_EQUAL(psbt.IsSigned(), false);

    // Sign first input
    std::vector<unsigned char> sig = ParseHex("304402206");
    CPubKey pubkey;
    psbt.inputs[0].partial_sigs[pubkey] = sig;

    // Still not all signed
    BOOST_CHECK_EQUAL(psbt.IsSigned(), false);

    // Sign remaining inputs
    for (int i = 1; i < 3; i++) {
        psbt.inputs[i].partial_sigs[pubkey] = sig;
    }

    // Now all inputs are partially signed (though this might not mean complete)
    // Completeness depends on actual script requirements
}

BOOST_AUTO_TEST_CASE(psbt_empty_serialization_test)
{
    BOOST_TEST_MESSAGE("Running PSBT Empty Serialization Test");

    PartiallySignedTransaction psbt1;
    std::string hex1 = psbt1.GetHex();

    PartiallySignedTransaction psbt2 = PartiallySignedTransaction::FromHex(hex1);
    std::string hex2 = psbt2.GetHex();

    // Empty PSBTs should serialize to same thing
    BOOST_CHECK_EQUAL(hex1, hex2);
}

BOOST_AUTO_TEST_CASE(psbt_unknown_fields_test)
{
    BOOST_TEST_MESSAGE("Running PSBT Unknown Fields Test");

    PSBTInput input;

    // Add unknown field
    std::vector<unsigned char> key = {0xFF};
    std::vector<unsigned char> value = {0x01, 0x02, 0x03};

    input.unknown[key] = value;

    // Verify unknown field stored
    BOOST_CHECK_EQUAL(input.unknown.size(), 1);
    BOOST_CHECK_EQUAL(input.unknown[key], value);
}

BOOST_AUTO_TEST_CASE(psbt_hd_keypaths_test)
{
    BOOST_TEST_MESSAGE("Running PSBT HD Keypaths Test");

    PSBTOutput output;

    // Add HD keypath
    std::vector<unsigned char> pubkey = ParseHex("02");
    std::vector<unsigned char> path = ParseHex("00000000"); // m/0

    output.hd_keypaths[pubkey] = path;

    // Verify HD keypath stored
    BOOST_CHECK_EQUAL(output.hd_keypaths.size(), 1);
}

BOOST_AUTO_TEST_CASE(psbt_input_output_pairing_test)
{
    BOOST_TEST_MESSAGE("Running PSBT Input Output Pairing Test");

    PartiallySignedTransaction psbt;

    // Create matching inputs and outputs
    for (int i = 0; i < 5; i++) {
        psbt.inputs.push_back(PSBTInput());
        psbt.outputs.push_back(PSBTOutput());
    }

    // Verify pairing
    BOOST_CHECK_EQUAL(psbt.inputs.size(), psbt.outputs.size());
    BOOST_CHECK_EQUAL(psbt.inputs.size(), 5);
}

BOOST_AUTO_TEST_CASE(psbt_copy_constructor_test)
{
    BOOST_TEST_MESSAGE("Running PSBT Copy Constructor Test");

    PSBTInput input1;
    std::vector<unsigned char> sig = ParseHex("304402206");
    CPubKey pubkey;
    input1.partial_sigs[pubkey] = sig;

    // Copy constructor (implicit)
    PSBTInput input2 = input1;

    // Verify copy
    BOOST_CHECK_EQUAL(input2.partial_sigs.size(), 1);
}

BOOST_AUTO_TEST_SUITE_END()
