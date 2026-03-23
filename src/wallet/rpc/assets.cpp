// Copyright (c) 2017-2019 The Raven Core developers
// Copyright (c) 2020-2024 The Avian developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <rpc/server.h>
#include <rpc/util.h>

#include <assets/assets.h>
#include <assets/assetdb.h>
#include <assets/ans.h>
#include <assets/messages.h>
#include <core_io.h>
#include <key_io.h>
#include <util/moneystr.h>
#include <validation.h>

#include <wallet/asset_tx.h>
#include <wallet/coincontrol.h>
#include <wallet/rpc/util.h>
#include <wallet/spend.h>
#include <wallet/wallet.h>

#include <univalue.h>

namespace wallet {

template <typename It>
static void safe_advance(It& it, It end, size_t n) {
    while (n-- > 0 && it != end)
        ++it;
};

RPCHelpMan listmyassets()
{
    return RPCHelpMan{
        "listmyassets",
        "Returns a list of all assets that are owned by this wallet.\n",
        {
            {"asset", RPCArg::Type::STR, RPCArg::Default{"*"}, "Filters results -- must be an asset name or a partial asset name followed by '*' ('*' matches all trailing characters)"},
            {"verbose", RPCArg::Type::BOOL, RPCArg::Default{false}, "When false result is just a list of asset balances -- when true results include outpoints"},
            {"count", RPCArg::Type::NUM, RPCArg::DefaultHint{"all"}, "Truncates results to include only the first count assets found"},
            {"start", RPCArg::Type::NUM, RPCArg::Default{0}, "Results skip over the first start assets found (if negative it skips back from the end)"},
            {"confs", RPCArg::Type::NUM, RPCArg::Default{0}, "Results are skipped if they don't have this number of confirmations"},
        },
        {
            RPCResult{"verbose=false",
                RPCResult::Type::OBJ_DYN, "", "",
                {
                    {RPCResult::Type::NUM, "asset_name", "asset balance"},
                }
            },
            RPCResult{"verbose=true",
                RPCResult::Type::OBJ_DYN, "", "",
                {
                    {RPCResult::Type::OBJ, "asset_name", "",
                    {
                        {RPCResult::Type::NUM, "balance", "the asset balance"},
                        {RPCResult::Type::ARR, "outpoints", "",
                        {
                            {RPCResult::Type::OBJ, "", "",
                            {
                                {RPCResult::Type::STR_HEX, "txid", "the txid"},
                                {RPCResult::Type::NUM, "vout", "the vout"},
                                {RPCResult::Type::NUM, "amount", "the amount"},
                            }},
                        }},
                    }},
                }
            },
        },
        RPCExamples{
            HelpExampleCli("listmyassets", "")
          + HelpExampleCli("listmyassets", "\"ASSET*\" true 10 20")
          + HelpExampleRpc("listmyassets", "")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            const std::shared_ptr<const CWallet> pwallet = GetWalletForJSONRPCRequest(request);
            if (!pwallet) return UniValue::VNULL;

            std::string filter = "*";
            if (!request.params[0].isNull())
                filter = request.params[0].get_str();
            if (filter.empty())
                filter = "*";

            bool verbose = false;
            if (!request.params[1].isNull())
                verbose = request.params[1].get_bool();

            size_t count = INT_MAX;
            if (!request.params[2].isNull()) {
                if (request.params[2].getInt<int>() < 1)
                    throw JSONRPCError(RPC_INVALID_PARAMETER, "count must be greater than 1.");
                count = request.params[2].getInt<int>();
            }

            long start = 0;
            if (!request.params[3].isNull()) {
                start = request.params[3].getInt<int>();
            }

            int confs = 0;
            if (!request.params[4].isNull()) {
                confs = request.params[4].getInt<int>();
            }

            // Get available asset coins from wallet
            LOCK(pwallet->cs_wallet);
            CoinFilterParams coin_params;
            coin_params.min_amount = 0;
            CoinsResult available = AvailableCoinsWithAssets(*pwallet, nullptr, std::nullopt, coin_params);

            // Build balances per asset
            std::string prefix;
            bool matchAll = (filter == "*");
            if (!matchAll && filter.back() == '*') {
                prefix = filter.substr(0, filter.size() - 1);
                matchAll = false;
            } else if (!matchAll) {
                prefix = filter;
            }

            std::map<std::string, CAmount> balances;
            std::map<std::string, std::vector<COutput>> assetOutputs;

            for (const auto& [assetName, outputs] : available.mapAssetCoins) {
                // Apply filter
                if (!matchAll) {
                    if (filter.back() == '*' || filter != assetName) {
                        // Prefix match
                        if (!prefix.empty() && assetName.find(prefix) != 0)
                            continue;
                        if (prefix.empty() && filter != assetName)
                            continue;
                    }
                }

                CAmount balance = 0;
                std::vector<COutput> filteredOutputs;
                for (const auto& output : outputs) {
                    // Check confirmations
                    if (confs > 0 && output.depth < confs)
                        continue;

                    CAssetOutputEntry data;
                    if (GetAssetData(output.txout.scriptPubKey, data)) {
                        balance += data.nAmount;
                        filteredOutputs.push_back(output);
                    }
                }
                if (balance > 0 || !filteredOutputs.empty()) {
                    balances[assetName] = balance;
                    if (verbose)
                        assetOutputs[assetName] = std::move(filteredOutputs);
                }
            }

            // Pagination
            auto bal = balances.begin();
            if (start >= 0)
                safe_advance(bal, balances.end(), (size_t)start);
            else
                safe_advance(bal, balances.end(), balances.size() + start);
            auto end = bal;
            safe_advance(end, balances.end(), count);

            // Generate output
            UniValue result(UniValue::VOBJ);
            if (verbose) {
                for (; bal != end && bal != balances.end(); bal++) {
                    UniValue asset(UniValue::VOBJ);
                    asset.pushKV("balance", AssetUnitValueFromAmount(bal->second, bal->first));

                    UniValue outpoints(UniValue::VARR);
                    if (assetOutputs.count(bal->first)) {
                        for (const auto& out : assetOutputs.at(bal->first)) {
                            UniValue tempOut(UniValue::VOBJ);
                            tempOut.pushKV("txid", out.outpoint.hash.GetHex());
                            tempOut.pushKV("vout", (int)out.outpoint.n);

                            CAssetOutputEntry data;
                            if (GetAssetData(out.txout.scriptPubKey, data)) {
                                tempOut.pushKV("amount", AssetUnitValueFromAmount(data.nAmount, bal->first));
                            }
                            outpoints.push_back(std::move(tempOut));
                        }
                    }
                    asset.pushKV("outpoints", std::move(outpoints));
                    result.pushKV(bal->first, std::move(asset));
                }
            } else {
                for (; bal != end && bal != balances.end(); bal++) {
                    result.pushKV(bal->first, AssetUnitValueFromAmount(bal->second, bal->first));
                }
            }

            return result;
        },
    };
}

//! Validate IPFS hash or txid message data
static void CheckIPFSTxidMessage(const std::string& message, int64_t expireTime)
{
    if (message.empty()) return;
    size_t msglen = message.length();

    // ANS checks
    bool fHasANS = (msglen >= CAvianNameSystemID::prefix.size() + 1) &&
                   (message.substr(0, CAvianNameSystemID::prefix.length()) == CAvianNameSystemID::prefix) &&
                   (msglen <= 64);
    if (fHasANS && !IsAvianNameSystemDeployed())
        throw JSONRPCError(RPC_INVALID_PARAMS, "ANS IDs not allowed when they are not deployed.");
    if (fHasANS && !CAvianNameSystemID::IsValidID(message))
        throw JSONRPCError(RPC_INVALID_PARAMS, "Invalid ANS ID");

    // IPFS CID or txid
    if (msglen == 46 || msglen == 64) {
        return; // Valid lengths for IPFS CID (Qm...) or txid
    }
    if (fHasANS) return;

    throw JSONRPCError(RPC_INVALID_PARAMS, "Invalid IPFS hash or txid (must be 46 or 64 characters)");
}

RPCHelpMan issue()
{
    return RPCHelpMan{
        "issue",
        "Issue an asset, subasset or unique asset.\n"
        "Asset name must not conflict with any existing asset.\n"
        "Unit as the number of decimals precision for the asset (0 for whole units (\"1\"), 8 for max precision (\"1.00000000\"))\n"
        "Reissuable is true/false for whether additional units can be issued by the original issuer.\n",
        {
            {"asset_name", RPCArg::Type::STR, RPCArg::Optional::NO, "a unique name"},
            {"qty", RPCArg::Type::NUM, RPCArg::Default{1}, "the number of units to be issued"},
            {"to_address", RPCArg::Type::STR, RPCArg::Default{""}, "address asset will be sent to, if empty address will be generated"},
            {"change_address", RPCArg::Type::STR, RPCArg::Default{""}, "address the AVN change will be sent to, if empty change address will be generated"},
            {"units", RPCArg::Type::NUM, RPCArg::Default{0}, "the number of decimals precision (0-8)"},
            {"reissuable", RPCArg::Type::BOOL, RPCArg::Default{true}, "whether future reissuance is allowed (false for unique assets)"},
            {"has_ipfs", RPCArg::Type::BOOL, RPCArg::Default{false}, "whether an ipfs hash is going to be added"},
            {"ipfs_hash", RPCArg::Type::STR, RPCArg::Default{""}, "an ipfs hash or txid hash (required if has_ipfs = true)"},
        },
        RPCResult{RPCResult::Type::ARR, "", "", {{RPCResult::Type::STR, "txid", "The transaction id"}}},
        RPCExamples{
            HelpExampleCli("issue", "\"ASSET_NAME\" 1000")
            + HelpExampleCli("issue", "\"ASSET_NAME\" 1000 \"myaddress\" \"changeaddress\" 8 false true \"QmTqu3Lk3gmTsQVtjU7rYYM37EAW4xNmbuEAp2Mjr4AV7E\"")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            const std::shared_ptr<CWallet> pwallet = GetWalletForJSONRPCRequest(request);
            if (!pwallet) return UniValue::VNULL;

            EnsureWalletIsUnlocked(*pwallet);

            std::string assetName = request.params[0].get_str();
            AssetType assetType;
            std::string assetError;
            if (!IsAssetNameValid(assetName, assetType, assetError))
                throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Invalid asset name: %s\nError: %s", assetName, assetError));

            if (assetType == AssetType::RESTRICTED)
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Use the issuerestricted RPC to issue a restricted asset");
            if (assetType == AssetType::QUALIFIER || assetType == AssetType::SUB_QUALIFIER)
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Use the issuequalifierasset RPC to issue a qualifier asset");
            if (assetType == AssetType::VOTE || assetType == AssetType::REISSUE || assetType == AssetType::OWNER || assetType == AssetType::INVALID)
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Unsupported asset type");

            CAmount nAmount = COIN;
            if (!request.params[1].isNull())
                nAmount = AmountFromValue(request.params[1]);

            std::string address;
            if (!request.params[2].isNull())
                address = request.params[2].get_str();
            if (!address.empty()) {
                CTxDestination destination = DecodeDestination(address);
                if (!IsValidDestination(destination))
                    throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, std::string("Invalid Avian address: ") + address);
            } else {
                auto op_dest = pwallet->GetNewDestination(OutputType::LEGACY, "");
                if (!op_dest) throw JSONRPCError(RPC_WALLET_KEYPOOL_RAN_OUT, util::ErrorString(op_dest).original);
                address = EncodeDestination(*op_dest);
            }

            std::string change_address;
            if (!request.params[3].isNull())
                change_address = request.params[3].get_str();
            if (!change_address.empty()) {
                CTxDestination destination = DecodeDestination(change_address);
                if (!IsValidDestination(destination))
                    throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, std::string("Invalid change address: ") + change_address);
            }

            int units = 0;
            if (!request.params[4].isNull())
                units = request.params[4].getInt<int>();

            bool reissuable = (assetType != AssetType::UNIQUE && assetType != AssetType::MSGCHANNEL);
            if (!request.params[5].isNull())
                reissuable = request.params[5].get_bool();

            bool has_ipfs = false;
            if (!request.params[6].isNull())
                has_ipfs = request.params[6].get_bool();

            std::string ipfs_hash;
            if (!request.params[7].isNull() && has_ipfs) {
                ipfs_hash = request.params[7].get_str();
                int64_t expireTime = 0;
                CheckIPFSTxidMessage(ipfs_hash, expireTime);
            }

            // Validate unique/msgchannel constraints
            if ((assetType == AssetType::UNIQUE || assetType == AssetType::MSGCHANNEL) && (nAmount != COIN || units != 0 || reissuable))
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameters for issuing a unique asset.");

            CNewAsset asset(assetName, nAmount, units, reissuable ? 1 : 0, has_ipfs ? 1 : 0, DecodeAssetData(ipfs_hash));

            CCoinControl ctrl;
            ctrl.destChange = DecodeDestination(change_address);

            CTransactionRef tx;
            CAmount nFeeRequired;
            std::pair<int, std::string> error;

            if (!CreateAssetTransaction(*pwallet, ctrl, asset, address, error, tx, nFeeRequired))
                throw JSONRPCError(error.first, error.second);

            std::string txid;
            if (!SendAssetTransaction(*pwallet, tx, error, txid))
                throw JSONRPCError(error.first, error.second);

            UniValue result(UniValue::VARR);
            result.push_back(txid);
            return result;
        },
    };
}

RPCHelpMan transfer()
{
    return RPCHelpMan{
        "transfer",
        "Transfers a quantity of an owned asset to a given address.\n",
        {
            {"asset_name", RPCArg::Type::STR, RPCArg::Optional::NO, "name of asset"},
            {"qty", RPCArg::Type::NUM, RPCArg::Optional::NO, "number of assets you want to send to the address"},
            {"to_address", RPCArg::Type::STR, RPCArg::Optional::NO, "address to send the asset to"},
            {"message", RPCArg::Type::STR, RPCArg::Default{""}, "once messaging is enabled, ipfs hash or txid hash to send along with the transfer"},
            {"expire_time", RPCArg::Type::NUM, RPCArg::Default{0}, "UTC timestamp of when the message expires"},
            {"change_address", RPCArg::Type::STR, RPCArg::Default{""}, "the transaction's AVN change will be sent to this address"},
            {"asset_change_address", RPCArg::Type::STR, RPCArg::Default{""}, "the transaction's asset change will be sent to this address"},
        },
        RPCResult{RPCResult::Type::ARR, "", "", {{RPCResult::Type::STR, "txid", "The transaction id"}}},
        RPCExamples{
            HelpExampleCli("transfer", "\"ASSET_NAME\" 20 \"address\"")
            + HelpExampleCli("transfer", "\"ASSET_NAME\" 20 \"address\" \"\" 0 \"change_address\" \"asset_change_address\"")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            const std::shared_ptr<CWallet> pwallet = GetWalletForJSONRPCRequest(request);
            if (!pwallet) return UniValue::VNULL;

            EnsureWalletIsUnlocked(*pwallet);

            std::string asset_name = request.params[0].get_str();

            if (IsAssetNameAQualifier(asset_name))
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Please use the transferqualifier RPC to send qualifier assets from this wallet.");

            CAmount nAmount = AmountFromValue(request.params[1]);

            std::string to_address = request.params[2].get_str();
            CTxDestination to_dest = DecodeDestination(to_address);
            if (!IsValidDestination(to_dest))
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, std::string("Invalid Avian address: ") + to_address);

            std::string message;
            if (!request.params[3].isNull())
                message = request.params[3].get_str();

            int64_t expireTime = 0;
            if (!request.params[4].isNull())
                expireTime = request.params[4].getInt<int64_t>();

            if (!message.empty()) {
                if (!AreMessagesDeployed())
                    throw JSONRPCError(RPC_INVALID_PARAMS, "Unable to send messages until messaging is enabled");
                CheckIPFSTxidMessage(message, expireTime);
            }

            std::string avn_change_address;
            if (!request.params[5].isNull())
                avn_change_address = request.params[5].get_str();

            std::string asset_change_address;
            if (!request.params[6].isNull())
                asset_change_address = request.params[6].get_str();

            CTxDestination avn_change_dest = DecodeDestination(avn_change_address);
            if (!avn_change_address.empty() && !IsValidDestination(avn_change_dest))
                throw JSONRPCError(RPC_INVALID_PARAMETER, std::string("AVN change address must be a valid address. Invalid address: ") + avn_change_address);

            CTxDestination asset_change_dest = DecodeDestination(asset_change_address);
            if (!asset_change_address.empty() && !IsValidDestination(asset_change_dest))
                throw JSONRPCError(RPC_INVALID_PARAMETER, std::string("Asset change address must be a valid address. Invalid address: ") + asset_change_address);

            std::vector<std::pair<CAssetTransfer, std::string>> vTransfers;
            CAssetTransfer assetTransfer(asset_name, nAmount, DecodeAssetData(message), expireTime);
            vTransfers.emplace_back(std::make_pair(assetTransfer, to_address));

            CCoinControl ctrl;
            ctrl.destChange = avn_change_dest;
            ctrl.destAssetChange = asset_change_dest;

            CTransactionRef tx;
            CAmount nFeeRequired;
            std::pair<int, std::string> error;

            if (!CreateTransferAssetTransaction(*pwallet, ctrl, vTransfers, "", error, tx, nFeeRequired))
                throw JSONRPCError(error.first, error.second);

            std::string txid;
            if (!SendAssetTransaction(*pwallet, tx, error, txid))
                throw JSONRPCError(error.first, error.second);

            UniValue result(UniValue::VARR);
            result.push_back(txid);
            return result;
        },
    };
}

RPCHelpMan reissue()
{
    return RPCHelpMan{
        "reissue",
        "Reissues a quantity of an asset to an owned address if you own the Owner Token.\n"
        "Can change the reissuable flag during reissuance.\n"
        "Can change the ipfs hash during reissuance.\n",
        {
            {"asset_name", RPCArg::Type::STR, RPCArg::Optional::NO, "name of asset that is being reissued"},
            {"qty", RPCArg::Type::NUM, RPCArg::Optional::NO, "number of assets to reissue"},
            {"to_address", RPCArg::Type::STR, RPCArg::Optional::NO, "address to send the asset to"},
            {"change_address", RPCArg::Type::STR, RPCArg::Default{""}, "address that the change of the transaction will be sent to"},
            {"reissuable", RPCArg::Type::BOOL, RPCArg::Default{true}, "whether future reissuance is allowed"},
            {"new_units", RPCArg::Type::NUM, RPCArg::Default{-1}, "the new units that will be associated with the asset"},
            {"new_ipfs", RPCArg::Type::STR, RPCArg::Default{""}, "whether to update the current ipfs hash or txid"},
        },
        RPCResult{RPCResult::Type::ARR, "", "", {{RPCResult::Type::STR, "txid", "The transaction id"}}},
        RPCExamples{
            HelpExampleCli("reissue", "\"ASSET_NAME\" 20 \"address\"")
            + HelpExampleCli("reissue", "\"ASSET_NAME\" 20 \"address\" \"change_address\" true 8 \"Qmd286K6pohQcTKYqnS1YhWrCiS4gz7Xi34sdwMe9USZ7u\"")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            const std::shared_ptr<CWallet> pwallet = GetWalletForJSONRPCRequest(request);
            if (!pwallet) return UniValue::VNULL;

            EnsureWalletIsUnlocked(*pwallet);

            std::string asset_name = request.params[0].get_str();
            CAmount nAmount = AmountFromValue(request.params[1]);
            std::string address = request.params[2].get_str();

            std::string changeAddress;
            if (!request.params[3].isNull())
                changeAddress = request.params[3].get_str();

            bool reissuable = true;
            if (!request.params[4].isNull())
                reissuable = request.params[4].get_bool();

            int newUnits = -1;
            if (!request.params[5].isNull())
                newUnits = request.params[5].getInt<int>();

            std::string newipfs;
            if (!request.params[6].isNull()) {
                newipfs = request.params[6].get_str();
                if (!newipfs.empty()) {
                    int64_t expireTime = 0;
                    CheckIPFSTxidMessage(newipfs, expireTime);
                }
            }

            CReissueAsset reissueAsset(asset_name, nAmount, newUnits, reissuable ? 1 : 0, DecodeAssetData(newipfs), "");

            CCoinControl ctrl;
            ctrl.destChange = DecodeDestination(changeAddress);

            CTransactionRef tx;
            CAmount nFeeRequired;
            std::pair<int, std::string> error;

            if (!CreateReissueAssetTransaction(*pwallet, ctrl, reissueAsset, address, error, tx, nFeeRequired))
                throw JSONRPCError(error.first, error.second);

            // Additional validation against resulting tx
            std::string strError;
            if (!ContextualCheckReissueAsset(passets, reissueAsset, strError, *tx))
                throw JSONRPCError(RPC_INVALID_REQUEST, strError);

            std::string txid;
            if (!SendAssetTransaction(*pwallet, tx, error, txid))
                throw JSONRPCError(error.first, error.second);

            UniValue result(UniValue::VARR);
            result.push_back(txid);
            return result;
        },
    };
}

// ============================================================================
// Helpers for tag / freeze / unfreeze wallet operations
// ============================================================================

static UniValue UpdateAddressTag(const JSONRPCRequest& request, const int8_t flag)
{
    const std::shared_ptr<CWallet> pwallet = GetWalletForJSONRPCRequest(request);
    if (!pwallet) return UniValue::VNULL;

    LOCK(pwallet->cs_wallet);
    EnsureWalletIsUnlocked(*pwallet);

    std::string tag_name = request.params[0].get_str();

    // Prepend # if not present
    if (tag_name[0] != QUALIFIER_CHAR)
        tag_name = std::string(1, QUALIFIER_CHAR) + tag_name;

    AssetType assetType;
    std::string assetError;
    if (!IsAssetNameValid(tag_name, assetType, assetError))
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Invalid qualifier name: %s\nError: %s", tag_name, assetError));
    if (assetType != AssetType::QUALIFIER && assetType != AssetType::SUB_QUALIFIER)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Name must be a valid qualifier name (starts with #)");

    std::string to_address = request.params[1].get_str();
    CTxDestination to_dest = DecodeDestination(to_address);
    if (!IsValidDestination(to_dest))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, std::string("Invalid Avian address: ") + to_address);

    std::string change_address;
    if (!request.params[2].isNull())
        change_address = request.params[2].get_str();

    std::string asset_data;
    if (!request.params[3].isNull())
        asset_data = request.params[3].get_str();

    if (change_address.empty()) {
        auto op_dest = pwallet->GetNewDestination(OutputType::LEGACY, "");
        if (!op_dest) throw JSONRPCError(RPC_WALLET_KEYPOOL_RAN_OUT, util::ErrorString(op_dest).original);
        change_address = EncodeDestination(*op_dest);
    } else {
        CTxDestination dest = DecodeDestination(change_address);
        if (!IsValidDestination(dest))
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, std::string("Invalid change address: ") + change_address);
    }

    // Transfer qualifier token to self (change address) to prove ownership
    std::vector<std::pair<CAssetTransfer, std::string>> vTransfers;
    CAssetTransfer assetTransfer(tag_name, QUALIFIER_ASSET_MIN_AMOUNT, DecodeAssetData(asset_data), 0);
    vTransfers.emplace_back(std::make_pair(assetTransfer, change_address));

    // Attach null asset tx data to tag/untag the address
    std::vector<std::pair<CNullAssetTxData, std::string>> nullAssetTxData;
    CNullAssetTxData nullData(tag_name, flag);
    nullAssetTxData.emplace_back(std::make_pair(nullData, to_address));

    CCoinControl ctrl;
    ctrl.destChange = DecodeDestination(change_address);

    CTransactionRef tx;
    CAmount nFeeRequired;
    std::pair<int, std::string> error;

    if (!CreateTransferAssetTransaction(*pwallet, ctrl, vTransfers, "", error, tx, nFeeRequired, &nullAssetTxData))
        throw JSONRPCError(error.first, error.second);

    std::string txid;
    if (!SendAssetTransaction(*pwallet, tx, error, txid))
        throw JSONRPCError(error.first, error.second);

    UniValue result(UniValue::VARR);
    result.push_back(txid);
    return result;
}

static UniValue UpdateAddressRestriction(const JSONRPCRequest& request, const int8_t flag)
{
    const std::shared_ptr<CWallet> pwallet = GetWalletForJSONRPCRequest(request);
    if (!pwallet) return UniValue::VNULL;

    LOCK(pwallet->cs_wallet);
    EnsureWalletIsUnlocked(*pwallet);

    std::string asset_name = request.params[0].get_str();

    // Prepend $ if not present
    if (asset_name[0] != RESTRICTED_CHAR)
        asset_name = std::string(1, RESTRICTED_CHAR) + asset_name;

    AssetType assetType;
    std::string assetError;
    if (!IsAssetNameValid(asset_name, assetType, assetError))
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Invalid restricted asset name: %s\nError: %s", asset_name, assetError));
    if (assetType != AssetType::RESTRICTED)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Name must be a valid restricted asset name (starts with $)");

    std::string address = request.params[1].get_str();
    CTxDestination addr_dest = DecodeDestination(address);
    if (!IsValidDestination(addr_dest))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, std::string("Invalid Avian address: ") + address);

    std::string change_address;
    if (!request.params[2].isNull())
        change_address = request.params[2].get_str();

    std::string asset_data;
    if (!request.params[3].isNull())
        asset_data = request.params[3].get_str();

    if (change_address.empty()) {
        auto op_dest = pwallet->GetNewDestination(OutputType::LEGACY, "");
        if (!op_dest) throw JSONRPCError(RPC_WALLET_KEYPOOL_RAN_OUT, util::ErrorString(op_dest).original);
        change_address = EncodeDestination(*op_dest);
    } else {
        CTxDestination dest = DecodeDestination(change_address);
        if (!IsValidDestination(dest))
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, std::string("Invalid change address: ") + change_address);
    }

    // Transfer owner token to self (change address) to prove ownership
    std::string ownerName = RestrictedNameToOwnerName(asset_name);
    std::vector<std::pair<CAssetTransfer, std::string>> vTransfers;
    CAssetTransfer assetTransfer(ownerName, OWNER_ASSET_AMOUNT, DecodeAssetData(asset_data), 0);
    vTransfers.emplace_back(std::make_pair(assetTransfer, change_address));

    // Attach null asset tx data to freeze/unfreeze the address
    std::vector<std::pair<CNullAssetTxData, std::string>> nullAssetTxData;
    CNullAssetTxData nullData(asset_name, flag);
    nullAssetTxData.emplace_back(std::make_pair(nullData, address));

    CCoinControl ctrl;
    ctrl.destChange = DecodeDestination(change_address);

    CTransactionRef tx;
    CAmount nFeeRequired;
    std::pair<int, std::string> error;

    if (!CreateTransferAssetTransaction(*pwallet, ctrl, vTransfers, "", error, tx, nFeeRequired, &nullAssetTxData))
        throw JSONRPCError(error.first, error.second);

    std::string txid;
    if (!SendAssetTransaction(*pwallet, tx, error, txid))
        throw JSONRPCError(error.first, error.second);

    UniValue result(UniValue::VARR);
    result.push_back(txid);
    return result;
}

static UniValue UpdateGlobalRestrictedAsset(const JSONRPCRequest& request, const int8_t flag)
{
    const std::shared_ptr<CWallet> pwallet = GetWalletForJSONRPCRequest(request);
    if (!pwallet) return UniValue::VNULL;

    LOCK(pwallet->cs_wallet);
    EnsureWalletIsUnlocked(*pwallet);

    std::string asset_name = request.params[0].get_str();

    // Prepend $ if not present
    if (asset_name[0] != RESTRICTED_CHAR)
        asset_name = std::string(1, RESTRICTED_CHAR) + asset_name;

    AssetType assetType;
    std::string assetError;
    if (!IsAssetNameValid(asset_name, assetType, assetError))
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Invalid restricted asset name: %s\nError: %s", asset_name, assetError));
    if (assetType != AssetType::RESTRICTED)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Name must be a valid restricted asset name (starts with $)");

    std::string change_address;
    if (!request.params[1].isNull())
        change_address = request.params[1].get_str();

    std::string asset_data;
    if (!request.params[2].isNull())
        asset_data = request.params[2].get_str();

    if (change_address.empty()) {
        auto op_dest = pwallet->GetNewDestination(OutputType::LEGACY, "");
        if (!op_dest) throw JSONRPCError(RPC_WALLET_KEYPOOL_RAN_OUT, util::ErrorString(op_dest).original);
        change_address = EncodeDestination(*op_dest);
    } else {
        CTxDestination dest = DecodeDestination(change_address);
        if (!IsValidDestination(dest))
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, std::string("Invalid change address: ") + change_address);
    }

    // Transfer owner token to self (change address) to prove ownership
    std::string ownerName = RestrictedNameToOwnerName(asset_name);
    std::vector<std::pair<CAssetTransfer, std::string>> vTransfers;
    CAssetTransfer assetTransfer(ownerName, OWNER_ASSET_AMOUNT, DecodeAssetData(asset_data), 0);
    vTransfers.emplace_back(std::make_pair(assetTransfer, change_address));

    // Attach global restriction data to freeze/unfreeze the asset globally
    std::vector<CNullAssetTxData> nullGlobalRestrictionData;
    CNullAssetTxData nullData(asset_name, flag);
    nullGlobalRestrictionData.push_back(nullData);

    CCoinControl ctrl;
    ctrl.destChange = DecodeDestination(change_address);

    CTransactionRef tx;
    CAmount nFeeRequired;
    std::pair<int, std::string> error;

    if (!CreateTransferAssetTransaction(*pwallet, ctrl, vTransfers, "", error, tx, nFeeRequired, nullptr, &nullGlobalRestrictionData))
        throw JSONRPCError(error.first, error.second);

    std::string txid;
    if (!SendAssetTransaction(*pwallet, tx, error, txid))
        throw JSONRPCError(error.first, error.second);

    UniValue result(UniValue::VARR);
    result.push_back(txid);
    return result;
}

// ============================================================================
// Wallet asset RPC commands
// ============================================================================

RPCHelpMan issueunique()
{
    return RPCHelpMan{
        "issueunique",
        "Issue unique asset(s).\n"
        "root_name must be an asset you own.\n"
        "An asset will be created for each element of asset_tags.\n"
        "If provided, ipfs_hashes must be the same length as asset_tags.\n"
        "Five (5) AVN will be burned and the owner must be in the wallet.\n",
        {
            {"root_name", RPCArg::Type::STR, RPCArg::Optional::NO, "name of the asset the unique asset(s) are being issued under"},
            {"asset_tags", RPCArg::Type::ARR, RPCArg::Optional::NO, "the unique tag for each asset which is to be issued",
                {
                    {"tag", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "the unique tag to be added to the root asset"},
                }
            },
            {"ipfs_hashes", RPCArg::Type::ARR, RPCArg::Optional::OMITTED, "ipfs hashes or txid hashes corresponding to each unique asset being issued",
                {
                    {"hash", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "ipfs hash or txid hash"},
                }
            },
            {"to_address", RPCArg::Type::STR, RPCArg::Default{""}, "address assets will be sent to, if empty address will be generated"},
            {"change_address", RPCArg::Type::STR, RPCArg::Default{""}, "address the AVN change will be sent to, if empty change address will be generated"},
        },
        RPCResult{RPCResult::Type::ARR, "", "", {{RPCResult::Type::STR, "txid", "The transaction id"}}},
        RPCExamples{
            HelpExampleCli("issueunique", "\"MY_ASSET\" \"[\\\"ALPHA\\\",\\\"BETA\\\"]\"")
            + HelpExampleCli("issueunique", "\"MY_ASSET\" \"[\\\"ALPHA\\\",\\\"BETA\\\"]\" \"[\\\"QmWWQSuPMS6aXCbZKpEjPHPUZN2NjB3YrhJTHsV4X3vb2t\\\",\\\"QmWWQSuPMS6aXCbZKpEjPHPUZN2NjB3YrhJTHsV4X3vb2t\\\"]\"")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            const std::shared_ptr<CWallet> pwallet = GetWalletForJSONRPCRequest(request);
            if (!pwallet) return UniValue::VNULL;

            EnsureWalletIsUnlocked(*pwallet);

            std::string rootName = request.params[0].get_str();
            AssetType assetType;
            std::string assetError;
            if (!IsAssetNameValid(rootName, assetType, assetError))
                throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Invalid asset name: %s\nError: %s", rootName, assetError));
            if (assetType != AssetType::ROOT && assetType != AssetType::SUB)
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Root asset must be a root or sub asset.");

            const UniValue& assetTags = request.params[1].get_array();
            if (assetTags.size() < 1)
                throw JSONRPCError(RPC_INVALID_PARAMETER, "asset_tags must have at least one element.");

            const UniValue& ipfsHashes = request.params[2];
            bool hasIPFS = !ipfsHashes.isNull() && ipfsHashes.isArray() && ipfsHashes.get_array().size() > 0;
            if (hasIPFS && ipfsHashes.get_array().size() != assetTags.size())
                throw JSONRPCError(RPC_INVALID_PARAMETER, "ipfs_hashes and asset_tags must be the same size.");

            std::string address;
            if (!request.params[3].isNull())
                address = request.params[3].get_str();
            if (!address.empty()) {
                CTxDestination destination = DecodeDestination(address);
                if (!IsValidDestination(destination))
                    throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, std::string("Invalid Avian address: ") + address);
            } else {
                auto op_dest = pwallet->GetNewDestination(OutputType::LEGACY, "");
                if (!op_dest) throw JSONRPCError(RPC_WALLET_KEYPOOL_RAN_OUT, util::ErrorString(op_dest).original);
                address = EncodeDestination(*op_dest);
            }

            std::string change_address;
            if (!request.params[4].isNull())
                change_address = request.params[4].get_str();
            if (!change_address.empty()) {
                CTxDestination destination = DecodeDestination(change_address);
                if (!IsValidDestination(destination))
                    throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, std::string("Invalid change address: ") + change_address);
            }

            std::vector<CNewAsset> assets;
            for (int i = 0; i < (int)assetTags.size(); i++) {
                std::string tag = assetTags[i].get_str();
                if (!IsUniqueTagValid(tag))
                    throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Invalid unique tag: %s", tag));

                std::string uniqueName = GetUniqueAssetName(rootName, tag);

                std::string ipfs_hash;
                if (hasIPFS) {
                    ipfs_hash = ipfsHashes[i].get_str();
                    CheckIPFSTxidMessage(ipfs_hash, 0);
                }

                CNewAsset asset(uniqueName, UNIQUE_ASSET_AMOUNT, UNIQUE_ASSET_UNITS, UNIQUE_ASSETS_REISSUABLE,
                                hasIPFS ? 1 : 0, DecodeAssetData(ipfs_hash));
                assets.push_back(asset);
            }

            CCoinControl ctrl;
            ctrl.destChange = DecodeDestination(change_address);

            CTransactionRef tx;
            CAmount nFeeRequired;
            std::pair<int, std::string> error;

            if (!CreateAssetTransaction(*pwallet, ctrl, assets, address, error, tx, nFeeRequired))
                throw JSONRPCError(error.first, error.second);

            std::string txid;
            if (!SendAssetTransaction(*pwallet, tx, error, txid))
                throw JSONRPCError(error.first, error.second);

            UniValue result(UniValue::VARR);
            result.push_back(txid);
            return result;
        },
    };
}

RPCHelpMan transferfromaddress()
{
    return RPCHelpMan{
        "transferfromaddress",
        "Transfer a quantity of an owned asset in a specific address to a given address.\n",
        {
            {"asset_name", RPCArg::Type::STR, RPCArg::Optional::NO, "name of asset"},
            {"from_address", RPCArg::Type::STR, RPCArg::Optional::NO, "address that the asset will be transferred from"},
            {"qty", RPCArg::Type::NUM, RPCArg::Optional::NO, "number of assets you want to send to the address"},
            {"to_address", RPCArg::Type::STR, RPCArg::Optional::NO, "address to send the asset to"},
            {"message", RPCArg::Type::STR, RPCArg::Default{""}, "once messaging is enabled, ipfs hash or txid hash to send along with the transfer"},
            {"expire_time", RPCArg::Type::NUM, RPCArg::Default{0}, "UTC timestamp of when the message expires"},
            {"avn_change_address", RPCArg::Type::STR, RPCArg::Default{""}, "the transaction's AVN change will be sent to this address"},
            {"asset_change_address", RPCArg::Type::STR, RPCArg::Default{""}, "the transaction's asset change will be sent to this address"},
        },
        RPCResult{RPCResult::Type::ARR, "", "", {{RPCResult::Type::STR, "txid", "The transaction id"}}},
        RPCExamples{
            HelpExampleCli("transferfromaddress", "\"ASSET_NAME\" \"fromaddress\" 20 \"toaddress\"")
            + HelpExampleCli("transferfromaddress", "\"ASSET_NAME\" \"fromaddress\" 20 \"toaddress\" \"\" 0 \"avn_change_address\" \"asset_change_address\"")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            const std::shared_ptr<CWallet> pwallet = GetWalletForJSONRPCRequest(request);
            if (!pwallet) return UniValue::VNULL;

            EnsureWalletIsUnlocked(*pwallet);

            std::string asset_name = request.params[0].get_str();
            std::string from_address = request.params[1].get_str();
            CAmount nAmount = AmountFromValue(request.params[2]);
            std::string to_address = request.params[3].get_str();

            CTxDestination from_dest = DecodeDestination(from_address);
            if (!IsValidDestination(from_dest))
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, std::string("Invalid from address: ") + from_address);

            CTxDestination to_dest = DecodeDestination(to_address);
            if (!IsValidDestination(to_dest))
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, std::string("Invalid Avian address: ") + to_address);

            std::string message;
            if (!request.params[4].isNull())
                message = request.params[4].get_str();

            int64_t expireTime = 0;
            if (!request.params[5].isNull())
                expireTime = request.params[5].getInt<int64_t>();

            if (!message.empty()) {
                if (!AreMessagesDeployed())
                    throw JSONRPCError(RPC_INVALID_PARAMS, "Unable to send messages until messaging is enabled");
                CheckIPFSTxidMessage(message, expireTime);
            }

            std::string avn_change_address;
            if (!request.params[6].isNull())
                avn_change_address = request.params[6].get_str();

            std::string asset_change_address;
            if (!request.params[7].isNull())
                asset_change_address = request.params[7].get_str();

            CTxDestination avn_change_dest = DecodeDestination(avn_change_address);
            if (!avn_change_address.empty() && !IsValidDestination(avn_change_dest))
                throw JSONRPCError(RPC_INVALID_PARAMETER, std::string("AVN change address must be a valid address. Invalid address: ") + avn_change_address);

            CTxDestination asset_change_dest = DecodeDestination(asset_change_address);
            if (!asset_change_address.empty() && !IsValidDestination(asset_change_dest))
                throw JSONRPCError(RPC_INVALID_PARAMETER, std::string("Asset change address must be a valid address. Invalid address: ") + asset_change_address);

            CCoinControl ctrl;
            ctrl.destChange = avn_change_dest;
            ctrl.destAssetChange = asset_change_dest;

            // Select asset coins only from the specified from_address
            {
                LOCK(pwallet->cs_wallet);
                CoinFilterParams coin_params;
                coin_params.min_amount = 0;
                CoinsResult available = AvailableCoinsWithAssets(*pwallet, nullptr, std::nullopt, coin_params);

                auto it = available.mapAssetCoins.find(asset_name);
                if (it == available.mapAssetCoins.end())
                    throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("No asset coins found for asset: %s", asset_name));

                for (const auto& output : it->second) {
                    CAssetOutputEntry data;
                    if (GetAssetData(output.txout.scriptPubKey, data)) {
                        if (EncodeDestination(data.destination) == from_address) {
                            ctrl.SelectAsset(output.outpoint);
                        }
                    }
                }
                if (!ctrl.HasAssetSelected())
                    throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("No asset coins found at address: %s for asset: %s", from_address, asset_name));
            }

            std::vector<std::pair<CAssetTransfer, std::string>> vTransfers;
            CAssetTransfer assetTransfer(asset_name, nAmount, DecodeAssetData(message), expireTime);
            vTransfers.emplace_back(std::make_pair(assetTransfer, to_address));

            CTransactionRef tx;
            CAmount nFeeRequired;
            std::pair<int, std::string> error;

            if (!CreateTransferAssetTransaction(*pwallet, ctrl, vTransfers, "", error, tx, nFeeRequired))
                throw JSONRPCError(error.first, error.second);

            std::string txid;
            if (!SendAssetTransaction(*pwallet, tx, error, txid))
                throw JSONRPCError(error.first, error.second);

            UniValue result(UniValue::VARR);
            result.push_back(txid);
            return result;
        },
    };
}

RPCHelpMan transferfromaddresses()
{
    return RPCHelpMan{
        "transferfromaddresses",
        "Transfer a quantity of an owned asset in specific address(es) to a given address.\n",
        {
            {"asset_name", RPCArg::Type::STR, RPCArg::Optional::NO, "name of asset"},
            {"from_addresses", RPCArg::Type::ARR, RPCArg::Optional::NO, "list of from addresses to send from",
                {
                    {"from_address", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "address to send asset from"},
                }
            },
            {"qty", RPCArg::Type::NUM, RPCArg::Optional::NO, "number of assets you want to send to the address"},
            {"to_address", RPCArg::Type::STR, RPCArg::Optional::NO, "address to send the asset to"},
            {"message", RPCArg::Type::STR, RPCArg::Default{""}, "once messaging is enabled, ipfs hash or txid hash to send along with the transfer"},
            {"expire_time", RPCArg::Type::NUM, RPCArg::Default{0}, "UTC timestamp of when the message expires"},
            {"avn_change_address", RPCArg::Type::STR, RPCArg::Default{""}, "the transaction's AVN change will be sent to this address"},
            {"asset_change_address", RPCArg::Type::STR, RPCArg::Default{""}, "the transaction's asset change will be sent to this address"},
        },
        RPCResult{RPCResult::Type::ARR, "", "", {{RPCResult::Type::STR, "txid", "The transaction id"}}},
        RPCExamples{
            HelpExampleCli("transferfromaddresses", "\"ASSET_NAME\" '[\"fromaddress1\",\"fromaddress2\"]' 20 \"toaddress\"")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            const std::shared_ptr<CWallet> pwallet = GetWalletForJSONRPCRequest(request);
            if (!pwallet) return UniValue::VNULL;

            EnsureWalletIsUnlocked(*pwallet);

            std::string asset_name = request.params[0].get_str();

            const UniValue& from_addresses_arr = request.params[1].get_array();
            if (from_addresses_arr.size() < 1)
                throw JSONRPCError(RPC_INVALID_PARAMETER, "from_addresses must have at least one element.");

            std::set<std::string> setFromAddresses;
            for (unsigned int i = 0; i < from_addresses_arr.size(); i++) {
                std::string addr = from_addresses_arr[i].get_str();
                CTxDestination dest = DecodeDestination(addr);
                if (!IsValidDestination(dest))
                    throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, std::string("Invalid from address: ") + addr);
                setFromAddresses.insert(addr);
            }

            CAmount nAmount = AmountFromValue(request.params[2]);
            std::string to_address = request.params[3].get_str();

            CTxDestination to_dest = DecodeDestination(to_address);
            if (!IsValidDestination(to_dest))
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, std::string("Invalid Avian address: ") + to_address);

            std::string message;
            if (!request.params[4].isNull())
                message = request.params[4].get_str();

            int64_t expireTime = 0;
            if (!request.params[5].isNull())
                expireTime = request.params[5].getInt<int64_t>();

            if (!message.empty()) {
                if (!AreMessagesDeployed())
                    throw JSONRPCError(RPC_INVALID_PARAMS, "Unable to send messages until messaging is enabled");
                CheckIPFSTxidMessage(message, expireTime);
            }

            std::string avn_change_address;
            if (!request.params[6].isNull())
                avn_change_address = request.params[6].get_str();

            std::string asset_change_address;
            if (!request.params[7].isNull())
                asset_change_address = request.params[7].get_str();

            CTxDestination avn_change_dest = DecodeDestination(avn_change_address);
            if (!avn_change_address.empty() && !IsValidDestination(avn_change_dest))
                throw JSONRPCError(RPC_INVALID_PARAMETER, std::string("AVN change address must be a valid address. Invalid address: ") + avn_change_address);

            CTxDestination asset_change_dest = DecodeDestination(asset_change_address);
            if (!asset_change_address.empty() && !IsValidDestination(asset_change_dest))
                throw JSONRPCError(RPC_INVALID_PARAMETER, std::string("Asset change address must be a valid address. Invalid address: ") + asset_change_address);

            CCoinControl ctrl;
            ctrl.destChange = avn_change_dest;
            ctrl.destAssetChange = asset_change_dest;

            // Select asset coins only from the specified addresses
            {
                LOCK(pwallet->cs_wallet);
                CoinFilterParams coin_params;
                coin_params.min_amount = 0;
                CoinsResult available = AvailableCoinsWithAssets(*pwallet, nullptr, std::nullopt, coin_params);

                auto it = available.mapAssetCoins.find(asset_name);
                if (it == available.mapAssetCoins.end())
                    throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("No asset coins found for asset: %s", asset_name));

                for (const auto& output : it->second) {
                    CAssetOutputEntry data;
                    if (GetAssetData(output.txout.scriptPubKey, data)) {
                        std::string outAddr = EncodeDestination(data.destination);
                        if (setFromAddresses.count(outAddr)) {
                            ctrl.SelectAsset(output.outpoint);
                        }
                    }
                }
                if (!ctrl.HasAssetSelected())
                    throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("No asset coins found at the specified addresses for asset: %s", asset_name));
            }

            std::vector<std::pair<CAssetTransfer, std::string>> vTransfers;
            CAssetTransfer assetTransfer(asset_name, nAmount, DecodeAssetData(message), expireTime);
            vTransfers.emplace_back(std::make_pair(assetTransfer, to_address));

            CTransactionRef tx;
            CAmount nFeeRequired;
            std::pair<int, std::string> error;

            if (!CreateTransferAssetTransaction(*pwallet, ctrl, vTransfers, "", error, tx, nFeeRequired))
                throw JSONRPCError(error.first, error.second);

            std::string txid;
            if (!SendAssetTransaction(*pwallet, tx, error, txid))
                throw JSONRPCError(error.first, error.second);

            UniValue result(UniValue::VARR);
            result.push_back(txid);
            return result;
        },
    };
}

RPCHelpMan issuequalifierasset()
{
    return RPCHelpMan{
        "issuequalifierasset",
        "Issue a qualifier asset.\n"
        "If the asset name doesn't have '#', it will be added automatically.\n"
        "Amount is a number between " + std::to_string(QUALIFIER_ASSET_MIN_AMOUNT / COIN) + " and " + std::to_string(QUALIFIER_ASSET_MAX_AMOUNT / COIN) + ".\n",
        {
            {"asset_name", RPCArg::Type::STR, RPCArg::Optional::NO, "a unique name, starts with '#'"},
            {"qty", RPCArg::Type::NUM, RPCArg::Default{1}, "the number of units to be issued (1 to 10)"},
            {"to_address", RPCArg::Type::STR, RPCArg::Default{""}, "address asset will be sent to, if empty address will be generated"},
            {"change_address", RPCArg::Type::STR, RPCArg::Default{""}, "address the AVN change will be sent to, if empty change address will be generated"},
            {"has_ipfs", RPCArg::Type::BOOL, RPCArg::Default{false}, "whether an ipfs hash is going to be added"},
            {"ipfs_hash", RPCArg::Type::STR, RPCArg::Default{""}, "an ipfs hash or txid hash (required if has_ipfs = true)"},
        },
        RPCResult{RPCResult::Type::ARR, "", "", {{RPCResult::Type::STR, "txid", "The transaction id"}}},
        RPCExamples{
            HelpExampleCli("issuequalifierasset", "\"#QUALIFIER_NAME\"")
            + HelpExampleCli("issuequalifierasset", "\"QUALIFIER_NAME\" 10")
            + HelpExampleCli("issuequalifierasset", "\"#QUALIFIER_NAME\" 5 \"myaddress\" \"changeaddress\" true \"QmTqu3Lk3gmTsQVtjU7rYYM37EAW4xNmbuEAp2Mjr4AV7E\"")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            const std::shared_ptr<CWallet> pwallet = GetWalletForJSONRPCRequest(request);
            if (!pwallet) return UniValue::VNULL;

            EnsureWalletIsUnlocked(*pwallet);

            std::string assetName = request.params[0].get_str();

            // Prepend # if not present
            if (assetName[0] != QUALIFIER_CHAR)
                assetName = std::string(1, QUALIFIER_CHAR) + assetName;

            AssetType assetType;
            std::string assetError;
            if (!IsAssetNameValid(assetName, assetType, assetError))
                throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Invalid qualifier asset name: %s\nError: %s", assetName, assetError));
            if (assetType != AssetType::QUALIFIER && assetType != AssetType::SUB_QUALIFIER)
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Name must be a valid qualifier name (starts with #)");

            CAmount nAmount = QUALIFIER_ASSET_MIN_AMOUNT;
            if (!request.params[1].isNull())
                nAmount = AmountFromValue(request.params[1]);

            if (nAmount < QUALIFIER_ASSET_MIN_AMOUNT || nAmount > QUALIFIER_ASSET_MAX_AMOUNT)
                throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Amount must be between %d and %d",
                    QUALIFIER_ASSET_MIN_AMOUNT / COIN, QUALIFIER_ASSET_MAX_AMOUNT / COIN));

            std::string address;
            if (!request.params[2].isNull())
                address = request.params[2].get_str();
            if (!address.empty()) {
                CTxDestination destination = DecodeDestination(address);
                if (!IsValidDestination(destination))
                    throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, std::string("Invalid Avian address: ") + address);
            } else {
                auto op_dest = pwallet->GetNewDestination(OutputType::LEGACY, "");
                if (!op_dest) throw JSONRPCError(RPC_WALLET_KEYPOOL_RAN_OUT, util::ErrorString(op_dest).original);
                address = EncodeDestination(*op_dest);
            }

            std::string change_address;
            if (!request.params[3].isNull())
                change_address = request.params[3].get_str();
            if (!change_address.empty()) {
                CTxDestination destination = DecodeDestination(change_address);
                if (!IsValidDestination(destination))
                    throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, std::string("Invalid change address: ") + change_address);
            }

            bool has_ipfs = false;
            if (!request.params[4].isNull())
                has_ipfs = request.params[4].get_bool();

            std::string ipfs_hash;
            if (!request.params[5].isNull() && has_ipfs) {
                ipfs_hash = request.params[5].get_str();
                CheckIPFSTxidMessage(ipfs_hash, 0);
            }

            CNewAsset asset(assetName, nAmount, QUALIFIER_ASSET_UNITS, 0 /* not reissuable */,
                            has_ipfs ? 1 : 0, DecodeAssetData(ipfs_hash));

            CCoinControl ctrl;
            ctrl.destChange = DecodeDestination(change_address);

            CTransactionRef tx;
            CAmount nFeeRequired;
            std::pair<int, std::string> error;

            if (!CreateAssetTransaction(*pwallet, ctrl, asset, address, error, tx, nFeeRequired))
                throw JSONRPCError(error.first, error.second);

            std::string txid;
            if (!SendAssetTransaction(*pwallet, tx, error, txid))
                throw JSONRPCError(error.first, error.second);

            UniValue result(UniValue::VARR);
            result.push_back(txid);
            return result;
        },
    };
}

RPCHelpMan issuerestrictedasset()
{
    return RPCHelpMan{
        "issuerestrictedasset",
        "Issue a restricted asset.\n"
        "Restricted asset names must start with '$'.\n"
        "Requires a verifier string that defines which qualifier tags an address must have to own/transfer the asset.\n",
        {
            {"asset_name", RPCArg::Type::STR, RPCArg::Optional::NO, "a unique name, starts with '$'"},
            {"qty", RPCArg::Type::NUM, RPCArg::Optional::NO, "the number of units to be issued"},
            {"verifier", RPCArg::Type::STR, RPCArg::Optional::NO, "the verifier string to be associated with the restricted asset (e.g. \"#TAG & #OTHER\")"},
            {"to_address", RPCArg::Type::STR, RPCArg::Optional::NO, "address asset will be sent to"},
            {"change_address", RPCArg::Type::STR, RPCArg::Default{""}, "address the AVN change will be sent to, if empty change address will be generated"},
            {"units", RPCArg::Type::NUM, RPCArg::Default{0}, "the number of decimals precision (0-8)"},
            {"reissuable", RPCArg::Type::BOOL, RPCArg::Default{true}, "whether future reissuance is allowed"},
            {"has_ipfs", RPCArg::Type::BOOL, RPCArg::Default{false}, "whether an ipfs hash is going to be added"},
            {"ipfs_hash", RPCArg::Type::STR, RPCArg::Default{""}, "an ipfs hash or txid hash (required if has_ipfs = true)"},
        },
        RPCResult{RPCResult::Type::ARR, "", "", {{RPCResult::Type::STR, "txid", "The transaction id"}}},
        RPCExamples{
            HelpExampleCli("issuerestrictedasset", "\"$RESTRICTED\" 1000 \"#TAG\" \"myaddress\"")
            + HelpExampleCli("issuerestrictedasset", "\"$RESTRICTED\" 1000 \"#TAG & #OTHER\" \"myaddress\" \"changeaddress\" 8 true true \"QmTqu3Lk3gmTsQVtjU7rYYM37EAW4xNmbuEAp2Mjr4AV7E\"")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            const std::shared_ptr<CWallet> pwallet = GetWalletForJSONRPCRequest(request);
            if (!pwallet) return UniValue::VNULL;

            EnsureWalletIsUnlocked(*pwallet);

            if (!AreRestrictedAssetsDeployed())
                throw JSONRPCError(RPC_INVALID_REQUEST, "Restricted assets are not deployed yet");

            std::string assetName = request.params[0].get_str();

            // Prepend $ if not present
            if (assetName[0] != RESTRICTED_CHAR)
                assetName = std::string(1, RESTRICTED_CHAR) + assetName;

            AssetType assetType;
            std::string assetError;
            if (!IsAssetNameValid(assetName, assetType, assetError))
                throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Invalid restricted asset name: %s\nError: %s", assetName, assetError));
            if (assetType != AssetType::RESTRICTED)
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Name must be a valid restricted asset name (starts with $)");

            CAmount nAmount = AmountFromValue(request.params[1]);

            std::string verifier = request.params[2].get_str();
            std::string verifierStripped = GetStrippedVerifierString(verifier);

            std::string address = request.params[3].get_str();
            CTxDestination destination = DecodeDestination(address);
            if (!IsValidDestination(destination))
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, std::string("Invalid Avian address: ") + address);

            // Validate the verifier string
            std::string strError;
            if (!ContextualCheckVerifierString(passets, verifierStripped, address, strError))
                throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Verifier string validation failed: %s", strError));

            std::string change_address;
            if (!request.params[4].isNull())
                change_address = request.params[4].get_str();
            if (!change_address.empty()) {
                CTxDestination dest = DecodeDestination(change_address);
                if (!IsValidDestination(dest))
                    throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, std::string("Invalid change address: ") + change_address);
            }

            int units = 0;
            if (!request.params[5].isNull())
                units = request.params[5].getInt<int>();

            bool reissuable = true;
            if (!request.params[6].isNull())
                reissuable = request.params[6].get_bool();

            bool has_ipfs = false;
            if (!request.params[7].isNull())
                has_ipfs = request.params[7].get_bool();

            std::string ipfs_hash;
            if (!request.params[8].isNull() && has_ipfs) {
                ipfs_hash = request.params[8].get_str();
                CheckIPFSTxidMessage(ipfs_hash, 0);
            }

            CNewAsset asset(assetName, nAmount, units, reissuable ? 1 : 0,
                            has_ipfs ? 1 : 0, DecodeAssetData(ipfs_hash));

            CCoinControl ctrl;
            ctrl.destChange = DecodeDestination(change_address);

            CTransactionRef tx;
            CAmount nFeeRequired;
            std::pair<int, std::string> error;

            if (!CreateAssetTransaction(*pwallet, ctrl, asset, address, error, tx, nFeeRequired, &verifierStripped))
                throw JSONRPCError(error.first, error.second);

            std::string txid;
            if (!SendAssetTransaction(*pwallet, tx, error, txid))
                throw JSONRPCError(error.first, error.second);

            UniValue result(UniValue::VARR);
            result.push_back(txid);
            return result;
        },
    };
}

RPCHelpMan reissuerestrictedasset()
{
    return RPCHelpMan{
        "reissuerestrictedasset",
        "Reissue a restricted asset.\n"
        "Restricted asset names must start with '$'.\n"
        "Can optionally change the verifier string, units, reissuability, and ipfs hash.\n",
        {
            {"asset_name", RPCArg::Type::STR, RPCArg::Optional::NO, "name of the restricted asset to reissue"},
            {"qty", RPCArg::Type::NUM, RPCArg::Optional::NO, "number of additional units to issue"},
            {"to_address", RPCArg::Type::STR, RPCArg::Optional::NO, "address to send the asset to"},
            {"change_verifier", RPCArg::Type::BOOL, RPCArg::Default{false}, "whether to change the verifier string"},
            {"new_verifier", RPCArg::Type::STR, RPCArg::Default{""}, "new verifier string (required if change_verifier is true)"},
            {"change_address", RPCArg::Type::STR, RPCArg::Default{""}, "address that the change of the transaction will be sent to"},
            {"new_units", RPCArg::Type::NUM, RPCArg::Default{-1}, "the new units that will be associated with the asset (-1 = no change)"},
            {"reissuable", RPCArg::Type::BOOL, RPCArg::Default{true}, "whether future reissuance is allowed"},
            {"new_ipfs", RPCArg::Type::STR, RPCArg::Default{""}, "whether to update the current ipfs hash or txid"},
        },
        RPCResult{RPCResult::Type::ARR, "", "", {{RPCResult::Type::STR, "txid", "The transaction id"}}},
        RPCExamples{
            HelpExampleCli("reissuerestrictedasset", "\"$RESTRICTED\" 1000 \"myaddress\"")
            + HelpExampleCli("reissuerestrictedasset", "\"$RESTRICTED\" 1000 \"myaddress\" true \"#TAG & #OTHER\"")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            const std::shared_ptr<CWallet> pwallet = GetWalletForJSONRPCRequest(request);
            if (!pwallet) return UniValue::VNULL;

            EnsureWalletIsUnlocked(*pwallet);

            if (!AreRestrictedAssetsDeployed())
                throw JSONRPCError(RPC_INVALID_REQUEST, "Restricted assets are not deployed yet");

            std::string asset_name = request.params[0].get_str();

            // Prepend $ if not present
            if (asset_name[0] != RESTRICTED_CHAR)
                asset_name = std::string(1, RESTRICTED_CHAR) + asset_name;

            AssetType assetType;
            std::string assetError;
            if (!IsAssetNameValid(asset_name, assetType, assetError))
                throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Invalid restricted asset name: %s\nError: %s", asset_name, assetError));
            if (assetType != AssetType::RESTRICTED)
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Name must be a valid restricted asset name (starts with $)");

            CAmount nAmount = AmountFromValue(request.params[1]);
            std::string address = request.params[2].get_str();

            CTxDestination destination = DecodeDestination(address);
            if (!IsValidDestination(destination))
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, std::string("Invalid Avian address: ") + address);

            bool change_verifier = false;
            if (!request.params[3].isNull())
                change_verifier = request.params[3].get_bool();

            std::string new_verifier;
            if (!request.params[4].isNull())
                new_verifier = request.params[4].get_str();

            std::string verifier_string;
            if (change_verifier) {
                if (new_verifier.empty())
                    throw JSONRPCError(RPC_INVALID_PARAMETER, "new_verifier must be provided when change_verifier is true");
                verifier_string = GetStrippedVerifierString(new_verifier);

                std::string strError;
                if (!ContextualCheckVerifierString(passets, verifier_string, address, strError))
                    throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Verifier string validation failed: %s", strError));
            }

            std::string changeAddress;
            if (!request.params[5].isNull())
                changeAddress = request.params[5].get_str();
            if (!changeAddress.empty()) {
                CTxDestination dest = DecodeDestination(changeAddress);
                if (!IsValidDestination(dest))
                    throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, std::string("Invalid change address: ") + changeAddress);
            }

            int newUnits = -1;
            if (!request.params[6].isNull())
                newUnits = request.params[6].getInt<int>();

            bool reissuable = true;
            if (!request.params[7].isNull())
                reissuable = request.params[7].get_bool();

            std::string newipfs;
            if (!request.params[8].isNull()) {
                newipfs = request.params[8].get_str();
                if (!newipfs.empty()) {
                    int64_t expireTime = 0;
                    CheckIPFSTxidMessage(newipfs, expireTime);
                }
            }

            CReissueAsset reissueAsset(asset_name, nAmount, newUnits, reissuable ? 1 : 0, DecodeAssetData(newipfs), "");

            CCoinControl ctrl;
            ctrl.destChange = DecodeDestination(changeAddress);

            CTransactionRef tx;
            CAmount nFeeRequired;
            std::pair<int, std::string> error;

            if (!CreateReissueAssetTransaction(*pwallet, ctrl, reissueAsset, address, error, tx, nFeeRequired,
                                               change_verifier ? &verifier_string : nullptr))
                throw JSONRPCError(error.first, error.second);

            // Additional validation against resulting tx
            std::string strError;
            if (!ContextualCheckReissueAsset(passets, reissueAsset, strError, *tx))
                throw JSONRPCError(RPC_INVALID_REQUEST, strError);

            std::string txid;
            if (!SendAssetTransaction(*pwallet, tx, error, txid))
                throw JSONRPCError(error.first, error.second);

            UniValue result(UniValue::VARR);
            result.push_back(txid);
            return result;
        },
    };
}

RPCHelpMan transferqualifier()
{
    return RPCHelpMan{
        "transferqualifier",
        "Transfer a qualifier asset owned by this wallet to the given address.\n",
        {
            {"qualifier_name", RPCArg::Type::STR, RPCArg::Optional::NO, "name of qualifier asset, starts with '#'"},
            {"qty", RPCArg::Type::NUM, RPCArg::Optional::NO, "number of assets you want to send to the address"},
            {"to_address", RPCArg::Type::STR, RPCArg::Optional::NO, "address to send the asset to"},
            {"change_address", RPCArg::Type::STR, RPCArg::Default{""}, "the transaction's AVN change will be sent to this address"},
            {"message", RPCArg::Type::STR, RPCArg::Default{""}, "once messaging is enabled, ipfs hash or txid hash to send along with the transfer"},
            {"expire_time", RPCArg::Type::NUM, RPCArg::Default{0}, "UTC timestamp of when the message expires"},
        },
        RPCResult{RPCResult::Type::ARR, "", "", {{RPCResult::Type::STR, "txid", "The transaction id"}}},
        RPCExamples{
            HelpExampleCli("transferqualifier", "\"#QUALIFIER\" 1 \"address\"")
            + HelpExampleCli("transferqualifier", "\"#QUALIFIER\" 1 \"address\" \"change_address\"")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            const std::shared_ptr<CWallet> pwallet = GetWalletForJSONRPCRequest(request);
            if (!pwallet) return UniValue::VNULL;

            EnsureWalletIsUnlocked(*pwallet);

            std::string qualifier_name = request.params[0].get_str();

            // Prepend # if not present
            if (qualifier_name[0] != QUALIFIER_CHAR)
                qualifier_name = std::string(1, QUALIFIER_CHAR) + qualifier_name;

            if (!IsAssetNameAQualifier(qualifier_name))
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Name must be a valid qualifier name (starts with #)");

            AssetType assetType;
            std::string assetError;
            if (!IsAssetNameValid(qualifier_name, assetType, assetError))
                throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Invalid qualifier name: %s\nError: %s", qualifier_name, assetError));

            CAmount nAmount = AmountFromValue(request.params[1]);

            std::string to_address = request.params[2].get_str();
            CTxDestination to_dest = DecodeDestination(to_address);
            if (!IsValidDestination(to_dest))
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, std::string("Invalid Avian address: ") + to_address);

            std::string change_address;
            if (!request.params[3].isNull())
                change_address = request.params[3].get_str();

            CTxDestination change_dest = DecodeDestination(change_address);
            if (!change_address.empty() && !IsValidDestination(change_dest))
                throw JSONRPCError(RPC_INVALID_PARAMETER, std::string("Change address must be a valid address. Invalid address: ") + change_address);

            std::string message;
            if (!request.params[4].isNull())
                message = request.params[4].get_str();

            int64_t expireTime = 0;
            if (!request.params[5].isNull())
                expireTime = request.params[5].getInt<int64_t>();

            if (!message.empty()) {
                if (!AreMessagesDeployed())
                    throw JSONRPCError(RPC_INVALID_PARAMS, "Unable to send messages until messaging is enabled");
                CheckIPFSTxidMessage(message, expireTime);
            }

            std::vector<std::pair<CAssetTransfer, std::string>> vTransfers;
            CAssetTransfer assetTransfer(qualifier_name, nAmount, DecodeAssetData(message), expireTime);
            vTransfers.emplace_back(std::make_pair(assetTransfer, to_address));

            CCoinControl ctrl;
            ctrl.destChange = change_dest;

            CTransactionRef tx;
            CAmount nFeeRequired;
            std::pair<int, std::string> error;

            if (!CreateTransferAssetTransaction(*pwallet, ctrl, vTransfers, "", error, tx, nFeeRequired))
                throw JSONRPCError(error.first, error.second);

            std::string txid;
            if (!SendAssetTransaction(*pwallet, tx, error, txid))
                throw JSONRPCError(error.first, error.second);

            UniValue result(UniValue::VARR);
            result.push_back(txid);
            return result;
        },
    };
}

RPCHelpMan addtagtoaddress()
{
    return RPCHelpMan{
        "addtagtoaddress",
        "Assign a qualifier tag to an address.\n",
        {
            {"tag_name", RPCArg::Type::STR, RPCArg::Optional::NO, "the qualifier tag to assign (e.g. '#TAG')"},
            {"to_address", RPCArg::Type::STR, RPCArg::Optional::NO, "the address to tag"},
            {"change_address", RPCArg::Type::STR, RPCArg::Default{""}, "the change address for the qualifier token, if empty a new address will be generated"},
            {"asset_data", RPCArg::Type::STR, RPCArg::Default{""}, "optional ipfs hash or txid hash"},
        },
        RPCResult{RPCResult::Type::ARR, "", "", {{RPCResult::Type::STR, "txid", "The transaction id"}}},
        RPCExamples{
            HelpExampleCli("addtagtoaddress", "\"#TAG\" \"address\"")
            + HelpExampleCli("addtagtoaddress", "\"#TAG\" \"address\" \"change_address\"")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            return UpdateAddressTag(request, 1);
        },
    };
}

RPCHelpMan removetagfromaddress()
{
    return RPCHelpMan{
        "removetagfromaddress",
        "Remove a qualifier tag from an address.\n",
        {
            {"tag_name", RPCArg::Type::STR, RPCArg::Optional::NO, "the qualifier tag to remove (e.g. '#TAG')"},
            {"to_address", RPCArg::Type::STR, RPCArg::Optional::NO, "the address to remove the tag from"},
            {"change_address", RPCArg::Type::STR, RPCArg::Default{""}, "the change address for the qualifier token, if empty a new address will be generated"},
            {"asset_data", RPCArg::Type::STR, RPCArg::Default{""}, "optional ipfs hash or txid hash"},
        },
        RPCResult{RPCResult::Type::ARR, "", "", {{RPCResult::Type::STR, "txid", "The transaction id"}}},
        RPCExamples{
            HelpExampleCli("removetagfromaddress", "\"#TAG\" \"address\"")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            return UpdateAddressTag(request, 0);
        },
    };
}

RPCHelpMan freezeaddress()
{
    return RPCHelpMan{
        "freezeaddress",
        "Freeze an address from transferring a restricted asset.\n",
        {
            {"asset_name", RPCArg::Type::STR, RPCArg::Optional::NO, "the restricted asset name (e.g. '$RESTRICTED')"},
            {"address", RPCArg::Type::STR, RPCArg::Optional::NO, "the address to freeze"},
            {"change_address", RPCArg::Type::STR, RPCArg::Default{""}, "the change address for the owner token, if empty a new address will be generated"},
            {"asset_data", RPCArg::Type::STR, RPCArg::Default{""}, "optional ipfs hash or txid hash"},
        },
        RPCResult{RPCResult::Type::ARR, "", "", {{RPCResult::Type::STR, "txid", "The transaction id"}}},
        RPCExamples{
            HelpExampleCli("freezeaddress", "\"$RESTRICTED\" \"address\"")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            return UpdateAddressRestriction(request, 1);
        },
    };
}

RPCHelpMan unfreezeaddress()
{
    return RPCHelpMan{
        "unfreezeaddress",
        "Unfreeze an address from a restricted asset freeze.\n",
        {
            {"asset_name", RPCArg::Type::STR, RPCArg::Optional::NO, "the restricted asset name (e.g. '$RESTRICTED')"},
            {"address", RPCArg::Type::STR, RPCArg::Optional::NO, "the address to unfreeze"},
            {"change_address", RPCArg::Type::STR, RPCArg::Default{""}, "the change address for the owner token, if empty a new address will be generated"},
            {"asset_data", RPCArg::Type::STR, RPCArg::Default{""}, "optional ipfs hash or txid hash"},
        },
        RPCResult{RPCResult::Type::ARR, "", "", {{RPCResult::Type::STR, "txid", "The transaction id"}}},
        RPCExamples{
            HelpExampleCli("unfreezeaddress", "\"$RESTRICTED\" \"address\"")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            return UpdateAddressRestriction(request, 0);
        },
    };
}

RPCHelpMan freezerestrictedasset()
{
    return RPCHelpMan{
        "freezerestrictedasset",
        "Globally freeze a restricted asset, preventing all transfers.\n",
        {
            {"asset_name", RPCArg::Type::STR, RPCArg::Optional::NO, "the restricted asset name (e.g. '$RESTRICTED')"},
            {"change_address", RPCArg::Type::STR, RPCArg::Default{""}, "the change address for the owner token, if empty a new address will be generated"},
            {"asset_data", RPCArg::Type::STR, RPCArg::Default{""}, "optional ipfs hash or txid hash"},
        },
        RPCResult{RPCResult::Type::ARR, "", "", {{RPCResult::Type::STR, "txid", "The transaction id"}}},
        RPCExamples{
            HelpExampleCli("freezerestrictedasset", "\"$RESTRICTED\"")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            return UpdateGlobalRestrictedAsset(request, 1);
        },
    };
}

RPCHelpMan unfreezerestrictedasset()
{
    return RPCHelpMan{
        "unfreezerestrictedasset",
        "Globally unfreeze a restricted asset, allowing transfers again.\n",
        {
            {"asset_name", RPCArg::Type::STR, RPCArg::Optional::NO, "the restricted asset name (e.g. '$RESTRICTED')"},
            {"change_address", RPCArg::Type::STR, RPCArg::Default{""}, "the change address for the owner token, if empty a new address will be generated"},
            {"asset_data", RPCArg::Type::STR, RPCArg::Default{""}, "optional ipfs hash or txid hash"},
        },
        RPCResult{RPCResult::Type::ARR, "", "", {{RPCResult::Type::STR, "txid", "The transaction id"}}},
        RPCExamples{
            HelpExampleCli("unfreezerestrictedasset", "\"$RESTRICTED\"")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            return UpdateGlobalRestrictedAsset(request, 0);
        },
    };
}

RPCHelpMan sendmessage()
{
    return RPCHelpMan{
        "sendmessage",
        "Send a message via an asset channel. Must own the owner token for the channel.\n",
        {
            {"channel_name", RPCArg::Type::STR, RPCArg::Optional::NO, "the channel name (asset name) to send a message on"},
            {"ipfs_hash", RPCArg::Type::STR, RPCArg::Optional::NO, "the IPFS hash of the message content"},
            {"expire_time", RPCArg::Type::NUM, RPCArg::Default{0}, "UTC timestamp of when the message expires"},
        },
        RPCResult{RPCResult::Type::ARR, "", "", {{RPCResult::Type::STR, "txid", "The transaction id"}}},
        RPCExamples{
            HelpExampleCli("sendmessage", "\"MY_CHANNEL\" \"QmTqu3Lk3gmTsQVtjU7rYYM37EAW4xNmbuEAp2Mjr4AV7E\"")
          + HelpExampleCli("sendmessage", "\"MY_CHANNEL\" \"QmTqu3Lk3gmTsQVtjU7rYYM37EAW4xNmbuEAp2Mjr4AV7E\" 15863654")
          + HelpExampleRpc("sendmessage", "\"MY_CHANNEL\", \"QmTqu3Lk3gmTsQVtjU7rYYM37EAW4xNmbuEAp2Mjr4AV7E\"")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            const std::shared_ptr<CWallet> pwallet = GetWalletForJSONRPCRequest(request);
            if (!pwallet) return UniValue::VNULL;

            EnsureWalletIsUnlocked(*pwallet);

            if (!AreMessagesDeployed())
                throw JSONRPCError(RPC_INVALID_REQUEST, "Messages are not deployed yet");

            std::string asset_name = request.params[0].get_str();
            std::string ipfs_hash = request.params[1].get_str();

            int64_t expire_time = 0;
            if (!request.params[2].isNull())
                expire_time = request.params[2].getInt<int64_t>();

            // Validate the channel name
            AssetType type;
            std::string strNameError;
            if (!IsAssetNameValid(asset_name, type, strNameError))
                throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Invalid channel name: %s\nError: %s", asset_name, strNameError));

            // Only MSGCHANNEL, OWNER, ROOT, SUB, RESTRICTED types are allowed
            if (type != AssetType::MSGCHANNEL && type != AssetType::OWNER &&
                type != AssetType::ROOT && type != AssetType::SUB && type != AssetType::RESTRICTED)
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid asset type. Only MSGCHANNEL, OWNER, ROOT, SUB, and RESTRICTED assets can send messages");

            // For ROOT, SUB, RESTRICTED types, append OWNER_TAG to the name
            if (type == AssetType::ROOT || type == AssetType::SUB || type == AssetType::RESTRICTED)
                asset_name += OWNER_TAG;

            // Validate the IPFS hash / message data
            CheckIPFSTxidMessage(ipfs_hash, expire_time);

            LOCK(pwallet->cs_wallet);

            // Get available asset coins
            CoinFilterParams coin_params;
            coin_params.min_amount = 0;
            CoinsResult available = AvailableCoinsWithAssets(*pwallet, nullptr, std::nullopt, coin_params);

            // Check that the wallet owns the asset
            if (!available.mapAssetCoins.count(asset_name))
                throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Wallet doesn't own the asset: %s", asset_name));

            // Get the destination address from the first asset output
            std::string address;
            for (const auto& output : available.mapAssetCoins.at(asset_name)) {
                CAssetOutputEntry data;
                if (GetAssetData(output.txout.scriptPubKey, data)) {
                    address = EncodeDestination(data.destination);
                    break;
                }
            }

            if (address.empty())
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Failed to get destination address from asset output");

            // Create the transfer with message data
            std::vector<std::pair<CAssetTransfer, std::string>> vTransfers;
            CAssetTransfer assetTransfer(asset_name, OWNER_ASSET_AMOUNT, DecodeAssetData(ipfs_hash), expire_time);
            vTransfers.emplace_back(std::make_pair(assetTransfer, address));

            CCoinControl ctrl;
            CTransactionRef tx;
            CAmount nFeeRequired;
            std::pair<int, std::string> error;

            if (!CreateTransferAssetTransaction(*pwallet, ctrl, vTransfers, "", error, tx, nFeeRequired))
                throw JSONRPCError(error.first, error.second);

            std::string txid;
            if (!SendAssetTransaction(*pwallet, tx, error, txid))
                throw JSONRPCError(error.first, error.second);

            UniValue result(UniValue::VARR);
            result.push_back(txid);
            return result;
        },
    };
}

RPCHelpMan consolidateutxos()
{
    return RPCHelpMan{
        "consolidateutxos",
        "Consolidate (compact) unspent transaction outputs (UTXOs) to the specified address.\n"
        "This reduces the number of small UTXOs by combining them into larger ones.\n"
        "Each batch processes up to 500 UTXOs.\n",
        {
            {"address", RPCArg::Type::STR, RPCArg::Optional::NO, "Destination address (must belong to this wallet)"},
            {"min_utxos", RPCArg::Type::NUM, RPCArg::Default{2000}, "Minimum UTXOs required to trigger consolidation"},
            {"max_batches", RPCArg::Type::NUM, RPCArg::Default{0}, "Maximum number of batches to process (0 = unlimited)"},
            {"min_amount", RPCArg::Type::AMOUNT, RPCArg::Default{"0.01"}, "Minimum UTXO amount in AVN to include"},
            {"max_amount", RPCArg::Type::AMOUNT, RPCArg::Default{"25.00"}, "Maximum UTXO amount in AVN to include"},
        },
        {
            RPCResult{"when wallet is already optimized",
                RPCResult::Type::STR, "", "Message indicating wallet is already optimized"
            },
            RPCResult{"when consolidation is performed",
                RPCResult::Type::ARR, "", "",
                {
                    {RPCResult::Type::OBJ, "", "per-batch result",
                    {
                        {RPCResult::Type::NUM, "selected_coins", "number of UTXOs selected in this batch"},
                        {RPCResult::Type::STR_AMOUNT, "selection_sum", "total value of selected UTXOs"},
                        {RPCResult::Type::STR_HEX, "txid", "consolidation transaction id"},
                        {RPCResult::Type::NUM, "batch_number", "batch sequence number"},
                    }},
                    {RPCResult::Type::OBJ, "", "summary (last element)",
                    {
                        {RPCResult::Type::NUM, "total_batches_processed", "number of batches completed"},
                        {RPCResult::Type::NUM, "remaining_utxos", "remaining UTXO count"},
                        {RPCResult::Type::STR, "status", "completion status"},
                    }},
                }
            },
        },
        RPCExamples{
            HelpExampleCli("consolidateutxos", "\"RaddressHere\"")
          + HelpExampleCli("consolidateutxos", "\"RaddressHere\" 1000 5")
          + HelpExampleCli("consolidateutxos", "\"RaddressHere\" 2000 0 0.01 25")
          + HelpExampleRpc("consolidateutxos", "\"RaddressHere\"")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
            if (!pwallet) return UniValue::VNULL;

            pwallet->BlockUntilSyncedToCurrentChain();
            LOCK(pwallet->cs_wallet);
            EnsureWalletIsUnlocked(*pwallet);

            // Parse destination address
            CTxDestination dest = DecodeDestination(request.params[0].get_str());
            if (!IsValidDestination(dest))
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid Avian address");

            if (!pwallet->IsMine(dest))
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY,
                    "Destination address does not belong to this wallet. "
                    "For security, consolidation is only allowed to your own addresses. "
                    "Use 'getnewaddress' to get a valid destination address.");

            // Parse parameters
            int minimumUtxoAmount = 2000;
            int maxBatches = 0;
            CAmount minSpendableAmount = 1000000;   // 0.01 AVN
            CAmount maxInputAmount = 2500000000LL;   // 25 AVN

            if (!request.params[1].isNull())
                minimumUtxoAmount = request.params[1].getInt<int>();
            if (!request.params[2].isNull())
                maxBatches = request.params[2].getInt<int>();
            if (!request.params[3].isNull())
                minSpendableAmount = AmountFromValue(request.params[3]);
            if (!request.params[4].isNull())
                maxInputAmount = AmountFromValue(request.params[4]);

            const int batchDivisor = 500;
            const CAmount maxOutputAmount = 1000000000000LL; // 10000 AVN

            // Get initial UTXO count
            CoinsResult allCoins = AvailableCoins(*pwallet);
            int utxoCount = static_cast<int>(allCoins.Size());

            if (utxoCount <= minimumUtxoAmount) {
                return "The wallet is already optimized.";
            }

            int nOps = (utxoCount - minimumUtxoAmount) / batchDivisor + 1;
            int nOdds = (utxoCount - minimumUtxoAmount) % batchDivisor;
            if (nOdds == 0) {
                nOdds = batchDivisor;
            } else if (nOdds < 10) {
                nOdds = batchDivisor;
                nOps--;
            }

            UniValue ret(UniValue::VARR);
            int batchCount = 0;
            CScript destScript = GetScriptForDestination(dest);

            while (nOps > 0) {
                if (maxBatches > 0 && batchCount >= maxBatches)
                    break;

                // Get fresh available coins each iteration
                CoinsResult availCoins = AvailableCoins(*pwallet);
                std::vector<COutput> vAllCoins = availCoins.All();

                // Filter UTXOs in the preferred range
                std::vector<COutput> vSortedCoins;
                for (const COutput& out : vAllCoins) {
                    CAmount utxoAmount = out.txout.nValue;
                    if (utxoAmount >= minSpendableAmount && utxoAmount <= maxInputAmount) {
                        vSortedCoins.push_back(out);
                    }
                }

                // Sort by amount (smallest first) for proper dust consolidation
                std::sort(vSortedCoins.begin(), vSortedCoins.end(),
                    [](const COutput& a, const COutput& b) {
                        return a.txout.nValue < b.txout.nValue;
                    });

                // If not enough small UTXOs, add some larger ones
                if (static_cast<int>(vSortedCoins.size()) < nOdds) {
                    std::vector<COutput> vLargerCoins;
                    for (const COutput& out : vAllCoins) {
                        CAmount utxoAmount = out.txout.nValue;
                        if (utxoAmount > maxInputAmount && utxoAmount <= 10000000000LL) { // up to 100 AVN
                            vLargerCoins.push_back(out);
                        }
                    }
                    std::sort(vLargerCoins.begin(), vLargerCoins.end(),
                        [](const COutput& a, const COutput& b) {
                            return a.txout.nValue < b.txout.nValue;
                        });
                    int needed = std::min(static_cast<int>(vLargerCoins.size()),
                                          nOdds - static_cast<int>(vSortedCoins.size()));
                    if (needed > 0)
                        vSortedCoins.insert(vSortedCoins.end(), vLargerCoins.begin(), vLargerCoins.begin() + needed);
                }

                // Select the smallest UTXOs for consolidation
                CCoinControl coinControl;
                coinControl.m_feerate = CFeeRate(1000); // 1 sat/byte minimum
                coinControl.m_allow_other_inputs = false;

                CAmount selectionSum = 0;
                int selectedCount = 0;

                for (const COutput& out : vSortedCoins) {
                    if (selectedCount >= nOdds)
                        break;
                    CAmount utxoValue = out.txout.nValue;
                    if (selectionSum + utxoValue > maxOutputAmount)
                        break;

                    coinControl.Select(out.outpoint);
                    selectionSum += utxoValue;
                    selectedCount++;
                }

                if (selectedCount == 0)
                    throw JSONRPCError(RPC_WALLET_ERROR, "No UTXOs available for consolidation");

                // Estimate fee
                CAmount estimatedFee = (180 + (selectedCount * 150)) * 1;
                if (estimatedFee < 10000) estimatedFee = 10000; // Minimum 0.0001 AVN
                if (selectionSum <= estimatedFee * 3)
                    throw JSONRPCError(RPC_WALLET_ERROR, strprintf(
                        "Selected amount %s too small for consolidation (estimated fee: %s)",
                        FormatMoney(selectionSum), FormatMoney(estimatedFee)));

                // Create the consolidation transaction
                std::vector<CRecipient> vecSend;
                vecSend.push_back({dest, selectionSum, /*fSubtractFeeFromAmount=*/true, /*scriptOverride=*/{}});

                auto res = CreateTransaction(*pwallet, vecSend, /*change_pos=*/std::nullopt, coinControl, /*sign=*/true);
                if (!res)
                    throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, util::ErrorString(res).original);

                pwallet->CommitTransaction(res->tx, /*mapValue=*/{}, /*orderForm=*/{});

                UniValue obj(UniValue::VOBJ);
                obj.pushKV("selected_coins", selectedCount);
                obj.pushKV("selection_sum", FormatMoney(selectionSum));
                obj.pushKV("txid", res->tx->GetHash().GetHex());
                obj.pushKV("batch_number", batchCount + 1);
                ret.push_back(obj);

                batchCount++;

                // Refresh UTXO count
                CoinsResult freshCoins = AvailableCoins(*pwallet);
                utxoCount = static_cast<int>(freshCoins.Size());

                if (utxoCount <= minimumUtxoAmount)
                    break;

                nOps = (utxoCount - minimumUtxoAmount) / batchDivisor;
                nOdds = (utxoCount - minimumUtxoAmount) % batchDivisor;
                if (nOdds == 0) {
                    nOdds = batchDivisor;
                } else if (nOdds < 10) {
                    nOdds = batchDivisor;
                    nOps--;
                }
            }

            if (batchCount > 0) {
                UniValue summary(UniValue::VOBJ);
                summary.pushKV("total_batches_processed", batchCount);
                summary.pushKV("remaining_utxos", utxoCount);
                if (maxBatches > 0 && batchCount >= maxBatches)
                    summary.pushKV("status", "stopped_at_batch_limit");
                else
                    summary.pushKV("status", "completed_all_eligible_utxos");
                ret.push_back(summary);
            }

            return ret;
        },
    };
}

} // namespace wallet
