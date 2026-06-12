// Copyright (c) 2025-present The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <bitcoin-build-config.h> // IWYU pragma: keep

#include <webui/webui.h>

#include <assets/assets.h>
#include <clientversion.h>
#include <common/args.h>
#include <httpserver.h>
#include <logging.h>
#include <net.h>
#include <node/context.h>
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
#include <wallet/context.h>
#include <wallet/wallet.h>
#include <wallet/walletutil.h>
#endif

#include <chrono>
#include <fstream>
#include <optional>
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
