// Copyright (c) 2025-present The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <webui/webui_internal.h>

#include <bitcoin-build-config.h> // IWYU pragma: keep

#include <addresstype.h>
#include <consensus/amount.h>
#include <core_io.h>
#include <httpserver.h>
#include <key_io.h>
#include <node/context.h>
#include <univalue.h>
#include <util/moneystr.h>
#include <util/result.h>
#include <util/strencodings.h>
#include <util/translation.h>

#ifdef ENABLE_WALLET
#include <assets/assets.h>
#include <assets/assetdb.h>
#include <interfaces/wallet.h>
#include <policy/feerate.h>
#include <sync.h>
#include <validation.h>
#include <wallet/asset_tx.h>
#include <wallet/coincontrol.h>
#include <wallet/context.h>
#include <wallet/receive.h>
#include <wallet/spend.h>
#include <wallet/wallet.h>
#endif

#include <event2/http.h>

#include <map>
#include <string>
#include <vector>

#ifdef ENABLE_WALLET

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
            req->WriteReply(HTTP_BAD_REQUEST, JsonError(asset_err.second));
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
        req->WriteReply(HTTP_BAD_REQUEST, JsonError(err.second));
        return false;
    }

    std::string txid;
    if (!wallet::SendAssetTransaction(*pwallet, txRef, err, txid)) {
        req->WriteReply(HTTP_BAD_REQUEST, JsonError(err.second));
        return false;
    }

    UniValue obj(UniValue::VOBJ);
    obj.pushKV("txid", txid);
    obj.pushKV("fee",  ValueFromAmount(nFeeRequired).write());
    SetJSONHeaders(req, *cors);
    req->WriteReply(HTTP_OK, obj.write());
    return true;
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

bool WebUIAssetsRoute(HTTPRequest* req, const std::string& wallet_name, const std::string& action)
{
#ifdef ENABLE_WALLET
    if (action == "send-asset")  return HandleWalletSendAsset(req, wallet_name);
    if (action == "assets")      return HandleWalletAssets(req, wallet_name);
    if (action == "utxos")       return HandleWalletUTXOs(req, wallet_name);
    if (action == "consolidate") return HandleWalletConsolidate(req, wallet_name);
#endif
    req->WriteReply(HTTP_NOT_FOUND, R"({"error":"not found"})");
    return false;
}
