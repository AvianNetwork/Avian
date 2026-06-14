// Copyright (c) 2025-present The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <bitcoin-build-config.h> // IWYU pragma: keep

#include <webui/webui.h>
#include <webui/assets.h>

#include <assets/ans.h>
#include <assets/assets.h>
#include <assets/assetdb.h>
#include <clientversion.h>
#include <common/args.h>
#include <common/url.h>
#include <common/signmessage.h>
#include <core_io.h>
#include <hash.h>
#include <httpserver.h>
#include <key_io.h>
#include <logging.h>
#include <net.h>
#include <node/context.h>
#include <node/transaction.h>
#include <node/types.h>
#include <outputtype.h>
#include <psbt.h>
#include <random.h>
#include <rpc/protocol.h>
#include <script/solver.h>
#include <streams.h>
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
#include <wallet/asset_tx.h>
#include <wallet/coincontrol.h>
#include <wallet/context.h>
#include <wallet/receive.h>
#include <wallet/spend.h>
#include <wallet/wallet.h>
#include <wallet/walletutil.h>
#endif

#include <event2/buffer.h>
#include <event2/http.h>
#include <kernel/mempool_entry.h>
#include <validationinterface.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

using node::NodeContext;

static NodeContext* g_node{nullptr};
static std::string g_webui_token;
static bool g_webui_cookie_generated{false};
static bool g_webui_use_password{false};
static std::string g_webui_password_hash; // hex-encoded SHA256 of configured password
static std::set<std::string> g_webui_allowed_hosts; // populated in StartWebUI()

// ---- Cookie auth -------------------------------------------------------

static fs::path GetWebUICookiePath()
{
    return gArgs.GetDataDirNet() / fs::PathFromString(WEBUI_COOKIE_FILE);
}

static bool InitWebUIAuth()
{
    std::string pw = gArgs.GetArg("-webuipassword", "");
    if (!pw.empty()) {
        // Password mode: hash the password and store; no cookie file needed.
        HashWriter h{};
        h.write({reinterpret_cast<const std::byte*>(pw.data()), pw.size()});
        g_webui_password_hash = HexStr(h.GetHash());
        g_webui_use_password = true;
        LogInfo("WebUI: using configured password authentication\n");
        return true;
    }

    // Cookie mode (default): generate random token and write to disk.
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

// Defend against DNS rebinding: only allow requests whose Host header matches
// an address the HTTP server is actually bound to. Port suffix is ignored.
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
    if (g_webui_allowed_hosts.count(hostname)) {
        return true;
    }
    req->WriteReply(HTTP_FORBIDDEN, R"({"error":"Host not allowed"})");
    return false;
}

static bool CheckWebUIToken(const std::string& submitted)
{
    bool ok = false;
    if (g_webui_use_password) {
        HashWriter h{};
        h.write({reinterpret_cast<const std::byte*>(submitted.data()), submitted.size()});
        std::string submitted_hash = HexStr(h.GetHash());
        ok = TimingResistantEqual(submitted_hash, g_webui_password_hash);
    } else {
        ok = TimingResistantEqual(submitted, g_webui_token);
    }
    if (!ok) UninterruptibleSleep(std::chrono::milliseconds{250});
    return ok;
}

static bool CheckWebUIAuth(HTTPRequest* req)
{
    auto [present, auth] = req->GetHeader("authorization");
    if (!present || auth.substr(0, 7) != "Bearer ") {
        req->WriteHeader("WWW-Authenticate", "Bearer realm=\"aviand-webui\"");
        req->WriteReply(HTTP_UNAUTHORIZED, R"({"error":"unauthorized"})");
        return false;
    }
    if (!CheckWebUIToken(auth.substr(7))) {
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

    // Strip scheme to get "host[:port]", then strip port, same as CheckWebUIHost.
    std::string host = origin;
    if (host.substr(0, 7) == "http://")  host = host.substr(7);
    if (host.substr(0, 8) == "https://") host = host.substr(8);
    if (!host.empty() && host.front() == '[') {
        size_t close = host.find(']');
        host = (close != std::string::npos) ? host.substr(1, close - 1) : host;
    } else {
        size_t colon = host.rfind(':');
        if (colon != std::string::npos) host = host.substr(0, colon);
    }
    if (g_webui_allowed_hosts.count(host)) return origin;

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

// ---- Auth info (unauthenticated) ---------------------------------------

// Tells the login page which auth mode is active so it can show the right hint.
static bool HandleAuthInfo(HTTPRequest* req)
{
    if (!CheckWebUIHost(req)) return false;
    auto cors = CheckWebUICORS(req);
    if (!cors) return false;
    UniValue obj(UniValue::VOBJ);
    obj.pushKV("mode", g_webui_use_password ? "password" : "cookie");
    SetJSONHeaders(req, *cors);
    req->WriteReply(HTTP_OK, obj.write());
    return true;
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
        try { int v = std::stoi(*lp); if (v > 0 && v <= 2000) limit = v; } catch (...) {}
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
    if (req->GetRequestMethod() != HTTPRequest::POST) {
        req->WriteReply(HTTP_BAD_METHOD, "");
        return false;
    }
    if (!CheckWebUIHost(req)) return false;
    auto cors = CheckWebUICORS(req);
    if (!cors) return false;
    if (!CheckWebUIAuth(req)) return false;

    // Optional body: { "type": "legacy"|"p2sh-segwit"|"bech32", "label": "..." }
    UniValue body;
    const std::string raw = req->ReadBody();
    if (!raw.empty()) body.read(raw);
    const std::string type_str = (body.isObject() && body["type"].isStr()) ? body["type"].get_str() : "";
    const std::string label    = (body.isObject() && body["label"].isStr()) ? body["label"].get_str() : "";

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

        OutputType out_type = w->getDefaultAddressType();
        if (!type_str.empty()) {
            auto parsed = ParseOutputType(type_str);
            if (!parsed) {
                req->WriteReply(HTTP_BAD_REQUEST, R"({"error":"Unknown address type. Use legacy, p2sh-segwit or bech32"})");
                return false;
            }
            out_type = *parsed;
        }

        auto dest_result = w->getNewDestination(out_type, label);
        if (!dest_result) {
            const std::string err_msg = util::ErrorString(dest_result).original;
            req->WriteReply(HTTP_BAD_REQUEST,
                "{\"error\":\"" + err_msg + "\"}");
            return false;
        }
        UniValue obj(UniValue::VOBJ);
        obj.pushKV("wallet",  wallet_name);
        obj.pushKV("address", EncodeDestination(*dest_result));
        obj.pushKV("type",    FormatOutputType(out_type));
        obj.pushKV("label",   label);
        SetJSONHeaders(req, *cors);
        req->WriteReply(HTTP_OK, obj.write());
        return true;
    }
    req->WriteReply(HTTP_NOT_FOUND, R"({"error":"Wallet not found or not loaded"})");
    return false;
}

static bool HandleWalletAddresses(HTTPRequest* req, const std::string& wallet_name)
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

        UniValue arr(UniValue::VARR);
        for (const auto& addr : w->getAddresses()) {
            // Only include receive addresses (skip send-side address book entries)
            if (addr.purpose != wallet::AddressPurpose::RECEIVE) continue;
            const char* addrType = "unknown";
            if (std::holds_alternative<PKHash>(addr.dest))             addrType = "legacy";
            else if (std::holds_alternative<WitnessV0KeyHash>(addr.dest)) addrType = "p2wpkh";
            else if (std::holds_alternative<ScriptHash>(addr.dest))    addrType = "p2sh";
            UniValue entry(UniValue::VOBJ);
            entry.pushKV("address", EncodeDestination(addr.dest));
            entry.pushKV("label",   addr.name);
            entry.pushKV("is_mine", addr.is_mine);
            entry.pushKV("type",    addrType);
            arr.push_back(entry);
        }
        UniValue obj(UniValue::VOBJ);
        obj.pushKV("wallet",    wallet_name);
        obj.pushKV("addresses", arr);
        SetJSONHeaders(req, *cors);
        req->WriteReply(HTTP_OK, obj.write());
        return true;
    }
    req->WriteReply(HTTP_NOT_FOUND, R"({"error":"Wallet not found or not loaded"})");
    return false;
}

static bool HandleWalletSetAddressLabel(HTTPRequest* req, const std::string& wallet_name)
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
        req->WriteReply(HTTP_BAD_REQUEST, R"({"error":"Invalid JSON"})");
        return false;
    }
    const UniValue& addrVal  = body["address"];
    const UniValue& labelVal = body["label"];
    if (!addrVal.isStr()) {
        req->WriteReply(HTTP_BAD_REQUEST, R"({"error":"address required"})");
        return false;
    }
    const CTxDestination dest = DecodeDestination(addrVal.get_str());
    if (!IsValidDestination(dest)) {
        req->WriteReply(HTTP_BAD_REQUEST, R"({"error":"Invalid address"})");
        return false;
    }
    const std::string label = labelVal.isStr() ? labelVal.get_str() : "";

    if (!g_node || !g_node->wallet_loader) {
        req->WriteReply(HTTP_SERVICE_UNAVAILABLE, R"({"error":"Wallet support not available"})");
        return false;
    }
    for (auto& w : g_node->wallet_loader->getWallets()) {
        if (w->getWalletName() != wallet_name) continue;
        w->setAddressBook(dest, label, wallet::AddressPurpose::RECEIVE);
        UniValue obj(UniValue::VOBJ);
        obj.pushKV("success", true);
        SetJSONHeaders(req, *cors);
        req->WriteReply(HTTP_OK, obj.write());
        return true;
    }
    req->WriteReply(HTTP_NOT_FOUND, R"({"error":"Wallet not found or not loaded"})");
    return false;
}

static bool HandleWalletSendAsset(HTTPRequest* req, const std::string& wallet_name)
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
        req->WriteReply(HTTP_BAD_REQUEST, R"({"error":"Invalid JSON"})");
        return false;
    }
    const UniValue& assetVal  = body["asset"];
    const UniValue& addrVal   = body["address"];
    const UniValue& amountVal = body["amount"];
    if (!assetVal.isStr() || !addrVal.isStr() || !amountVal.isStr()) {
        req->WriteReply(HTTP_BAD_REQUEST, R"({"error":"asset, address and amount required"})");
        return false;
    }
    const std::string asset_name = assetVal.get_str();
    const std::string address    = addrVal.get_str();
    CAmount nAmount;
    if (!ParseFixedPoint(amountVal.get_str(), 8, &nAmount) || nAmount <= 0) {
        req->WriteReply(HTTP_BAD_REQUEST, R"({"error":"Invalid amount"})");
        return false;
    }

    if (!g_node || !g_node->wallet_loader) {
        req->WriteReply(HTTP_SERVICE_UNAVAILABLE, R"({"error":"Wallet support not available"})");
        return false;
    }

    wallet::WalletContext* wctx = g_node->wallet_loader->context();
    if (!wctx) {
        req->WriteReply(HTTP_SERVICE_UNAVAILABLE, R"({"error":"Wallet context unavailable"})");
        return false;
    }
    auto pwallet = wallet::GetWallet(*wctx, wallet_name);
    if (!pwallet) {
        req->WriteReply(HTTP_NOT_FOUND, R"({"error":"Wallet not found or not loaded"})");
        return false;
    }
    if (pwallet->IsLocked()) {
        req->WriteReply(423, R"({"error":"Wallet is locked. Unlock it first."})");
        return false;
    }

    // Verify the wallet actually holds this asset
    std::pair<int, std::string> asset_err;
    {
        LOCK(pwallet->cs_wallet);
        if (!wallet::VerifyWalletHasAsset(*pwallet, asset_name, asset_err)) {
            req->WriteReply(HTTP_BAD_REQUEST,
                "{\"error\":\"" + asset_err.second + "\"}");
            return false;
        }
    }

    wallet::CCoinControl coin_control;
    CAssetTransfer transfer(asset_name, nAmount);
    std::vector<std::pair<CAssetTransfer, std::string>> transfers{{transfer, address}};
    std::pair<int, std::string> err;
    CTransactionRef txRef;
    CAmount nFeeRequired;

    if (!wallet::CreateTransferAssetTransaction(*pwallet, coin_control, transfers, "", err, txRef, nFeeRequired)) {
        req->WriteReply(HTTP_BAD_REQUEST,
            "{\"error\":\"" + err.second + "\"}");
        return false;
    }

    std::string txid;
    if (!wallet::SendAssetTransaction(*pwallet, txRef, err, txid)) {
        req->WriteReply(HTTP_BAD_REQUEST,
            "{\"error\":\"" + err.second + "\"}");
        return false;
    }

    UniValue obj(UniValue::VOBJ);
    obj.pushKV("txid", txid);
    obj.pushKV("fee",  ValueFromAmount(nFeeRequired).write());
    SetJSONHeaders(req, *cors);
    req->WriteReply(HTTP_OK, obj.write());
    return true;
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
    {
        LOCK(cs_main);
        for (const auto& [name, amount] : asset_balances) {
            UniValue aobj(UniValue::VOBJ);
            aobj.pushKV("name",    name);
            aobj.pushKV("balance", AssetUnitValueFromAmount(amount, name));
            if (passets) {
                CNewAsset meta;
                if (passets->GetAssetMetaDataIfExists(name, meta) && meta.nHasIPFS && !meta.strIPFSHash.empty()) {
                    const std::string cid = EncodeAssetData(meta.strIPFSHash);
                    if (!cid.empty()) aobj.pushKV("ipfs", cid);
                }
            }
            arr.push_back(aobj);
        }
    }
    UniValue obj(UniValue::VOBJ);
    obj.pushKV("wallet", wallet_name);
    obj.pushKV("assets", arr);
    SetJSONHeaders(req, *cors);
    req->WriteReply(HTTP_OK, obj.write());
    return true;
}

static bool HandleWalletUnlock(HTTPRequest* req, const std::string& wallet_name)
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
    const UniValue& passVal = body["passphrase"];
    if (!passVal.isStr()) {
        req->WriteReply(HTTP_BAD_REQUEST, R"({"error":"\"passphrase\" field required"})");
        return false;
    }
    SecureString passphrase;
    const std::string& ps = passVal.get_str();
    passphrase.assign(ps.begin(), ps.end());

    int64_t timeout{0};
    if (body["timeout"].isNum()) {
        timeout = body["timeout"].getInt<int64_t>();
        if (timeout < 0) timeout = 0;
        if (timeout > 86400) timeout = 86400; // cap at 24 h
    }

    for (auto& w : g_node->wallet_loader->getWallets()) {
        if (w->getWalletName() != wallet_name) continue;

        if (!w->isCrypted()) {
            req->WriteReply(HTTP_BAD_REQUEST, R"({"error":"Wallet is not encrypted"})");
            return false;
        }
        if (!w->unlock(passphrase)) {
            UninterruptibleSleep(std::chrono::milliseconds{250});
            req->WriteReply(HTTP_BAD_REQUEST, R"({"error":"Incorrect passphrase"})");
            return false;
        }
        if (timeout > 0) {
            wallet::WalletContext* wctx = g_node->wallet_loader->context();
            std::thread([wctx, wallet_name, timeout]() {
                std::this_thread::sleep_for(std::chrono::seconds(timeout));
                if (!wctx) return;
                auto pwallet = wallet::GetWallet(*wctx, wallet_name);
                if (pwallet) pwallet->Lock();
            }).detach();
        }
        UniValue obj(UniValue::VOBJ);
        obj.pushKV("success", true);
        if (timeout > 0) obj.pushKV("relock_in", timeout);
        SetJSONHeaders(req, *cors);
        req->WriteReply(HTTP_OK, obj.write());
        return true;
    }
    req->WriteReply(HTTP_NOT_FOUND, R"({"error":"Wallet not found or not loaded"})");
    return false;
}

static bool HandleWalletLock(HTTPRequest* req, const std::string& wallet_name)
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
    for (auto& w : g_node->wallet_loader->getWallets()) {
        if (w->getWalletName() != wallet_name) continue;

        if (!w->isCrypted()) {
            req->WriteReply(HTTP_BAD_REQUEST, R"({"error":"Wallet is not encrypted"})");
            return false;
        }
        w->lock();
        UniValue obj(UniValue::VOBJ);
        obj.pushKV("success", true);
        SetJSONHeaders(req, *cors);
        req->WriteReply(HTTP_OK, obj.write());
        return true;
    }
    req->WriteReply(HTTP_NOT_FOUND, R"({"error":"Wallet not found or not loaded"})");
    return false;
}

static bool HandleWalletSignMessage(HTTPRequest* req, const std::string& wallet_name)
{
    // POST {address, message} → {address, message, signature}
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
    const UniValue& msgVal  = body["message"];
    if (!addrVal.isStr() || addrVal.get_str().empty()) {
        req->WriteReply(HTTP_BAD_REQUEST, R"({"error":"\"address\" field required"})");
        return false;
    }
    if (!msgVal.isStr()) {
        req->WriteReply(HTTP_BAD_REQUEST, R"({"error":"\"message\" field required"})");
        return false;
    }
    CTxDestination dest = DecodeDestination(addrVal.get_str());
    if (!IsValidDestination(dest)) {
        req->WriteReply(HTTP_BAD_REQUEST, R"({"error":"Invalid address"})");
        return false;
    }
    // Extract hash160 — accepts both P2PKH and P2WPKH since they share the same key
    PKHash pkhash;
    if (const auto* p = std::get_if<PKHash>(&dest)) {
        pkhash = *p;
    } else if (const auto* p = std::get_if<WitnessV0KeyHash>(&dest)) {
        pkhash = PKHash(uint160(*p));
    } else {
        req->WriteReply(HTTP_BAD_REQUEST, R"({"error":"Address must be P2PKH or P2WPKH for message signing"})");
        return false;
    }

    for (auto& w : g_node->wallet_loader->getWallets()) {
        if (w->getWalletName() != wallet_name) continue;

        if (w->isCrypted() && w->isLocked()) {
            req->WriteReply(423, R"({"error":"Wallet is locked. Unlock it first."})");
            return false;
        }
        std::string signature;
        const SigningResult result = w->signMessage(msgVal.get_str(), pkhash, signature);
        if (result != SigningResult::OK) {
            const char* err = (result == SigningResult::PRIVATE_KEY_NOT_AVAILABLE)
                ? "Private key not available for this address"
                : "Message signing failed";
            req->WriteReply(HTTP_BAD_REQUEST, std::string{R"({"error":")"} + err + "\"}");
            return false;
        }
        UniValue obj(UniValue::VOBJ);
        obj.pushKV("address",   addrVal.get_str());
        obj.pushKV("message",   msgVal.get_str());
        obj.pushKV("signature", signature);
        SetJSONHeaders(req, *cors);
        req->WriteReply(HTTP_OK, obj.write());
        return true;
    }
    req->WriteReply(HTTP_NOT_FOUND, R"({"error":"Wallet not found or not loaded"})");
    return false;
}

static bool HandleWalletPSBTCreate(HTTPRequest* req, const std::string& wallet_name)
{
    // POST {recipients: [{address, amount, subtractFee}]} → {psbt, fee}
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
    const UniValue& recipientsVal = body["recipients"];
    if (!recipientsVal.isArray() || recipientsVal.empty()) {
        req->WriteReply(HTTP_BAD_REQUEST, R"({"error":"\"recipients\" array required"})");
        return false;
    }

    std::vector<wallet::CRecipient> recipients;
    for (size_t i = 0; i < recipientsVal.size(); ++i) {
        const UniValue& r = recipientsVal[i];
        if (!r.isObject()) {
            req->WriteReply(HTTP_BAD_REQUEST, R"({"error":"Each recipient must be an object"})");
            return false;
        }
        CTxDestination dest = DecodeDestination(r["address"].getValStr());
        if (!IsValidDestination(dest)) {
            req->WriteReply(HTTP_BAD_REQUEST, R"({"error":"Invalid recipient address"})");
            return false;
        }
        int64_t amount_raw{0};
        if (!ParseFixedPoint(r["amount"].getValStr(), 8, &amount_raw) || amount_raw <= 0 || !MoneyRange(amount_raw)) {
            req->WriteReply(HTTP_BAD_REQUEST, R"({"error":"Invalid or out-of-range amount"})");
            return false;
        }
        const bool subtract_fee = r["subtractFee"].isBool() && r["subtractFee"].get_bool();
        recipients.push_back({dest, static_cast<CAmount>(amount_raw), subtract_fee, {}});
    }

    for (auto& w : g_node->wallet_loader->getWallets()) {
        if (w->getWalletName() != wallet_name) continue;

        // PSBT creation skips signing, so it works even on a locked wallet
        wallet::CCoinControl coin_control;
        int change_pos{-1};
        CAmount fee{0};
        auto tx_result = w->createTransaction(recipients, coin_control, /*sign=*/false, change_pos, fee);
        if (!tx_result) {
            UniValue obj(UniValue::VOBJ);
            obj.pushKV("success", false);
            obj.pushKV("error",   util::ErrorString(tx_result).original);
            SetJSONHeaders(req, *cors);
            req->WriteReply(HTTP_BAD_REQUEST, obj.write());
            return false;
        }

        CMutableTransaction mtx(**tx_result);
        PartiallySignedTransaction psbtx{mtx};
        bool complete{false};
        size_t n_signed{0};
        auto fill_err = w->fillPSBT(std::nullopt, /*sign=*/false, /*bip32derivs=*/true, &n_signed, psbtx, complete);
        if (fill_err) {
            req->WriteReply(HTTP_INTERNAL_SERVER_ERROR, R"({"error":"Failed to build PSBT"})");
            return false;
        }

        DataStream ss{};
        ss << psbtx;
        UniValue obj(UniValue::VOBJ);
        obj.pushKV("psbt", EncodeBase64(ss.str()));
        obj.pushKV("fee",  ValueFromAmount(fee));
        SetJSONHeaders(req, *cors);
        req->WriteReply(HTTP_OK, obj.write());
        return true;
    }
    req->WriteReply(HTTP_NOT_FOUND, R"({"error":"Wallet not found or not loaded"})");
    return false;
}

static bool HandleWalletPSBTSign(HTTPRequest* req, const std::string& wallet_name)
{
    // POST {psbt: "base64..."} → {psbt: "base64...", complete: bool, signed_inputs: n}
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
    const UniValue& psbtVal = body["psbt"];
    if (!psbtVal.isStr() || psbtVal.get_str().empty()) {
        req->WriteReply(HTTP_BAD_REQUEST, R"({"error":"\"psbt\" field required"})");
        return false;
    }

    // Optional sighash type — defaults to ALL|FORKID (0x41).
    std::optional<int> sighash_type;
    if (body.exists("sighash") && body["sighash"].isStr()) {
        static const std::map<std::string, int> SIGHASH_MAP = {
            {"ALL|FORKID",                  0x41},
            {"NONE|FORKID",                 0x42},
            {"SINGLE|FORKID",               0x43},
            {"ALL|FORKID|ANYONECANPAY",     0xC1},
            {"NONE|FORKID|ANYONECANPAY",    0xC2},
            {"SINGLE|FORKID|ANYONECANPAY",  0xC3},
        };
        const auto it = SIGHASH_MAP.find(body["sighash"].get_str());
        if (it == SIGHASH_MAP.end()) {
            req->WriteReply(HTTP_BAD_REQUEST, R"({"error":"Unknown sighash type"})");
            return false;
        }
        sighash_type = it->second;
    }

    PartiallySignedTransaction psbtx;
    std::string parse_err;
    if (!DecodeBase64PSBT(psbtx, psbtVal.get_str(), parse_err)) {
        req->WriteReply(HTTP_BAD_REQUEST, "{\"error\":\"Invalid PSBT: " + parse_err + "\"}");
        return false;
    }

    for (auto& w : g_node->wallet_loader->getWallets()) {
        if (w->getWalletName() != wallet_name) continue;

        if (w->isCrypted() && w->isLocked()) {
            req->WriteReply(423, R"({"error":"Wallet is locked. Unlock it first."})");
            return false;
        }

        bool complete{false};
        size_t n_signed{0};
        auto fill_err = w->fillPSBT(sighash_type, /*sign=*/true, /*bip32derivs=*/true, &n_signed, psbtx, complete);
        if (fill_err) {
            req->WriteReply(HTTP_BAD_REQUEST, R"({"error":"Failed to sign PSBT"})");
            return false;
        }

        DataStream ss{};
        ss << psbtx;
        UniValue obj(UniValue::VOBJ);
        obj.pushKV("psbt",          EncodeBase64(ss.str()));
        obj.pushKV("complete",      complete);
        obj.pushKV("signed_inputs", static_cast<int>(n_signed));
        SetJSONHeaders(req, *cors);
        req->WriteReply(HTTP_OK, obj.write());
        return true;
    }
    req->WriteReply(HTTP_NOT_FOUND, R"({"error":"Wallet not found or not loaded"})");
    return false;
}

static bool HandleWalletUTXOs(HTTPRequest* req, const std::string& wallet_name)
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

    CAmount min_amount = 0;
    CAmount max_amount = MAX_MONEY;
    if (auto p = req->GetQueryParameter("min_amount")) {
        try { int64_t v = std::stoll(*p); if (v >= 0) min_amount = v; } catch (...) {}
    }
    if (auto p = req->GetQueryParameter("max_amount")) {
        try { int64_t v = std::stoll(*p); if (v > 0) max_amount = v; } catch (...) {}
    }
    const bool show_all = req->GetQueryParameter("all").has_value();

    for (auto& w : g_node->wallet_loader->getWallets()) {
        if (w->getWalletName() != wallet_name) continue;

        int count = 0;
        CAmount total = 0;
        UniValue utxo_arr(UniValue::VARR);

        // Unspent UTXOs — listCoins() provides depth_in_main_chain
        for (const auto& coins : w->listCoins()) {
            for (const auto& outpair : coins.second) {
                const COutPoint& outpoint = std::get<0>(outpair);
                const interfaces::WalletTxOut& out = std::get<1>(outpair);
                const CAmount val = out.txout.nValue;
                if (val < min_amount || val > max_amount) continue;
                ++count;
                total += val;

                CTxDestination addr;
                std::string address_str;
                if (ExtractDestination(out.txout.scriptPubKey, addr))
                    address_str = EncodeDestination(addr);

                UniValue u(UniValue::VOBJ);
                u.pushKV("txid",          outpoint.hash.GetHex());
                u.pushKV("vout",          (int)outpoint.n);
                u.pushKV("address",       address_str);
                u.pushKV("amount",        ValueFromAmount(val));
                u.pushKV("confirmations", out.depth_in_main_chain);
                u.pushKV("is_spent",      false);
                utxo_arr.push_back(u);
            }
        }

        // Spent outputs — only when ?all=true
        if (show_all) {
            const auto all_txs = w->getWalletTxs();

            // Build spending map: (txid_hex, vout) → spending_txid_hex
            std::map<std::pair<std::string, uint32_t>, std::string> spending_map;
            for (const auto& wtx : all_txs) {
                const std::string stxid = wtx.tx->GetHash().GetHex();
                for (const auto& vin : wtx.tx->vin) {
                    if (!vin.prevout.IsNull())
                        spending_map[{vin.prevout.hash.GetHex(), vin.prevout.n}] = stxid;
                }
            }

            for (const auto& wtx : all_txs) {
                const std::string txid = wtx.tx->GetHash().GetHex();
                for (size_t i = 0; i < wtx.tx->vout.size(); ++i) {
                    if (i >= wtx.txout_is_mine.size() || !wtx.txout_is_mine[i]) continue;
                    auto sp_it = spending_map.find({txid, (uint32_t)i});
                    if (sp_it == spending_map.end()) continue; // unspent — already in listCoins()

                    const CAmount val = wtx.tx->vout[i].nValue;
                    if (val < min_amount || val > max_amount) continue;
                    ++count;

                    std::string address_str;
                    if (i < wtx.txout_address.size() && IsValidDestination(wtx.txout_address[i])) {
                        address_str = EncodeDestination(wtx.txout_address[i]);
                    } else {
                        CTxDestination addr;
                        if (ExtractDestination(wtx.tx->vout[i].scriptPubKey, addr))
                            address_str = EncodeDestination(addr);
                    }

                    UniValue u(UniValue::VOBJ);
                    u.pushKV("txid",     txid);
                    u.pushKV("vout",     (int)i);
                    u.pushKV("address",  address_str);
                    u.pushKV("amount",   ValueFromAmount(val));
                    u.pushKV("is_spent", true);
                    u.pushKV("spent_by", sp_it->second);
                    utxo_arr.push_back(u);
                }
            }
        }

        UniValue obj(UniValue::VOBJ);
        obj.pushKV("wallet",      wallet_name);
        obj.pushKV("count",       count);
        obj.pushKV("total_value", ValueFromAmount(total));
        obj.pushKV("utxos",       utxo_arr);
        SetJSONHeaders(req, *cors);
        req->WriteReply(HTTP_OK, obj.write());
        return true;
    }
    req->WriteReply(HTTP_NOT_FOUND, R"({"error":"Wallet not found or not loaded"})");
    return false;
}

static bool HandleWalletConsolidate(HTTPRequest* req, const std::string& wallet_name)
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

    const UniValue& destVal = body["destination"];
    if (!destVal.isStr() || destVal.get_str().empty()) {
        req->WriteReply(HTTP_BAD_REQUEST, R"({"error":"\"destination\" field required"})");
        return false;
    }
    CTxDestination dest = DecodeDestination(destVal.get_str());
    if (!IsValidDestination(dest)) {
        req->WriteReply(HTTP_BAD_REQUEST, R"({"error":"Invalid Avian address"})");
        return false;
    }

    const CAmount min_amount = body["min_amount"].isNum() ? body["min_amount"].getInt<int64_t>() : 100000LL;
    const CAmount max_amount = body["max_amount"].isNum() ? body["max_amount"].getInt<int64_t>() : 2500000000LL;
    const int max_utxos_per_batch = [&] {
        int v = body["max_utxos_per_batch"].isNum() ? body["max_utxos_per_batch"].getInt<int>() : 200;
        return (v >= 2 && v <= 500) ? v : 200;
    }();
    const int max_batches = body["max_batches"].isNum() ? body["max_batches"].getInt<int>() : 0;

    for (auto& w : g_node->wallet_loader->getWallets()) {
        if (w->getWalletName() != wallet_name) continue;

        if (w->isCrypted() && w->isLocked()) {
            req->WriteReply(423, R"({"error":"Wallet is locked. Unlock it first."})");
            return false;
        }

        int batches = 0;
        int utxos_consolidated = 0;
        UniValue txids(UniValue::VARR);
        std::string err_msg;

        while (true) {
            if (max_batches > 0 && batches >= max_batches) break;

            wallet::CCoinControl coin_control;
            coin_control.m_feerate = CFeeRate(1000);
            coin_control.fOverrideFeeRate = true;

            CAmount batch_total = 0;
            int batch_count = 0;

            for (const auto& coins : w->listCoins()) {
                for (const auto& outpair : coins.second) {
                    if (batch_count >= max_utxos_per_batch) break;
                    const COutPoint& output = std::get<0>(outpair);
                    const CAmount val = std::get<1>(outpair).txout.nValue;
                    if (val < min_amount || val > max_amount) continue;
                    coin_control.Select(output);
                    batch_total += val;
                    ++batch_count;
                }
                if (batch_count >= max_utxos_per_batch) break;
            }

            if (batch_count < 2) break;

            wallet::CRecipient recipient{dest, batch_total, /*subtract_fee=*/true, {}};
            int change_pos{-1};
            CAmount fee{0};
            auto tx_result = w->createTransaction({recipient}, coin_control, /*sign=*/true, change_pos, fee);
            if (!tx_result) {
                err_msg = util::ErrorString(tx_result).original;
                break;
            }
            w->commitTransaction(*tx_result, /*value_map=*/{}, /*order_form=*/{});
            txids.push_back((*tx_result)->GetHash().GetHex());
            ++batches;
            utxos_consolidated += batch_count;
        }

        UniValue obj(UniValue::VOBJ);
        obj.pushKV("success",             err_msg.empty() || batches > 0);
        obj.pushKV("batches",             batches);
        obj.pushKV("utxos_consolidated",  utxos_consolidated);
        obj.pushKV("txids",               txids);
        if (!err_msg.empty()) obj.pushKV("error", err_msg);
        SetJSONHeaders(req, *cors);
        req->WriteReply(HTTP_OK, obj.write());
        return true;
    }
    req->WriteReply(HTTP_NOT_FOUND, R"({"error":"Wallet not found or not loaded"})");
    return false;
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
    const std::string wallet_name = UrlDecode(rest.substr(0, slash));
    const std::string action = rest.substr(slash + 1);

#ifdef ENABLE_WALLET
    if (action == "summary")         return HandleWalletSummary(req, wallet_name);
    if (action == "transactions")    return HandleWalletTransactions(req, wallet_name);
    if (action == "receive-address") return HandleWalletReceiveAddress(req, wallet_name);
    if (action == "addresses")          return HandleWalletAddresses(req, wallet_name);
    if (action == "set-address-label")  return HandleWalletSetAddressLabel(req, wallet_name);
    if (action == "send-asset")         return HandleWalletSendAsset(req, wallet_name);
    if (action == "assets")          return HandleWalletAssets(req, wallet_name);
    if (action == "send")            return HandleWalletSend(req, wallet_name);
    if (action == "unlock")          return HandleWalletUnlock(req, wallet_name);
    if (action == "lock")            return HandleWalletLock(req, wallet_name);
    if (action == "signmessage")     return HandleWalletSignMessage(req, wallet_name);
    if (action == "psbt/create")     return HandleWalletPSBTCreate(req, wallet_name);
    if (action == "psbt/sign")       return HandleWalletPSBTSign(req, wallet_name);
    if (action == "utxos")           return HandleWalletUTXOs(req, wallet_name);
    if (action == "consolidate")     return HandleWalletConsolidate(req, wallet_name);
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
        req->WriteReply(HTTP_BAD_REQUEST, "{\"error\":\"Invalid PSBT: " + parse_err + "\"}");
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
        req->WriteReply(HTTP_BAD_REQUEST, "{\"error\":\"Invalid PSBT: " + parse_err + "\"}");
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
        req->WriteReply(HTTP_BAD_REQUEST, "{\"error\":\"" + err_string + "\"}");
        return false;
    }

    UniValue obj(UniValue::VOBJ);
    obj.pushKV("txid", tx->GetHash().GetHex());
    SetJSONHeaders(req, *cors);
    req->WriteReply(HTTP_OK, obj.write());
    return true;
}

static bool HandlePSBTNodeRoute(HTTPRequest* req, const std::string& path)
{
    static constexpr std::string_view PSBT_PREFIX{"/webui/api/psbt/"};
    const std::string action = path.substr(PSBT_PREFIX.size());

    if (action == "decode")    return HandlePSBTDecode(req);
    if (action == "broadcast") return HandlePSBTBroadcast(req);

    req->WriteReply(HTTP_NOT_FOUND, R"({"error":"not found"})");
    return false;
}

// ---- Static file serving -----------------------------------------------

static std::string_view GetMimeType(const std::string& path)
{
    const auto dot = path.rfind('.');
    if (dot == std::string::npos) return "application/octet-stream";
    const std::string_view ext{path.data() + dot + 1, path.size() - dot - 1};
    if (ext == "html")  return "text/html; charset=UTF-8";
    if (ext == "css")   return "text/css";
    if (ext == "js" || ext == "mjs") return "application/javascript";
    if (ext == "json")  return "application/json";
    if (ext == "svg")   return "image/svg+xml";
    if (ext == "png")   return "image/png";
    if (ext == "ico")   return "image/x-icon";
    if (ext == "woff2") return "font/woff2";
    if (ext == "woff")  return "font/woff";
    if (ext == "ttf")   return "font/ttf";
    return "application/octet-stream";
}

// Serves static files from -webuiassets dir (development) or embedded assets (production).
// No auth required — the HTML/CSS/JS itself is not sensitive.
static bool HandleStaticFile(HTTPRequest* req, const std::string& path)
{
    if (req->GetRequestMethod() != HTTPRequest::GET) {
        req->WriteReply(HTTP_BAD_METHOD, "");
        return false;
    }
    if (!CheckWebUIHost(req)) return false;

    // Strip /webui prefix and normalise to a relative file path
    static constexpr std::string_view WEBUI_PREFIX{"/webui"};
    std::string file_path = path.substr(WEBUI_PREFIX.size());
    if (file_path.empty() || file_path == "/") {
        file_path = "index.html";
    } else if (file_path.front() == '/') {
        file_path = file_path.substr(1);
    }

    // Block path traversal
    if (file_path.find("..") != std::string::npos) {
        req->WriteReply(HTTP_FORBIDDEN, "");
        return false;
    }

    const std::string mime{GetMimeType(file_path)};

    // 1. Disk override — serve live files from -webuiassets for development
    const std::string assets_dir = gArgs.GetArg("-webuiassets", "");
    if (!assets_dir.empty()) {
        const fs::path disk_path = fs::PathFromString(assets_dir) / fs::PathFromString(file_path);
        std::ifstream f(disk_path, std::ios::binary);
        if (f) {
            std::string content{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
            req->WriteHeader("Content-Type", mime);
            req->WriteReply(HTTP_OK, content);
            return true;
        }
    }

    // 2. Embedded assets (built into the binary)
    const auto& assets = GetEmbeddedAssets();
    auto it = assets.find(file_path);
    // SPA fallback: unknown paths → index.html for client-side routing
    if (it == assets.end()) it = assets.find("index.html");
    if (it != assets.end()) {
        req->WriteHeader("Content-Type", std::string(it->second.content_type));
        req->WriteReply(HTTP_OK, it->second.content);
        return true;
    }

    req->WriteReply(HTTP_NOT_FOUND, "Not found");
    return false;
}

// ---- Server-Sent Events ------------------------------------------------

static std::mutex g_sse_mutex;
static std::vector<evhttp_request*> g_sse_clients;

static void OnSSEClose(evhttp_connection* /*conn*/, void* ctx)
{
    auto* raw = static_cast<evhttp_request*>(ctx);
    std::lock_guard<std::mutex> lock(g_sse_mutex);
    auto& v = g_sse_clients;
    v.erase(std::remove(v.begin(), v.end(), raw), v.end());
}

static void PushSSEEvent(const std::string& event_name, const std::string& json_data)
{
    struct event_base* base = EventBase();
    if (!base) return;
    const std::string frame = "event: " + event_name + "\ndata: " + json_data + "\n\n";
    // Must write to evhttp connections on the libevent thread.
    HTTPEvent* ev = new HTTPEvent(base, /*deleteWhenTriggered=*/true, [frame]() {
        std::lock_guard<std::mutex> lock(g_sse_mutex);
        for (auto* raw : g_sse_clients) {
            evbuffer* buf = evbuffer_new();
            evbuffer_add(buf, frame.data(), frame.size());
            evhttp_send_reply_chunk(raw, buf);
            evbuffer_free(buf);
        }
    });
    struct timeval zero{0, 0};
    ev->trigger(&zero);
}

class WebUINotifier : public CValidationInterface
{
public:
    void UpdatedBlockTip(const CBlockIndex* pindexNew, const CBlockIndex*, bool fInitialDownload) override
    {
        if (fInitialDownload || !pindexNew) return;
        UniValue data(UniValue::VOBJ);
        data.pushKV("height", pindexNew->nHeight);
        data.pushKV("hash",   pindexNew->GetBlockHash().GetHex());
        data.pushKV("time",   static_cast<int64_t>(pindexNew->GetBlockTime()));
        PushSSEEvent("block", data.write());
    }

    void TransactionAddedToMempool(const NewMempoolTransactionInfo& tx, uint64_t /*seq*/) override
    {
        UniValue data(UniValue::VOBJ);
        data.pushKV("txid", tx.info.m_tx->GetHash().GetHex());
        PushSSEEvent("mempool", data.write());
    }
};

static std::unique_ptr<WebUINotifier> g_webui_notifier;

static bool HandleSSEEvents(HTTPRequest* req, const std::string&)
{
    if (req->GetRequestMethod() != HTTPRequest::GET) {
        req->WriteReply(HTTP_BAD_METHOD, "");
        return false;
    }
    if (!CheckWebUIHost(req)) return false;

    // EventSource API cannot set Authorization headers — auth via query param.
    const auto token_param = req->GetQueryParameter("token");
    if (!token_param || !CheckWebUIToken(*token_param)) {
        req->WriteReply(HTTP_UNAUTHORIZED, R"({"error":"unauthorized"})");
        return false;
    }

    // CORS: check Origin against allowed hosts; echo it back if allowed.
    auto [hasOrigin, origin] = req->GetHeader("Origin");
    if (hasOrigin) {
        std::string hostname = origin;
        const auto scheme_end = hostname.find("://");
        if (scheme_end != std::string::npos) hostname = hostname.substr(scheme_end + 3);
        const auto colon = hostname.find(':');
        if (colon != std::string::npos) hostname = hostname.substr(0, colon);
        if (!g_webui_allowed_hosts.count(hostname)) {
            req->WriteReply(HTTP_FORBIDDEN, R"({"error":"origin not allowed"})");
            return false;
        }
        req->WriteHeader("Access-Control-Allow-Origin", origin);
    }

    req->WriteHeader("Content-Type",      "text/event-stream");
    req->WriteHeader("Cache-Control",     "no-cache");
    req->WriteHeader("X-Accel-Buffering", "no");
    req->WriteHeader("Connection",        "keep-alive");

    evhttp_connection* conn = req->GetConnection();
    evhttp_request*    raw  = req->GetRaw();

    req->StartChunkedReply(200);
    req->SendChunk(": connected\n\n");  // flush proxy buffers

    if (conn) evhttp_connection_set_closecb(conn, OnSSEClose, raw);

    {
        std::lock_guard<std::mutex> lock(g_sse_mutex);
        g_sse_clients.push_back(raw);
    }

    return true;
}

// ---- Main dispatcher ---------------------------------------------------

static bool WebUIDispatch(HTTPRequest* req, const std::string& /*prefix*/)
{
    std::string uri = req->GetURI();
    const size_t qmark = uri.find('?');
    const std::string path = (qmark != std::string::npos) ? uri.substr(0, qmark) : uri;

    // ── API routes (require auth) ──────────────────────────────────────
    if (path.starts_with("/webui/api/")) {
        // Unauthenticated meta-endpoint: lets the login page know which auth mode is active.
        if (path == "/webui/api/auth/info")        return HandleAuthInfo(req);
        if (path == "/webui/api/events")           return HandleSSEEvents(req, path);
        if (path == "/webui/api/node/status")      return HandleNodeStatus(req);
        if (path == "/webui/api/node/features")     return HandleNodeFeatures(req);
        if (path == "/webui/api/wallets/loaded")    return HandleWalletsLoaded(req);
        if (path == "/webui/api/wallets/available") return HandleWalletsAvailable(req);
        if (path == "/webui/api/wallets/load")      return HandleWalletLoad(req);
        if (path == "/webui/api/wallets/create")    return HandleWalletCreate(req);
        if (path == "/webui/api/wallets/unload")    return HandleWalletUnload(req);
        if (path == "/webui/api/verifymessage")     return HandleVerifyMessage(req);
        if (path.starts_with("/webui/api/wallet/")) return HandleWalletRoute(req, path);
        if (path.starts_with("/webui/api/ans/"))    return HandleANSRoute(req, path);
        if (path.starts_with("/webui/api/psbt/"))   return HandlePSBTNodeRoute(req, path);
        req->WriteReply(HTTP_NOT_FOUND, R"({"error":"not found"})");
        return false;
    }

    // ── Static file serving (no auth) ─────────────────────────────────
    return HandleStaticFile(req, path);
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

    // Build the set of hosts that CheckWebUIHost() and CheckWebUICORS() will accept.
    // Always allow loopback; also allow any host embedded in -rpcbind (when -rpcallowip
    // is set, mirroring HTTPBindAddresses) and any host in -webuibind.
    g_webui_allowed_hosts = {"127.0.0.1", "localhost", "::1"};
    if (!gArgs.GetArgs("-rpcallowip").empty()) {
        for (const std::string& bind : gArgs.GetArgs("-rpcbind")) {
            uint16_t port{0};
            std::string host;
            if (SplitHostPort(bind, port, host) && !host.empty()) {
                g_webui_allowed_hosts.insert(host);
            }
        }
    }
    if (gArgs.IsArgSet("-webuibind")) {
        uint16_t port{0};
        std::string host;
        if (SplitHostPort(gArgs.GetArg("-webuibind", ""), port, host) && !host.empty()) {
            g_webui_allowed_hosts.insert(host);
        }
    }

    g_webui_notifier = std::make_unique<WebUINotifier>();
    if (node.validation_signals) {
        node.validation_signals->RegisterValidationInterface(g_webui_notifier.get());
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
    // Stop receiving validation events so no new SSE frames are queued.
    if (g_node && g_node->validation_signals && g_webui_notifier) {
        g_node->validation_signals->UnregisterValidationInterface(g_webui_notifier.get());
    }
    std::lock_guard<std::mutex> lock(g_sse_mutex);
    for (auto* raw : g_sse_clients) {
        evhttp_send_reply_end(raw);
    }
    g_sse_clients.clear();
}

void StopWebUI()
{
    g_webui_notifier.reset();
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
