// Copyright (c) 2025-present The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <webui/webui_internal.h>
#include <webui/webui.h>

#include <common/args.h>
#include <crypto/hmac_sha256.h>
#include <httpserver.h>
#include <logging.h>
#include <random.h>
#include <span.h>
#include <sync.h>
#include <util/fs.h>
#include <util/fs_helpers.h>
#include <util/strencodings.h>
#include <util/time.h>
#include <univalue.h>

#include <event2/http.h>

#include <chrono>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

// Defined in webui.cpp; referenced here for host validation.
extern std::string g_webui_token;
extern bool        g_webui_cookie_generated;
extern bool        g_webui_use_password;
extern std::string g_webui_password_hash;
extern std::vector<unsigned char> g_webui_password_salt;
extern std::mutex                 g_webui_session_mutex;
extern std::set<std::string>      g_webui_session_tokens;
extern std::set<std::string> g_webui_allowed_hosts;

static fs::path GetWebUICookiePath()
{
    return gArgs.GetDataDirNet() / fs::PathFromString(WEBUI_COOKIE_FILE);
}

// PBKDF2-HMAC-SHA256, single 32-byte output block (RFC 8018). Deliberately slow —
// used only for the one-time password check in HandleAuthLogin(). Everyday request
// validation never re-hashes the password; it checks the opaque, high-entropy
// session token login mints (see CheckWebUIToken / CreateWebUISessionToken), so the
// cost below is paid once per login, not once per API call. The iteration count is
// kept well below current OWASP guidance (600k+) because Avian nodes commonly run
// on low-power hardware (e.g. Raspberry Pi) where this runs synchronously in the
// HTTP request thread.
static constexpr uint32_t WEBUI_PBKDF2_ITERATIONS{100000};

static void Pbkdf2HmacSha256(const std::string& password, const std::vector<unsigned char>& salt,
                              uint32_t iterations, unsigned char out[CHMAC_SHA256::OUTPUT_SIZE])
{
    unsigned char block[4]{0, 0, 0, 1};
    unsigned char u[CHMAC_SHA256::OUTPUT_SIZE];
    CHMAC_SHA256(UCharCast(password.data()), password.size())
        .Write(salt.data(), salt.size())
        .Write(block, sizeof(block))
        .Finalize(u);
    unsigned char t[CHMAC_SHA256::OUTPUT_SIZE];
    std::memcpy(t, u, sizeof(t));
    for (uint32_t i = 1; i < iterations; ++i) {
        CHMAC_SHA256(UCharCast(password.data()), password.size()).Write(u, sizeof(u)).Finalize(u);
        for (size_t k = 0; k < sizeof(t); ++k) t[k] ^= u[k];
    }
    std::memcpy(out, t, sizeof(t));
}

bool InitWebUIAuth()
{
    std::string pw = gArgs.GetArg("-webuipassword", "");
    if (!pw.empty()) {
        // Password mode: derive a slow-to-compute verifier (see Pbkdf2HmacSha256
        // above) instead of a single fast hash, and salt it so a leaked hash can't
        // be checked against a precomputed table. No cookie file needed.
        unsigned char salt[16];
        GetRandBytes(salt);
        g_webui_password_salt.assign(salt, salt + sizeof(salt));

        unsigned char digest[CHMAC_SHA256::OUTPUT_SIZE];
        Pbkdf2HmacSha256(pw, g_webui_password_salt, WEBUI_PBKDF2_ITERATIONS, digest);
        g_webui_password_hash = HexStr(digest);
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

// Defend against DNS rebinding: only allow requests whose Host header matches
// an address the HTTP server is actually bound to. Port suffix is ignored.
bool CheckWebUIHost(HTTPRequest* req)
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

bool CheckWebUIToken(const std::string& submitted)
{
    bool ok = false;
    if (g_webui_use_password) {
        // Password mode never compares the password itself on a per-request basis —
        // only the opaque session token a successful /auth/login minted.
        std::lock_guard<std::mutex> lock(g_webui_session_mutex);
        for (const std::string& token : g_webui_session_tokens) {
            if (TimingResistantEqual(submitted, token)) { ok = true; break; }
        }
    } else {
        ok = TimingResistantEqual(submitted, g_webui_token);
    }
    if (!ok) UninterruptibleSleep(std::chrono::milliseconds{250});
    return ok;
}

// One-time password check, used only by HandleAuthLogin().
bool CheckWebUIPassword(const std::string& submitted)
{
    unsigned char digest[CHMAC_SHA256::OUTPUT_SIZE];
    Pbkdf2HmacSha256(submitted, g_webui_password_salt, WEBUI_PBKDF2_ITERATIONS, digest);
    bool ok = TimingResistantEqual(HexStr(digest), g_webui_password_hash);
    if (!ok) UninterruptibleSleep(std::chrono::milliseconds{250});
    return ok;
}

std::string CreateWebUISessionToken()
{
    unsigned char rand_bytes[32];
    GetRandBytes(rand_bytes);
    std::string token = HexStr(rand_bytes);
    std::lock_guard<std::mutex> lock(g_webui_session_mutex);
    g_webui_session_tokens.insert(token);
    return token;
}

void RevokeWebUISessionToken(const std::string& token)
{
    std::lock_guard<std::mutex> lock(g_webui_session_mutex);
    g_webui_session_tokens.erase(token);
}

// EventSource cannot set an Authorization header, so /webui/api/events is
// authenticated via a query-string ticket instead of the long-lived session
// token/cookie. Tickets are minted by HandleSSETicket() (which itself requires
// a normal Bearer token), are valid for one connection attempt only, and
// expire quickly — so a copy that ends up in a proxy log, browser history, or
// a screenshot is useless almost immediately, unlike the real bearer token.
static std::mutex g_webui_sse_ticket_mutex;
static std::map<std::string, std::chrono::steady_clock::time_point> g_webui_sse_tickets;
static constexpr std::chrono::seconds WEBUI_SSE_TICKET_TTL{30};

static void PruneExpiredSSETickets()
{
    const auto now = std::chrono::steady_clock::now();
    for (auto it = g_webui_sse_tickets.begin(); it != g_webui_sse_tickets.end();) {
        it = (it->second < now) ? g_webui_sse_tickets.erase(it) : std::next(it);
    }
}

static std::string CreateWebUISSETicket()
{
    unsigned char rand_bytes[32];
    GetRandBytes(rand_bytes);
    std::string ticket = HexStr(rand_bytes);
    std::lock_guard<std::mutex> lock(g_webui_sse_ticket_mutex);
    PruneExpiredSSETickets();
    g_webui_sse_tickets.emplace(ticket, std::chrono::steady_clock::now() + WEBUI_SSE_TICKET_TTL);
    return ticket;
}

bool ConsumeWebUISSETicket(const std::string& ticket)
{
    std::lock_guard<std::mutex> lock(g_webui_sse_ticket_mutex);
    PruneExpiredSSETickets();
    return g_webui_sse_tickets.erase(ticket) > 0;
}

bool CheckWebUIAuth(HTTPRequest* req)
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
std::optional<std::string> CheckWebUICORS(HTTPRequest* req)
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

void SetJSONHeaders(HTTPRequest* req, const std::string& allowed_origin)
{
    req->WriteHeader("Content-Type", "application/json");
    if (!allowed_origin.empty()) {
        req->WriteHeader("Access-Control-Allow-Origin", allowed_origin);
    }
}

// Exchanges the configured -webuipassword for a fresh, opaque session token.
// Unauthenticated by design (it's the login step itself), but only meaningful
// when password mode is active.
bool HandleAuthLogin(HTTPRequest* req)
{
    if (req->GetRequestMethod() != HTTPRequest::POST) {
        req->WriteReply(HTTP_BAD_METHOD, "");
        return false;
    }
    if (!CheckWebUIHost(req)) return false;
    auto cors = CheckWebUICORS(req);
    if (!cors) return false;

    if (!g_webui_use_password) {
        req->WriteReply(HTTP_BAD_REQUEST, R"({"error":"Password authentication is not enabled"})");
        return false;
    }

    UniValue body;
    if (!body.read(req->ReadBody()) || !body.isObject()) {
        req->WriteReply(HTTP_BAD_REQUEST, R"({"error":"Invalid request body"})");
        return false;
    }
    const UniValue& passVal = body["password"];
    if (!passVal.isStr() || passVal.get_str().empty()) {
        req->WriteReply(HTTP_BAD_REQUEST, R"({"error":"\"password\" field required"})");
        return false;
    }

    if (!CheckWebUIPassword(passVal.get_str())) {
        req->WriteHeader("WWW-Authenticate", "Bearer realm=\"aviand-webui\"");
        req->WriteReply(HTTP_UNAUTHORIZED, R"({"error":"unauthorized"})");
        return false;
    }

    UniValue obj(UniValue::VOBJ);
    obj.pushKV("success", true);
    obj.pushKV("token", CreateWebUISessionToken());
    SetJSONHeaders(req, *cors);
    req->WriteReply(HTTP_OK, obj.write());
    return true;
}

// Revokes the calling session's token so it can no longer authenticate.
bool HandleAuthLogout(HTTPRequest* req)
{
    if (req->GetRequestMethod() != HTTPRequest::POST) {
        req->WriteReply(HTTP_BAD_METHOD, "");
        return false;
    }
    if (!CheckWebUIHost(req)) return false;
    auto cors = CheckWebUICORS(req);
    if (!cors) return false;
    if (!CheckWebUIAuth(req)) return false;

    if (g_webui_use_password) {
        auto [present, auth] = req->GetHeader("authorization");
        if (present && auth.substr(0, 7) == "Bearer ") {
            RevokeWebUISessionToken(auth.substr(7));
        }
    }

    UniValue obj(UniValue::VOBJ);
    obj.pushKV("success", true);
    SetJSONHeaders(req, *cors);
    req->WriteReply(HTTP_OK, obj.write());
    return true;
}

// Mints a single-use, short-lived ticket that HandleSSEEvents() will accept in
// place of a bearer token, since EventSource cannot send one. Requires normal
// auth to call, same as any other /webui/api/* route.
bool HandleSSETicket(HTTPRequest* req)
{
    if (req->GetRequestMethod() != HTTPRequest::POST) {
        req->WriteReply(HTTP_BAD_METHOD, "");
        return false;
    }
    if (!CheckWebUIHost(req)) return false;
    auto cors = CheckWebUICORS(req);
    if (!cors) return false;
    if (!CheckWebUIAuth(req)) return false;

    UniValue obj(UniValue::VOBJ);
    obj.pushKV("ticket", CreateWebUISSETicket());
    SetJSONHeaders(req, *cors);
    req->WriteReply(HTTP_OK, obj.write());
    return true;
}
