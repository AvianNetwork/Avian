// Copyright (c) 2025-present The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_WEBUI_WEBUI_H
#define BITCOIN_WEBUI_WEBUI_H

#include <any>
#include <string>

static constexpr bool DEFAULT_WEBUI_ENABLE{false};
static constexpr int DEFAULT_WEBUI_PORT{8766};
inline const std::string DEFAULT_WEBUI_BIND{"127.0.0.1"};
inline const std::string WEBUI_COOKIE_FILE{"webui.cookie"};

/** Start the Web UI HTTP endpoint. Registers routes on the existing HTTP server.
 *  context must be a NodeContext*. */
void StartWebUI(const std::any& context);

/** Signal Web UI shutdown. Currently a no-op (mirrors InterruptREST). */
void InterruptWebUI();

/** Stop the Web UI HTTP endpoint and remove the session cookie file. */
void StopWebUI();

#endif // BITCOIN_WEBUI_WEBUI_H
