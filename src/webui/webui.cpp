// Copyright (c) 2025-present The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <bitcoin-build-config.h> // IWYU pragma: keep

#include <webui/webui.h>
#include <webui/webui_internal.h>

#include <common/args.h>
#include <httpserver.h>
#include <logging.h>
#include <net.h>
#include <node/context.h>
#include <univalue.h>
#include <util/fs.h>
#include <util/strencodings.h>
#include <util/time.h>
#include <validation.h>
#include <validationinterface.h>

#include <event2/buffer.h>
#include <event2/event.h>
#include <event2/http.h>
#include <kernel/mempool_entry.h>

#include <algorithm>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

using node::NodeContext;

// ---- Globals (extern'd by auth.cpp, static.cpp via webui_internal.h) ---

node::NodeContext* g_node{nullptr};
std::string g_webui_token;
bool g_webui_cookie_generated{false};
bool g_webui_use_password{false};
std::string g_webui_password_hash; // hex-encoded SHA256 of configured password
std::set<std::string> g_webui_allowed_hosts; // populated in StartWebUI()

// ---- Server-Sent Events ------------------------------------------------

static std::mutex g_sse_mutex;
static std::vector<evhttp_request*> g_sse_clients;

static void OnSSEClose(evhttp_connection* conn, void* ctx)
{
    auto* raw = static_cast<evhttp_request*>(ctx);
    {
        std::lock_guard<std::mutex> lock(g_sse_mutex);
        auto& v = g_sse_clients;
        v.erase(std::remove(v.begin(), v.end(), raw), v.end());
    }
    // Our close callback replaces the one httpserver installs, so forward to it
    // so that StopHTTPServer's WaitUntilEmpty() can unblock on shutdown.
    HTTPNotifyConnectionClose(conn);
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

    // ── CORS preflight ────────────────────────────────────────────────
    // Authorization: Bearer triggers a preflight OPTIONS from the browser.
    // Validate Host and Origin with the same rules as real requests, then
    // respond 204 with the full set of CORS headers. No auth token needed.
    if (req->GetRequestMethod() == HTTPRequest::OPTIONS && path.starts_with("/webui/api/")) {
        if (!CheckWebUIHost(req)) return false;
        auto cors = CheckWebUICORS(req);
        if (!cors) return false;
        if (!cors->empty()) {
            req->WriteHeader("Access-Control-Allow-Origin",  *cors);
            req->WriteHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
            req->WriteHeader("Access-Control-Allow-Headers", "Authorization, Content-Type");
            req->WriteHeader("Access-Control-Max-Age",       "600");
        }
        req->WriteReply(204);
        return true;
    }

    // ── API routes (require auth) ──────────────────────────────────────
    if (path.starts_with("/webui/api/")) {
        // Unauthenticated meta-endpoint: lets the login page know which auth mode is active.
        if (path == "/webui/api/auth/info")        return HandleAuthInfo(req);
        if (path == "/webui/api/events")           return HandleSSEEvents(req, path);
        if (path == "/webui/api/node/status")      return WebUINodeAPIRoute(req, path);
        if (path == "/webui/api/node/features")    return WebUINodeAPIRoute(req, path);
        if (path == "/webui/api/verifymessage")    return WebUINodeAPIRoute(req, path);
        if (path.starts_with("/webui/api/ans/"))   return WebUINodeAPIRoute(req, path);
        if (path.starts_with("/webui/api/psbt/"))  return WebUINodeAPIRoute(req, path);
        if (path.starts_with("/webui/api/wallets/")) return WebUIWalletsRoute(req, path);
        if (path.starts_with("/webui/api/wallet/")) return WebUIWalletRoute(req, path);
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
    // Snapshot and clear under the lock; PushSSEEvent closures will see an empty list.
    std::vector<evhttp_request*> to_close;
    {
        std::lock_guard<std::mutex> lock(g_sse_mutex);
        to_close = std::move(g_sse_clients);
    }
    if (to_close.empty()) return;

    struct event_base* base = EventBase();
    if (!base) return;

    // SSE connections are long-lived: evhttp_send_reply_end() alone does not close
    // the underlying TCP connection, so StopHTTPServer's WaitUntilEmpty() would hang
    // waiting for g_requests to drain.  Schedule evhttp_connection_free() on the
    // libevent thread (the only thread where evhttp calls are safe).  That triggers
    // OnSSEClose → HTTPNotifyConnectionClose → g_requests.RemoveConnection(), which
    // unblocks WaitUntilEmpty().  Clear the on_complete_cb first so it cannot fire
    // concurrently and attempt to access the connection while it is being freed.
    auto* clients = new std::vector<evhttp_request*>(std::move(to_close));
    event_base_once(base, -1, EV_TIMEOUT, [](evutil_socket_t, short, void* arg) {
        auto* vec = static_cast<std::vector<evhttp_request*>*>(arg);
        for (auto* raw : *vec) {
            evhttp_request_set_on_complete_cb(raw, nullptr, nullptr);
            if (evhttp_connection* conn = evhttp_request_get_connection(raw)) {
                evhttp_connection_free(conn);
            }
        }
        delete vec;
    }, clients, nullptr);
}

void StopWebUI()
{
    g_webui_notifier.reset();
    UnregisterHTTPHandler("/webui/", false);
    if (g_webui_cookie_generated) {
        try {
            fs::remove(gArgs.GetDataDirNet() / fs::PathFromString(WEBUI_COOKIE_FILE));
        } catch (const fs::filesystem_error& e) {
            LogWarning("WebUI: Unable to remove cookie file %s: %s\n",
                       fs::PathToString(e.path1()), e.code().message());
        }
        g_webui_cookie_generated = false;
    }
    g_node = nullptr;
}
