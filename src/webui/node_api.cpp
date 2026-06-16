// Copyright (c) 2025-present The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <webui/webui_internal.h>

#include <addresstype.h>
#include <consensus/amount.h>
#include <assets/ans.h>
#include <assets/assets.h>
#include <assets/assetdb.h>
#include <clientversion.h>
#include <common/signmessage.h>
#include <common/system.h>
#include <common/url.h>
#include <core_io.h>
#include <httpserver.h>
#include <key_io.h>
#include <net.h>
#include <node/context.h>
#include <node/transaction.h>
#include <node/types.h>
#include <psbt.h>
#include <rpc/request.h>
#include <rpc/server.h>
#include <streams.h>
#include <univalue.h>
#include <uint256.h>
#include <util/moneystr.h>
#include <util/strencodings.h>
#include <util/time.h>
#include <validation.h>
#include <kernel/mempool_entry.h>
#include <validationinterface.h>

#include <event2/http.h>

#include <limits>
#include <map>
#include <optional>
#include <string>
#include <vector>

using node::NodeContext;

// ---- Node API handlers -------------------------------------------------

static bool HandleNodeStatus(HTTPRequest* req)
{
    if (req->GetRequestMethod() != HTTPRequest::GET) {
        req->WriteReply(HTTP_BAD_METHOD, "");
        return false;
    }
    if (!CheckWebUIHost(req)) return false;
    auto cors = CheckWebUICORS(req);
    if (!cors) return false;
    if (!CheckWebUIAuth(req)) return false;

    if (!g_node || !g_node->chainman) {
        req->WriteReply(HTTP_SERVICE_UNAVAILABLE, R"({"error":"Node not ready"})");
        return false;
    }

    UniValue obj(UniValue::VOBJ);
    obj.pushKV("version", FormatFullVersion());

    {
        LOCK(cs_main);
        auto& chainman = *g_node->chainman;
        const CBlockIndex* tip = chainman.ActiveChain().Tip();

        obj.pushKV("network", chainman.GetParams().GetChainTypeString());
        obj.pushKV("blocks", chainman.ActiveChain().Height());
        obj.pushKV("headers", chainman.m_best_header ? chainman.m_best_header->nHeight : -1);
        if (tip) {
            obj.pushKV("bestblockhash", tip->GetBlockHash().GetHex());
            obj.pushKV("verificationprogress", chainman.GuessVerificationProgress(tip));
        } else {
            obj.pushKV("bestblockhash", UniValue{});
            obj.pushKV("verificationprogress", 0.0);
        }
        obj.pushKV("initialblockdownload", chainman.IsInitialBlockDownload());
    }

    obj.pushKV("connections",
        g_node->connman ? static_cast<int>(g_node->connman->GetNodeCount(ConnectionDirection::Both)) : 0);
    obj.pushKV("mempoolsize",
        g_node->mempool ? static_cast<int>(g_node->mempool->size()) : 0);
    obj.pushKV("uptime", GetTime() - GetStartupTime());

    SetJSONHeaders(req, *cors);
    req->WriteReply(HTTP_OK, obj.write());
    return true;
}

static UniValue FeatureFlag(bool compiled, bool active)
{
    UniValue f(UniValue::VOBJ);
    f.pushKV("compiled", compiled);
    f.pushKV("active",   active);
    return f;
}

static bool HandleNodeFeatures(HTTPRequest* req)
{
    if (req->GetRequestMethod() != HTTPRequest::GET) {
        req->WriteReply(HTTP_BAD_METHOD, "");
        return false;
    }
    if (!CheckWebUIHost(req)) return false;
    auto cors = CheckWebUICORS(req);
    if (!cors) return false;
    if (!CheckWebUIAuth(req)) return false;

    if (!g_node || !g_node->chainman) {
        req->WriteReply(HTTP_SERVICE_UNAVAILABLE, R"({"error":"Node not ready"})");
        return false;
    }

    UniValue obj(UniValue::VOBJ);
    {
        LOCK(cs_main);
        obj.pushKV("network", g_node->chainman->GetParams().GetChainTypeString());
    }

    UniValue features(UniValue::VOBJ);
    // Assets and ANS are always compiled into Avian Core; active = deployment check.
    features.pushKV("assets",           FeatureFlag(true, AreAssetsDeployed()));
    features.pushKV("restrictedAssets", FeatureFlag(true, AreRestrictedAssetsDeployed()));
    features.pushKV("messages",         FeatureFlag(true, AreMessagesDeployed()));
    features.pushKV("ans",              FeatureFlag(true, IsAvianNameSystemDeployed()));
    // PSBT is always available.
    features.pushKV("psbt",             FeatureFlag(true, true));
    // Post-quantum: requires both liboqs compile support and DEPLOYMENT_MLDSA44 activation.
    {
        const CBlockIndex* tip = g_node->chainman->ActiveChain().Tip();
        bool pq_active = tip && DeploymentActiveAt(*tip, *g_node->chainman, Consensus::DEPLOYMENT_MLDSA44);
#ifdef HAVE_LIBOQS
        features.pushKV("postQuantum",  FeatureFlag(true, pq_active));
#else
        features.pushKV("postQuantum",  FeatureFlag(false, false));
#endif
    }
    obj.pushKV("features", features);

    SetJSONHeaders(req, *cors);
    req->WriteReply(HTTP_OK, obj.write());
    return true;
}

// ---- ANS API handlers --------------------------------------------------

static bool HandleANSResolve(HTTPRequest* req, const std::string& name, const std::optional<std::string>& coin)
{
    if (!CheckWebUIHost(req)) return false;
    auto cors = CheckWebUICORS(req);
    if (!cors) return false;
    if (!CheckWebUIAuth(req)) return false;

    // Normalise: uppercase, strip .AVN suffix
    std::string base = name;
    for (auto& c : base) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    if (base.size() > 4 && base.substr(base.size() - 4) == ".AVN")
        base = base.substr(0, base.size() - 4);
    if (base.empty()) {
        req->WriteReply(HTTP_BAD_REQUEST, R"({"error":"Name must not be empty"})");
        return false;
    }

    LOCK(cs_main);
    if (!passets) {
        req->WriteReply(HTTP_SERVICE_UNAVAILABLE, R"({"error":"Asset cache not available"})");
        return false;
    }

    // Cross-chain resolution via sub-asset NAME.AVN/COIN (AIP-0010)
    if (coin) {
        std::string coinUpper = *coin;
        for (auto& c : coinUpper) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
        const std::string subAssetName = base + CAvianNameSystemID::domain + "/" + coinUpper;
        CNewAsset subAsset;
        if (!passets->GetAssetMetaDataIfExists(subAssetName, subAsset)) {
            req->WriteReply(HTTP_NOT_FOUND, JsonError("Cross-chain sub-asset not found: " + subAssetName));
            return false;
        }
        if (!subAsset.nHasANS || subAsset.strANSID.empty()) {
            req->WriteReply(HTTP_NOT_FOUND, R"({"error":"Sub-asset has no ANS record"})");
            return false;
        }
        CAvianNameSystemID ansID(subAsset.strANSID);
        if (ansID.type() != CAvianNameSystemID::XADDR || ansID.addr().empty()) {
            req->WriteReply(HTTP_NOT_FOUND, R"({"error":"Sub-asset ANS record is not an external address"})");
            return false;
        }
        UniValue obj(UniValue::VOBJ);
        obj.pushKV("name",    subAssetName);
        obj.pushKV("address", ansID.addr());
        obj.pushKV("source",  "ans_xaddr");
        SetJSONHeaders(req, *cors);
        req->WriteReply(HTTP_OK, obj.write());
        return true;
    }

    // Avian resolution
    const std::string assetName = base + CAvianNameSystemID::domain;
    CNewAsset asset;
    if (!passets->GetAssetMetaDataIfExists(assetName, asset)) {
        req->WriteReply(HTTP_NOT_FOUND, JsonError("AVN name not found: " + assetName));
        return false;
    }

    // Tier 1a: ADDR record
    if (asset.nHasANS && !asset.strANSID.empty()) {
        CAvianNameSystemID ansID(asset.strANSID);
        if (ansID.type() == CAvianNameSystemID::ADDR && !ansID.addr().empty()) {
            UniValue obj(UniValue::VOBJ);
            obj.pushKV("name",    assetName);
            obj.pushKV("address", ansID.addr());
            obj.pushKV("source",  "ans_record");
            SetJSONHeaders(req, *cors);
            req->WriteReply(HTTP_OK, obj.write());
            return true;
        }
        // Tier 1b: PROFILE record
        if (ansID.type() == CAvianNameSystemID::PROFILE && !ansID.profile().addr.empty()) {
            const ANSProfileData& pd = ansID.profile();
            UniValue obj(UniValue::VOBJ);
            obj.pushKV("name",    assetName);
            obj.pushKV("address", pd.addr);
            obj.pushKV("source",  "ans_profile");
            if (!pd.name.empty())   obj.pushKV("display_name", pd.name);
            if (!pd.avatar.empty()) obj.pushKV("avatar", pd.avatar_binary ? HexStr(pd.avatar) : pd.avatar);
            if (!pd.banner.empty()) obj.pushKV("banner", pd.banner_binary ? HexStr(pd.banner) : pd.banner);
            if (!pd.url.empty())    obj.pushKV("url", pd.url);
            SetJSONHeaders(req, *cors);
            req->WriteReply(HTTP_OK, obj.write());
            return true;
        }
    }

    // Tier 2: owner token fallback (requires -assetindex)
    if (!fAssetIndex || !passetsdb) {
        req->WriteReply(HTTP_NOT_FOUND,
            JsonError("No ANS record for " + assetName + "; enable -assetindex for owner-token fallback"));
        return false;
    }
    const std::string ownerToken = assetName + "!";
    std::vector<std::pair<std::string, CAmount>> ownerAddrs;
    int dbTotal{0};
    if (!passetsdb->AssetAddressDir(ownerAddrs, dbTotal, false, ownerToken, 1, 0) || ownerAddrs.empty()) {
        req->WriteReply(HTTP_NOT_FOUND,
            JsonError("No ANS record and no owner found for: " + assetName));
        return false;
    }
    UniValue obj(UniValue::VOBJ);
    obj.pushKV("name",    assetName);
    obj.pushKV("address", ownerAddrs[0].first);
    obj.pushKV("source",  "owner_token");
    SetJSONHeaders(req, *cors);
    req->WriteReply(HTTP_OK, obj.write());
    return true;
}

static bool HandleANSWhois(HTTPRequest* req, const std::string& address)
{
    if (!CheckWebUIHost(req)) return false;
    auto cors = CheckWebUICORS(req);
    if (!cors) return false;
    if (!CheckWebUIAuth(req)) return false;

    if (!fAssetIndex || !passetsdb || !passets) {
        req->WriteReply(HTTP_SERVICE_UNAVAILABLE,
            R"({"error":"whois requires -assetindex"})");
        return false;
    }

    LOCK(cs_main);

    const std::string& domain = CAvianNameSystemID::domain;

    auto addrHash = [](const std::string& addr) -> uint160 {
        CTxDestination d = DecodeDestination(addr);
        if (auto* p = std::get_if<PKHash>(&d))           return uint160(*p);
        if (auto* p = std::get_if<WitnessV0KeyHash>(&d)) return uint160(*p);
        return uint160();
    };
    const uint160 queryHash = addrHash(address);

    // Build combined balance map
    std::map<std::string, CAmount> combined;
    {
        std::vector<std::pair<std::string, CAmount>> vecDB;
        int dbTotal{0};
        passetsdb->AddressDir(vecDB, dbTotal, false, address, std::numeric_limits<size_t>::max(), 0);
        for (const auto& [nm, amt] : vecDB) combined[nm] = amt;
        for (const auto& [pair, amt] : passets->mapAssetsAddressAmount)
            if (pair.second == address) combined[pair.first] = amt;
    }

    UniValue ownerOf(UniValue::VARR);
    UniValue holds(UniValue::VARR);
    for (const auto& [nm, amt] : combined) {
        if (amt <= 0) continue;
        if (nm.size() > domain.size() + 1 && nm.back() == '!') {
            std::string base = nm.substr(0, nm.size() - 1);
            if (base.size() > domain.size() &&
                base.substr(base.size() - domain.size()) == domain)
                ownerOf.push_back(base);
        } else if (nm.size() > domain.size() &&
                   nm.substr(nm.size() - domain.size()) == domain) {
            holds.push_back(nm);
        }
    }

    UniValue registeredAs(UniValue::VARR);
    {
        std::vector<CDatabasedAssetData> ansAssets;
        passetsdb->AssetDir(ansAssets, "*", std::numeric_limits<size_t>::max(), 0);
        for (const auto& data : ansAssets) {
            const CNewAsset& a = data.asset;
            if (a.strName.size() <= domain.size() ||
                a.strName.substr(a.strName.size() - domain.size()) != domain) continue;
            if (!a.nHasANS || a.strANSID.empty()) continue;
            if (!CAvianNameSystemID::IsValidID(a.strANSID)) continue;
            CAvianNameSystemID ans(a.strANSID);
            std::string ansAddr;
            if (ans.type() == CAvianNameSystemID::ADDR)
                ansAddr = ans.addr();
            else if (ans.type() == CAvianNameSystemID::PROFILE)
                ansAddr = ans.profile().addr;
            if (!ansAddr.empty() && !queryHash.IsNull() && addrHash(ansAddr) == queryHash)
                registeredAs.push_back(a.strName);
        }
    }

    UniValue obj(UniValue::VOBJ);
    obj.pushKV("address",       address);
    obj.pushKV("owner_of",      ownerOf);
    obj.pushKV("registered_as", registeredAs);
    obj.pushKV("holds",         holds);
    SetJSONHeaders(req, *cors);
    req->WriteReply(HTTP_OK, obj.write());
    return true;
}

static bool HandleAssetCheck(HTTPRequest* req)
{
    // GET ?name=NAME -> { name, valid, error? } or { name, valid, type, exists }
    if (req->GetRequestMethod() != HTTPRequest::GET) {
        req->WriteReply(HTTP_BAD_METHOD, "");
        return false;
    }
    if (!CheckWebUIHost(req)) return false;
    auto cors = CheckWebUICORS(req);
    if (!cors) return false;
    if (!CheckWebUIAuth(req)) return false;

    auto nameParam = req->GetQueryParameter("name");
    if (!nameParam || nameParam->empty()) {
        req->WriteReply(HTTP_BAD_REQUEST, R"({"error":"Missing \"name\" query parameter"})");
        return false;
    }
    const std::string& name = *nameParam;

    UniValue obj(UniValue::VOBJ);
    obj.pushKV("name", name);

    AssetType assetType;
    std::string error;
    if (!IsAssetNameValid(name, assetType, error)) {
        obj.pushKV("valid", false);
        obj.pushKV("error", error);
        SetJSONHeaders(req, *cors);
        req->WriteReply(HTTP_OK, obj.write());
        return true;
    }
    obj.pushKV("valid", true);

    std::string typeStr;
    switch (assetType) {
    case AssetType::ROOT:   typeStr = "ROOT";   break;
    case AssetType::SUB:    typeStr = "SUB";    break;
    case AssetType::UNIQUE: typeStr = "UNIQUE"; break;
    default:                typeStr = "OTHER";  break;
    }
    obj.pushKV("type", typeStr);

    LOCK(cs_main);
    CNewAsset asset;
    const bool exists = passets && passets->GetAssetMetaDataIfExists(name, asset);
    obj.pushKV("exists", exists);
    if (exists) {
        obj.pushKV("reissuable", static_cast<bool>(asset.nReissuable));
        obj.pushKV("units", asset.units);
        obj.pushKV("quantity", AssetUnitValueFromAmount(asset.nAmount, name));
        if (asset.nHasIPFS && !asset.strIPFSHash.empty()) {
            const std::string cid = EncodeAssetData(asset.strIPFSHash);
            if (!cid.empty()) obj.pushKV("ipfs_hash", cid);
        }
    }
    SetJSONHeaders(req, *cors);
    req->WriteReply(HTTP_OK, obj.write());
    return true;
}

// ---- PSBT and message API handlers (node-level) ------------------------

static bool HandleVerifyMessage(HTTPRequest* req)
{
    // POST {address, signature, message} → {valid: bool}
    if (req->GetRequestMethod() != HTTPRequest::POST) {
        req->WriteReply(HTTP_BAD_METHOD, "");
        return false;
    }
    if (!CheckWebUIHost(req)) return false;
    auto cors = CheckWebUICORS(req);
    if (!cors) return false;
    if (!CheckWebUIAuth(req)) return false;

    UniValue body;
    if (!body.read(req->ReadBody()) || !body.isObject()) {
        req->WriteReply(HTTP_BAD_REQUEST, R"({"error":"Invalid request body"})");
        return false;
    }
    const UniValue& addrVal = body["address"];
    const UniValue& sigVal  = body["signature"];
    const UniValue& msgVal  = body["message"];
    if (!addrVal.isStr() || !sigVal.isStr() || !msgVal.isStr()) {
        req->WriteReply(HTTP_BAD_REQUEST,
            R"({"error":"\"address\", \"signature\", and \"message\" fields required"})");
        return false;
    }

    const MessageVerificationResult result = MessageVerify(
        addrVal.get_str(), sigVal.get_str(), msgVal.get_str());

    UniValue obj(UniValue::VOBJ);
    if (result == MessageVerificationResult::OK) {
        obj.pushKV("valid", true);
    } else {
        obj.pushKV("valid", false);
        switch (result) {
        case MessageVerificationResult::ERR_INVALID_ADDRESS:
            obj.pushKV("error", "Invalid address"); break;
        case MessageVerificationResult::ERR_ADDRESS_NO_KEY:
            obj.pushKV("error", "Address has no key (must be P2PKH legacy address)"); break;
        case MessageVerificationResult::ERR_MALFORMED_SIGNATURE:
            obj.pushKV("error", "Malformed signature"); break;
        case MessageVerificationResult::ERR_PUBKEY_NOT_RECOVERED:
            obj.pushKV("error", "Public key could not be recovered from signature"); break;
        case MessageVerificationResult::ERR_NOT_SIGNED:
            obj.pushKV("error", "Message not signed by this address"); break;
        default:
            obj.pushKV("error", "Verification failed"); break;
        }
    }
    SetJSONHeaders(req, *cors);
    req->WriteReply(HTTP_OK, obj.write());
    return true;
}

static bool HandlePSBTDecode(HTTPRequest* req)
{
    // POST {psbt: "base64..."} → {inputs, outputs, fee, complete}
    if (req->GetRequestMethod() != HTTPRequest::POST) {
        req->WriteReply(HTTP_BAD_METHOD, "");
        return false;
    }
    if (!CheckWebUIHost(req)) return false;
    auto cors = CheckWebUICORS(req);
    if (!cors) return false;
    if (!CheckWebUIAuth(req)) return false;

    UniValue body;
    if (!body.read(req->ReadBody()) || !body.isObject()) {
        req->WriteReply(HTTP_BAD_REQUEST, R"({"error":"Invalid request body"})");
        return false;
    }
    const UniValue& psbtVal = body["psbt"];
    if (!psbtVal.isStr() || psbtVal.get_str().empty()) {
        req->WriteReply(HTTP_BAD_REQUEST, R"({"error":"\"psbt\" field required"})");
        return false;
    }

    PartiallySignedTransaction psbtx;
    std::string parse_err;
    if (!DecodeBase64PSBT(psbtx, psbtVal.get_str(), parse_err)) {
        req->WriteReply(HTTP_BAD_REQUEST, JsonError("Invalid PSBT: " + parse_err));
        return false;
    }
    if (!psbtx.tx) {
        req->WriteReply(HTTP_BAD_REQUEST, R"({"error":"PSBT has no transaction"})");
        return false;
    }

    bool all_amounts_known{true};
    CAmount total_in{0};

    UniValue inputs_arr(UniValue::VARR);
    for (size_t i = 0; i < psbtx.tx->vin.size(); ++i) {
        const CTxIn& txin = psbtx.tx->vin[i];
        UniValue inp(UniValue::VOBJ);
        inp.pushKV("txid", txin.prevout.hash.GetHex());
        inp.pushKV("vout", static_cast<int>(txin.prevout.n));

        if (i < psbtx.inputs.size()) {
            const PSBTInput& pin = psbtx.inputs[i];
            // Segwit inputs carry only the output being spent; legacy carry the full prev tx
            if (!pin.witness_utxo.IsNull()) {
                inp.pushKV("amount", ValueFromAmount(pin.witness_utxo.nValue));
                total_in += pin.witness_utxo.nValue;
            } else if (pin.non_witness_utxo) {
                const uint32_t vout_idx = txin.prevout.n;
                if (vout_idx < pin.non_witness_utxo->vout.size()) {
                    const CAmount amt = pin.non_witness_utxo->vout[vout_idx].nValue;
                    inp.pushKV("amount", ValueFromAmount(amt));
                    total_in += amt;
                } else {
                    all_amounts_known = false;
                }
            } else {
                all_amounts_known = false;
            }

            const bool has_sigs = !pin.partial_sigs.empty()
                || !pin.final_script_sig.empty()
                || !pin.final_script_witness.stack.empty();
            inp.pushKV("signed", has_sigs);
        } else {
            all_amounts_known = false;
            inp.pushKV("signed", false);
        }
        inputs_arr.push_back(inp);
    }

    CAmount total_out{0};
    UniValue outputs_arr(UniValue::VARR);
    for (const CTxOut& txout : psbtx.tx->vout) {
        UniValue out(UniValue::VOBJ);
        CTxDestination dest;
        if (ExtractDestination(txout.scriptPubKey, dest)) {
            out.pushKV("address", EncodeDestination(dest));
        } else {
            out.pushKV("script", HexStr(txout.scriptPubKey));
        }
        out.pushKV("amount", ValueFromAmount(txout.nValue));
        total_out += txout.nValue;
        outputs_arr.push_back(out);
    }

    // Complete = all inputs have final_script_sig or final_script_witness
    bool complete{!psbtx.inputs.empty()};
    for (const auto& pin : psbtx.inputs) {
        if (pin.final_script_sig.empty() && pin.final_script_witness.stack.empty()) {
            complete = false;
            break;
        }
    }

    UniValue obj(UniValue::VOBJ);
    obj.pushKV("inputs",   inputs_arr);
    obj.pushKV("outputs",  outputs_arr);
    if (all_amounts_known) {
        obj.pushKV("fee", ValueFromAmount(total_in - total_out));
    }
    obj.pushKV("complete", complete);
    SetJSONHeaders(req, *cors);
    req->WriteReply(HTTP_OK, obj.write());
    return true;
}

static bool HandlePSBTBroadcast(HTTPRequest* req)
{
    // POST {psbt: "base64..."} → {txid}
    if (req->GetRequestMethod() != HTTPRequest::POST) {
        req->WriteReply(HTTP_BAD_METHOD, "");
        return false;
    }
    if (!CheckWebUIHost(req)) return false;
    auto cors = CheckWebUICORS(req);
    if (!cors) return false;
    if (!CheckWebUIAuth(req)) return false;

    if (!g_node) {
        req->WriteReply(HTTP_SERVICE_UNAVAILABLE, R"({"error":"Node not ready"})");
        return false;
    }

    UniValue body;
    if (!body.read(req->ReadBody()) || !body.isObject()) {
        req->WriteReply(HTTP_BAD_REQUEST, R"({"error":"Invalid request body"})");
        return false;
    }
    const UniValue& psbtVal = body["psbt"];
    if (!psbtVal.isStr() || psbtVal.get_str().empty()) {
        req->WriteReply(HTTP_BAD_REQUEST, R"({"error":"\"psbt\" field required"})");
        return false;
    }

    PartiallySignedTransaction psbtx;
    std::string parse_err;
    if (!DecodeBase64PSBT(psbtx, psbtVal.get_str(), parse_err)) {
        req->WriteReply(HTTP_BAD_REQUEST, JsonError("Invalid PSBT: " + parse_err));
        return false;
    }

    CMutableTransaction mtx;
    if (!FinalizeAndExtractPSBT(psbtx, mtx)) {
        req->WriteReply(HTTP_BAD_REQUEST,
            R"({"error":"PSBT is not fully signed and cannot be finalized"})");
        return false;
    }

    CTransactionRef tx = MakeTransactionRef(std::move(mtx));
    std::string err_string;
    const node::TransactionError broadcast_err = BroadcastTransaction(
        *g_node, tx, err_string, /*max_tx_fee=*/0, /*relay=*/true, /*wait_callback=*/false);
    if (broadcast_err != node::TransactionError::OK) {
        req->WriteReply(HTTP_BAD_REQUEST, JsonError(err_string));
        return false;
    }

    UniValue obj(UniValue::VOBJ);
    obj.pushKV("txid", tx->GetHash().GetHex());
    SetJSONHeaders(req, *cors);
    req->WriteReply(HTTP_OK, obj.write());
    return true;
}

// ---- RPC console handler -----------------------------------------------

static bool HandleRPCExecute(HTTPRequest* req)
{
    if (req->GetRequestMethod() != HTTPRequest::POST) {
        req->WriteReply(HTTP_BAD_METHOD, "");
        return false;
    }
    if (!CheckWebUIHost(req)) return false;
    auto cors = CheckWebUICORS(req);
    if (!cors) return false;
    if (!CheckWebUIAuth(req)) return false;

    UniValue body;
    if (!body.read(req->ReadBody()) || !body.isObject()) {
        req->WriteReply(HTTP_BAD_REQUEST, R"({"error":"Invalid request body"})");
        return false;
    }
    const UniValue& methodVal = body["method"];
    if (!methodVal.isStr() || methodVal.get_str().empty()) {
        req->WriteReply(HTTP_BAD_REQUEST, R"({"error":"\"method\" field required"})");
        return false;
    }

    JSONRPCRequest jreq;
    jreq.context   = g_node;
    jreq.strMethod = methodVal.get_str();
    jreq.params    = body["params"].isArray() ? body["params"] : UniValue(UniValue::VARR);

    const UniValue& walletVal = body["wallet"];
    if (walletVal.isStr() && !walletVal.get_str().empty()) {
        jreq.URI = "/wallet/" + walletVal.get_str();
    }

    // Follow JSON-RPC convention: always return 200, put errors in "error" field.
    UniValue obj(UniValue::VOBJ);
    try {
        UniValue result = tableRPC.execute(jreq);
        obj.pushKV("result", result);
        obj.pushKV("error",  UniValue{});
    } catch (const UniValue& e) {
        obj.pushKV("result", UniValue{});
        obj.pushKV("error",  e);
    } catch (const std::exception& e) {
        UniValue err(UniValue::VOBJ);
        err.pushKV("message", e.what());
        obj.pushKV("result", UniValue{});
        obj.pushKV("error",  err);
    }
    SetJSONHeaders(req, *cors);
    req->WriteReply(HTTP_OK, obj.write());
    return true;
}

// ---- Peer API handlers -------------------------------------------------

static bool HandlePeerList(HTTPRequest* req)
{
    if (req->GetRequestMethod() != HTTPRequest::GET) {
        req->WriteReply(HTTP_BAD_METHOD, "");
        return false;
    }
    if (!CheckWebUIHost(req)) return false;
    auto cors = CheckWebUICORS(req);
    if (!cors) return false;
    if (!CheckWebUIAuth(req)) return false;

    JSONRPCRequest jreq;
    jreq.context = g_node;
    jreq.strMethod = "getpeerinfo";
    jreq.params = UniValue(UniValue::VARR);

    try {
        UniValue result = tableRPC.execute(jreq);
        UniValue obj(UniValue::VOBJ);
        obj.pushKV("peers", result);
        SetJSONHeaders(req, *cors);
        req->WriteReply(HTTP_OK, obj.write());
        return true;
    } catch (const UniValue& e) {
        const std::string msg = e["message"].isStr() ? e["message"].get_str() : "RPC error";
        req->WriteReply(HTTP_INTERNAL_SERVER_ERROR, JsonError(msg));
        return false;
    }
}

static bool HandleBannedList(HTTPRequest* req)
{
    if (req->GetRequestMethod() != HTTPRequest::GET) {
        req->WriteReply(HTTP_BAD_METHOD, "");
        return false;
    }
    if (!CheckWebUIHost(req)) return false;
    auto cors = CheckWebUICORS(req);
    if (!cors) return false;
    if (!CheckWebUIAuth(req)) return false;

    JSONRPCRequest jreq;
    jreq.context = g_node;
    jreq.strMethod = "listbanned";
    jreq.params = UniValue(UniValue::VARR);

    try {
        UniValue result = tableRPC.execute(jreq);
        UniValue obj(UniValue::VOBJ);
        obj.pushKV("banned", result);
        SetJSONHeaders(req, *cors);
        req->WriteReply(HTTP_OK, obj.write());
        return true;
    } catch (const UniValue& e) {
        const std::string msg = e["message"].isStr() ? e["message"].get_str() : "RPC error";
        req->WriteReply(HTTP_INTERNAL_SERVER_ERROR, JsonError(msg));
        return false;
    }
}

static bool HandlePeerBan(HTTPRequest* req)
{
    if (req->GetRequestMethod() != HTTPRequest::POST) {
        req->WriteReply(HTTP_BAD_METHOD, "");
        return false;
    }
    if (!CheckWebUIHost(req)) return false;
    auto cors = CheckWebUICORS(req);
    if (!cors) return false;
    if (!CheckWebUIAuth(req)) return false;

    UniValue body;
    if (!body.read(req->ReadBody()) || !body.isObject()) {
        req->WriteReply(HTTP_BAD_REQUEST, R"({"error":"Invalid request body"})");
        return false;
    }
    const UniValue& subnetVal = body["subnet"];
    if (!subnetVal.isStr() || subnetVal.get_str().empty()) {
        req->WriteReply(HTTP_BAD_REQUEST, R"({"error":"\"subnet\" field required"})");
        return false;
    }
    const int64_t bantime = body["bantime"].isNum() ? body["bantime"].getInt<int64_t>() : 86400;

    JSONRPCRequest jreq;
    jreq.context = g_node;
    jreq.strMethod = "setban";
    jreq.params = UniValue(UniValue::VARR);
    jreq.params.push_back(subnetVal.get_str());
    jreq.params.push_back("add");
    jreq.params.push_back(bantime);

    try {
        tableRPC.execute(jreq);
        UniValue obj(UniValue::VOBJ);
        obj.pushKV("success", true);
        SetJSONHeaders(req, *cors);
        req->WriteReply(HTTP_OK, obj.write());
        return true;
    } catch (const UniValue& e) {
        const std::string msg = e["message"].isStr() ? e["message"].get_str() : "RPC error";
        req->WriteReply(HTTP_BAD_REQUEST, JsonError(msg));
        return false;
    }
}

static bool HandlePeerUnban(HTTPRequest* req)
{
    if (req->GetRequestMethod() != HTTPRequest::POST) {
        req->WriteReply(HTTP_BAD_METHOD, "");
        return false;
    }
    if (!CheckWebUIHost(req)) return false;
    auto cors = CheckWebUICORS(req);
    if (!cors) return false;
    if (!CheckWebUIAuth(req)) return false;

    UniValue body;
    if (!body.read(req->ReadBody()) || !body.isObject()) {
        req->WriteReply(HTTP_BAD_REQUEST, R"({"error":"Invalid request body"})");
        return false;
    }
    const UniValue& subnetVal = body["subnet"];
    if (!subnetVal.isStr() || subnetVal.get_str().empty()) {
        req->WriteReply(HTTP_BAD_REQUEST, R"({"error":"\"subnet\" field required"})");
        return false;
    }

    JSONRPCRequest jreq;
    jreq.context = g_node;
    jreq.strMethod = "setban";
    jreq.params = UniValue(UniValue::VARR);
    jreq.params.push_back(subnetVal.get_str());
    jreq.params.push_back("remove");

    try {
        tableRPC.execute(jreq);
        UniValue obj(UniValue::VOBJ);
        obj.pushKV("success", true);
        SetJSONHeaders(req, *cors);
        req->WriteReply(HTTP_OK, obj.write());
        return true;
    } catch (const UniValue& e) {
        const std::string msg = e["message"].isStr() ? e["message"].get_str() : "RPC error";
        req->WriteReply(HTTP_BAD_REQUEST, JsonError(msg));
        return false;
    }
}

static bool HandlePeerAdd(HTTPRequest* req)
{
    if (req->GetRequestMethod() != HTTPRequest::POST) {
        req->WriteReply(HTTP_BAD_METHOD, "");
        return false;
    }
    if (!CheckWebUIHost(req)) return false;
    auto cors = CheckWebUICORS(req);
    if (!cors) return false;
    if (!CheckWebUIAuth(req)) return false;

    UniValue body;
    if (!body.read(req->ReadBody()) || !body.isObject()) {
        req->WriteReply(HTTP_BAD_REQUEST, R"({"error":"Invalid request body"})");
        return false;
    }
    const UniValue& addrVal = body["addr"];
    if (!addrVal.isStr() || addrVal.get_str().empty()) {
        req->WriteReply(HTTP_BAD_REQUEST, R"({"error":"\"addr\" field required"})");
        return false;
    }

    JSONRPCRequest jreq;
    jreq.context = g_node;
    jreq.strMethod = "addnode";
    jreq.params = UniValue(UniValue::VARR);
    jreq.params.push_back(addrVal.get_str());
    jreq.params.push_back("add");

    try {
        tableRPC.execute(jreq);
        UniValue obj(UniValue::VOBJ);
        obj.pushKV("success", true);
        SetJSONHeaders(req, *cors);
        req->WriteReply(HTTP_OK, obj.write());
        return true;
    } catch (const UniValue& e) {
        const std::string msg = e["message"].isStr() ? e["message"].get_str() : "RPC error";
        req->WriteReply(HTTP_BAD_REQUEST, JsonError(msg));
        return false;
    }
}

static bool HandlePeerDisconnect(HTTPRequest* req)
{
    if (req->GetRequestMethod() != HTTPRequest::POST) {
        req->WriteReply(HTTP_BAD_METHOD, "");
        return false;
    }
    if (!CheckWebUIHost(req)) return false;
    auto cors = CheckWebUICORS(req);
    if (!cors) return false;
    if (!CheckWebUIAuth(req)) return false;

    UniValue body;
    if (!body.read(req->ReadBody()) || !body.isObject()) {
        req->WriteReply(HTTP_BAD_REQUEST, R"({"error":"Invalid request body"})");
        return false;
    }
    const UniValue& idVal = body["id"];
    if (!idVal.isNum()) {
        req->WriteReply(HTTP_BAD_REQUEST, R"({"error":"\"id\" (numeric peer id) required"})");
        return false;
    }

    JSONRPCRequest jreq;
    jreq.context = g_node;
    jreq.strMethod = "disconnectnode";
    jreq.params = UniValue(UniValue::VARR);
    jreq.params.push_back(std::string{});           // address — empty, use nodeid
    jreq.params.push_back(idVal.getInt<int>());

    try {
        tableRPC.execute(jreq);
        UniValue obj(UniValue::VOBJ);
        obj.pushKV("success", true);
        SetJSONHeaders(req, *cors);
        req->WriteReply(HTTP_OK, obj.write());
        return true;
    } catch (const UniValue& e) {
        const std::string msg = e["message"].isStr() ? e["message"].get_str() : "RPC error";
        req->WriteReply(HTTP_BAD_REQUEST, JsonError(msg));
        return false;
    }
}

// ---- Node API route dispatcher -----------------------------------------

bool WebUINodeAPIRoute(HTTPRequest* req, const std::string& path)
{
    if (path == "/webui/api/rpc")                     return HandleRPCExecute(req);
    if (path == "/webui/api/node/status")             return HandleNodeStatus(req);
    if (path == "/webui/api/node/features")           return HandleNodeFeatures(req);
    if (path == "/webui/api/node/peers")              return HandlePeerList(req);
    if (path == "/webui/api/node/banned")             return HandleBannedList(req);
    if (path == "/webui/api/node/peers/ban")          return HandlePeerBan(req);
    if (path == "/webui/api/node/peers/unban")        return HandlePeerUnban(req);
    if (path == "/webui/api/node/peers/add")          return HandlePeerAdd(req);
    if (path == "/webui/api/node/peers/disconnect")   return HandlePeerDisconnect(req);
    if (path == "/webui/api/verifymessage")           return HandleVerifyMessage(req);
    if (path == "/webui/api/assets/check")            return HandleAssetCheck(req);

    if (path.starts_with("/webui/api/ans/")) {
        if (req->GetRequestMethod() != HTTPRequest::GET) {
            req->WriteReply(HTTP_BAD_METHOD, "");
            return false;
        }
        static constexpr std::string_view ANS_PREFIX{"/webui/api/ans/"};
        const std::string rest = path.substr(ANS_PREFIX.size());
        const size_t slash = rest.find('/');
        if (slash == std::string::npos || slash == 0) {
            req->WriteReply(HTTP_NOT_FOUND, R"({"error":"not found"})");
            return false;
        }
        const std::string action = rest.substr(0, slash);
        const std::string param  = rest.substr(slash + 1);
        if (param.empty()) {
            req->WriteReply(HTTP_BAD_REQUEST, R"({"error":"Missing parameter"})");
            return false;
        }
        if (action == "resolve") {
            auto coin = req->GetQueryParameter("coin");
            return HandleANSResolve(req, param, coin);
        }
        if (action == "whois") return HandleANSWhois(req, param);
        req->WriteReply(HTTP_NOT_FOUND, R"({"error":"not found"})");
        return false;
    }

    if (path.starts_with("/webui/api/psbt/")) {
        static constexpr std::string_view PSBT_PREFIX{"/webui/api/psbt/"};
        const std::string action = path.substr(PSBT_PREFIX.size());
        if (action == "decode")    return HandlePSBTDecode(req);
        if (action == "broadcast") return HandlePSBTBroadcast(req);
        req->WriteReply(HTTP_NOT_FOUND, R"({"error":"not found"})");
        return false;
    }

    req->WriteReply(HTTP_NOT_FOUND, R"({"error":"not found"})");
    return false;
}
