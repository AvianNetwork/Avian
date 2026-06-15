// Copyright (c) 2025-present The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <webui/webui_internal.h>
#include <webui/webui.h>

#include <common/args.h>
#include <hash.h>
#include <httpserver.h>
#include <logging.h>
#include <random.h>
#include <sync.h>
#include <util/fs.h>
#include <util/fs_helpers.h>
#include <util/strencodings.h>
#include <util/time.h>
#include <univalue.h>

#include <event2/http.h>

#include <chrono>
#include <fstream>
#include <set>
#include <string>

// Defined in webui.cpp; referenced here for host validation.
extern std::string g_webui_token;
extern bool        g_webui_cookie_generated;
extern bool        g_webui_use_password;
extern std::string g_webui_password_hash;
extern std::set<std::string> g_webui_allowed_hosts;

static fs::path GetWebUICookiePath()
{
    return gArgs.GetDataDirNet() / fs::PathFromString(WEBUI_COOKIE_FILE);
}

bool InitWebUIAuth()
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
