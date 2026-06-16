// Copyright (c) 2025-present The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <webui/webui_internal.h>

#include <bitcoin-build-config.h> // IWYU pragma: keep

#include <addresstype.h>
#include <consensus/amount.h>
#include <core_io.h>
#include <common/signmessage.h>
#include <common/url.h>
#include <httpserver.h>
#include <key_io.h>
#include <node/context.h>
#include <outputtype.h>
#include <psbt.h>
#include <rpc/request.h>
#include <rpc/server.h>
#include <streams.h>
#include <sync.h>
#include <univalue.h>
#include <uint256.h>
#include <util/moneystr.h>
#include <util/result.h>
#include <util/strencodings.h>
#include <util/time.h>
#include <util/translation.h>

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

#include <event2/http.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

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
        const bool is_locked = w->isCrypted() && w->isLocked();
        int64_t relock_at_val{0};
        if (!is_locked) {
            wallet::WalletContext* wctx = g_node->wallet_loader->context();
            if (wctx) {
                auto pwallet = wallet::GetWallet(*wctx, wallet_name);
                if (pwallet) {
                    LOCK(pwallet->cs_wallet);
                    relock_at_val = pwallet->nRelockTime;
                }
            }
        }
        obj.pushKV("name",           wallet_name);
        obj.pushKV("encrypted",      w->isCrypted());
        obj.pushKV("locked",         is_locked);
        if (!is_locked && relock_at_val > 0) obj.pushKV("unlocked_until", relock_at_val);
        obj.pushKV("balance",        ValueFromAmount(bal.balance));
        obj.pushKV("unconfirmed",    ValueFromAmount(bal.unconfirmed_balance));
        obj.pushKV("immature",       ValueFromAmount(bal.immature_balance));
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
            req->WriteReply(HTTP_BAD_REQUEST, JsonError(err_msg));
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

    int64_t timeout{300};
    if (body["timeout"].isNum()) {
        timeout = body["timeout"].getInt<int64_t>();
        if (timeout < 0) timeout = 0;
        if (timeout > 86400) timeout = 86400;
    }

    UniValue params(UniValue::VARR);
    params.push_back(passVal.get_str());
    params.push_back(timeout);

    JSONRPCRequest jreq;
    jreq.strMethod = "walletpassphrase";
    jreq.params    = std::move(params);
    jreq.URI       = "/wallet/" + wallet_name;

    try {
        tableRPC.execute(jreq);
    } catch (const UniValue& objError) {
        UninterruptibleSleep(std::chrono::milliseconds{250});
        const UniValue& msg = objError.exists("message") ? objError["message"] : objError;
        req->WriteReply(HTTP_BAD_REQUEST, "{\"error\":" + msg.write() + "}");
        return false;
    } catch (const std::exception& e) {
        req->WriteReply(HTTP_INTERNAL_SERVER_ERROR, JsonError(e.what()));
        return false;
    }

    UniValue obj(UniValue::VOBJ);
    obj.pushKV("success", true);
    if (timeout > 0) obj.pushKV("relock_in", timeout);
    SetJSONHeaders(req, *cors);
    req->WriteReply(HTTP_OK, obj.write());
    return true;
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
    JSONRPCRequest jreq;
    jreq.strMethod = "walletlock";
    jreq.params    = UniValue(UniValue::VARR);
    jreq.URI       = "/wallet/" + wallet_name;

    try {
        tableRPC.execute(jreq);
    } catch (const UniValue& objError) {
        const UniValue& msg = objError.exists("message") ? objError["message"] : objError;
        req->WriteReply(HTTP_BAD_REQUEST, "{\"error\":" + msg.write() + "}");
        return false;
    } catch (const std::exception& e) {
        req->WriteReply(HTTP_INTERNAL_SERVER_ERROR, JsonError(e.what()));
        return false;
    }

    UniValue obj(UniValue::VOBJ);
    obj.pushKV("success", true);
    SetJSONHeaders(req, *cors);
    req->WriteReply(HTTP_OK, obj.write());
    return true;
}

static bool HandleWalletChangePassphrase(HTTPRequest* req, const std::string& wallet_name)
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
        req->WriteReply(HTTP_BAD_REQUEST, R"({"error":"Invalid request body"})");
        return false;
    }
    const UniValue& oldPass = body["old_passphrase"];
    const UniValue& newPass = body["new_passphrase"];
    if (!oldPass.isStr() || !newPass.isStr()) {
        req->WriteReply(HTTP_BAD_REQUEST,
            R"({"error":"\"old_passphrase\" and \"new_passphrase\" required"})");
        return false;
    }

    JSONRPCRequest jreq;
    jreq.context   = g_node;
    jreq.strMethod = "walletpassphrasechange";
    jreq.URI       = "/wallet/" + wallet_name;
    jreq.params    = UniValue(UniValue::VARR);
    jreq.params.push_back(oldPass.get_str());
    jreq.params.push_back(newPass.get_str());

    try {
        tableRPC.execute(jreq);
    } catch (const UniValue& e) {
        const std::string msg = e["message"].isStr() ? e["message"].get_str() : "RPC error";
        req->WriteReply(HTTP_BAD_REQUEST, JsonError(msg));
        return false;
    } catch (const std::exception& e) {
        req->WriteReply(HTTP_INTERNAL_SERVER_ERROR, JsonError(e.what()));
        return false;
    }

    UniValue obj(UniValue::VOBJ);
    obj.pushKV("success", true);
    SetJSONHeaders(req, *cors);
    req->WriteReply(HTTP_OK, obj.write());
    return true;
}

static bool HandleWalletEncrypt(HTTPRequest* req, const std::string& wallet_name)
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
        req->WriteReply(HTTP_BAD_REQUEST, R"({"error":"Invalid request body"})");
        return false;
    }
    const UniValue& passVal = body["passphrase"];
    if (!passVal.isStr() || passVal.get_str().empty()) {
        req->WriteReply(HTTP_BAD_REQUEST, R"({"error":"\"passphrase\" required"})");
        return false;
    }

    JSONRPCRequest jreq;
    jreq.context   = g_node;
    jreq.strMethod = "encryptwallet";
    jreq.URI       = "/wallet/" + wallet_name;
    jreq.params    = UniValue(UniValue::VARR);
    jreq.params.push_back(passVal.get_str());

    try {
        UniValue result = tableRPC.execute(jreq);
        UniValue obj(UniValue::VOBJ);
        obj.pushKV("success", true);
        // encryptwallet returns a shutdown message; relay it so the UI can show it
        if (result.isStr()) obj.pushKV("message", result.get_str());
        SetJSONHeaders(req, *cors);
        req->WriteReply(HTTP_OK, obj.write());
        return true;
    } catch (const UniValue& e) {
        const std::string msg = e["message"].isStr() ? e["message"].get_str() : "RPC error";
        req->WriteReply(HTTP_BAD_REQUEST, JsonError(msg));
        return false;
    } catch (const std::exception& e) {
        req->WriteReply(HTTP_INTERNAL_SERVER_ERROR, JsonError(e.what()));
        return false;
    }
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
            req->WriteReply(HTTP_BAD_REQUEST, JsonError(err));
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
        req->WriteReply(HTTP_BAD_REQUEST, JsonError("Invalid PSBT: " + parse_err));
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

static bool HandleWalletPSBTFund(HTTPRequest* req, const std::string& wallet_name)
{
    // POST {inputs, outputs, locktime?, options?} → raw walletcreatefundedpsbt result
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
    if (!body.exists("outputs") || !body["outputs"].isArray()) {
        req->WriteReply(HTTP_BAD_REQUEST, R"({"error":"\"outputs\" array required"})");
        return false;
    }

    UniValue params(UniValue::VARR);
    params.push_back(body.exists("inputs")   ? body["inputs"]   : UniValue(UniValue::VARR));
    params.push_back(body["outputs"]);
    params.push_back(body.exists("locktime") ? body["locktime"] : UniValue(0));
    params.push_back(body.exists("options")  ? body["options"]  : UniValue(UniValue::VOBJ));

    JSONRPCRequest jreq;
    jreq.strMethod = "walletcreatefundedpsbt";
    jreq.params    = std::move(params);
    jreq.URI       = "/wallet/" + wallet_name;

    try {
        UniValue result = tableRPC.execute(jreq);
        SetJSONHeaders(req, *cors);
        req->WriteReply(HTTP_OK, result.write());
        return true;
    } catch (const UniValue& objError) {
        const UniValue& msg = objError.exists("message") ? objError["message"] : objError;
        req->WriteReply(HTTP_BAD_REQUEST, "{\"error\":" + msg.write() + "}");
        return false;
    } catch (const std::exception& e) {
        req->WriteReply(HTTP_INTERNAL_SERVER_ERROR, JsonError(e.what()));
        return false;
    }
}

#endif // ENABLE_WALLET

// ---- Route dispatchers -------------------------------------------------

bool WebUIWalletsRoute(HTTPRequest* req, const std::string& path)
{
    if (path == "/webui/api/wallets/loaded")    return HandleWalletsLoaded(req);
    if (path == "/webui/api/wallets/available") return HandleWalletsAvailable(req);
    if (path == "/webui/api/wallets/load")      return HandleWalletLoad(req);
    if (path == "/webui/api/wallets/create")    return HandleWalletCreate(req);
    if (path == "/webui/api/wallets/unload")    return HandleWalletUnload(req);
    req->WriteReply(HTTP_NOT_FOUND, R"({"error":"not found"})");
    return false;
}

bool WebUIWalletRoute(HTTPRequest* req, const std::string& path)
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
    if (action == "summary")            return HandleWalletSummary(req, wallet_name);
    if (action == "transactions")       return HandleWalletTransactions(req, wallet_name);
    if (action == "receive-address")    return HandleWalletReceiveAddress(req, wallet_name);
    if (action == "addresses")          return HandleWalletAddresses(req, wallet_name);
    if (action == "set-address-label")  return HandleWalletSetAddressLabel(req, wallet_name);
    if (action == "send")               return HandleWalletSend(req, wallet_name);
    if (action == "unlock")             return HandleWalletUnlock(req, wallet_name);
    if (action == "lock")               return HandleWalletLock(req, wallet_name);
    if (action == "change-passphrase")  return HandleWalletChangePassphrase(req, wallet_name);
    if (action == "encrypt")            return HandleWalletEncrypt(req, wallet_name);
    if (action == "signmessage")        return HandleWalletSignMessage(req, wallet_name);
    if (action == "psbt/create")        return HandleWalletPSBTCreate(req, wallet_name);
    if (action == "psbt/sign")          return HandleWalletPSBTSign(req, wallet_name);
    if (action == "psbt/fund")          return HandleWalletPSBTFund(req, wallet_name);
    // Asset-related actions delegated to assets_api.cpp
    if (action == "send-asset" ||
        action == "assets"     ||
        action == "utxos"      ||
        action == "consolidate")        return WebUIAssetsRoute(req, wallet_name, action);
#endif

    req->WriteReply(HTTP_NOT_FOUND, R"({"error":"not found"})");
    return false;
}
