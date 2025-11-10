// Copyright (c) 2009-2022 The Bitcoin Core developers
// Copyright (c) 2025 The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "psbt.h"
#include "core_io.h"
#include "keystore.h"
#include "rpc/server.h"
#include "script/sign.h"
#include "script/signingprovider.h"
#include "util.h"

#include <univalue.h>

UniValue createpsbt(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() < 2 || request.params.size() > 3)
        throw std::runtime_error(
            "createpsbt [\"txid\":n,...] [{\"address\":amount},...] ( locktime )\n"
            "\nCreates a transaction in Partially Signed Transaction format.\n"
            "Implements the Creator role.\n"
            "\nArguments:\n"
            "1. \"inputs\"             (array, required) A json array of json objects\n"
            "     [\n"
            "       {\n"
            "         \"txid\":\"id\",    (string, required) The transaction id\n"
            "         \"vout\":n         (numeric, required) The output number\n"
            "       }\n"
            "       ,...\n"
            "     ]\n"
            "2. \"outputs\"            (array, required) a json array with outputs (key-value pairs).\n"
            "   For compatibility reasons, a dictionary, which holds the key-value pairs directly, is also\n"
            "   accepted as second parameter.\n"
            "     [\n"
            "       {\n"
            "         \"address\": x.xxx   (obj, optional) A key-value pair. The key (string) is the avian address,\n"
            "                             the value (float or string) is the amount in " +
            CURRENCY_UNIT + "\n"
                            "       }\n"
                            "       ,...\n"
                            "     ]\n"
                            "3. \"locktime\"           (numeric, optional, default=0) Raw locktime. Non-0 value also locktime-activates inputs\n"
                            "\nResult:\n"
                            "  \"psbt\"    (string)  The resulting raw transaction (base64 encoded string)\n"
                            "\nExamples:\n" +
            HelpExampleCli("createpsbt", "\"[{\\\"txid\\\":\\\"myid\\\",\\\"vout\\\":0}]\" \"[{\\\"address\\\":0.01}]\""));

    RPCTypeCheck(request.params, {UniValue::VARR, UniValue::VOBJ, UniValue::VNUM}, true);

    // Create transaction
    CMutableTransaction mtx;

    // Add inputs
    const UniValue& inputs = request.params[0].get_array();
    for (size_t idx = 0; idx < inputs.size(); idx++) {
        const UniValue& input = inputs[idx];
        if (!input.isObject()) {
            throw JSONRPCError(RPC_TYPE_ERROR, "Invalid type in inputs");
        }

        const UniValue& txid_value = input.find_value("txid");
        const UniValue& vout_value = input.find_value("vout");

        if (txid_value.isNull() || vout_value.isNull()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Missing txid or vout");
        }

        uint256 txid(uint256S(txid_value.get_str()));
        int nOutput = vout_value.get_int();

        if (nOutput < 0) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid vout");
        }

        mtx.vin.push_back(CTxIn(COutPoint(txid, nOutput)));
    }

    // Add outputs
    const UniValue& outputs = request.params[1];
    if (!outputs.isObject()) {
        throw JSONRPCError(RPC_TYPE_ERROR, "outputs must be an object");
    }

    for (const std::string& name_ : outputs.getKeys()) {
        CTxDestination destination = DecodeDestination(name_);
        if (!IsValidDestination(destination)) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, std::string("Invalid Avian address: ") + name_);
        }

        CAmount nAmount = AmountFromValue(outputs[name_]);
        if (nAmount < 0)
            throw JSONRPCError(RPC_TYPE_ERROR, "Invalid amount");

        CScript scriptPubKey = GetScriptForDestination(destination);
        mtx.vout.push_back(CTxOut(nAmount, scriptPubKey));
    }

    // Set locktime
    if (!request.params[2].isNull()) {
        int64_t nLockTime = request.params[2].get_int64();
        if (nLockTime < 0 || nLockTime > 0xffffffff)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Locktime out of range");
        mtx.nLockTime = (unsigned int)nLockTime;
    }

    // Create PSBT
    PartiallySignedTransaction psbtx(mtx);

    return psbtx.GetHex();
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

    // Decode PSBT
    PartiallySignedTransaction psbtx = PartiallySignedTransaction::FromHex(request.params[0].get_str());

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

        if (psbtin.utxo) {
            UniValue utxoObj(UniValue::VOBJ);
            TxToUniv(*psbtin.utxo, uint256(), utxoObj);
            input.push_back(Pair("utxo", utxoObj));
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
            "1. \"psbt\" (string) A base64 string of a PSBT\n"
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

    PartiallySignedTransaction psbtx = PartiallySignedTransaction::FromHex(request.params[0].get_str());
    bool extract = request.params.size() > 1 ? request.params[1].get_bool() : true;

    UniValue result(UniValue::VOBJ);
    result.push_back(Pair("complete", psbtx.IsSigned()));

    if (extract && psbtx.IsSigned()) {
        std::string hex = EncodeHexTx(psbtx.tx);
        result.push_back(Pair("hex", hex));
    } else {
        result.push_back(Pair("psbt", psbtx.GetHex()));
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

    PartiallySignedTransaction psbtx = PartiallySignedTransaction::FromHex(request.params[0].get_str());

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
    if (request.fHelp || request.params.size() != 2)
        throw std::runtime_error(
            "utxoupdatepsbt \"psbt\" \"[{\\\"txid\\\":\\\"id\\\",\\\"vout\\\":n,\\\"scriptPubKey\\\":\\\"hex\\\"},...]\"\n"
            "\nUpdates all segwit inputs and outputs in a PSBT with data from output descriptors, the UTXO set or the mempool.\n"
            "\nArguments:\n"
            "1. \"psbt\"        (string, required) A base64 string of a PSBT\n"
            "2. \"descriptors\" (string) A JSON array with objects\n"
            "     [\n"
            "       {\n"
            "         \"txid\"         (string, required) The transaction id\n"
            "         \"vout\"         (numeric, required) The output number\n"
            "         \"scriptPubKey\" (string, required) The output script\n"
            "       }\n"
            "       ,...\n"
            "     ]\n"
            "\nResult:\n"
            "  \"psbt\"    (string)  The updated PSBT. Same format as input parameter output\n"
            "\nExamples:\n" +
            HelpExampleCli("utxoupdatepsbt", "\"psbt\" \"[{\\\"txid\\\":\\\"id\\\",\\\"vout\\\":0}]\""));

    RPCTypeCheck(request.params, {UniValue::VSTR, UniValue::VARR});

    PartiallySignedTransaction psbtx = PartiallySignedTransaction::FromHex(request.params[0].get_str());

    // TODO: Implement UTXO update logic
    // This would:
    // 1. Parse the descriptor array
    // 2. For each input in PSBT, look for matching UTXO data
    // 3. Update psbtx.inputs[i].utxo with the previous transaction
    // 4. Update psbtx.inputs[i].txout with the output being spent

    return psbtx.GetHex();
}

UniValue walletprocesspsbt(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() < 1 || request.params.size() > 3)
        throw std::runtime_error(
            "walletprocesspsbt \"psbt\" ( sign finalize )\n"
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

    PartiallySignedTransaction psbtx = PartiallySignedTransaction::FromHex(request.params[0].get_str());

    bool fSign = request.params.size() > 1 ? request.params[1].get_bool() : true;
    bool fFinalize = false; // Default to not finalizing

    // TODO: Implement wallet signing
    // This would require:
    // 1. Get the wallet
    // 2. For each input, look up previous outputs
    // 3. Sign using wallet keys if available
    // 4. Add signatures to psbtx

    UniValue result(UniValue::VOBJ);
    result.push_back(Pair("psbt", psbtx.GetHex()));
    result.push_back(Pair("complete", psbtx.IsSigned()));

    return result;
}

static const CRPCCommand commands[] =
    {
        //  category              name                      actor (function)         argNames
        //  --------------------- ------------------------  -----------------------  ----------
        {"rawtransactions", "createpsbt", &createpsbt, {"inputs", "outputs", "locktime"}},
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
