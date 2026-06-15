// Copyright (c) 2025-present The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#pragma once

#include <httpserver.h>
#include <string>
#include <optional>

// HTTP status codes (HTTP_OK, HTTP_UNAUTHORIZED, HTTP_NOT_FOUND, etc.)
#include <rpc/protocol.h>

namespace node { struct NodeContext; }

extern node::NodeContext* g_node;

// auth.cpp
bool InitWebUIAuth();
bool CheckWebUIHost(HTTPRequest* req);
std::optional<std::string> CheckWebUICORS(HTTPRequest* req);
bool CheckWebUIAuth(HTTPRequest* req);
bool CheckWebUIToken(const std::string& submitted);
void SetJSONHeaders(HTTPRequest* req, const std::string& origin);

// json.cpp
std::string JsonError(const std::string& msg);

// static.cpp
bool HandleAuthInfo(HTTPRequest* req);
bool HandleStaticFile(HTTPRequest* req, const std::string& path);

// node_api.cpp
bool WebUINodeAPIRoute(HTTPRequest* req, const std::string& path);

// wallet_api.cpp
bool WebUIWalletsRoute(HTTPRequest* req, const std::string& path);
bool WebUIWalletRoute(HTTPRequest* req, const std::string& path);

// assets_api.cpp
bool WebUIAssetsRoute(HTTPRequest* req, const std::string& wallet_name, const std::string& action);
