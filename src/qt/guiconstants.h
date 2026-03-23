// Copyright (c) 2011-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_GUICONSTANTS_H
#define BITCOIN_QT_GUICONSTANTS_H

#include <chrono>
#include <cstdint>

#include <clientversion.h>

#include <QColor>

using namespace std::chrono_literals;

/* A delay between model updates */
static constexpr auto MODEL_UPDATE_DELAY{250ms};

/* A delay between shutdown pollings */
static constexpr auto SHUTDOWN_POLLING_DELAY{200ms};

/* AskPassphraseDialog -- Maximum passphrase length */
static const int MAX_PASSPHRASE_SIZE = 1024;

/* BitcoinGUI -- Size of icons in status bar */
static const int STATUSBAR_ICONSIZE = 20;

static const bool DEFAULT_SPLASHSCREEN = true;

/* Invalid field background style */
#define STYLE_INVALID "border: 3px solid #FF8080"

/* Valid field background style */
#define STYLE_VALID "border: 1px solid #18acbe"

/* Transaction list -- unconfirmed transaction */
#define COLOR_UNCONFIRMED QColor(128, 128, 128)
/* Transaction list -- negative amount */
#define COLOR_NEGATIVE QColor(255, 0, 0)
/* Transaction list -- bare address (without label) */
#define COLOR_BAREADDRESS QColor(140, 140, 140)
/* Transaction list -- TX status decoration - danger, tx needs attention */
#define COLOR_TX_STATUS_DANGER QColor(200, 100, 100)
/* Transaction list -- TX status decoration - default color */
#define COLOR_BLACK QColor(0, 0, 0)
/* Widget Background color - default color */
#define COLOR_WHITE QColor(255, 255, 255)

/* Avian brand colors */
#define COLOR_AVIAN_2B737F QColor("#2B737F")
#define COLOR_AVIAN_19827B QColor("#19827B")
#define COLOR_AVIAN_18A7B7 QColor("#18A7B7")
#define COLOR_AVIAN_34E2D6 QColor("#34E2D6")

#define COLOR_WALLETFRAME_SHADOW QColor(0, 0, 0, 71)

/** LIGHT MODE */
#define COLOR_BACKGROUND_LIGHT QColor("#ffffff")
#define COLOR_WIDGET_BACKGROUND QColor("#ffffff")
#define COLOR_DARK_ORANGE QColor("#f05339")
#define COLOR_LIGHT_ORANGE QColor("#f79433")
#define COLOR_DARK_BLUE QColor("#3d3d3d")
#define COLOR_LIGHT_BLUE QColor("#3d3d3d")
#define COLOR_SHADOW_LIGHT QColor("#cacaca")
#define COLOR_TOOLBAR_NOT_SELECTED_TEXT QColor("#e8e6e6")
#define COLOR_TOOLBAR_SELECTED_TEXT QColor("#000000")
#define COLOR_SENDENTRIES_BACKGROUND QColor("#ffffff")
#define COLOR_ASSET_TEXT QColor(255, 255, 255)

/** DARK MODE */
#define COLOR_WIDGET_BACKGROUND_DARK QColor("#2E2E2E")
#define COLOR_SHADOW_DARK QColor("#1B1B1B")
#define COLOR_LIGHT_BLUE_DARK QColor("#1D1D1D")
#define COLOR_DARK_BLUE_DARK QColor("#1D1D1D")
#define COLOR_PRICING_WIDGET QColor("#2E2E2E")
#define COLOR_TOOLBAR_NOT_SELECTED_TEXT_DARK_MODE QColor("#6c80c5")
#define COLOR_TOOLBAR_SELECTED_TEXT_DARK_MODE QColor("#ffffff")
#define COLOR_SENDENTRIES_BACKGROUND_DARK QColor("#2E2E2E")

/* Label colors */
#define STRING_LABEL_COLOR QColor("#000000")
#define STRING_LABEL_COLOR_WARNING "color: #ff6666"

/* Tooltips longer than this (in characters) are converted into rich text,
   so that they can be word-wrapped.
 */
static const int TOOLTIP_WRAP_THRESHOLD = 80;

/* Number of frames in spinner animation */
#define SPINNER_FRAMES 36

#define QAPP_ORG_NAME "Avian"
#define QAPP_ORG_DOMAIN "avn.network"
// Include the major version in the QSettings application name so different
// major versions can run side-by-side without sharing GUI settings.
#define QAPP_APP_NAME_DEFAULT "Avian-Qt-v" STRINGIZE(CLIENT_VERSION_MAJOR)
#define QAPP_APP_NAME_TESTNET "Avian-Qt-testnet-v" STRINGIZE(CLIENT_VERSION_MAJOR)
#define QAPP_APP_NAME_TESTNET4 "Avian-Qt-testnet4-v" STRINGIZE(CLIENT_VERSION_MAJOR)
#define QAPP_APP_NAME_SIGNET "Avian-Qt-signet-v" STRINGIZE(CLIENT_VERSION_MAJOR)
#define QAPP_APP_NAME_REGTEST "Avian-Qt-regtest-v" STRINGIZE(CLIENT_VERSION_MAJOR)

/* One gigabyte (GB) in bytes */
static constexpr uint64_t GB_BYTES{1000000000};

// Default prune target displayed in GUI.
static constexpr int DEFAULT_PRUNE_TARGET_GB{2};

#endif // BITCOIN_QT_GUICONSTANTS_H
