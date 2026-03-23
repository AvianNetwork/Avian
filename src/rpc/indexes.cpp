// Copyright (c) 2017-2019 The Raven Core developers
// Copyright (c) 2020-2024 The Avian developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <rpc/server.h>
#include <rpc/server_util.h>
#include <rpc/util.h>

#include <addressindex.h>
#include <spentindex.h>
#include <assets/assets.h>
#include <chain.h>
#include <core_io.h>
#include <key_io.h>
#include <node/context.h>
#include <txmempool.h>
#include <validation.h>

#include <univalue.h>

#include <cstring>

static bool getAddressFromIndex(const int& type, const uint160& hash, std::string& address)
{
    if (type == 2) {
        address = EncodeDestination(ScriptHash(hash));
    } else if (type == 1) {
        address = EncodeDestination(PKHash(hash));
    } else {
        return false;
    }
    return true;
}

static bool getAddressesFromParams(const UniValue& params, std::vector<std::pair<uint160, int>>& addresses)
{
    if (params[0].isStr()) {
        CTxDestination dest = DecodeDestination(params[0].get_str());
        if (!IsValidDestination(dest)) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid address");
        }
        uint160 hashBytes;
        int type = 0;
        if (auto* keyID = std::get_if<PKHash>(&dest)) {
            hashBytes = ToKeyID(*keyID);
            type = 1;
        } else if (auto* scriptID = std::get_if<ScriptHash>(&dest)) {
            CScriptID sid = ToScriptID(*scriptID);
            std::memcpy(hashBytes.data(), sid.data(), 20);
            type = 2;
        } else {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Unsupported address type");
        }
        addresses.push_back(std::make_pair(hashBytes, type));
    } else if (params[0].isObject()) {
        const UniValue& addressValues = params[0]["addresses"];
        if (!addressValues.isArray()) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Addresses is expected to be an array");
        }

        for (unsigned int i = 0; i < addressValues.size(); i++) {
            CTxDestination dest = DecodeDestination(addressValues[i].get_str());
            if (!IsValidDestination(dest)) {
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid address");
            }
            uint160 hashBytes;
            int type = 0;
            if (auto* keyID = std::get_if<PKHash>(&dest)) {
                hashBytes = ToKeyID(*keyID);
                type = 1;
            } else if (auto* scriptID = std::get_if<ScriptHash>(&dest)) {
                CScriptID sid = ToScriptID(*scriptID);
                std::memcpy(hashBytes.data(), sid.data(), 20);
                type = 2;
            } else {
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Unsupported address type");
            }
            addresses.push_back(std::make_pair(hashBytes, type));
        }
    } else {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid address");
    }

    return true;
}

static bool heightSort(const std::pair<CAddressUnspentKey, CAddressUnspentValue>& a,
                       const std::pair<CAddressUnspentKey, CAddressUnspentValue>& b)
{
    return a.second.blockHeight < b.second.blockHeight;
}

static bool timestampSort(const std::pair<CMempoolAddressDeltaKey, CMempoolAddressDelta>& a,
                          const std::pair<CMempoolAddressDeltaKey, CMempoolAddressDelta>& b)
{
    return a.second.time < b.second.time;
}

static RPCHelpMan getaddressmempool()
{
    return RPCHelpMan{"getaddressmempool",
        "\nReturns all mempool deltas for an address (requires addressindex to be enabled).\n",
        {
            {"addresses", RPCArg::Type::OBJ, RPCArg::Optional::NO, "Address object",
                {
                    {"addresses", RPCArg::Type::ARR, RPCArg::Optional::NO, "Array of base58check encoded addresses",
                        {
                            {"address", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "The base58check encoded address"},
                        },
                    },
                },
            },
            {"includeAssets", RPCArg::Type::BOOL, RPCArg::Default{false}, "If true, include asset deltas"},
        },
        RPCResult{RPCResult::Type::ARR, "", "",
            {
                {RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR, "address", "The base58check encoded address"},
                        {RPCResult::Type::STR, "assetName", "The asset name (AVN for Aviancoin)"},
                        {RPCResult::Type::STR_HEX, "txid", "The related txid"},
                        {RPCResult::Type::NUM, "index", "The related input or output index"},
                        {RPCResult::Type::NUM, "satoshis", "The difference of satoshis"},
                        {RPCResult::Type::NUM, "timestamp", "The time the transaction entered the mempool"},
                        {RPCResult::Type::STR_HEX, "prevtxid", /*optional=*/true, "The previous txid (if spending)"},
                        {RPCResult::Type::NUM, "prevout", /*optional=*/true, "The previous output index (if spending)"},
                    },
                },
            },
        },
        RPCExamples{
            HelpExampleCli("getaddressmempool", "'{\"addresses\": [\"RXissueAssetXXXXXXXXXXXXXXXXXhhZGt\"]}'")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            if (!fAddressIndex)
                throw JSONRPCError(RPC_MISC_ERROR, "Address index not enabled");

            std::vector<std::pair<uint160, int>> addresses;
            if (!getAddressesFromParams(request.params, addresses)) {
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid address");
            }

            bool includeAssets = false;
            if (!request.params[1].isNull()) {
                includeAssets = request.params[1].get_bool();
            }

            if (includeAssets && !AreAssetsDeployed())
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Assets aren't active. includeAssets can't be true.");

            const CTxMemPool& mempool = EnsureAnyMemPool(request.context);
            std::vector<std::pair<CMempoolAddressDeltaKey, CMempoolAddressDelta>> indexes;

            {
                LOCK(mempool.cs);
                if (includeAssets) {
                    mempool.getAddressIndex(addresses, indexes);
                } else {
                    mempool.getAddressIndex(addresses, AVN, indexes);
                }
            }

            std::sort(indexes.begin(), indexes.end(), timestampSort);

            UniValue result(UniValue::VARR);
            for (const auto& [key, delta] : indexes) {
                std::string address;
                if (!getAddressFromIndex(key.type, key.addressBytes, address)) {
                    throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Unknown address type");
                }

                UniValue obj(UniValue::VOBJ);
                obj.pushKV("address", address);
                obj.pushKV("assetName", key.asset);
                obj.pushKV("txid", key.txhash.GetHex());
                obj.pushKV("index", (int)key.index);
                obj.pushKV("satoshis", delta.amount);
                obj.pushKV("timestamp", delta.time);
                if (delta.amount < 0) {
                    obj.pushKV("prevtxid", delta.prevhash.GetHex());
                    obj.pushKV("prevout", (int)delta.prevout);
                }
                result.push_back(std::move(obj));
            }

            return result;
        },
    };
}

static RPCHelpMan getaddressutxos()
{
    return RPCHelpMan{"getaddressutxos",
        "\nReturns all unspent outputs for an address (requires addressindex to be enabled).\n",
        {
            {"addresses", RPCArg::Type::OBJ, RPCArg::Optional::NO, "Address object",
                {
                    {"addresses", RPCArg::Type::ARR, RPCArg::Optional::NO, "Array of base58check encoded addresses",
                        {
                            {"address", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "The base58check encoded address"},
                        },
                    },
                    {"chainInfo", RPCArg::Type::BOOL, RPCArg::Default{false}, "Include chain info with results"},
                    {"assetName", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Get UTXOs for a particular asset ('*' for all)"},
                    {"limit", RPCArg::Type::NUM, RPCArg::Default{0}, "Maximum number of UTXOs to return (0 = no limit)"},
                    {"offset", RPCArg::Type::NUM, RPCArg::Default{0}, "Number of UTXOs to skip"},
                },
            },
        },
        RPCResult{RPCResult::Type::ARR, "", "",
            {
                {RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR, "address", "The address"},
                        {RPCResult::Type::STR, "assetName", "The asset name"},
                        {RPCResult::Type::STR_HEX, "txid", "The output txid"},
                        {RPCResult::Type::NUM, "outputIndex", "The output index"},
                        {RPCResult::Type::STR_HEX, "script", "The script hex encoded"},
                        {RPCResult::Type::NUM, "satoshis", "The number of satoshis"},
                        {RPCResult::Type::NUM, "height", "The block height"},
                    },
                },
            },
        },
        RPCExamples{
            HelpExampleCli("getaddressutxos", "'{\"addresses\": [\"RXissueAssetXXXXXXXXXXXXXXXXXhhZGt\"]}'")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            if (!fAddressIndex)
                throw JSONRPCError(RPC_MISC_ERROR, "Address index not enabled");

            bool includeChainInfo = false;
            std::string assetName = AVN;
            int limit = 0;
            int offset = 0;

            if (request.params[0].isObject()) {
                const UniValue& chainInfo = request.params[0]["chainInfo"];
                if (chainInfo.isBool()) includeChainInfo = chainInfo.get_bool();

                const UniValue& assetNameParam = request.params[0]["assetName"];
                if (assetNameParam.isStr()) {
                    if (!AreAssetsDeployed())
                        throw JSONRPCError(RPC_INVALID_PARAMETER, "Assets aren't active.");
                    assetName = assetNameParam.get_str();
                }

                const UniValue& limitParam = request.params[0]["limit"];
                if (limitParam.isNum()) { limit = limitParam.getInt<int>(); if (limit < 0) limit = 0; }

                const UniValue& offsetParam = request.params[0]["offset"];
                if (offsetParam.isNum()) { offset = offsetParam.getInt<int>(); if (offset < 0) offset = 0; }
            }

            std::vector<std::pair<uint160, int>> addresses;
            if (!getAddressesFromParams(request.params, addresses)) {
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid address");
            }

            std::vector<std::pair<CAddressUnspentKey, CAddressUnspentValue>> unspentOutputs;
            for (const auto& [addrHash, addrType] : addresses) {
                if (assetName == "*") {
                    if (!GetAddressUnspent(addrHash, addrType, unspentOutputs))
                        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "No information available for address");
                } else {
                    if (!GetAddressUnspent(addrHash, addrType, assetName, unspentOutputs))
                        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "No information available for address");
                }
            }

            std::sort(unspentOutputs.begin(), unspentOutputs.end(), heightSort);

            size_t startIdx = std::min((size_t)offset, unspentOutputs.size());
            size_t endIdx = (limit > 0) ? std::min((size_t)offset + limit, unspentOutputs.size()) : unspentOutputs.size();

            UniValue utxos(UniValue::VARR);
            for (size_t idx = startIdx; idx < endIdx; idx++) {
                const auto& [uKey, uVal] = unspentOutputs[idx];
                std::string address;
                if (!getAddressFromIndex(uKey.type, uKey.hashBytes, address))
                    throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Unknown address type");

                UniValue output(UniValue::VOBJ);
                output.pushKV("address", address);
                output.pushKV("assetName", uKey.asset);
                output.pushKV("txid", uKey.txhash.GetHex());
                output.pushKV("outputIndex", (int)uKey.index);
                output.pushKV("script", HexStr(uVal.script));
                output.pushKV("satoshis", uVal.satoshis);
                output.pushKV("height", uVal.blockHeight);
                utxos.push_back(std::move(output));
            }

            if (includeChainInfo) {
                UniValue result(UniValue::VOBJ);
                result.pushKV("utxos", utxos);
                result.pushKV("total", (int)unspentOutputs.size());
                result.pushKV("limit", limit);
                result.pushKV("offset", offset);

                ChainstateManager& chainman = EnsureAnyChainman(request.context);
                LOCK(cs_main);
                const CChain& active_chain = chainman.ActiveChain();
                result.pushKV("hash", active_chain.Tip()->GetBlockHash().GetHex());
                result.pushKV("height", active_chain.Height());
                return result;
            }

            return utxos;
        },
    };
}

static RPCHelpMan getaddressdeltas()
{
    return RPCHelpMan{"getaddressdeltas",
        "\nReturns all changes for an address (requires addressindex to be enabled).\n",
        {
            {"addresses", RPCArg::Type::OBJ, RPCArg::Optional::NO, "Address object",
                {
                    {"addresses", RPCArg::Type::ARR, RPCArg::Optional::NO, "Array of base58check encoded addresses",
                        {
                            {"address", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "The base58check encoded address"},
                        },
                    },
                    {"start", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "The start block height"},
                    {"end", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "The end block height"},
                    {"chainInfo", RPCArg::Type::BOOL, RPCArg::Default{false}, "Include chain info in results"},
                    {"assetName", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Get deltas for a particular asset"},
                    {"limit", RPCArg::Type::NUM, RPCArg::Default{0}, "Maximum number of deltas (0 = no limit)"},
                    {"offset", RPCArg::Type::NUM, RPCArg::Default{0}, "Number of deltas to skip"},
                },
            },
        },
        RPCResult{RPCResult::Type::ARR, "", "",
            {
                {RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR, "assetName", "The asset name"},
                        {RPCResult::Type::NUM, "satoshis", "The difference of satoshis"},
                        {RPCResult::Type::STR_HEX, "txid", "The related txid"},
                        {RPCResult::Type::NUM, "index", "The related input or output index"},
                        {RPCResult::Type::NUM, "blockindex", "The related block index"},
                        {RPCResult::Type::NUM, "height", "The block height"},
                        {RPCResult::Type::STR, "address", "The address"},
                    },
                },
            },
        },
        RPCExamples{
            HelpExampleCli("getaddressdeltas", "'{\"addresses\": [\"RXissueAssetXXXXXXXXXXXXXXXXXhhZGt\"]}'")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            if (!fAddressIndex)
                throw JSONRPCError(RPC_MISC_ERROR, "Address index not enabled");

            if (!request.params[0].isObject())
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Expected object parameter");

            const UniValue& startValue = request.params[0]["start"];
            const UniValue& endValue = request.params[0]["end"];

            bool includeChainInfo = false;
            const UniValue& chainInfo = request.params[0]["chainInfo"];
            if (chainInfo.isBool()) includeChainInfo = chainInfo.get_bool();

            std::string assetName = AVN;
            const UniValue& assetNameParam = request.params[0]["assetName"];
            if (assetNameParam.isStr()) {
                if (!AreAssetsDeployed())
                    throw JSONRPCError(RPC_INVALID_PARAMETER, "Assets aren't active.");
                assetName = assetNameParam.get_str();
            }

            int limit = 0, offset = 0;
            const UniValue& limitParam = request.params[0]["limit"];
            if (limitParam.isNum()) { limit = limitParam.getInt<int>(); if (limit < 0) limit = 0; }
            const UniValue& offsetParam = request.params[0]["offset"];
            if (offsetParam.isNum()) { offset = offsetParam.getInt<int>(); if (offset < 0) offset = 0; }

            int start = 0, end = 0;
            if (startValue.isNum() && endValue.isNum()) {
                start = startValue.getInt<int>();
                end = endValue.getInt<int>();
                if (start <= 0 || end <= 0)
                    throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Start and end must be greater than zero");
                if (end < start)
                    throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "End must be greater than or equal to start");
            }

            std::vector<std::pair<uint160, int>> addresses;
            if (!getAddressesFromParams(request.params, addresses))
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid address");

            std::vector<std::pair<CAddressIndexKey, CAmount>> addressIndex;
            for (const auto& [addrHash, addrType] : addresses) {
                if (start > 0 && end > 0) {
                    if (!GetAddressIndex(addrHash, addrType, assetName, addressIndex, start, end))
                        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "No information available for address");
                } else {
                    if (!GetAddressIndex(addrHash, addrType, assetName, addressIndex))
                        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "No information available for address");
                }
            }

            size_t startIdx = std::min((size_t)offset, addressIndex.size());
            size_t endIdx = (limit > 0) ? std::min((size_t)offset + limit, addressIndex.size()) : addressIndex.size();

            UniValue deltas(UniValue::VARR);
            for (size_t idx = startIdx; idx < endIdx; idx++) {
                const auto& [aKey, aAmount] = addressIndex[idx];
                std::string address;
                if (!getAddressFromIndex(aKey.type, aKey.hashBytes, address))
                    throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Unknown address type");

                UniValue delta(UniValue::VOBJ);
                delta.pushKV("assetName", aKey.asset);
                delta.pushKV("satoshis", aAmount);
                delta.pushKV("txid", aKey.txhash.GetHex());
                delta.pushKV("index", (int)aKey.index);
                delta.pushKV("blockindex", (int)aKey.txindex);
                delta.pushKV("height", aKey.blockHeight);
                delta.pushKV("address", address);
                deltas.push_back(std::move(delta));
            }

            if (includeChainInfo && start > 0 && end > 0) {
                ChainstateManager& chainman = EnsureAnyChainman(request.context);
                LOCK(cs_main);
                const CChain& active_chain = chainman.ActiveChain();

                if (start > active_chain.Height() || end > active_chain.Height())
                    throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Start or end is outside chain range");

                UniValue result(UniValue::VOBJ);
                UniValue startInfo(UniValue::VOBJ);
                UniValue endInfo(UniValue::VOBJ);

                startInfo.pushKV("hash", active_chain[start]->GetBlockHash().GetHex());
                startInfo.pushKV("height", start);
                endInfo.pushKV("hash", active_chain[end]->GetBlockHash().GetHex());
                endInfo.pushKV("height", end);

                result.pushKV("deltas", deltas);
                result.pushKV("total", (int)addressIndex.size());
                result.pushKV("limit", limit);
                result.pushKV("offset", offset);
                result.pushKV("start", startInfo);
                result.pushKV("end", endInfo);
                return result;
            }

            return deltas;
        },
    };
}

static RPCHelpMan getaddressbalance()
{
    return RPCHelpMan{"getaddressbalance",
        "\nReturns the balance for an address(es) (requires addressindex to be enabled).\n",
        {
            {"addresses", RPCArg::Type::OBJ, RPCArg::Optional::NO, "Address object",
                {
                    {"addresses", RPCArg::Type::ARR, RPCArg::Optional::NO, "Array of base58check encoded addresses",
                        {
                            {"address", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "The base58check encoded address"},
                        },
                    },
                },
            },
            {"includeAssets", RPCArg::Type::BOOL, RPCArg::Default{false}, "If true, include asset balances"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::NUM, "balance", "The current balance in satoshis"},
                {RPCResult::Type::NUM, "received", "The total number of satoshis received"},
            },
        },
        RPCExamples{
            HelpExampleCli("getaddressbalance", "'{\"addresses\": [\"RXissueAssetXXXXXXXXXXXXXXXXXhhZGt\"]}'")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            if (!fAddressIndex)
                throw JSONRPCError(RPC_MISC_ERROR, "Address index not enabled");

            std::vector<std::pair<uint160, int>> addresses;
            if (!getAddressesFromParams(request.params, addresses))
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid address");

            bool includeAssets = false;
            if (!request.params[1].isNull())
                includeAssets = request.params[1].get_bool();

            if (includeAssets && !AreAssetsDeployed())
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Assets aren't active.");

            if (includeAssets) {
                std::vector<std::pair<CAddressIndexKey, CAmount>> addressIndex;
                for (const auto& [addrHash, addrType] : addresses) {
                    if (!GetAddressIndex(addrHash, addrType, addressIndex))
                        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "No information available for address");
                }

                std::map<std::string, std::pair<CAmount, CAmount>> balances;
                for (const auto& [aKey, aAmount] : addressIndex) {
                    auto& [received, balance] = balances[aKey.asset];
                    if (aAmount > 0) received += aAmount;
                    balance += aAmount;
                }

                UniValue result(UniValue::VARR);
                for (const auto& [name, amounts] : balances) {
                    UniValue balance(UniValue::VOBJ);
                    balance.pushKV("assetName", name);
                    balance.pushKV("balance", amounts.second);
                    balance.pushKV("received", amounts.first);
                    result.push_back(std::move(balance));
                }
                return result;
            } else {
                std::vector<std::pair<CAddressIndexKey, CAmount>> addressIndex;
                for (const auto& [addrHash, addrType] : addresses) {
                    if (!GetAddressIndex(addrHash, addrType, AVN, addressIndex))
                        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "No information available for address");
                }

                CAmount balance = 0;
                CAmount received = 0;
                for (const auto& [aKey, aAmount] : addressIndex) {
                    if (aAmount > 0) received += aAmount;
                    balance += aAmount;
                }

                UniValue result(UniValue::VOBJ);
                result.pushKV("balance", balance);
                result.pushKV("received", received);
                return result;
            }
        },
    };
}

static RPCHelpMan getaddresstxids()
{
    return RPCHelpMan{"getaddresstxids",
        "\nReturns the txids for an address(es) (requires addressindex to be enabled).\n",
        {
            {"addresses", RPCArg::Type::OBJ, RPCArg::Optional::NO, "Address object",
                {
                    {"addresses", RPCArg::Type::ARR, RPCArg::Optional::NO, "Array of base58check encoded addresses",
                        {
                            {"address", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "The base58check encoded address"},
                        },
                    },
                    {"start", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "The start block height"},
                    {"end", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "The end block height"},
                    {"limit", RPCArg::Type::NUM, RPCArg::Default{0}, "Maximum number of txids (0 = no limit)"},
                    {"offset", RPCArg::Type::NUM, RPCArg::Default{0}, "Number of txids to skip"},
                },
            },
            {"includeAssets", RPCArg::Type::BOOL, RPCArg::Default{false}, "If true, include asset transactions"},
        },
        RPCResult{RPCResult::Type::ARR, "", "",
            {
                {RPCResult::Type::STR_HEX, "txid", "The transaction id"},
            },
        },
        RPCExamples{
            HelpExampleCli("getaddresstxids", "'{\"addresses\": [\"RXissueAssetXXXXXXXXXXXXXXXXXhhZGt\"]}'")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            if (!fAddressIndex)
                throw JSONRPCError(RPC_MISC_ERROR, "Address index not enabled");

            std::vector<std::pair<uint160, int>> addresses;
            if (!getAddressesFromParams(request.params, addresses))
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid address");

            int start = 0, end = 0, limit = 0, offset = 0;
            if (request.params[0].isObject()) {
                const UniValue& startValue = request.params[0]["start"];
                const UniValue& endValue = request.params[0]["end"];
                if (startValue.isNum() && endValue.isNum()) {
                    start = startValue.getInt<int>();
                    end = endValue.getInt<int>();
                }
                const UniValue& limitParam = request.params[0]["limit"];
                if (limitParam.isNum()) { limit = limitParam.getInt<int>(); if (limit < 0) limit = 0; }
                const UniValue& offsetParam = request.params[0]["offset"];
                if (offsetParam.isNum()) { offset = offsetParam.getInt<int>(); if (offset < 0) offset = 0; }
            }

            bool includeAssets = false;
            if (!request.params[1].isNull())
                includeAssets = request.params[1].get_bool();

            if (includeAssets && !AreAssetsDeployed())
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Assets aren't active.");

            std::vector<std::pair<CAddressIndexKey, CAmount>> addressIndex;
            for (const auto& [addrHash, addrType] : addresses) {
                if (includeAssets) {
                    if (start > 0 && end > 0) {
                        if (!GetAddressIndex(addrHash, addrType, addressIndex, start, end))
                            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "No information available for address");
                    } else {
                        if (!GetAddressIndex(addrHash, addrType, addressIndex))
                            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "No information available for address");
                    }
                } else {
                    if (start > 0 && end > 0) {
                        if (!GetAddressIndex(addrHash, addrType, AVN, addressIndex, start, end))
                            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "No information available for address");
                    } else {
                        if (!GetAddressIndex(addrHash, addrType, AVN, addressIndex))
                            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "No information available for address");
                    }
                }
            }

            std::set<std::pair<int, std::string>> txids;
            UniValue result(UniValue::VARR);

            for (const auto& [aKey, aAmount] : addressIndex) {
                std::string txid = aKey.txhash.GetHex();
                if (txids.insert(std::make_pair(aKey.blockHeight, txid)).second) {
                    result.push_back(txid);
                }
            }

            // Apply pagination
            if (limit > 0 || offset > 0) {
                size_t startIdx = std::min((size_t)offset, (size_t)result.size());
                size_t endIdx = (limit > 0) ? std::min((size_t)offset + limit, (size_t)result.size()) : (size_t)result.size();

                UniValue paginatedResult(UniValue::VARR);
                for (size_t idx = startIdx; idx < endIdx; idx++) {
                    paginatedResult.push_back(result[(int)idx]);
                }

                UniValue response(UniValue::VOBJ);
                response.pushKV("txids", paginatedResult);
                response.pushKV("total", (int)result.size());
                response.pushKV("limit", limit);
                response.pushKV("offset", offset);
                return response;
            }

            return result;
        },
    };
}

static RPCHelpMan getspentinfo()
{
    return RPCHelpMan{"getspentinfo",
        "\nReturns the txid and index where an output is spent.\n",
        {
            {"txid_index", RPCArg::Type::OBJ, RPCArg::Optional::NO, "Transaction output identifier",
                {
                    {"txid", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The hex string of the txid"},
                    {"index", RPCArg::Type::NUM, RPCArg::Optional::NO, "The output index"},
                },
            },
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::STR_HEX, "txid", "The spending transaction id"},
                {RPCResult::Type::NUM, "index", "The spending input index"},
                {RPCResult::Type::NUM, "height", "The block height"},
            },
        },
        RPCExamples{
            HelpExampleCli("getspentinfo", "'{\"txid\": \"0437cd7f8525ceed2324359c2d0ba26006d92d856a9c20fa0241106ee5a597c9\", \"index\": 0}'")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            if (!fSpentIndex)
                throw JSONRPCError(RPC_MISC_ERROR, "Spent index not enabled");

            const UniValue& txidValue = request.params[0]["txid"];
            const UniValue& indexValue = request.params[0]["index"];

            if (!txidValue.isStr() || !indexValue.isNum())
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid txid or index");

            auto txid_parsed = uint256::FromHex(txidValue.get_str());
            if (!txid_parsed)
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid txid");

            uint256 txid = *txid_parsed;
            int outputIndex = indexValue.getInt<int>();

            CSpentIndexKey key(txid, outputIndex);
            CSpentIndexValue value;

            if (!GetSpentIndex(key, value))
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Unable to get spent info");

            UniValue obj(UniValue::VOBJ);
            obj.pushKV("txid", value.txid.GetHex());
            obj.pushKV("index", (int)value.inputIndex);
            obj.pushKV("height", value.blockHeight);
            return obj;
        },
    };
}

void RegisterIndexRPCCommands(CRPCTable &t)
{
    static const CRPCCommand commands[]{
        {"blockchain", &getaddressbalance},
        {"blockchain", &getaddressdeltas},
        {"blockchain", &getaddressutxos},
        {"blockchain", &getaddressmempool},
        {"blockchain", &getaddresstxids},
        {"blockchain", &getspentinfo},
    };

    for (const auto& c : commands) {
        t.appendCommand(c.name, &c);
    }
}
