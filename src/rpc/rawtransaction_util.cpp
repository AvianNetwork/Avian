// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <rpc/rawtransaction_util.h>

#include <addresstype.h>
#include <assets/assets.h>
#include <assets/assettypes.h>
#include <coins.h>
#include <consensus/amount.h>
#include <core_io.h>
#include <key_io.h>
#include <policy/policy.h>
#include <primitives/transaction.h>
#include <rpc/request.h>
#include <rpc/util.h>
#include <script/sign.h>
#include <script/signingprovider.h>
#include <tinyformat.h>
#include <univalue.h>
#include <util/rbf.h>
#include <util/string.h>
#include <util/strencodings.h>
#include <util/translation.h>

void AddInputs(CMutableTransaction& rawTx, const UniValue& inputs_in, std::optional<bool> rbf)
{
    UniValue inputs;
    if (inputs_in.isNull()) {
        inputs = UniValue::VARR;
    } else {
        inputs = inputs_in.get_array();
    }

    for (unsigned int idx = 0; idx < inputs.size(); idx++) {
        const UniValue& input = inputs[idx];
        const UniValue& o = input.get_obj();

        Txid txid = Txid::FromUint256(ParseHashO(o, "txid"));

        const UniValue& vout_v = o.find_value("vout");
        if (!vout_v.isNum())
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, missing vout key");
        int nOutput = vout_v.getInt<int>();
        if (nOutput < 0)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, vout cannot be negative");

        uint32_t nSequence;

        if (rbf.value_or(true)) {
            nSequence = MAX_BIP125_RBF_SEQUENCE; /* CTxIn::SEQUENCE_FINAL - 2 */
        } else if (rawTx.nLockTime) {
            nSequence = CTxIn::MAX_SEQUENCE_NONFINAL; /* CTxIn::SEQUENCE_FINAL - 1 */
        } else {
            nSequence = CTxIn::SEQUENCE_FINAL;
        }

        // set the sequence number if passed in the parameters object
        const UniValue& sequenceObj = o.find_value("sequence");
        if (sequenceObj.isNum()) {
            int64_t seqNr64 = sequenceObj.getInt<int64_t>();
            if (seqNr64 < 0 || seqNr64 > CTxIn::SEQUENCE_FINAL) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, sequence number is out of range");
            } else {
                nSequence = (uint32_t)seqNr64;
            }
        }

        CTxIn in(COutPoint(txid, nOutput), CScript(), nSequence);

        rawTx.vin.push_back(in);
    }
}

UniValue NormalizeOutputs(const UniValue& outputs_in)
{
    if (outputs_in.isNull()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, output argument must be non-null");
    }

    const bool outputs_is_obj = outputs_in.isObject();
    UniValue outputs = outputs_is_obj ? outputs_in.get_obj() : outputs_in.get_array();

    if (!outputs_is_obj) {
        // Translate array of key-value pairs into dict
        UniValue outputs_dict = UniValue(UniValue::VOBJ);
        for (size_t i = 0; i < outputs.size(); ++i) {
            const UniValue& output = outputs[i];
            if (!output.isObject()) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, key-value pair not an object as expected");
            }
            if (output.size() != 1) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, key-value pair must contain exactly one key");
            }
            outputs_dict.pushKVs(output);
        }
        outputs = std::move(outputs_dict);
    }
    return outputs;
}

// AVN: the asset operations an output may name in place of an amount, as 4.x had them in
// createrawtransaction. Each turns into one or more outputs that are a script and no AVN.
namespace {
using ParsedOutputs = std::vector<std::pair<CTxDestination, CAmount>>;

[[noreturn]] void BadAssetParam(const std::string& what)
{
    throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, " + what);
}

void PushAssetScript(ParsedOutputs& outputs, const CScript& script)
{
    outputs.emplace_back(CNoDestination{script}, CAmount{0});
}

//! The contextual checks read the asset database, which a node has and a tool does not.
CAssetsCache* AssetCache()
{
    if (!passets) throw JSONRPCError(RPC_MISC_ERROR, "The asset database is not available");
    return passets;
}

std::string NeedStr(const UniValue& data, const std::string& key)
{
    const UniValue& value{data.find_value(key)};
    if (!value.isStr()) BadAssetParam("missing data for key: " + key);
    return value.get_str();
}

std::string OptStr(const UniValue& data, const std::string& key)
{
    const UniValue& value{data.find_value(key)};
    if (value.isNull()) return {};
    if (!value.isStr()) BadAssetParam(key + " must be a string");
    return value.get_str();
}

int NeedInt(const UniValue& data, const std::string& key)
{
    const UniValue& value{data.find_value(key)};
    if (!value.isNum()) BadAssetParam("missing data for key: " + key);
    return value.getInt<int>();
}

int OptInt(const UniValue& data, const std::string& key, int fallback)
{
    const UniValue& value{data.find_value(key)};
    if (value.isNull()) return fallback;
    if (!value.isNum()) BadAssetParam(key + " must be a number");
    return value.getInt<int>();
}

CAmount NeedAmount(const UniValue& data, const std::string& key)
{
    const UniValue& value{data.find_value(key)};
    if (value.isNull()) BadAssetParam("missing data for key: " + key);
    return AmountFromValue(value);
}

//! An address given in the data, or the output's own when it is left out.
CTxDestination OptAddress(const UniValue& data, const std::string& key, const CTxDestination& fallback)
{
    const std::string address{OptStr(data, key)};
    if (address.empty()) return fallback;
    const CTxDestination dest{DecodeDestination(address)};
    if (!IsValidDestination(dest)) BadAssetParam(key + " is not a valid Avian address");
    return dest;
}

//! 1 when has_<what> says so, and then <what>'s own key has to be there.
int OptFlagged(const UniValue& data, const std::string& flag, const std::string& key, std::string& value)
{
    const int has{OptInt(data, flag, 0)};
    if (has != 0 && has != 1) BadAssetParam(flag + " must be 0 or 1");
    value = has ? NeedStr(data, key) : std::string{};
    return has;
}

//! The quantity of a qualifier that goes back to its holder, one unless said otherwise.
CAmount ChangeQuantity(const UniValue& data)
{
    const UniValue& value{data.find_value("change_quantity")};
    if (value.isNull()) return COIN;
    const CAmount quantity{AmountFromValue(value)};
    if (quantity < COIN) BadAssetParam("change_quantity must be at least 1");
    return quantity;
}

//! OP_AVN_ASSET <key hash>, which the tag or the restriction is appended to.
CScript NullAssetDataScript(const std::string& address)
{
    const CTxDestination dest{DecodeDestination(address)};
    CScript script;
    if (const auto* pkh = std::get_if<PKHash>(&dest)) {
        script << OP_AVN_ASSET << ToByteVector(*pkh);
    } else if (const auto* sh = std::get_if<ScriptHash>(&dest)) {
        script << OP_AVN_ASSET << ToByteVector(*sh);
    } else {
        BadAssetParam("supplied address is not a valid Avian address: " + address);
    }
    return script;
}

std::vector<std::string> NeedAddresses(const UniValue& data)
{
    const UniValue& addresses{data.find_value("addresses")};
    if (!addresses.isArray() || addresses.size() < 1 || addresses.size() > 10) {
        BadAssetParam("value for key addresses must be an array of size 1 to 10");
    }
    std::vector<std::string> result;
    for (size_t i = 0; i < addresses.size(); ++i) {
        if (!addresses[i].isStr()) BadAssetParam("an address must be a string");
        result.push_back(addresses[i].get_str());
    }
    return result;
}

void PushTransfer(ParsedOutputs& outputs, const CTxDestination& to, const std::string& name, CAmount amount,
                  const std::string& message = "", int64_t expire_time = 0)
{
    const CAssetTransfer transfer{name, amount, message, expire_time};
    std::string error;
    if (!transfer.IsValid(error)) throw JSONRPCError(RPC_INVALID_PARAMETER, error);
    CScript script{GetScriptForDestination(to)};
    transfer.ConstructTransaction(script);
    PushAssetScript(outputs, script);
}

void PushNewAsset(ParsedOutputs& outputs, const CTxDestination& to, const CNewAsset& asset)
{
    std::string error;
    if (!ContextualCheckNewAsset(AssetCache(), asset, error)) throw JSONRPCError(RPC_INVALID_PARAMETER, error);
    CScript script{GetScriptForDestination(to)};
    asset.ConstructTransaction(script);
    PushAssetScript(outputs, script);
}

//! A verifier string as it is stored, checked against the address the restricted asset goes to.
std::string CheckedVerifier(const std::string& verifier, const CTxDestination& destination)
{
    const std::string stripped{GetStrippedVerifierString(verifier)};
    std::string error;
    if (!ContextualCheckVerifierString(AssetCache(), stripped, EncodeDestination(destination), error)) {
        BadAssetParam("verifier string is not valid for this address. Error Msg - " + error);
    }
    return stripped;
}

void PushVerifier(ParsedOutputs& outputs, const std::string& stripped)
{
    CScript script;
    CNullAssetTxVerifierString{stripped}.ConstructTransaction(script);
    PushAssetScript(outputs, script);
}

void ParseAssetOperation(const std::string& op, const UniValue& data, const CTxDestination& destination, ParsedOutputs& outputs)
{
    if (!data.isObject()) BadAssetParam(strprintf("the format must follow { \"%s\": {\"key\": value, ...} }", op));

    if (op == "transfer") {
        // {"ASSET_NAME": quantity, ...}: one output an asset
        const auto& names = data.getKeys();
        const auto& quantities = data.getValues();
        if (names.empty()) BadAssetParam("the format must follow { \"transfer\": {\"asset_name\": amount, ...} }");
        for (size_t i = 0; i < names.size(); ++i) {
            PushTransfer(outputs, destination, names[i], AmountFromValue(quantities[i]));
        }
    } else if (op == "transferwithmessage") {
        // {"ASSET_NAME": quantity, "message": hash, "expire_time": n}. 4.x read this and built
        // nothing: its whole body sat under the test for an invalid name.
        std::string name;
        for (const std::string& key : data.getKeys()) {
            if (key != "message" && key != "expire_time") {
                if (!name.empty()) BadAssetParam("transferwithmessage takes one asset");
                name = key;
            }
        }
        if (name.empty() || !IsAssetNameValid(name)) BadAssetParam("missing valid asset name to transferwithmessage");
        const UniValue& expire_time{data.find_value("expire_time")};
        if (!expire_time.isNull() && !expire_time.isNum()) BadAssetParam("expire_time must be a number");
        PushTransfer(outputs, destination, name, NeedAmount(data, name), DecodeAssetData(NeedStr(data, "message")),
                     expire_time.isNull() ? 0 : expire_time.getInt<int64_t>());
    } else if (op == "issue") {
        const std::string name{NeedStr(data, "asset_name")};
        if (IsAssetNameAnRestricted(name)) BadAssetParam("asset_name can't be a restricted asset name. Please use issue_restricted with the correct parameters");
        if (IsAssetNameAQualifier(name)) BadAssetParam("asset_name can't be a qualifier name. Please use issue_qualifier with the correct parameters");
        AssetType type;
        if (!IsAssetNameValid(name, type)) BadAssetParam("invalid asset name");

        std::string ipfs_hash, ans_id;
        const int has_ipfs{OptFlagged(data, "has_ipfs", "ipfs_hash", ipfs_hash)};
        const int has_ans{OptFlagged(data, "has_ans", "ans_id", ans_id)};
        const CNewAsset asset{name, NeedAmount(data, "asset_quantity"), NeedInt(data, "units"), NeedInt(data, "reissuable"),
                              has_ipfs, DecodeAssetData(ipfs_hash), has_ans, ans_id};
        std::string error;
        if (!ContextualCheckNewAsset(AssetCache(), asset, error)) throw JSONRPCError(RPC_INVALID_PARAMETER, error);

        // Only a root or a sub asset has an owner token of its own, and it comes first.
        if (type == AssetType::ROOT || type == AssetType::SUB) {
            CScript owner_script{GetScriptForDestination(destination)};
            asset.ConstructOwnerTransaction(owner_script);
            PushAssetScript(outputs, owner_script);
        }
        PushNewAsset(outputs, destination, asset);
    } else if (op == "issue_unique") {
        const std::string root_name{NeedStr(data, "root_name")};
        const UniValue& tags{data.find_value("asset_tags")};
        if (!tags.isArray() || tags.size() < 1) BadAssetParam("missing data for key: asset_tags");
        // 4.x took the hashes only when both lists were given; each is on its own here.
        const UniValue& ipfs_hashes{data.find_value("ipfs_hashes")};
        if (!ipfs_hashes.isNull() && (!ipfs_hashes.isArray() || ipfs_hashes.size() != tags.size())) {
            BadAssetParam("ipfs_hashes must be an array the size of asset_tags");
        }
        const UniValue& ans_ids{data.find_value("ans_ids")};
        if (!ans_ids.isNull() && (!ans_ids.isArray() || ans_ids.size() != tags.size())) {
            BadAssetParam("ans_ids must be an array the size of asset_tags");
        }

        // The root's owner token authorises this, and goes back
        PushTransfer(outputs, destination, root_name + OWNER_TAG, OWNER_ASSET_AMOUNT);
        for (size_t i = 0; i < tags.size(); ++i) {
            const std::string ipfs_hash{ipfs_hashes.isNull() ? "" : DecodeAssetData(ipfs_hashes[i].get_str())};
            const std::string ans_id{ans_ids.isNull() ? "" : ans_ids[i].get_str()};
            PushNewAsset(outputs, destination,
                         CNewAsset{GetUniqueAssetName(root_name, tags[i].get_str()), UNIQUE_ASSET_AMOUNT, UNIQUE_ASSET_UNITS,
                                   UNIQUE_ASSETS_REISSUABLE, ipfs_hash.empty() ? 0 : 1, ipfs_hash, ans_id.empty() ? 0 : 1, ans_id});
        }
    } else if (op == "reissue" || op == "reissue_restricted") {
        const bool restricted{op == "reissue_restricted"};
        const std::string name{NeedStr(data, "asset_name")};
        if (restricted && !IsAssetNameAnRestricted(name)) BadAssetParam("asset_name must be a restricted asset name. e.g $ASSET_NAME");
        if (!restricted && IsAssetNameAnRestricted(name)) BadAssetParam("asset_name can't be a restricted asset name. Please use reissue_restricted with the correct parameters");

        CReissueAsset reissue;
        reissue.strName = name;
        reissue.nAmount = NeedAmount(data, "asset_quantity");
        reissue.nReissuable = int8_t(OptInt(data, "reissuable", 1));
        if (reissue.nReissuable != 0 && reissue.nReissuable != 1) BadAssetParam("reissuable data must be a 0 or 1");
        // -1 leaves the units as they are. 4.x sent 0 here, which only an asset of 0 units accepts.
        reissue.nUnits = int8_t(OptInt(data, "new_units", -1));
        reissue.strIPFSHash = DecodeAssetData(OptStr(data, "ipfs_hash"));
        reissue.strANSID = OptStr(data, "ans_id");
        const CTxDestination owner_change{OptAddress(data, "owner_change_address", destination)};

        std::string verifier;
        if (restricted && !data.find_value("verifier_string").isNull()) {
            verifier = CheckedVerifier(NeedStr(data, "verifier_string"), destination);
        }

        std::string error;
        if (!ContextualCheckReissueAsset(AssetCache(), reissue, error)) throw JSONRPCError(RPC_INVALID_PARAMETER, error);

        // verifier string if it changes, owner token back, reissue last
        if (!verifier.empty()) PushVerifier(outputs, verifier);
        PushTransfer(outputs, owner_change, restricted ? RestrictedNameToOwnerName(name) : name + OWNER_TAG, OWNER_ASSET_AMOUNT);
        CScript script{GetScriptForDestination(destination)};
        reissue.ConstructTransaction(script);
        PushAssetScript(outputs, script);
    } else if (op == "issue_restricted") {
        const std::string name{NeedStr(data, "asset_name")};
        if (!IsAssetNameAnRestricted(name)) BadAssetParam("asset_name must be a restricted asset name. e.g $ASSET_NAME");
        const CTxDestination owner_change{OptAddress(data, "owner_change_address", destination)};
        const std::string verifier{CheckedVerifier(NeedStr(data, "verifier_string"), destination)};

        std::string ipfs_hash, ans_id;
        const int has_ipfs{OptFlagged(data, "has_ipfs", "ipfs_hash", ipfs_hash)};
        const int has_ans{OptFlagged(data, "has_ans", "ans_id", ans_id)};
        const CNewAsset asset{name, NeedAmount(data, "asset_quantity"), NeedInt(data, "units"), NeedInt(data, "reissuable"),
                              has_ipfs, DecodeAssetData(ipfs_hash), has_ans, ans_id};
        std::string error;
        if (!ContextualCheckNewAsset(AssetCache(), asset, error)) throw JSONRPCError(RPC_INVALID_PARAMETER, error);

        // verifier string, root owner token back, issuance last
        PushVerifier(outputs, verifier);
        PushTransfer(outputs, owner_change, RestrictedNameToOwnerName(name), OWNER_ASSET_AMOUNT);
        PushNewAsset(outputs, destination, asset);
    } else if (op == "issue_qualifier") {
        const std::string name{NeedStr(data, "asset_name")};
        if (!IsAssetNameAQualifier(name)) BadAssetParam("asset_name must be a qualifier or subqualifier name. e.g #MY_QUALIFIER or #MY_ROOT/#MY_SUB");
        const bool sub_qualifier{IsAssetNameASubQualifier(name)};
        if (!sub_qualifier && !data.find_value("root_change_address").isNull()) {
            BadAssetParam("root_change_address only allowed when issuing a subqualifier.");
        }
        const CTxDestination root_change{OptAddress(data, "root_change_address", destination)};

        const UniValue& quantity{data.find_value("asset_quantity")};
        const CAmount amount{quantity.isNull() ? COIN : AmountFromValue(quantity)};
        if (amount < QUALIFIER_ASSET_MIN_AMOUNT || amount > QUALIFIER_ASSET_MAX_AMOUNT) {
            BadAssetParam("qualifiers are only allowed to be issued in quantities between 1 and 10.");
        }
        std::string ipfs_hash;
        const int has_ipfs{OptFlagged(data, "has_ipfs", "ipfs_hash", ipfs_hash)};
        const CNewAsset asset{name, amount, /*units=*/0, /*nReissuable=*/0, has_ipfs, DecodeAssetData(ipfs_hash)};
        std::string error;
        if (!ContextualCheckNewAsset(AssetCache(), asset, error)) throw JSONRPCError(RPC_INVALID_PARAMETER, error);

        // A sub-qualifier is authorised by its root qualifier, which goes back first
        if (sub_qualifier) PushTransfer(outputs, root_change, GetParentName(name), ChangeQuantity(data));
        PushNewAsset(outputs, destination, asset);
    } else if (op == "tag_addresses" || op == "untag_addresses") {
        const std::string qualifier{NeedStr(data, "qualifier")};
        if (!IsAssetNameAQualifier(qualifier)) BadAssetParam("a valid qualifier name must be provided, e.g. #MY_QUALIFIER");
        const std::vector<std::string> addresses{NeedAddresses(data)};

        // The qualifier authorises this, and goes back to the output's address
        PushTransfer(outputs, destination, qualifier, ChangeQuantity(data));
        for (const std::string& address : addresses) {
            CScript script{NullAssetDataScript(address)};
            CNullAssetTxData{qualifier, int8_t(op == "tag_addresses" ? 1 : 0)}.ConstructTransaction(script);
            PushAssetScript(outputs, script);
        }
    } else if (op == "freeze_addresses" || op == "unfreeze_addresses") {
        const std::string name{NeedStr(data, "asset_name")};
        if (!IsAssetNameAnRestricted(name)) BadAssetParam("a valid restricted asset name must be provided, e.g. $MY_ASSET");
        const std::vector<std::string> addresses{NeedAddresses(data)};

        // The root owner token authorises this, and goes back to the output's address
        PushTransfer(outputs, destination, RestrictedNameToOwnerName(name), OWNER_ASSET_AMOUNT);
        for (const std::string& address : addresses) {
            CScript script{NullAssetDataScript(address)};
            CNullAssetTxData{name, int8_t(op == "freeze_addresses" ? 1 : 0)}.ConstructTransaction(script);
            PushAssetScript(outputs, script);
        }
    } else if (op == "freeze_asset" || op == "unfreeze_asset") {
        const std::string name{NeedStr(data, "asset_name")};
        if (!IsAssetNameAnRestricted(name)) BadAssetParam("a valid restricted asset name must be provided, e.g. $MY_ASSET");

        PushTransfer(outputs, destination, RestrictedNameToOwnerName(name), OWNER_ASSET_AMOUNT);
        CScript script;
        CNullAssetTxData{name, int8_t(op == "freeze_asset" ? 1 : 0)}.ConstructGlobalRestrictionTransaction(script);
        PushAssetScript(outputs, script);
    } else {
        BadAssetParam("unknown output type: " + op);
    }
}
} // namespace

std::vector<std::pair<CTxDestination, CAmount>> ParseOutputs(const UniValue& outputs)
{
    // Track AVN and asset destinations separately so the same address
    // may appear once as an AVN payment AND once for an asset operation.
    std::set<CTxDestination> avn_destinations;
    std::set<CTxDestination> asset_destinations;
    std::vector<std::pair<CTxDestination, CAmount>> parsed_outputs;
    bool has_data{false};

    // Iterate positionally: outputs[name_] always returns the FIRST matching key,
    // which breaks duplicate-key entries (e.g. one AVN + one asset to the same
    // address).  Using getValues()[i] gives the correct value for each entry.
    const auto& keys = outputs.getKeys();
    const auto& values = outputs.getValues();

    for (size_t i = 0; i < keys.size(); ++i) {
        const std::string& name_ = keys[i];
        const UniValue& val = values[i];

        if (name_ == "data") {
            if (has_data) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, duplicate key: data");
            }
            has_data = true;
            std::vector<unsigned char> data = ParseHexV(val.getValStr(), "Data");
            CTxDestination destination{CNoDestination{CScript() << OP_RETURN << data}};
            parsed_outputs.emplace_back(destination, CAmount{0});
        } else {
            CTxDestination destination{DecodeDestination(name_)};
            if (!IsValidDestination(destination)) {
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, std::string("Invalid Avian address: ") + name_);
            }

            if (val.isObject()) {
                // Asset operation: {"ADDRESS": {"operation": {...}}}
                if (!asset_destinations.insert(destination).second) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER,
                                       std::string("Invalid parameter, duplicated asset operation address: ") + name_);
                }
                if (val.getKeys().size() != 1) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, an output takes exactly one asset operation");
                }
                ParseAssetOperation(val.getKeys()[0], val.getValues()[0], destination, parsed_outputs);
            } else {
                if (!avn_destinations.insert(destination).second) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER,
                                       std::string("Invalid parameter, duplicated address: ") + name_);
                }
                CAmount amount{AmountFromValue(val)};
                parsed_outputs.emplace_back(destination, amount);
            }
        }
    }
    return parsed_outputs;
}

void AddOutputs(CMutableTransaction& rawTx, const UniValue& outputs_in)
{
    UniValue outputs(UniValue::VOBJ);
    outputs = NormalizeOutputs(outputs_in);

    std::vector<std::pair<CTxDestination, CAmount>> parsed_outputs = ParseOutputs(outputs);
    for (const auto& [destination, nAmount] : parsed_outputs) {
        CScript scriptPubKey = GetScriptForDestination(destination);

        CTxOut out(nAmount, scriptPubKey);
        rawTx.vout.push_back(out);
    }
}

CMutableTransaction ConstructTransaction(const UniValue& inputs_in, const UniValue& outputs_in, const UniValue& locktime, std::optional<bool> rbf, const uint32_t version)
{
    CMutableTransaction rawTx;

    if (!locktime.isNull()) {
        int64_t nLockTime = locktime.getInt<int64_t>();
        if (nLockTime < 0 || nLockTime > LOCKTIME_MAX)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, locktime out of range");
        rawTx.nLockTime = nLockTime;
    }

    if (version < TX_MIN_STANDARD_VERSION || version > TX_MAX_STANDARD_VERSION) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Invalid parameter, version out of range(%d~%d)", TX_MIN_STANDARD_VERSION, TX_MAX_STANDARD_VERSION));
    }
    rawTx.version = version;

    AddInputs(rawTx, inputs_in, rbf);
    AddOutputs(rawTx, outputs_in);

    if (rbf.has_value() && rbf.value() && rawTx.vin.size() > 0 && !SignalsOptInRBF(CTransaction(rawTx))) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter combination: Sequence number(s) contradict replaceable option");
    }

    return rawTx;
}

/** Pushes a JSON object for script verification or signing errors to vErrorsRet. */
static void TxInErrorToJSON(const CTxIn& txin, UniValue& vErrorsRet, const std::string& strMessage)
{
    UniValue entry(UniValue::VOBJ);
    entry.pushKV("txid", txin.prevout.hash.ToString());
    entry.pushKV("vout", (uint64_t)txin.prevout.n);
    UniValue witness(UniValue::VARR);
    for (unsigned int i = 0; i < txin.scriptWitness.stack.size(); i++) {
        witness.push_back(HexStr(txin.scriptWitness.stack[i]));
    }
    entry.pushKV("witness", std::move(witness));
    entry.pushKV("scriptSig", HexStr(txin.scriptSig));
    entry.pushKV("sequence", (uint64_t)txin.nSequence);
    entry.pushKV("error", strMessage);
    vErrorsRet.push_back(std::move(entry));
}

void ParsePrevouts(const UniValue& prevTxsUnival, FlatSigningProvider* keystore, std::map<COutPoint, Coin>& coins)
{
    if (!prevTxsUnival.isNull()) {
        const UniValue& prevTxs = prevTxsUnival.get_array();
        for (unsigned int idx = 0; idx < prevTxs.size(); ++idx) {
            const UniValue& p = prevTxs[idx];
            if (!p.isObject()) {
                throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "expected object with {\"txid'\",\"vout\",\"scriptPubKey\"}");
            }

            const UniValue& prevOut = p.get_obj();

            RPCTypeCheckObj(prevOut,
                {
                    {"txid", UniValueType(UniValue::VSTR)},
                    {"vout", UniValueType(UniValue::VNUM)},
                    {"scriptPubKey", UniValueType(UniValue::VSTR)},
                });

            Txid txid = Txid::FromUint256(ParseHashO(prevOut, "txid"));

            int nOut = prevOut.find_value("vout").getInt<int>();
            if (nOut < 0) {
                throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "vout cannot be negative");
            }

            COutPoint out(txid, nOut);
            std::vector<unsigned char> pkData(ParseHexO(prevOut, "scriptPubKey"));
            CScript scriptPubKey(pkData.begin(), pkData.end());

            {
                auto coin = coins.find(out);
                if (coin != coins.end() && !coin->second.IsSpent() && coin->second.out.scriptPubKey != scriptPubKey) {
                    std::string err("Previous output scriptPubKey mismatch:\n");
                    err = err + ScriptToAsmStr(coin->second.out.scriptPubKey) + "\nvs:\n"+
                        ScriptToAsmStr(scriptPubKey);
                    throw JSONRPCError(RPC_DESERIALIZATION_ERROR, err);
                }
                Coin newcoin;
                newcoin.out.scriptPubKey = scriptPubKey;
                newcoin.out.nValue = MAX_MONEY;
                if (prevOut.exists("amount")) {
                    newcoin.out.nValue = AmountFromValue(prevOut.find_value("amount"));
                }
                newcoin.nHeight = 1;
                coins[out] = std::move(newcoin);
            }

            // if redeemScript and private keys were given, add redeemScript to the keystore so it can be signed
            const bool is_p2sh = scriptPubKey.IsPayToScriptHash();
            const bool is_p2wsh = scriptPubKey.IsPayToWitnessScriptHash();
            if (keystore && (is_p2sh || is_p2wsh)) {
                RPCTypeCheckObj(prevOut,
                    {
                        {"redeemScript", UniValueType(UniValue::VSTR)},
                        {"witnessScript", UniValueType(UniValue::VSTR)},
                    }, true);
                const UniValue& rs{prevOut.find_value("redeemScript")};
                const UniValue& ws{prevOut.find_value("witnessScript")};
                if (rs.isNull() && ws.isNull()) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER, "Missing redeemScript/witnessScript");
                }

                // work from witnessScript when possible
                std::vector<unsigned char> scriptData(!ws.isNull() ? ParseHexV(ws, "witnessScript") : ParseHexV(rs, "redeemScript"));
                CScript script(scriptData.begin(), scriptData.end());
                keystore->scripts.emplace(CScriptID(script), script);
                // Automatically also add the P2WSH wrapped version of the script (to deal with P2SH-P2WSH).
                // This is done for redeemScript only for compatibility, it is encouraged to use the explicit witnessScript field instead.
                CScript witness_output_script{GetScriptForDestination(WitnessV0ScriptHash(script))};
                keystore->scripts.emplace(CScriptID(witness_output_script), witness_output_script);

                if (!ws.isNull() && !rs.isNull()) {
                    // if both witnessScript and redeemScript are provided,
                    // they should either be the same (for backwards compat),
                    // or the redeemScript should be the encoded form of
                    // the witnessScript (ie, for p2sh-p2wsh)
                    if (ws.get_str() != rs.get_str()) {
                        std::vector<unsigned char> redeemScriptData(ParseHexV(rs, "redeemScript"));
                        CScript redeemScript(redeemScriptData.begin(), redeemScriptData.end());
                        if (redeemScript != witness_output_script) {
                            throw JSONRPCError(RPC_INVALID_PARAMETER, "redeemScript does not correspond to witnessScript");
                        }
                    }
                }

                if (is_p2sh) {
                    const CTxDestination p2sh{ScriptHash(script)};
                    const CTxDestination p2sh_p2wsh{ScriptHash(witness_output_script)};
                    if (scriptPubKey == GetScriptForDestination(p2sh)) {
                        // traditional p2sh; arguably an error if
                        // we got here with rs.IsNull(), because
                        // that means the p2sh script was specified
                        // via witnessScript param, but for now
                        // we'll just quietly accept it
                    } else if (scriptPubKey == GetScriptForDestination(p2sh_p2wsh)) {
                        // p2wsh encoded as p2sh; ideally the witness
                        // script was specified in the witnessScript
                        // param, but also support specifying it via
                        // redeemScript param for backwards compat
                        // (in which case ws.IsNull() == true)
                    } else {
                        // otherwise, can't generate scriptPubKey from
                        // either script, so we got unusable parameters
                        throw JSONRPCError(RPC_INVALID_PARAMETER, "redeemScript/witnessScript does not match scriptPubKey");
                    }
                } else if (is_p2wsh) {
                    // plain p2wsh; could throw an error if script
                    // was specified by redeemScript rather than
                    // witnessScript (ie, ws.IsNull() == true), but
                    // accept it for backwards compat
                    const CTxDestination p2wsh{WitnessV0ScriptHash(script)};
                    if (scriptPubKey != GetScriptForDestination(p2wsh)) {
                        throw JSONRPCError(RPC_INVALID_PARAMETER, "redeemScript/witnessScript does not match scriptPubKey");
                    }
                }
            }
        }
    }
}

void SignTransaction(CMutableTransaction& mtx, const SigningProvider* keystore, const std::map<COutPoint, Coin>& coins, const UniValue& hashType, UniValue& result)
{
    std::optional<int> nHashType = ParseSighashString(hashType);
    if (!nHashType) {
        // Avian: Default to ALL|FORKID since UAHF is permanently active
        nHashType = SIGHASH_ALL | SIGHASH_FORKID;
    }

    // Script verification errors
    std::map<int, bilingual_str> input_errors;

    bool complete = SignTransaction(mtx, keystore, coins, *nHashType, input_errors);
    SignTransactionResultToJSON(mtx, complete, coins, input_errors, result);
}

void SignTransactionResultToJSON(CMutableTransaction& mtx, bool complete, const std::map<COutPoint, Coin>& coins, const std::map<int, bilingual_str>& input_errors, UniValue& result)
{
    // Make errors UniValue
    UniValue vErrors(UniValue::VARR);
    for (const auto& err_pair : input_errors) {
        if (err_pair.second.original == "Missing amount") {
            // This particular error needs to be an exception for some reason
            throw JSONRPCError(RPC_TYPE_ERROR, strprintf("Missing amount for %s", coins.at(mtx.vin.at(err_pair.first).prevout).out.ToString()));
        }
        TxInErrorToJSON(mtx.vin.at(err_pair.first), vErrors, err_pair.second.original);
    }

    result.pushKV("hex", EncodeHexTx(CTransaction(mtx)));
    result.pushKV("complete", complete);
    if (!vErrors.empty()) {
        if (result.exists("errors")) {
            vErrors.push_backV(result["errors"].getValues());
        }
        result.pushKV("errors", std::move(vErrors));
    }
}
