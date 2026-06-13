// Copyright (c) 2025-present The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <bitcoin-build-config.h> // IWYU pragma: keep

#include <webui/webui.h>

#include <assets/ans.h>
#include <assets/assets.h>
#include <assets/assetdb.h>
#include <clientversion.h>
#include <common/args.h>
#include <core_io.h>
#include <httpserver.h>
#include <key_io.h>
#include <logging.h>
#include <net.h>
#include <node/context.h>
#include <outputtype.h>
#include <random.h>
#include <rpc/protocol.h>
#include <sync.h>
#include <util/fs.h>
#include <util/fs_helpers.h>
#include <util/strencodings.h>
#include <util/time.h>
#include <util/result.h>
#include <validation.h>
#include <univalue.h>

#ifdef ENABLE_WALLET
#include <interfaces/wallet.h>
#include <support/allocators/secure.h>
#include <wallet/coincontrol.h>
#include <wallet/context.h>
#include <wallet/receive.h>
#include <wallet/spend.h>
#include <wallet/wallet.h>
#include <wallet/walletutil.h>
#endif

#include <algorithm>
#include <chrono>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

using node::NodeContext;

static NodeContext* g_node{nullptr};
static std::string g_webui_token;
static bool g_webui_cookie_generated{false};

// ---- Cookie auth -------------------------------------------------------

static fs::path GetWebUICookiePath()
{
    return gArgs.GetDataDirNet() / fs::PathFromString(WEBUI_COOKIE_FILE);
}

static bool InitWebUIAuth()
{
    unsigned char rand_bytes[32];
    GetRandBytes(rand_bytes);
    g_webui_token = HexStr(rand_bytes);

    fs::path filepath = GetWebUICookiePath();
    fs::path filepath_tmp = filepath;
    filepath_tmp += ".tmp";

    std::ofstream file;
    file.open(filepath_tmp);
    if (!file.is_open()) {
        LogWarning("WebUI: Unable to open cookie file %s for writing", fs::PathToString(filepath_tmp));
        return false;
    }
    file << g_webui_token;
    file.close();

    if (!RenameOver(filepath_tmp, filepath)) {
        LogWarning("WebUI: Unable to rename cookie file %s to %s",
                   fs::PathToString(filepath_tmp), fs::PathToString(filepath));
        return false;
    }
    std::error_code ec;
    fs::permissions(filepath,
                    fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::replace, ec);
    if (ec) {
        LogWarning("WebUI: Unable to set permissions on cookie file: %s", ec.message());
    }

    g_webui_cookie_generated = true;
    LogInfo("Generated WebUI authentication cookie %s\n", fs::PathToString(filepath));
    return true;
}

// ---- Request validators ------------------------------------------------

// Defend against DNS rebinding: only allow requests whose Host header names
// a loopback address. Port suffix is ignored.
static bool CheckWebUIHost(HTTPRequest* req)
{
    auto [present, host] = req->GetHeader("host");
    if (!present || host.empty()) {
        req->WriteReply(HTTP_FORBIDDEN, R"({"error":"Missing Host header"})");
        return false;
    }
    // Strip port suffix (handles "localhost:8332", "[::1]:8332", "127.0.0.1:8332")
    std::string hostname = host;
    if (!hostname.empty() && hostname.front() == '[') {
        // IPv6 literal: "[::1]" or "[::1]:port"
        size_t close = hostname.find(']');
        hostname = (close != std::string::npos) ? hostname.substr(1, close - 1) : hostname;
    } else {
        size_t colon = hostname.rfind(':');
        if (colon != std::string::npos) hostname = hostname.substr(0, colon);
    }
    if (hostname == "127.0.0.1" || hostname == "localhost" || hostname == "::1") {
        return true;
    }
    req->WriteReply(HTTP_FORBIDDEN, R"({"error":"Host not allowed"})");
    return false;
}

static bool CheckWebUIAuth(HTTPRequest* req)
{
    auto [present, auth] = req->GetHeader("authorization");
    if (!present || auth.substr(0, 7) != "Bearer ") {
        req->WriteHeader("WWW-Authenticate", "Bearer realm=\"aviand-webui\"");
        req->WriteReply(HTTP_UNAUTHORIZED, R"({"error":"unauthorized"})");
        return false;
    }
    std::string token = auth.substr(7);
    if (!TimingResistantEqual(token, g_webui_token)) {
        UninterruptibleSleep(std::chrono::milliseconds{250});
        req->WriteHeader("WWW-Authenticate", "Bearer realm=\"aviand-webui\"");
        req->WriteReply(HTTP_UNAUTHORIZED, R"({"error":"unauthorized"})");
        return false;
    }
    return true;
}

// Returns the CORS origin to echo (empty = no Origin header, allowed).
// Writes HTTP 403 and returns nullopt on violation.
static std::optional<std::string> CheckWebUICORS(HTTPRequest* req)
{
    auto [present, origin] = req->GetHeader("origin");
    if (!present) return std::string{};
    if (origin == "http://127.0.0.1" || origin == "http://localhost") return origin;
    req->WriteReply(HTTP_FORBIDDEN, R"({"error":"CORS: origin not allowed"})");
    return std::nullopt;
}

static void SetJSONHeaders(HTTPRequest* req, const std::string& allowed_origin)
{
    req->WriteHeader("Content-Type", "application/json");
    if (!allowed_origin.empty()) {
        req->WriteHeader("Access-Control-Allow-Origin", allowed_origin);
    }
}

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
    // Post-quantum: compiled only when liboqs present; no separate runtime activation.
#ifdef HAVE_LIBOQS
    features.pushKV("postQuantum",      FeatureFlag(true, true));
#else
    features.pushKV("postQuantum",      FeatureFlag(false, false));
#endif
    obj.pushKV("features", features);

    SetJSONHeaders(req, *cors);
    req->WriteReply(HTTP_OK, obj.write());
    return true;
}

// ---- Wallet management handlers ----------------------------------------

static bool HandleWalletsLoaded(HTTPRequest* req)
{
    if (req->GetRequestMethod() != HTTPRequest::GET) {
        req->WriteReply(HTTP_BAD_METHOD, "");
        return false;
    }
    if (!CheckWebUIHost(req)) return false;
    auto cors = CheckWebUICORS(req);
    if (!cors) return false;
    if (!CheckWebUIAuth(req)) return false;

    UniValue obj(UniValue::VOBJ);

#ifdef ENABLE_WALLET
    if (!g_node || !g_node->wallet_loader) {
        obj.pushKV("walletSupportEnabled", false);
        obj.pushKV("wallets", UniValue{UniValue::VARR});
    } else {
        obj.pushKV("walletSupportEnabled", true);
        UniValue arr(UniValue::VARR);
        for (auto& wallet : g_node->wallet_loader->getWallets()) {
            UniValue wobj(UniValue::VOBJ);
            wobj.pushKV("name",      wallet->getWalletName());
            wobj.pushKV("encrypted", wallet->isCrypted());
            arr.push_back(wobj);
        }
        obj.pushKV("wallets", arr);
    }
#else
    obj.pushKV("walletSupportEnabled", false);
    obj.pushKV("wallets", UniValue{UniValue::VARR});
#endif

    SetJSONHeaders(req, *cors);
    req->WriteReply(HTTP_OK, obj.write());
    return true;
}

static bool HandleWalletsAvailable(HTTPRequest* req)
{
    if (req->GetRequestMethod() != HTTPRequest::GET) {
        req->WriteReply(HTTP_BAD_METHOD, "");
        return false;
    }
    if (!CheckWebUIHost(req)) return false;
    auto cors = CheckWebUICORS(req);
    if (!cors) return false;
    if (!CheckWebUIAuth(req)) return false;

#ifdef ENABLE_WALLET
    if (!g_node || !g_node->wallet_loader) {
        req->WriteReply(HTTP_SERVICE_UNAVAILABLE, R"({"error":"Wallet support not available"})");
        return false;
    }
    UniValue obj(UniValue::VOBJ);
    obj.pushKV("walletdir", g_node->wallet_loader->getWalletDir());
    UniValue arr(UniValue::VARR);
    for (auto& [name, format] : g_node->wallet_loader->listWalletDir()) {
        UniValue wobj(UniValue::VOBJ);
        wobj.pushKV("name", name);
        arr.push_back(wobj);
    }
    obj.pushKV("wallets", arr);
    SetJSONHeaders(req, *cors);
    req->WriteReply(HTTP_OK, obj.write());
    return true;
#else
    req->WriteReply(HTTP_SERVICE_UNAVAILABLE, R"({"error":"Wallet support not compiled in"})");
    return false;
#endif
}

static bool HandleWalletLoad(HTTPRequest* req)
{
    if (req->GetRequestMethod() != HTTPRequest::POST) {
        req->WriteReply(HTTP_BAD_METHOD, "");
        return false;
    }
    if (!CheckWebUIHost(req)) return false;
    auto cors = CheckWebUICORS(req);
    if (!cors) return false;
    if (!CheckWebUIAuth(req)) return false;

#ifdef ENABLE_WALLET
    if (!g_node || !g_node->wallet_loader) {
        req->WriteReply(HTTP_SERVICE_UNAVAILABLE, R"({"error":"Wallet support not available"})");
        return false;
    }
    UniValue body;
    if (!body.read(req->ReadBody()) || !body.isObject()) {
        req->WriteReply(HTTP_BAD_REQUEST, R"({"error":"Invalid request body"})");
        return false;
    }
    const UniValue& nameVal = body["name"];
    if (!nameVal.isStr() || nameVal.get_str().empty()) {
        req->WriteReply(HTTP_BAD_REQUEST, R"({"error":"\"name\" field required"})");
        return false;
    }
    std::vector<bilingual_str> warnings;
    auto result = g_node->wallet_loader->loadWallet(nameVal.get_str(), warnings);
    UniValue obj(UniValue::VOBJ);
    if (!result) {
        obj.pushKV("success", false);
        obj.pushKV("error",   util::ErrorString(result).original);
        SetJSONHeaders(req, *cors);
        req->WriteReply(HTTP_BAD_REQUEST, obj.write());
        return false;
    }
    obj.pushKV("success", true);
    obj.pushKV("name",    nameVal.get_str());
    UniValue warn_arr(UniValue::VARR);
    for (const auto& w : warnings) warn_arr.push_back(w.original);
    obj.pushKV("warnings", warn_arr);
    SetJSONHeaders(req, *cors);
    req->WriteReply(HTTP_OK, obj.write());
    return true;
#else
    req->WriteReply(HTTP_SERVICE_UNAVAILABLE, R"({"error":"Wallet support not compiled in"})");
    return false;
#endif
}

static bool HandleWalletCreate(HTTPRequest* req)
{
    if (req->GetRequestMethod() != HTTPRequest::POST) {
        req->WriteReply(HTTP_BAD_METHOD, "");
        return false;
    }
    if (!CheckWebUIHost(req)) return false;
    auto cors = CheckWebUICORS(req);
    if (!cors) return false;
    if (!CheckWebUIAuth(req)) return false;

#ifdef ENABLE_WALLET
    if (!g_node || !g_node->wallet_loader) {
        req->WriteReply(HTTP_SERVICE_UNAVAILABLE, R"({"error":"Wallet support not available"})");
        return false;
    }
    UniValue body;
    if (!body.read(req->ReadBody()) || !body.isObject()) {
        req->WriteReply(HTTP_BAD_REQUEST, R"({"error":"Invalid request body"})");
        return false;
    }
    const UniValue& nameVal = body["name"];
    if (!nameVal.isStr() || nameVal.get_str().empty()) {
        req->WriteReply(HTTP_BAD_REQUEST, R"({"error":"\"name\" field required"})");
        return false;
    }
    SecureString passphrase;
    const UniValue& passVal = body["passphrase"];
    if (passVal.isStr()) {
        const std::string& pass_str = passVal.get_str();
        passphrase.assign(pass_str.begin(), pass_str.end());
    }
    uint64_t flags = wallet::WALLET_FLAG_DESCRIPTORS;
    if (body["blank"].isBool() && body["blank"].get_bool()) {
        flags |= wallet::WALLET_FLAG_BLANK_WALLET;
    }
    if (body["watchonly"].isBool() && body["watchonly"].get_bool()) {
        flags |= wallet::WALLET_FLAG_DISABLE_PRIVATE_KEYS;
        flags |= wallet::WALLET_FLAG_BLANK_WALLET;
    }
    std::vector<bilingual_str> warnings;
    auto result = g_node->wallet_loader->createWallet(nameVal.get_str(), passphrase, flags, warnings);
    UniValue obj(UniValue::VOBJ);
    if (!result) {
        obj.pushKV("success", false);
        obj.pushKV("error",   util::ErrorString(result).original);
        SetJSONHeaders(req, *cors);
        req->WriteReply(HTTP_BAD_REQUEST, obj.write());
        return false;
    }
    obj.pushKV("success", true);
    obj.pushKV("name",    nameVal.get_str());
    UniValue warn_arr(UniValue::VARR);
    for (const auto& w : warnings) warn_arr.push_back(w.original);
    obj.pushKV("warnings", warn_arr);
    SetJSONHeaders(req, *cors);
    req->WriteReply(HTTP_OK, obj.write());
    return true;
#else
    req->WriteReply(HTTP_SERVICE_UNAVAILABLE, R"({"error":"Wallet support not compiled in"})");
    return false;
#endif
}

static bool HandleWalletUnload(HTTPRequest* req)
{
    if (req->GetRequestMethod() != HTTPRequest::POST) {
        req->WriteReply(HTTP_BAD_METHOD, "");
        return false;
    }
    if (!CheckWebUIHost(req)) return false;
    auto cors = CheckWebUICORS(req);
    if (!cors) return false;
    if (!CheckWebUIAuth(req)) return false;

#ifdef ENABLE_WALLET
    if (!g_node || !g_node->wallet_loader) {
        req->WriteReply(HTTP_SERVICE_UNAVAILABLE, R"({"error":"Wallet support not available"})");
        return false;
    }
    UniValue body;
    if (!body.read(req->ReadBody()) || !body.isObject()) {
        req->WriteReply(HTTP_BAD_REQUEST, R"({"error":"Invalid request body"})");
        return false;
    }
    const UniValue& nameVal = body["name"];
    if (!nameVal.isStr() || nameVal.get_str().empty()) {
        req->WriteReply(HTTP_BAD_REQUEST, R"({"error":"\"name\" field required"})");
        return false;
    }
    const std::string wallet_name = nameVal.get_str();

    wallet::WalletContext* wctx = g_node->wallet_loader->context();
    if (!wctx) {
        req->WriteReply(HTTP_SERVICE_UNAVAILABLE, R"({"error":"Wallet context not available"})");
        return false;
    }
    std::shared_ptr<wallet::CWallet> wallet = wallet::GetWallet(*wctx, wallet_name);
    if (!wallet) {
        req->WriteReply(HTTP_BAD_REQUEST, R"({"error":"Wallet not found or not loaded"})");
        return false;
    }
    std::vector<bilingual_str> warnings;
    {
        wallet::WalletRescanReserver reserver(*wallet);
        if (!reserver.reserve()) {
            req->WriteReply(HTTP_BAD_REQUEST, R"({"error":"Wallet is rescanning, try again later"})");
            return false;
        }
        if (!wallet::RemoveWallet(*wctx, wallet, std::nullopt, warnings)) {
            req->WriteReply(HTTP_BAD_REQUEST, R"({"error":"Wallet already unloaded"})");
            return false;
        }
    }
    // wallet local is the last holder; this blocks until destruction completes.
    wallet::WaitForDeleteWallet(std::move(wallet));

    UniValue obj(UniValue::VOBJ);
    obj.pushKV("success", true);
    UniValue warn_arr(UniValue::VARR);
    for (const auto& w : warnings) warn_arr.push_back(w.original);
    obj.pushKV("warnings", warn_arr);
    SetJSONHeaders(req, *cors);
    req->WriteReply(HTTP_OK, obj.write());
    return true;
#else
    req->WriteReply(HTTP_SERVICE_UNAVAILABLE, R"({"error":"Wallet support not compiled in"})");
    return false;
#endif
}

// ---- Wallet-scoped API handlers ----------------------------------------

#ifdef ENABLE_WALLET

static bool HandleWalletSummary(HTTPRequest* req, const std::string& wallet_name)
{
    if (req->GetRequestMethod() != HTTPRequest::GET) {
        req->WriteReply(HTTP_BAD_METHOD, "");
        return false;
    }
    if (!CheckWebUIHost(req)) return false;
    auto cors = CheckWebUICORS(req);
    if (!cors) return false;
    if (!CheckWebUIAuth(req)) return false;

    if (!g_node || !g_node->wallet_loader) {
        req->WriteReply(HTTP_SERVICE_UNAVAILABLE, R"({"error":"Wallet support not available"})");
        return false;
    }
    for (auto& w : g_node->wallet_loader->getWallets()) {
        if (w->getWalletName() != wallet_name) continue;

        interfaces::WalletBalances bal = w->getBalances();
        UniValue obj(UniValue::VOBJ);
        obj.pushKV("name",        wallet_name);
        obj.pushKV("encrypted",   w->isCrypted());
        obj.pushKV("locked",      w->isCrypted() && w->isLocked());
        obj.pushKV("balance",     ValueFromAmount(bal.balance));
        obj.pushKV("unconfirmed", ValueFromAmount(bal.unconfirmed_balance));
        obj.pushKV("immature",    ValueFromAmount(bal.immature_balance));
        SetJSONHeaders(req, *cors);
        req->WriteReply(HTTP_OK, obj.write());
        return true;
    }
    req->WriteReply(HTTP_NOT_FOUND, R"({"error":"Wallet not found or not loaded"})");
    return false;
}

static bool HandleWalletTransactions(HTTPRequest* req, const std::string& wallet_name)
{
    if (req->GetRequestMethod() != HTTPRequest::GET) {
        req->WriteReply(HTTP_BAD_METHOD, "");
        return false;
    }
    if (!CheckWebUIHost(req)) return false;
    auto cors = CheckWebUICORS(req);
    if (!cors) return false;
    if (!CheckWebUIAuth(req)) return false;

    if (!g_node || !g_node->wallet_loader) {
        req->WriteReply(HTTP_SERVICE_UNAVAILABLE, R"({"error":"Wallet support not available"})");
        return false;
    }

    int limit = 50;
    if (auto lp = req->GetQueryParameter("limit")) {
        try { int v = std::stoi(*lp); if (v > 0 && v <= 500) limit = v; } catch (...) {}
    }

    for (auto& w : g_node->wallet_loader->getWallets()) {
        if (w->getWalletName() != wallet_name) continue;

        std::set<interfaces::WalletTx> txs = w->getWalletTxs();

        std::vector<const interfaces::WalletTx*> sorted;
        sorted.reserve(txs.size());
        for (const auto& tx : txs) sorted.push_back(&tx);
        std::sort(sorted.begin(), sorted.end(),
            [](const interfaces::WalletTx* a, const interfaces::WalletTx* b) {
                return a->time > b->time;
            });

        UniValue arr(UniValue::VARR);
        int count = 0;
        for (const auto* wtx : sorted) {
            if (count++ >= limit) break;

            const Txid& txid = wtx->tx->GetHash();
            interfaces::WalletTxStatus status{};
            int num_blocks{0};
            int64_t block_time{0};
            int confirmations{0};
            if (w->tryGetTxStatus(txid, status, num_blocks, block_time)) {
                confirmations = status.depth_in_main_chain;
            }

            UniValue tobj(UniValue::VOBJ);
            tobj.pushKV("txid",          txid.GetHex());
            tobj.pushKV("time",          wtx->time);
            tobj.pushKV("amount",        ValueFromAmount(wtx->credit - wtx->debit));
            tobj.pushKV("credit",        ValueFromAmount(wtx->credit));
            tobj.pushKV("debit",         ValueFromAmount(wtx->debit));
            tobj.pushKV("confirmations", confirmations);
            tobj.pushKV("coinbase",      wtx->is_coinbase);

            UniValue addrs(UniValue::VARR);
            for (size_t i = 0; i < wtx->txout_address.size(); ++i) {
                bool is_mine = i < wtx->txout_address_is_mine.size() && wtx->txout_address_is_mine[i];
                if (is_mine) addrs.push_back(EncodeDestination(wtx->txout_address[i]));
            }
            tobj.pushKV("addresses", addrs);
            arr.push_back(tobj);
        }

        UniValue obj(UniValue::VOBJ);
        obj.pushKV("wallet",       wallet_name);
        obj.pushKV("count",        static_cast<int>(arr.size()));
        obj.pushKV("transactions", arr);
        SetJSONHeaders(req, *cors);
        req->WriteReply(HTTP_OK, obj.write());
        return true;
    }
    req->WriteReply(HTTP_NOT_FOUND, R"({"error":"Wallet not found or not loaded"})");
    return false;
}

static bool HandleWalletReceiveAddress(HTTPRequest* req, const std::string& wallet_name)
{
    if (req->GetRequestMethod() != HTTPRequest::GET) {
        req->WriteReply(HTTP_BAD_METHOD, "");
        return false;
    }
    if (!CheckWebUIHost(req)) return false;
    auto cors = CheckWebUICORS(req);
    if (!cors) return false;
    if (!CheckWebUIAuth(req)) return false;

    if (!g_node || !g_node->wallet_loader) {
        req->WriteReply(HTTP_SERVICE_UNAVAILABLE, R"({"error":"Wallet support not available"})");
        return false;
    }
    for (auto& w : g_node->wallet_loader->getWallets()) {
        if (w->getWalletName() != wallet_name) continue;

        if (w->isCrypted() && w->isLocked()) {
            req->WriteReply(423, R"({"error":"Wallet is locked. Unlock it first."})");
            return false;
        }
        auto dest_result = w->getNewDestination(OutputType::BECH32, "");
        if (!dest_result) {
            const std::string err_msg = util::ErrorString(dest_result).original;
            req->WriteReply(HTTP_BAD_REQUEST,
                "{\"error\":\"" + err_msg + "\"}");
            return false;
        }
        UniValue obj(UniValue::VOBJ);
        obj.pushKV("wallet",  wallet_name);
        obj.pushKV("address", EncodeDestination(*dest_result));
        SetJSONHeaders(req, *cors);
        req->WriteReply(HTTP_OK, obj.write());
        return true;
    }
    req->WriteReply(HTTP_NOT_FOUND, R"({"error":"Wallet not found or not loaded"})");
    return false;
}

static bool HandleWalletSend(HTTPRequest* req, const std::string& wallet_name)
{
    if (req->GetRequestMethod() != HTTPRequest::POST) {
        req->WriteReply(HTTP_BAD_METHOD, "");
        return false;
    }
    if (!CheckWebUIHost(req)) return false;
    auto cors = CheckWebUICORS(req);
    if (!cors) return false;
    if (!CheckWebUIAuth(req)) return false;

    if (!g_node || !g_node->wallet_loader) {
        req->WriteReply(HTTP_SERVICE_UNAVAILABLE, R"({"error":"Wallet support not available"})");
        return false;
    }

    UniValue body;
    if (!body.read(req->ReadBody()) || !body.isObject()) {
        req->WriteReply(HTTP_BAD_REQUEST, R"({"error":"Invalid request body"})");
        return false;
    }
    const UniValue& addrVal = body["address"];
    if (!addrVal.isStr() || addrVal.get_str().empty()) {
        req->WriteReply(HTTP_BAD_REQUEST, R"({"error":"\"address\" field required"})");
        return false;
    }
    CTxDestination dest = DecodeDestination(addrVal.get_str());
    if (!IsValidDestination(dest)) {
        req->WriteReply(HTTP_BAD_REQUEST, R"({"error":"Invalid Avian address"})");
        return false;
    }
    const UniValue& amtVal = body["amount"];
    if (!amtVal.isNum() && !amtVal.isStr()) {
        req->WriteReply(HTTP_BAD_REQUEST, R"({"error":"\"amount\" field required"})");
        return false;
    }
    int64_t amount_raw{0};
    if (!ParseFixedPoint(amtVal.getValStr(), 8, &amount_raw) || amount_raw <= 0 || !MoneyRange(amount_raw)) {
        req->WriteReply(HTTP_BAD_REQUEST, R"({"error":"Invalid or out-of-range amount"})");
        return false;
    }
    const bool subtract_fee = body["subtractFee"].isBool() && body["subtractFee"].get_bool();

    for (auto& w : g_node->wallet_loader->getWallets()) {
        if (w->getWalletName() != wallet_name) continue;

        if (w->isCrypted() && w->isLocked()) {
            req->WriteReply(423, R"({"error":"Wallet is locked. Unlock it first."})");
            return false;
        }

        wallet::CRecipient recipient{dest, static_cast<CAmount>(amount_raw), subtract_fee, {}};
        wallet::CCoinControl coin_control;
        int change_pos{-1};
        CAmount fee{0};
        auto tx_result = w->createTransaction({recipient}, coin_control, /*sign=*/true, change_pos, fee);
        if (!tx_result) {
            UniValue obj(UniValue::VOBJ);
            obj.pushKV("success", false);
            obj.pushKV("error",   util::ErrorString(tx_result).original);
            SetJSONHeaders(req, *cors);
            req->WriteReply(HTTP_BAD_REQUEST, obj.write());
            return false;
        }
        w->commitTransaction(*tx_result, /*value_map=*/{}, /*order_form=*/{});

        UniValue obj(UniValue::VOBJ);
        obj.pushKV("txid", (*tx_result)->GetHash().GetHex());
        obj.pushKV("fee",  ValueFromAmount(fee));
        SetJSONHeaders(req, *cors);
        req->WriteReply(HTTP_OK, obj.write());
        return true;
    }
    req->WriteReply(HTTP_NOT_FOUND, R"({"error":"Wallet not found or not loaded"})");
    return false;
}

static bool HandleWalletAssets(HTTPRequest* req, const std::string& wallet_name)
{
    if (req->GetRequestMethod() != HTTPRequest::GET) {
        req->WriteReply(HTTP_BAD_METHOD, "");
        return false;
    }
    if (!CheckWebUIHost(req)) return false;
    auto cors = CheckWebUICORS(req);
    if (!cors) return false;
    if (!CheckWebUIAuth(req)) return false;

    if (!g_node || !g_node->wallet_loader) {
        req->WriteReply(HTTP_SERVICE_UNAVAILABLE, R"({"error":"Wallet support not available"})");
        return false;
    }
    wallet::WalletContext* wctx = g_node->wallet_loader->context();
    if (!wctx) {
        req->WriteReply(HTTP_SERVICE_UNAVAILABLE, R"({"error":"Wallet context not available"})");
        return false;
    }
    std::shared_ptr<wallet::CWallet> pwallet = wallet::GetWallet(*wctx, wallet_name);
    if (!pwallet) {
        req->WriteReply(HTTP_NOT_FOUND, R"({"error":"Wallet not found or not loaded"})");
        return false;
    }

    std::map<std::string, CAmount> asset_balances;
    {
        LOCK(pwallet->cs_wallet);
        wallet::CoinFilterParams coin_params;
        coin_params.min_amount = 0;
        wallet::CoinsResult available = wallet::AvailableCoinsWithAssets(*pwallet, nullptr, std::nullopt, coin_params);
        for (const auto& [name, outputs] : available.mapAssetCoins) {
            CAmount total{0};
            for (const auto& output : outputs) {
                CAssetOutputEntry data;
                if (GetAssetData(output.txout.scriptPubKey, data)) {
                    total += data.nAmount;
                }
            }
            if (total > 0) asset_balances[name] = total;
        }
    }

    UniValue arr(UniValue::VARR);
    for (const auto& [name, amount] : asset_balances) {
        UniValue aobj(UniValue::VOBJ);
        aobj.pushKV("name",    name);
        aobj.pushKV("balance", AssetUnitValueFromAmount(amount, name));
        arr.push_back(aobj);
    }
    UniValue obj(UniValue::VOBJ);
    obj.pushKV("wallet", wallet_name);
    obj.pushKV("assets", arr);
    SetJSONHeaders(req, *cors);
    req->WriteReply(HTTP_OK, obj.write());
    return true;
}

#endif // ENABLE_WALLET

static bool HandleWalletRoute(HTTPRequest* req, const std::string& path)
{
    static constexpr std::string_view WALLET_PREFIX{"/webui/api/wallet/"};
    const std::string rest = path.substr(WALLET_PREFIX.size());
    const size_t slash = rest.find('/');
    if (slash == std::string::npos || slash == 0) {
        req->WriteReply(HTTP_NOT_FOUND, R"({"error":"not found"})");
        return false;
    }
    const std::string wallet_name = rest.substr(0, slash);
    const std::string action = rest.substr(slash + 1);

#ifdef ENABLE_WALLET
    if (action == "summary")         return HandleWalletSummary(req, wallet_name);
    if (action == "transactions")    return HandleWalletTransactions(req, wallet_name);
    if (action == "receive-address") return HandleWalletReceiveAddress(req, wallet_name);
    if (action == "assets")          return HandleWalletAssets(req, wallet_name);
    if (action == "send")            return HandleWalletSend(req, wallet_name);
#endif

    req->WriteReply(HTTP_NOT_FOUND, R"({"error":"not found"})");
    return false;
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
            req->WriteReply(HTTP_NOT_FOUND,
                "{\"error\":\"Cross-chain sub-asset not found: " + subAssetName + "\"}");
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
        req->WriteReply(HTTP_NOT_FOUND, "{\"error\":\"AVN name not found: " + assetName + "\"}");
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
            "{\"error\":\"No ANS record for " + assetName + "; enable -assetindex for owner-token fallback\"}");
        return false;
    }
    const std::string ownerToken = assetName + "!";
    std::vector<std::pair<std::string, CAmount>> ownerAddrs;
    int dbTotal{0};
    if (!passetsdb->AssetAddressDir(ownerAddrs, dbTotal, false, ownerToken, 1, 0) || ownerAddrs.empty()) {
        req->WriteReply(HTTP_NOT_FOUND,
            "{\"error\":\"No ANS record and no owner found for: " + assetName + "\"}");
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

static bool HandleANSRoute(HTTPRequest* req, const std::string& path)
{
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

// ---- Static HTML -------------------------------------------------------

static bool HandleRoot(HTTPRequest* req)
{
    if (req->GetRequestMethod() != HTTPRequest::GET) {
        req->WriteReply(HTTP_BAD_METHOD, "");
        return false;
    }
    if (!CheckWebUIHost(req)) return false;
    static const std::string html =
        "<!DOCTYPE html>\n"
        "<html>\n"
        "<head><meta charset=\"UTF-8\"><title>Avian Core Web UI</title></head>\n"
        "<body>\n"
        "<h1>Avian Core Web UI</h1>\n"
        "<p>Node is running. <strong>Phase 1 backend — developer token flow only.</strong></p>\n"
        "<p>Read the session token from <code>webui.cookie</code> in the datadir and pass it as\n"
        "<code>Authorization: Bearer &lt;token&gt;</code> on all <code>/api/*</code> requests.</p>\n"
        "</body>\n"
        "</html>\n";
    req->WriteHeader("Content-Type", "text/html; charset=UTF-8");
    req->WriteReply(HTTP_OK, html);
    return true;
}

// ---- Main dispatcher ---------------------------------------------------

static bool WebUIDispatch(HTTPRequest* req, const std::string& /*prefix*/)
{
    std::string uri = req->GetURI();
    size_t qmark = uri.find('?');
    const std::string path = (qmark != std::string::npos) ? uri.substr(0, qmark) : uri;

    if (path == "/webui" || path == "/webui/") return HandleRoot(req);

    if (path == "/webui/api/node/status")       return HandleNodeStatus(req);
    if (path == "/webui/api/node/features")      return HandleNodeFeatures(req);
    if (path == "/webui/api/wallets/loaded")     return HandleWalletsLoaded(req);
    if (path == "/webui/api/wallets/available")  return HandleWalletsAvailable(req);
    if (path == "/webui/api/wallets/load")       return HandleWalletLoad(req);
    if (path == "/webui/api/wallets/create")     return HandleWalletCreate(req);
    if (path == "/webui/api/wallets/unload")     return HandleWalletUnload(req);

    if (path.starts_with("/webui/api/wallet/")) return HandleWalletRoute(req, path);
    if (path.starts_with("/webui/api/ans/"))    return HandleANSRoute(req, path);

    req->WriteReply(HTTP_NOT_FOUND, R"({"error":"not found"})");
    return false;
}

// ---- Lifecycle ---------------------------------------------------------

void StartWebUI(NodeContext& node)
{
    g_node = &node;
    if (!InitWebUIAuth()) {
        LogWarning("WebUI: Failed to initialise authentication, web UI will not start\n");
        g_node = nullptr;
        return;
    }
    RegisterHTTPHandler("/webui/", false, WebUIDispatch);

    if (gArgs.IsArgSet("-webuiport") || gArgs.IsArgSet("-webuibind")) {
        const uint16_t port = static_cast<uint16_t>(gArgs.GetIntArg("-webuiport", DEFAULT_WEBUI_PORT));
        const std::string bind_addr = gArgs.GetArg("-webuibind", DEFAULT_WEBUI_BIND);
        if (!BindHTTPAdditionalPort(bind_addr, port)) {
            LogWarning("WebUI: Failed to bind dedicated port %d on %s\n", port, bind_addr);
        }
    }

    LogInfo("WebUI endpoint started at /webui/ (token in webui.cookie)\n");
}

void InterruptWebUI()
{
    // No async operations to interrupt in Phase 1.
}

void StopWebUI()
{
    UnregisterHTTPHandler("/webui/", false);
    if (g_webui_cookie_generated) {
        try {
            fs::remove(GetWebUICookiePath());
        } catch (const fs::filesystem_error& e) {
            LogWarning("WebUI: Unable to remove cookie file %s: %s\n",
                       fs::PathToString(e.path1()), e.code().message());
        }
        g_webui_cookie_generated = false;
    }
    g_node = nullptr;
}
