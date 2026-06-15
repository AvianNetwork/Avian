// Copyright (c) 2025-present The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <webui/webui_internal.h>
#include <webui/assets.h>

#include <common/args.h>
#include <httpserver.h>
#include <univalue.h>
#include <util/fs.h>

#include <event2/http.h>

#include <fstream>
#include <string>

// Defined in webui.cpp
extern bool g_webui_use_password;

// ---- Auth info (unauthenticated) ---------------------------------------

// Tells the login page which auth mode is active so it can show the right hint.
bool HandleAuthInfo(HTTPRequest* req)
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
bool HandleStaticFile(HTTPRequest* req, const std::string& path)
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
