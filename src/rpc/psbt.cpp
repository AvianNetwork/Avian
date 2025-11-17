// Copyright (c) 2009-2022 The Bitcoin Core developers
// Copyright (c) 2025 The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "../psbt.h"
#include "base58.h"
#include "core_io.h"
#include "keystore.h"
#include "random.h"
#include "rpc/server.h"
#include "script/sign.h"
#include "util.h"
#include "utilstrencodings.h"
#include "validation.h"
#include "wallet/coincontrol.h"
#include "wallet/psbtwallet.h"
#include "wallet/rpcwallet.h"
#include "wallet/wallet.h"

#include <univalue.h>

/**
 * Decode a base64-encoded PSBT string and deserialize it into a PartiallySignedTransaction.
 * BIP 174 specifies PSBTs are encoded in base64 (not hex).
 *
 * @param psbtx Output: The decoded PartiallySignedTransaction
 * @param psbt_str Input: Base64-encoded PSBT string
 * @param error Output: Error message if decoding fails
 * @return true if successful, false otherwise
 */
bool DecodePSBT(PartiallySignedTransaction& psbtx, const std::string& psbt_str, std::string& error)
{
    // Decode base64 to binary
    std::vector<unsigned char> psbt_data;
    try {
        psbt_data = DecodeBase64(psbt_str.c_str());
        if (psbt_data.empty()) {
            error = "Invalid base64 encoding";
            return false;
        }
    } catch (const std::exception& e) {
        error = std::string("Base64 decoding failed: ") + e.what();
        return false;
    }

    // Now deserialize the PSBT from the decoded bytes
    try {
        CDataStream stream(psbt_data, SER_NETWORK, PROTOCOL_VERSION);
        stream >> psbtx;
    } catch (const std::ios_base::failure& e) {
        error = std::string("PSBT deserialization failed: ") + e.what();
        return false;
    } catch (const std::exception& e) {
        error = std::string("PSBT deserialization failed: ") + e.what();
        return false;
    }

    return true;
}

UniValue createpsbt(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() < 2 || request.params.size() > 4)
        throw std::runtime_error(
            "createpsbt [{\"txid\":\"id\",\"vout\":n,\"sequence\":n},...] [{\"address\":amount,...},{\"data\":\"hex\"},...] ( locktime replaceable )\n"
            "\nCreates a transaction in Partially Signed Transaction format.\n"
            "Implements the Creator role.\n"
            "\nIMPORTANT: Remember to include a change output when input amount exceeds recipient amount + fee.\n"
            "Any amount not sent to recipient(s) or data output will be treated as a mining fee.\n"
            "\nArguments:\n"
            "1. inputs                      (json array, required) The inputs\n"
            "     [\n"
            "       {\n"
            "         \"txid\": \"hex\",        (string, required) The transaction id\n"
            "         \"vout\": n,              (numeric, required) The output number\n"
            "         \"sequence\": n,          (numeric, optional, default=depends on replaceable) The sequence number\n"
            "       },\n"
            "       ...\n"
            "     ]\n"
            "2. outputs                     (json array, required) The outputs (includes recipients AND change).\n"
            "                               Each key may only appear once, i.e. there can only be one 'data' output, and no address may be duplicated.\n"
            "                               At least one output of either type must be specified.\n"
            "                               For compatibility reasons, a dictionary, which holds the key-value pairs directly, is also\n"
            "                               accepted as second parameter.\n"
            "     [\n"
            "       {\n"
            "         \"address\": amount,      (numeric or string, required) A key-value pair. The key (string) is the avian address, the value (float or string) is the amount in " +
            CURRENCY_UNIT + "\n"
                            "         ...\n"
                            "       },\n"
                            "       {\n"
                            "         \"data\": \"hex\",        (string, optional) A key-value pair. The key must be \"data\", the value is hex-encoded data\n"
                            "       },\n"
                            "       ...\n"
                            "     ]\n"
                            "3. locktime                    (numeric, optional, default=0) Raw locktime. Non-0 value also locktime-activates inputs\n"
                            "4. replaceable                 (boolean, optional, default=true) Marks this transaction as BIP125-replaceable.\n"
                            "                               Allows this transaction to be replaced by a transaction with higher fees. If provided, it is an error if explicit sequence numbers are incompatible.\n"
                            "\nResult:\n"
                            "  \"psbt\"    (string)  The resulting raw transaction (base64-encoded string)\n"
                            "\nExamples:\n" +
            HelpExampleCli("createpsbt", "\"[{\\\"txid\\\":\\\"myid\\\",\\\"vout\\\":0}]\" \"[{\\\"data\\\":\\\"00010203\\\"}]\"") +
            HelpExampleCli("createpsbt", "\"[{\\\"txid\\\":\\\"myid\\\",\\\"vout\\\":0}]\" \"[{\\\"address\\\":\\\"RRecipient1Addr\\\",\\\"amount\\\":0.5},{\\\"address\\\":\\\"RRecipient2Addr\\\",\\\"amount\\\":9.499}]\""));

    // Use RPCTypeCheck to parse param[0] and param[2], [3] - but for param[1], handle separately
    // because it can be either VARR or VOBJ
    if (request.params.size() < 2 || request.params.size() > 4) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Wrong number of parameters");
    }

    // Manually parse the JSON strings for inputs and outputs since we need flexible typing for outputs
    UniValue inputs_uv;
    UniValue outputs_uv;

    if (!inputs_uv.read(request.params[0].get_str())) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid inputs JSON");
    }

    if (!inputs_uv.isArray()) {
        throw JSONRPCError(RPC_TYPE_ERROR, "inputs must be an array");
    }

    if (!outputs_uv.read(request.params[1].get_str())) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid outputs JSON");
    }

    if (!outputs_uv.isArray() && !outputs_uv.isObject()) {
        throw JSONRPCError(RPC_TYPE_ERROR, "outputs must be an array or object");
    }

    // Now validate and parse remaining params
    int nLocktime = 0;
    if (request.params.size() > 2 && !request.params[2].isNull()) {
        nLocktime = request.params[2].get_int();
        if (nLocktime < 0 || nLocktime > 0xffffffff) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Locktime out of range");
        }
    }

    bool rbf = true;
    if (request.params.size() > 3 && !request.params[3].isNull()) {
        rbf = request.params[3].get_bool();
    }

    // Create transaction
    CMutableTransaction mtx;
    mtx.nVersion = 2;
    mtx.nLockTime = (unsigned int)nLocktime;

    // Add inputs
    // Use getValues() to get the array elements
    const std::vector<UniValue>& inputs = inputs_uv.getValues();
    for (size_t idx = 0; idx < inputs.size(); idx++) {
        const UniValue& input = inputs[idx];
        if (!input.isObject()) {
            throw JSONRPCError(RPC_TYPE_ERROR, "Invalid type in inputs");
        }

        // Get txid, vout, and sequence
        std::string txid_str = "";
        int nOutput = -1;
        uint32_t nSequence = rbf ? 0xfffffffd : 0xffffffff; // BIP125 RBF sequence
        if (nLocktime > 0) nSequence = 0xfffffffe;          // Require locktime

        // Safely extract fields using find_value() function instead of getKeys/getValues
        const UniValue& txid_val = find_value(input, "txid");
        if (!txid_val.isNull()) {
            txid_str = txid_val.get_str();
        }

        const UniValue& vout_val = find_value(input, "vout");
        if (!vout_val.isNull()) {
            nOutput = vout_val.get_int();
        }

        const UniValue& seq_val = find_value(input, "sequence");
        if (!seq_val.isNull()) {
            nSequence = seq_val.get_int();
        }

        if (txid_str.empty() || nOutput < 0) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Missing or invalid txid or vout");
        }

        // Check sequence is compatible with replaceable flag
        if (rbf && nSequence > 0xfffffffd) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Explicit sequence number is incompatible with replaceable=true");
        }

        uint256 txid(uint256S(txid_str));
        mtx.vin.push_back(CTxIn(COutPoint(txid, nOutput), CScript(), nSequence));
    }

    // Add outputs
    bool data_key_seen = false;

    if (outputs_uv.isArray()) {
        // Array format: [{"address":"...", "amount":...}, {"data":"hex"}]
        // Use getValues() to get array elements
        const std::vector<UniValue>& outputs_array = outputs_uv.getValues();
        for (size_t idx = 0; idx < outputs_array.size(); idx++) {
            const UniValue& output = outputs_array[idx];
            if (!output.isObject()) {
                throw JSONRPCError(RPC_TYPE_ERROR, "Invalid type in outputs array");
            }

            // Check if this is a data output using find_value
            const UniValue& data_val = find_value(output, "data");
            bool is_data = !data_val.isNull();

            if (is_data) {
                if (data_key_seen) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER, "Multiple data outputs are not allowed");
                }
                data_key_seen = true;

                // Process data output
                std::string hex_data = data_val.get_str();
                std::vector<unsigned char> data = ParseHex(hex_data);
                CScript scriptPubKey = CScript() << OP_RETURN << data;
                mtx.vout.push_back(CTxOut(0, scriptPubKey));
            } else {
                // Process address output
                const UniValue& address_val = find_value(output, "address");
                const UniValue& amount_val = find_value(output, "amount");

                if (address_val.isNull()) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER, "Missing address in output");
                }

                std::string address = address_val.get_str();
                CAmount nAmount = 0;
                if (!amount_val.isNull()) {
                    nAmount = AmountFromValue(amount_val);
                }

                CTxDestination destination = DecodeDestination(address);
                if (!IsValidDestination(destination)) {
                    throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, std::string("Invalid Avian address: ") + address);
                }

                if (nAmount < 0)
                    throw JSONRPCError(RPC_TYPE_ERROR, "Invalid amount");

                CScript scriptPubKey = GetScriptForDestination(destination);
                mtx.vout.push_back(CTxOut(nAmount, scriptPubKey));
            }
        }
    } else {
        // Object format: {"address": amount, "address2": amount2, "data": "hex"}
        std::vector<std::string> keys = outputs_uv.getKeys();
        for (const std::string& key : keys) {
            if (key == "data") {
                // Data output
                if (data_key_seen) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER, "Multiple data outputs are not allowed");
                }
                data_key_seen = true;

                std::string hex_data = outputs_uv[key].get_str();
                std::vector<unsigned char> data = ParseHex(hex_data);
                CScript scriptPubKey = CScript() << OP_RETURN << data;
                mtx.vout.push_back(CTxOut(0, scriptPubKey));
            } else {
                // Address output
                CTxDestination destination = DecodeDestination(key);
                if (!IsValidDestination(destination)) {
                    throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, std::string("Invalid Avian address: ") + key);
                }

                CAmount nAmount = AmountFromValue(outputs_uv[key]);
                if (nAmount < 0)
                    throw JSONRPCError(RPC_TYPE_ERROR, "Invalid amount");

                CScript scriptPubKey = GetScriptForDestination(destination);
                mtx.vout.push_back(CTxOut(nAmount, scriptPubKey));
            }
        }
    }

    if (mtx.vout.empty()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "At least one output must be specified");
    }

    // Create PSBT
    PartiallySignedTransaction psbtx(mtx);

    // Encode as base64 per BIP 174
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << psbtx;
    std::string ssStr(ss.str());
    return UniValue(EncodeBase64(ssStr));
}

UniValue converttopsbt(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() < 1 || request.params.size() > 3)
        throw std::runtime_error(
            "converttopsbt \"hexstring\" ( permitsigdata iswitness )\n"
            "\nConverts a network serialized transaction to a PSBT.\n"
            "This should be used only with createrawtransaction and fundrawtransaction.\n"
            "createpsbt and walletcreatefundedpsbt should be used for new applications.\n"
            "\nArguments:\n"
            "1. hexstring           (string, required) The hex string of a raw transaction\n"
            "2. permitsigdata       (boolean, optional, default=false) If true, any signatures in the input will be discarded and conversion\n"
            "                       will continue. If false, RPC will fail if any signatures are present.\n"
            "3. iswitness           (boolean, optional, default=heuristic) Whether the transaction is a serialized witness transaction.\n"
            "                       If not present, heuristic tests will be used in decoding.\n"
            "                       If true, only witness deserialization will be tried.\n"
            "                       If false, only non-witness deserialization will be tried.\n"
            "\nResult:\n"
            "  \"psbt\"    (string)  The resulting raw transaction (base64-encoded string)\n"
            "\nExamples:\n" +
            HelpExampleCli("createrawtransaction", "\"[{\\\"txid\\\":\\\"myid\\\",\\\"vout\\\":0}]\" \"{\\\"RRecipientAddr\\\":0.5}\"") +
            HelpExampleCli("converttopsbt", "\"rawtransactionhex\""));

    // Decode hex string
    CMutableTransaction tx;
    bool permit_sig_data = false;
    bool witness_specified = false;
    bool is_witness = false;

    if (request.params.size() > 1 && !request.params[1].isNull()) {
        permit_sig_data = request.params[1].get_bool();
    }

    if (request.params.size() > 2 && !request.params[2].isNull()) {
        witness_specified = true;
        is_witness = request.params[2].get_bool();
    }

    const bool try_no_witness = witness_specified ? !is_witness : true;

    if (!DecodeHexTx(tx, request.params[0].get_str(), try_no_witness)) {
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "TX decode failed");
    }

    // Remove all scriptSigs and scriptWitnesses from inputs
    for (CTxIn& input : tx.vin) {
        if ((!input.scriptSig.empty() || !input.scriptWitness.IsNull()) && !permit_sig_data) {
            throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "Inputs must not have scriptSigs and scriptWitnesses");
        }
        input.scriptSig.clear();
        input.scriptWitness.SetNull();
    }

    // Make a blank PSBT
    PartiallySignedTransaction psbtx;
    psbtx.tx = tx;
    for (unsigned int i = 0; i < tx.vin.size(); ++i) {
        psbtx.inputs.emplace_back();
    }
    for (unsigned int i = 0; i < tx.vout.size(); ++i) {
        psbtx.outputs.emplace_back();
    }

    // Serialize the PSBT
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << psbtx;

    return EncodeBase64(std::string(ss.begin(), ss.end()));
}

UniValue combinepsbt(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() < 1)
        throw std::runtime_error(
            "combinepsbt [\"psbt\",...]\n"
            "\nCombine multiple partially signed Avian transactions into one transaction.\n"
            "Implements the Combiner role.\n"
            "\nArguments:\n"
            "1. txs                         (json array or stringified array, required) The base64 strings of partially signed transactions\n"
            "     [\n"
            "       \"psbt\",                 (string) A base64 string of a PSBT\n"
            "       ...\n"
            "     ]\n"
            "\nResult:\n"
            "  \"psbt\"    (string)  The base64-encoded combined PSBT\n"
            "\nExamples:\n" +
            HelpExampleCli("combinepsbt", "\"[\\\"mybase64_1\\\", \\\"mybase64_2\\\", \\\"mybase64_3\\\"]\""));

    // Parse array of PSBT strings
    UniValue txs_uv = request.params[0];
    if (txs_uv.isStr()) {
        UniValue txs_parsed;
        if (!txs_parsed.read(txs_uv.get_str()) || !txs_parsed.isArray()) {
            throw JSONRPCError(RPC_TYPE_ERROR, "PSBTs must be an array");
        }
        txs_uv = txs_parsed;
    }

    if (!txs_uv.isArray()) {
        throw JSONRPCError(RPC_TYPE_ERROR, "PSBTs must be an array");
    }

    if (txs_uv.size() == 0) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "PSBTs array cannot be empty");
    }

    // Decode all PSBTs
    std::vector<PartiallySignedTransaction> psbts;
    for (unsigned int i = 0; i < txs_uv.size(); ++i) {
        std::string psbt_str = txs_uv[i].get_str();

        // Decode from base64
        std::vector<unsigned char> psbt_data = DecodeBase64(psbt_str.c_str());
        if (psbt_data.empty() && !psbt_str.empty()) {
            throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "PSBT must be base64 encoded");
        }

        // Deserialize PSBT
        PartiallySignedTransaction psbt;
        try {
            CDataStream ss(psbt_data, SER_NETWORK, PROTOCOL_VERSION);
            ss >> psbt;
        } catch (const std::exception& e) {
            throw JSONRPCError(RPC_DESERIALIZATION_ERROR, strprintf("PSBT decode failed: %s", e.what()));
        }

        psbts.push_back(psbt);
    }

    // Combine all PSBTs
    PartiallySignedTransaction merged_psbt;
    if (!CombinePSBTs(merged_psbt, psbts)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "PSBTs not compatible (different transactions or conflicting data)");
    }

    // Serialize and encode result
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << merged_psbt;

    return EncodeBase64(std::string(ss.begin(), ss.end()));
}

UniValue joinpsbts(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() < 1)
        throw std::runtime_error(
            "joinpsbts [\"psbt\",...]\n"
            "\nJoin multiple distinct Partially Signed Bitcoin Transactions into one transaction.\n"
            "Inputs and outputs are combined from all PSBTs.\n"
            "This is useful for joining independent PSBTs that have different inputs/outputs.\n"
            "\nArguments:\n"
            "1. txs                         (json array or stringified array, required) The base64 strings of PSBTs to join\n"
            "     [\n"
            "       \"psbt\",                 (string) A base64 string of a PSBT\n"
            "       ...\n"
            "     ]\n"
            "\nResult:\n"
            "  \"psbt\"    (string)  The base64-encoded joined PSBT\n"
            "\nExamples:\n" +
            HelpExampleCli("joinpsbts", "\"[\\\"mybase64_1\\\", \\\"mybase64_2\\\"]\""));

    // Parse array of PSBT strings
    UniValue txs_uv = request.params[0];
    if (txs_uv.isStr()) {
        UniValue txs_parsed;
        if (!txs_parsed.read(txs_uv.get_str()) || !txs_parsed.isArray()) {
            throw JSONRPCError(RPC_TYPE_ERROR, "PSBTs must be an array");
        }
        txs_uv = txs_parsed;
    }

    if (!txs_uv.isArray()) {
        throw JSONRPCError(RPC_TYPE_ERROR, "PSBTs must be an array");
    }

    if (txs_uv.size() < 2) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Need at least 2 PSBTs to join");
    }

    // Decode all PSBTs
    std::vector<PartiallySignedTransaction> psbts;
    for (unsigned int i = 0; i < txs_uv.size(); ++i) {
        std::string psbt_str = txs_uv[i].get_str();

        // Decode from base64
        std::vector<unsigned char> psbt_data = DecodeBase64(psbt_str.c_str());
        if (psbt_data.empty() && !psbt_str.empty()) {
            throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "PSBT must be base64 encoded");
        }

        PartiallySignedTransaction psbt;
        try {
            CDataStream ss(psbt_data, SER_NETWORK, PROTOCOL_VERSION);
            ss >> psbt;
        } catch (const std::exception& e) {
            throw JSONRPCError(RPC_DESERIALIZATION_ERROR, strprintf("PSBT decode failed: %s", e.what()));
        }

        psbts.push_back(psbt);
    }

    // Create a new blank transaction with version from first PSBT
    CMutableTransaction tx;
    tx.nVersion = psbts[0].tx.nVersion;
    tx.nLockTime = psbts[0].tx.nLockTime;

    // Find highest version and lowest locktime
    for (size_t i = 1; i < psbts.size(); ++i) {
        if (psbts[i].tx.nVersion > tx.nVersion) {
            tx.nVersion = psbts[i].tx.nVersion;
        }
        if (psbts[i].tx.nLockTime < tx.nLockTime) {
            tx.nLockTime = psbts[i].tx.nLockTime;
        }
    }

    // Create blank PSBT with this transaction
    PartiallySignedTransaction result_psbt;
    result_psbt.tx = tx;

    // Add all inputs and outputs from all PSBTs
    for (size_t i = 0; i < psbts.size(); ++i) {
        const PartiallySignedTransaction& psbt = psbts[i];

        // Add each input
        for (size_t j = 0; j < psbt.inputs.size(); ++j) {
            try {
                result_psbt.AddInput(psbt.tx.vin[j]);
                // Copy PSBT input metadata
                result_psbt.inputs.back().Merge(psbt.inputs[j]);
            } catch (const std::exception& e) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Cannot add input: %s", e.what()));
            }
        }

        // Add each output
        for (size_t j = 0; j < psbt.outputs.size(); ++j) {
            result_psbt.AddOutput(psbt.tx.vout[j]);
            // Copy PSBT output metadata
            result_psbt.outputs.back().Merge(psbt.outputs[j]);
        }
    }

    // Serialize and encode result
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << result_psbt;

    return EncodeBase64(std::string(ss.begin(), ss.end()));
}

UniValue walletcreatefundedpsbt(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() < 1 || request.params.size() > 4)
        throw std::runtime_error(
            "walletcreatefundedpsbt \"outputs\" ( locktime options )\n"
            "\nCreates and funds a transaction in Partially Signed Transaction format.\n"
            "Implements the Creator and Updater roles.\n"
            "Wallet automatically selects coins to pay outputs, adds change, and estimates fees.\n"
            "\nArguments:\n"
            "1. outputs                     (json array, required) The outputs (recipients only, no change needed).\n"
            "     [\n"
            "       {\n"
            "         \"address\": amount,      (numeric or string, required) A key-value pair. The key (string) is the avian address, the value (float or string) is the amount in " +
            CURRENCY_UNIT + "\n"
                            "       },\n"
                            "       {\n"
                            "         \"data\": \"hex\"         (string, optional) A key-value pair. The key must be \"data\", the value is hex-encoded data for OP_RETURN\n"
                            "       }\n"
                            "     ]\n"
                            "2. locktime                    (numeric, optional, default=0) Raw locktime. Non-0 value also locktime-activates inputs\n"
                            "3. options                     (json object, optional)\n"
                            "     {\n"
                            "       \"changeAddress\": \"address\",    (string, optional) The Avian address to receive the change\n"
                            "       \"changePosition\": n,            (numeric, optional) The index of the change output\n"
                            "       \"includeWatching\": bool,        (boolean, optional, default=false) Include watch-only inputs\n"
                            "       \"lockUnspents\": bool,           (boolean, optional, default=false) Lock selected unspent outputs\n"
                            "       \"minconf\": n,                   (numeric, optional, default=0) If add_inputs is true, require inputs with at least this many confirmations\n"
                            "       \"maxconf\": n,                   (numeric, optional) Require inputs with at most this many confirmations\n"
                            "       \"subtractFeeFromOutputs\": [n],  (array, optional) The output indices to subtract fees from\n"
                            "     }\n"
                            "\nResult:\n"
                            "{\n"
                            "  \"psbt\": \"value\",           (string) The funded PSBT (base64)\n"
                            "  \"fee\": x.xxx,               (numeric) The fee amount used (in " +
            CURRENCY_UNIT + ")\n"
                            "  \"changepos\": n              (numeric) Position of change output (-1 if no change)\n"
                            "}\n"
                            "\nExamples:\n" +
            HelpExampleCli("walletcreatefundedpsbt", "\"{\\\"RXt29uFKBr8RnyUqyp7m71S4DXPtauYyXm\\\":1}\"") +
            HelpExampleCli("walletcreatefundedpsbt", "\"[{\\\"address\\\":\\\"RRecipient1Addr\\\",\\\"amount\\\":1},{\\\"address\\\":\\\"RRecipient2Addr\\\",\\\"amount\\\":0.5},{\\\"data\\\":\\\"00010203\\\"}]\" 0 \"{\\\"changeAddress\\\":\\\"RChangeAddr\\\"}\""));

    // Get wallet
    CWallet* const pwallet = GetWalletForJSONRPCRequest(request);
    if (!pwallet) {
        throw JSONRPCError(RPC_WALLET_NOT_FOUND, "Wallet not found");
    }

    // Parse outputs
    UniValue outputs_uv;
    if (!outputs_uv.read(request.params[0].get_str())) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid outputs JSON");
    }

    if (!outputs_uv.isArray() && !outputs_uv.isObject()) {
        throw JSONRPCError(RPC_TYPE_ERROR, "outputs must be an array or object");
    }

    int nLocktime = 0;
    if (request.params.size() > 1 && !request.params[1].isNull()) {
        nLocktime = request.params[1].get_int();
        if (nLocktime < 0 || nLocktime > 0xffffffff) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Locktime out of range");
        }
    }

    // Parse options
    UniValue options(request.params.size() > 2 && !request.params[2].isNull() ? request.params[2] : UniValue::VOBJ);
    if (!options.isObject()) {
        throw JSONRPCError(RPC_TYPE_ERROR, "options must be an object");
    }

    bool includeWatching = find_value(options, "includeWatching").isNull() ? false : find_value(options, "includeWatching").get_bool();
    bool lockUnspents = find_value(options, "lockUnspents").isNull() ? false : find_value(options, "lockUnspents").get_bool();
    int minconf = find_value(options, "minconf").isNull() ? 0 : find_value(options, "minconf").get_int();
    int maxconf = find_value(options, "maxconf").isNull() ? 9999999 : find_value(options, "maxconf").get_int();
    std::string changeAddress = find_value(options, "changeAddress").isNull() ? "" : find_value(options, "changeAddress").get_str();
    int changePosition = find_value(options, "changePosition").isNull() ? -1 : find_value(options, "changePosition").get_int();

    // Parse subtractFeeFromOutputs
    std::set<int> setSubtractFeeFromOutputs;
    const UniValue& subtractFee = find_value(options, "subtractFeeFromOutputs");
    if (!subtractFee.isNull() && subtractFee.isArray()) {
        const std::vector<UniValue>& indices = subtractFee.getValues();
        for (const auto& idx : indices) {
            setSubtractFeeFromOutputs.insert(idx.get_int());
        }
    }

    // Parse recipient outputs
    CMutableTransaction mtx;
    mtx.nVersion = 2;
    mtx.nLockTime = nLocktime;
    bool data_key_seen = false;

    if (outputs_uv.isArray()) {
        const std::vector<UniValue>& outputs_array = outputs_uv.getValues();
        for (size_t idx = 0; idx < outputs_array.size(); idx++) {
            const UniValue& output = outputs_array[idx];
            if (!output.isObject()) {
                throw JSONRPCError(RPC_TYPE_ERROR, "Invalid type in outputs array");
            }

            const UniValue& data_val = find_value(output, "data");
            if (!data_val.isNull()) {
                // Data output
                if (data_key_seen) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER, "Multiple data outputs are not allowed");
                }
                data_key_seen = true;

                std::string hex_data = data_val.get_str();
                std::vector<unsigned char> data = ParseHex(hex_data);
                CScript scriptPubKey = CScript() << OP_RETURN << data;
                mtx.vout.push_back(CTxOut(0, scriptPubKey));
            } else {
                // Address output
                const UniValue& address_val = find_value(output, "address");
                const UniValue& amount_val = find_value(output, "amount");

                if (address_val.isNull()) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER, "Missing address in output");
                }

                std::string address = address_val.get_str();
                CAmount nAmount = 0;
                if (!amount_val.isNull()) {
                    nAmount = AmountFromValue(amount_val);
                }

                CTxDestination destination = DecodeDestination(address);
                if (!IsValidDestination(destination)) {
                    throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, std::string("Invalid Avian address: ") + address);
                }

                if (nAmount < 0) {
                    throw JSONRPCError(RPC_TYPE_ERROR, "Invalid amount");
                }

                CScript scriptPubKey = GetScriptForDestination(destination);
                mtx.vout.push_back(CTxOut(nAmount, scriptPubKey));
            }
        }
    } else {
        // Object format
        std::vector<std::string> keys = outputs_uv.getKeys();
        for (const std::string& key : keys) {
            if (key == "data") {
                if (data_key_seen) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER, "Multiple data outputs are not allowed");
                }
                data_key_seen = true;

                std::string hex_data = outputs_uv[key].get_str();
                std::vector<unsigned char> data = ParseHex(hex_data);
                CScript scriptPubKey = CScript() << OP_RETURN << data;
                mtx.vout.push_back(CTxOut(0, scriptPubKey));
            } else {
                CTxDestination destination = DecodeDestination(key);
                if (!IsValidDestination(destination)) {
                    throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, std::string("Invalid Avian address: ") + key);
                }

                CAmount nAmount = AmountFromValue(outputs_uv[key]);
                if (nAmount < 0) {
                    throw JSONRPCError(RPC_TYPE_ERROR, "Invalid amount");
                }

                CScript scriptPubKey = GetScriptForDestination(destination);
                mtx.vout.push_back(CTxOut(nAmount, scriptPubKey));
            }
        }
    }

    if (mtx.vout.empty()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "At least one output must be specified");
    }

    // Set up coin control for wallet's coin selection
    CCoinControl coin_control;

    if (!changeAddress.empty()) {
        CTxDestination changeDest = DecodeDestination(changeAddress);
        if (!IsValidDestination(changeDest)) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, std::string("Invalid change address: ") + changeAddress);
        }
        coin_control.destChange = changeDest;
    }

    coin_control.fAllowWatchOnly = includeWatching;

    // Use wallet's FundTransaction to handle coin selection and fee estimation
    CAmount nFeeRet = 0;
    int nChangePosInOut = changePosition;
    std::string strError = "";

    if (!pwallet->FundTransaction(mtx, nFeeRet, nChangePosInOut, strError, lockUnspents, setSubtractFeeFromOutputs, coin_control)) {
        throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, strError.empty() ? "Insufficient funds or transaction creation failed" : strError);
    }

    // Create PSBT from funded transaction
    PartiallySignedTransaction psbtx(mtx);

    std::string utxo_error;
    if (!EnsurePSBTInputUTXOs(pwallet, psbtx, utxo_error)) {
        throw JSONRPCError(RPC_WALLET_ERROR, utxo_error);
    }

    UniValue result(UniValue::VOBJ);
    // Encode as base64 per BIP 174
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << psbtx;
    std::string ssStr(ss.str());
    result.push_back(Pair("psbt", EncodeBase64(ssStr)));
    result.push_back(Pair("fee", ValueFromAmount(nFeeRet)));
    result.push_back(Pair("changepos", nChangePosInOut));

    return result;
}

UniValue decodepsbt(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 1)
        throw std::runtime_error(
            "decodepsbt \"psbt\"\n"
            "\nReturn a JSON object representing the serialized, base64-encoded partially signed Avian transaction.\n"
            "\nArguments:\n"
            "1. \"psbt\"   (string, required) The PSBT base64 string\n"
            "\nResult:\n"
            "{\n"
            "  \"tx\" : {transaction},   (json) The decoded network transaction\n"
            "  \"unknown\" : {               (json) The \"global\" unknown fields\n"
            "    \"key\" : \"value\"         (json) (key-value pair) An unknown field\n"
            "    ,...\n"
            "  },\n"
            "  \"inputs\" : [                (array of json objects)\n"
            "    {\n"
            "      \"utxo\" : {transaction}, (json, optional) Previous output information\n"
            "      \"partial_signatures\" : {  (json) Partial signatures\n"
            "        \"pubkey\" : \"value\",  (string) The public key and its corresponding signature if one was provided\n"
            "        ,...\n"
            "      },\n"
            "      \"sighash\" : \"type\",    (string, optional) The sighash type to be used\n"
            "      \"unknown\" : {            (json) The \"inputs\" unknown fields\n"
            "        \"key\" : \"value\"      (json) (key-value pair) An unknown field\n"
            "        ,...\n"
            "      }\n"
            "    }\n"
            "    ,...\n"
            "  ],\n"
            "  \"outputs\" : [               (array of json objects)\n"
            "    {\n"
            "      \"redeem_script\" : \"script\", (string, optional) The redeem script\n"
            "      \"unknown\" : {           (json) The \"outputs\" unknown fields\n"
            "        \"key\" : \"value\"     (json) (key-value pair) An unknown field\n"
            "        ,...\n"
            "      }\n"
            "    }\n"
            "    ,...\n"
            "  ]\n"
            "}\n"
            "\nExamples:\n" +
            HelpExampleCli("decodepsbt", "\"psbt\""));

    RPCTypeCheck(request.params, {UniValue::VSTR});

    // Decode PSBT from hex
    std::string psbt_str = request.params[0].get_str();
    PartiallySignedTransaction psbtx;
    std::string error;

    if (!DecodePSBT(psbtx, psbt_str, error)) {
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR,
            std::string("PSBT decode failed: ") + error);
    }

    UniValue result(UniValue::VOBJ);

    // Add transaction
    UniValue txObj(UniValue::VOBJ);
    TxToUniv(psbtx.tx, uint256(), txObj);
    result.push_back(Pair("tx", txObj));

    // Add inputs
    UniValue inputs(UniValue::VARR);
    for (size_t i = 0; i < psbtx.inputs.size(); ++i) {
        UniValue input(UniValue::VOBJ);
        const PSBTInput& psbtin = psbtx.inputs[i];

        // Only show utxo if it's actually populated (not just an empty default)
        if (psbtin.utxo && (psbtin.utxo->vin.size() > 0 || psbtin.utxo->vout.size() > 0)) {
            UniValue utxoObj(UniValue::VOBJ);
            TxToUniv(*psbtin.utxo, uint256(), utxoObj);
            input.push_back(Pair("utxo", utxoObj));
        }

        // Witness UTXO (CTxOut) - for witness/segwit inputs
        if (psbtin.txout.nValue >= 0 || !psbtin.txout.scriptPubKey.empty()) {
            UniValue txout_obj(UniValue::VOBJ);
            txout_obj.push_back(Pair("amount", ValueFromAmount(psbtin.txout.nValue)));
            txout_obj.push_back(Pair("scriptPubKey", HexStr(psbtin.txout.scriptPubKey.begin(), psbtin.txout.scriptPubKey.end())));
            input.push_back(Pair("witness_utxo", txout_obj));
        }

        // Partial signatures
        UniValue partsigs(UniValue::VOBJ);
        for (const auto& sig : psbtin.partial_sigs) {
            partsigs.push_back(Pair(HexStr(sig.first.begin(), sig.first.end()),
                HexStr(sig.second.begin(), sig.second.end())));
        }
        if (!partsigs.empty()) {
            input.push_back(Pair("partial_signatures", partsigs));
        }

        inputs.push_back(input);
    }
    result.push_back(Pair("inputs", inputs));

    // Add outputs
    UniValue outputs(UniValue::VARR);
    for (size_t i = 0; i < psbtx.outputs.size(); ++i) {
        UniValue output(UniValue::VOBJ);
        const PSBTOutput& psbtout = psbtx.outputs[i];

        if (!psbtout.redeem_script.empty()) {
            output.push_back(Pair("redeem_script", HexStr(psbtout.redeem_script.begin(), psbtout.redeem_script.end())));
        }

        outputs.push_back(output);
    }
    result.push_back(Pair("outputs", outputs));

    return result;
}

UniValue finalizepsbt(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() < 1 || request.params.size() > 2)
        throw std::runtime_error(
            "finalizepsbt \"psbt\" ( extract )\n"
            "\nFinalize the inputs of a PSBT. If the transaction is fully signed, it will produce a\n"
            "network serialized transaction which can be broadcast with sendrawtransaction. Otherwise a PSBT will\n"
            "be returned which must be further processed.\n"
            "\nArguments:\n"
            "1. \"psbt\" (string) A base64 string of a PSBT (or JSON object from walletprocesspsbt)\n"
            "2. extract (boolean, optional, default=true) If true and the transaction is complete,\n"
            "           extract and return the complete transaction in normal network serialization instead of the PSBT.\n"
            "\nResult:\n"
            "{\n"
            "  \"psbt\" : \"value\",           (string) The base64-encoded partially signed transaction if not extracted\n"
            "  \"hex\" : \"value\",            (string) The hex-encoded network transaction if extracted\n"
            "  \"complete\" : true|false,      (boolean) If the transaction has a complete set of signatures\n"
            "}\n"
            "\nExamples:\n" +
            HelpExampleCli("finalizepsbt", "\"psbt\""));

    RPCTypeCheck(request.params, {UniValue::VSTR, UniValue::VBOOL});

    // Handle both raw PSBT hex and JSON object from walletprocesspsbt
    std::string psbt_str = request.params[0].get_str();

    // If the input looks like JSON (starts with '{'), parse it to extract the psbt field
    if (!psbt_str.empty() && psbt_str[0] == '{') {
        UniValue psbt_obj(UniValue::VOBJ);
        if (!psbt_obj.read(psbt_str)) {
            throw JSONRPCError(RPC_PARSE_ERROR, "Invalid JSON format for PSBT object");
        }
        const UniValue& psbt_field = find_value(psbt_obj, "psbt");
        if (psbt_field.isNull()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "PSBT JSON object must contain 'psbt' field");
        }
        psbt_str = psbt_field.get_str();
    }

    // Decode PSBT from hex
    PartiallySignedTransaction psbtx;
    std::string error;
    if (!DecodePSBT(psbtx, psbt_str, error)) {
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR,
            std::string("PSBT decode failed: ") + error);
    }
    bool extract = request.params.size() > 1 ? request.params[1].get_bool() : true;

    UniValue result(UniValue::VOBJ);
    result.push_back(Pair("complete", psbtx.IsSigned()));

    if (extract && psbtx.IsSigned()) {
        // Construct the final transaction with signatures from the PSBT
        CMutableTransaction finalTx(psbtx.tx);

        // Apply final_script_sig to each input
        for (size_t i = 0; i < finalTx.vin.size() && i < psbtx.inputs.size(); ++i) {
            if (!psbtx.inputs[i].final_script_sig.empty()) {
                finalTx.vin[i].scriptSig = CScript(psbtx.inputs[i].final_script_sig.begin(),
                    psbtx.inputs[i].final_script_sig.end());
            }
            // Note: witness data would go into scriptWitness field for witness transactions
        }

        std::string hex = EncodeHexTx(finalTx);
        result.push_back(Pair("hex", hex));
    } else {
        // Encode as base64 per BIP 174
        CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
        ss << psbtx;
        std::string ssStr(ss.str());
        result.push_back(Pair("psbt", EncodeBase64(ssStr)));
    }

    return result;
}

UniValue analyzepsbt(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 1)
        throw std::runtime_error(
            "analyzepsbt \"psbt\"\n"
            "\nAnalyze the solvability of a Partially Signed Transaction.\n"
            "\nArguments:\n"
            "1. \"psbt\"   (string, required) A base64 string of a PSBT\n"
            "\nResult:\n"
            "{\n"
            "  \"inputs\" : [                 (array of objects) Problem with inputs, \"partial_signatures\" being a key-value pair of pubkey and its current status\n"
            "    {\n"
            "      \"txid\" : \"txid\",        (string) The transaction id\n"
            "      \"vout\" : n,              (numeric) The index of the output\n"
            "      \"sighash\" : \"type\",    (string) The signature hash type to use for signing\n"
            "      \"missing_pubkeys\" : [    (array, optional) Any public keys that are missing that could complete this input\n"
            "        \"pubkey\",             (string) A public key\n"
            "      ],\n"
            "      \"missing_signatures\" : [ (array, optional) Any signatures that are missing that would allow this input to be spent\n"
            "        \"pubkey\",             (string) A public key\n"
            "      ],\n"
            "      \"estimated_size\" : n,   (numeric, optional) Estimated size of the final signed transaction input in bytes\n"
            "      \"estimated_vsize\" : n   (numeric, optional) Estimated virtual size of the final signed transaction input in bytes\n"
            "    }\n"
            "    ,...\n"
            "  ],\n"
            "  \"estimated_size\" : n,       (numeric) Estimated size of the final signed transaction in bytes\n"
            "  \"estimated_vsize\" : n,      (numeric) Estimated virtual size of the final signed transaction in bytes\n"
            "  \"estimated_fee\" : x.xxx     (numeric, optional) Estimated fee amount if the transaction is signed (only if a rate is provided)\n"
            "}\n"
            "\nExamples:\n" +
            HelpExampleCli("analyzepsbt", "\"psbt\""));

    RPCTypeCheck(request.params, {UniValue::VSTR});

    // Decode PSBT from hex
    PartiallySignedTransaction psbtx;
    std::string error;
    if (!DecodePSBT(psbtx, request.params[0].get_str(), error)) {
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR,
            std::string("PSBT decode failed: ") + error);
    }

    UniValue result(UniValue::VOBJ);
    UniValue inputs_result(UniValue::VARR);

    for (size_t i = 0; i < psbtx.inputs.size(); ++i) {
        UniValue input_analysis(UniValue::VOBJ);
        const PSBTInput& input = psbtx.inputs[i];

        input_analysis.push_back(Pair("txid", psbtx.tx.vin[i].prevout.hash.GetHex()));
        input_analysis.push_back(Pair("vout", (int)psbtx.tx.vin[i].prevout.n));

        // Determine if input is complete
        bool is_complete = input.IsSigned();
        input_analysis.push_back(Pair("is_complete", is_complete));

        inputs_result.push_back(input_analysis);
    }

    result.push_back(Pair("inputs", inputs_result));
    result.push_back(Pair("complete", psbtx.IsSigned()));

    return result;
}

UniValue utxoupdatepsbt(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() < 1 || request.params.size() > 2)
        throw std::runtime_error(
            "utxoupdatepsbt \"psbt\" ( [inputs] )\n"
            "\nUpdates all inputs in a PSBT with UTXO data from the blockchain.\n"
            "\nArguments:\n"
            "1. \"psbt\"        (string, required) A base64 string of a PSBT\n"
            "2. \"descriptors\" (string, optional) A JSON array with custom UTXO data\n"
            "     [\n"
            "       {\n"
            "         \"txid\"         (string, required) The transaction id\n"
            "         \"vout\"         (numeric, required) The output number\n"
            "         \"scriptPubKey\" (string, optional) The output script (looked up if not provided)\n"
            "         \"amount\"       (numeric, optional) The output amount (looked up if not provided)\n"
            "       }\n"
            "       ,...\n"
            "     ]\n"
            "\nResult:\n"
            "  \"psbt\"    (string)  The updated PSBT\n"
            "\nExamples:\n" +
            HelpExampleCli("utxoupdatepsbt", "\"psbt\""));

    RPCTypeCheck(request.params, {UniValue::VSTR, UniValue::VARR}, true);

    // Decode PSBT from hex
    PartiallySignedTransaction psbtx;
    std::string error;
    if (!DecodePSBT(psbtx, request.params[0].get_str(), error)) {
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR,
            std::string("PSBT decode failed: ") + error);
    }

    // Try to get wallet for faster transaction lookup
    CWallet* pwallet = nullptr;
    try {
        pwallet = GetWalletForJSONRPCRequest(request);
    } catch (...) {
        // Wallet not available, that's okay - we'll use blockchain lookup
    }

    // Build a map from (txid, vout) to scriptPubKey and amount for quick lookup
    std::map<std::pair<uint256, uint32_t>, std::pair<CScript, CAmount>> utxo_map;

    // If descriptors are provided, parse them first
    if (request.params.size() > 1 && !request.params[1].isNull()) {
        const UniValue& descriptors_uv = request.params[1];
        if (!descriptors_uv.isArray()) {
            throw JSONRPCError(RPC_TYPE_ERROR, "descriptors must be an array");
        }

        const std::vector<UniValue>& descriptors = descriptors_uv.getValues();

        for (const auto& desc : descriptors) {
            if (!desc.isObject()) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "UTXO descriptor must be an object");
            }

            const UniValue& txid_uv = find_value(desc, "txid");
            const UniValue& vout_uv = find_value(desc, "vout");
            const UniValue& scriptPubKey_uv = find_value(desc, "scriptPubKey");
            const UniValue& amount_uv = find_value(desc, "amount");

            if (txid_uv.isNull() || vout_uv.isNull()) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "UTXO descriptor must have txid and vout");
            }

            uint256 txid;
            txid.SetHex(txid_uv.get_str());
            uint32_t vout = vout_uv.get_int();

            CScript script;
            CAmount amount = 0;

            // If scriptPubKey is provided, use it; otherwise we'll look it up
            if (!scriptPubKey_uv.isNull()) {
                std::string scriptHex = scriptPubKey_uv.get_str();
                std::vector<unsigned char> scriptData = ParseHex(scriptHex);
                script = CScript(scriptData.begin(), scriptData.end());
            }

            // If amount is provided, use it; otherwise we'll look it up
            if (!amount_uv.isNull()) {
                amount = AmountFromValue(amount_uv);
            }

            utxo_map[std::make_pair(txid, vout)] = std::make_pair(script, amount);
        }
    }

    // Update PSBT inputs with UTXO data
    for (size_t i = 0; i < psbtx.inputs.size() && i < psbtx.tx.vin.size(); ++i) {
        const CTxIn& txin = psbtx.tx.vin[i];
        PSBTInput& input = psbtx.inputs[i];

        // Check if we have pre-provided data for this input
        auto it = utxo_map.find(std::make_pair(txin.prevout.hash, txin.prevout.n));

        CScript scriptPubKey;
        CAmount amount = 0;
        bool found = false;

        if (it != utxo_map.end()) {
            // Use pre-provided data
            scriptPubKey = it->second.first;
            amount = it->second.second;
            found = true;
        }

        // Try to look up the full previous transaction
        CTransactionRef txPrev;
        uint256 blockHash;

        // First, try wallet if available (faster)
        if (pwallet) {
            const CWalletTx* wtx = pwallet->GetWalletTx(txin.prevout.hash);
            if (wtx) {
                txPrev = wtx->tx;
            }
        }

        // If not in wallet, try blockchain lookup
        if (!txPrev) {
            GetTransaction(txin.prevout.hash, txPrev, Params().GetConsensus(), blockHash, false);
        }

        // Extract UTXO data from transaction
        if (txPrev) {
            if (txin.prevout.n < txPrev->vout.size()) {
                const CTxOut& prevTxOut = txPrev->vout[txin.prevout.n];
                scriptPubKey = prevTxOut.scriptPubKey;
                amount = prevTxOut.nValue;
                found = true;

                // Determine whether to use non-witness or witness UTXO format
                // For witness scripts (P2WPKH, P2WSH): use PSBT_IN_WITNESS_UTXO (CTxOut only)
                // For non-witness scripts (P2PKH, P2SH): use PSBT_IN_NON_WITNESS_UTXO (full tx)
                int witness_version = 0;
                std::vector<unsigned char> witness_program;
                bool is_witness = scriptPubKey.IsWitnessProgram(witness_version, witness_program);

                if (is_witness) {
                    // For witness inputs, store only the CTxOut
                    input.txout.scriptPubKey = scriptPubKey;
                    input.txout.nValue = amount;
                } else {
                    // For non-witness inputs, store the full previous transaction
                    input.utxo = txPrev;
                    input.txout.SetNull();
                }
            }
        }

        if (!input.utxo && input.txout.nValue < 0 && !scriptPubKey.empty()) {
            int fallback_version = 0;
            std::vector<unsigned char> fallback_program;
            if (scriptPubKey.IsWitnessProgram(fallback_version, fallback_program)) {
                input.txout.scriptPubKey = scriptPubKey;
                input.txout.nValue = amount;
            }
        }
    }

    UniValue result(UniValue::VOBJ);
    // Encode as base64 per BIP 174
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << psbtx;
    std::string ssStr(ss.str());
    result.push_back(Pair("psbt", EncodeBase64(ssStr)));
    result.push_back(Pair("inputs_processed", (int)psbtx.inputs.size()));

    return result;
}

UniValue walletprocesspsbt(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() < 1 || request.params.size() > 3)
        throw std::runtime_error(
            "walletprocesspsbt \"psbt\" ( sign sighashtype )\n"
            "\nUpdate a PSBT with input information from our wallet and then sign inputs\n"
            "that we can sign for.\n"
            "\nArguments:\n"
            "1. \"psbt\"           (string, required) The transaction base64 string\n"
            "2. sign              (boolean, optional, default=true) Also sign the transaction when updating\n"
            "3. \"sighashtype\"    (string, optional, default=\"ALL\") The signature hash type to sign with if not specified by the PSBT. Must be one of \"ALL\" \"NONE\" \"SINGLE\" \"ALL|ANYONECANPAY\" \"NONE|ANYONECANPAY\" \"SINGLE|ANYONECANPAY\"\n"
            "\nResult:\n"
            "{\n"
            "  \"psbt\" : \"value\",        (string) The base64-encoded partially signed transaction\n"
            "  \"complete\" : true|false,   (boolean) If the transaction has a complete set of signatures\n"
            "}\n"
            "\nExamples:\n" +
            HelpExampleCli("walletprocesspsbt", "\"psbt\""));

    RPCTypeCheck(request.params, {UniValue::VSTR, UniValue::VBOOL, UniValue::VSTR}, true);

    // Get wallet - returns null if wallet disabled, error if specific wallet not found
    CWallet* const pwallet = GetWalletForJSONRPCRequest(request);
    if (!pwallet) {
        throw JSONRPCError(RPC_WALLET_NOT_FOUND, "Wallet not found");
    }

    // Decode PSBT from hex
    PartiallySignedTransaction psbtx;
    std::string error;
    if (!DecodePSBT(psbtx, request.params[0].get_str(), error)) {
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR,
            std::string("PSBT decode failed: ") + error);
    }

    bool fSign = request.params.size() > 1 ? request.params[1].get_bool() : true;

    // Parse sighash type
    int nHashType = SIGHASH_ALL;
    if (request.params.size() > 2) {
        std::string sighashstr = request.params[2].get_str();
        if (sighashstr == "ALL")
            nHashType = SIGHASH_ALL;
        else if (sighashstr == "NONE")
            nHashType = SIGHASH_NONE;
        else if (sighashstr == "SINGLE")
            nHashType = SIGHASH_SINGLE;
        else if (sighashstr == "ALL|ANYONECANPAY")
            nHashType = SIGHASH_ALL | SIGHASH_ANYONECANPAY;
        else if (sighashstr == "NONE|ANYONECANPAY")
            nHashType = SIGHASH_NONE | SIGHASH_ANYONECANPAY;
        else if (sighashstr == "SINGLE|ANYONECANPAY")
            nHashType = SIGHASH_SINGLE | SIGHASH_ANYONECANPAY;
        else
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid sighash type");
    }

    // Add FORKID flag if enabled (matches wallet behavior)
    if (IsForkIDUAHFenabledForCurrentBlock()) {
        nHashType |= SIGHASH_FORKID;
    }

    // Sign each input that we can sign for
    if (fSign) {
        for (size_t i = 0; i < psbtx.inputs.size() && i < psbtx.tx.vin.size(); ++i) {
            PSBTInput& input = psbtx.inputs[i];

            // Get the scriptPubKey from the PSBT input data
            CScript scriptPubKey;
            CAmount amount = 0;

            // Try to get scriptPubKey from the non-witness UTXO first
            if (input.utxo && input.utxo->vin.size() + input.utxo->vout.size() > 0) {
                // We have the full previous transaction
                CTxIn& txin = psbtx.tx.vin[i];
                if (txin.prevout.n < input.utxo->vout.size()) {
                    scriptPubKey = input.utxo->vout[txin.prevout.n].scriptPubKey;
                    amount = input.utxo->vout[txin.prevout.n].nValue;
                }
            } else if (!input.txout.IsNull()) {
                // We have the witness UTXO (CTxOut)
                scriptPubKey = input.txout.scriptPubKey;
                amount = input.txout.nValue;
            }

            // If we don't have scriptPubKey, we can't sign
            if (scriptPubKey.empty()) {
                continue;
            }

            // Create a mutable transaction copy to sign
            CMutableTransaction txToSign(psbtx.tx);

            // Use ProduceSignature with MutableTransactionSignatureCreator to generate signatures
            SignatureData sigdata;
            bool signed_ok = ProduceSignature(MutableTransactionSignatureCreator(pwallet, &txToSign, i, amount, nHashType), scriptPubKey, sigdata);

            // Store only if signature verification succeeded
            if (signed_ok) {
                input.final_script_sig.assign(sigdata.scriptSig.begin(), sigdata.scriptSig.end());
                if (!sigdata.scriptWitness.stack.empty()) {
                    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
                    ss << (uint64_t)sigdata.scriptWitness.stack.size();
                    for (const auto& item : sigdata.scriptWitness.stack) {
                        ss << item;
                    }
                    input.final_script_witness.assign(ss.begin(), ss.end());
                }
            }
        }
    }

    UniValue result(UniValue::VOBJ);
    // Encode as base64 per BIP 174
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << psbtx;
    std::string ssStr(ss.str());
    result.push_back(Pair("psbt", EncodeBase64(ssStr)));
    result.push_back(Pair("complete", psbtx.IsSigned()));

    return result;
}

static const CRPCCommand commands[] =
    {
        //  category              name                          actor (function)            argNames
        //  --------------------- ----------------------------  -----------------------  ----------
        {"rawtransactions", "createpsbt", &createpsbt, {"inputs", "outputs", "locktime"}},
        {"rawtransactions", "converttopsbt", &converttopsbt, {"hexstring", "permitsigdata", "iswitness"}},
        {"rawtransactions", "combinepsbt", &combinepsbt, {"txs"}},
        {"rawtransactions", "joinpsbts", &joinpsbts, {"txs"}},
        {"wallet", "walletcreatefundedpsbt", &walletcreatefundedpsbt, {"outputs", "locktime", "conf_target", "options"}},
        {"rawtransactions", "decodepsbt", &decodepsbt, {"psbt"}},
        {"rawtransactions", "finalizepsbt", &finalizepsbt, {"psbt", "extract"}},
        {"rawtransactions", "analyzepsbt", &analyzepsbt, {"psbt"}},
        {"rawtransactions", "utxoupdatepsbt", &utxoupdatepsbt, {"psbt", "descriptors"}},
        {"wallet", "walletprocesspsbt", &walletprocesspsbt, {"psbt", "sign", "sighashtype"}},
};

void RegisterPSBTRPCCommands(CRPCTable& t)
{
    for (unsigned int vcidx = 0; vcidx < ARRAYLEN(commands); vcidx++)
        t.appendCommand(commands[vcidx].name, &commands[vcidx]);
}
