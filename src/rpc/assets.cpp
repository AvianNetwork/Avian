// Copyright (c) 2017-2019 The Raven Core developers
// Copyright (c) 2020-2024 The Avian developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <rpc/server.h>
#include <rpc/server_util.h>
#include <rpc/util.h>

#include <assets/assets.h>
#include <assets/assetdb.h>
#include <assets/restricteddb.h>
#include <core_io.h>
#include <validation.h>

#include <assets/ans.h>
#include <assets/assetsnapshotdb.h>
#include <assets/snapshotrequestdb.h>
#include <key_io.h>

#include <univalue.h>

extern CAssetSnapshotDB* pAssetSnapshotDb;
extern CSnapshotRequestDB* pSnapshotRequestDb;

static RPCHelpMan listassets()
{
    return RPCHelpMan{
        "listassets",
        "Returns a list of all assets.\n"
        "This could be a slow/expensive operation as it reads from the database.\n",
        {
            {"asset", RPCArg::Type::STR, RPCArg::Default{"*"}, "Filters results -- must be an asset name or a partial asset name followed by '*' ('*' matches all trailing characters)"},
            {"verbose", RPCArg::Type::BOOL, RPCArg::Default{false}, "When false result is just a list of asset names -- when true results are asset name mapped to metadata"},
            {"count", RPCArg::Type::NUM, RPCArg::DefaultHint{"all"}, "Truncates results to include only the first count assets found"},
            {"start", RPCArg::Type::NUM, RPCArg::Default{0}, "Results skip over the first start assets found (if negative it skips back from the end)"},
        },
        {
            RPCResult{"verbose=false",
                RPCResult::Type::ARR, "", "",
                {
                    {RPCResult::Type::STR, "", "asset name"},
                }
            },
            RPCResult{"verbose=true",
                RPCResult::Type::OBJ_DYN, "", "",
                {
                    {RPCResult::Type::OBJ, "asset_name", "",
                    {
                        {RPCResult::Type::STR, "name", "the asset name"},
                        {RPCResult::Type::NUM, "amount", "the total amount issued"},
                        {RPCResult::Type::NUM, "units", "the number of decimal places"},
                        {RPCResult::Type::NUM, "reissuable", "1 if reissuable"},
                        {RPCResult::Type::NUM, "has_ipfs", "1 if has IPFS data"},
                        {RPCResult::Type::NUM, "block_height", "the block height the asset was created"},
                        {RPCResult::Type::STR_HEX, "blockhash", "the block hash the asset was created"},
                    }},
                }
            },
        },
        RPCExamples{
            HelpExampleCli("listassets", "")
          + HelpExampleCli("listassets", "\"ASSET*\" true 10 20")
          + HelpExampleRpc("listassets", "")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            if (!passetsdb)
                throw JSONRPCError(RPC_INTERNAL_ERROR, "asset db unavailable.");

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

            std::vector<CDatabasedAssetData> assets;
            if (!passetsdb->AssetDir(assets, filter, count, start))
                throw JSONRPCError(RPC_INTERNAL_ERROR, "couldn't retrieve asset directory.");

            UniValue result;
            result = verbose ? UniValue(UniValue::VOBJ) : UniValue(UniValue::VARR);

            for (const auto& data : assets) {
                const CNewAsset& asset = data.asset;
                if (verbose) {
                    UniValue detail(UniValue::VOBJ);
                    detail.pushKV("name", asset.strName);
                    detail.pushKV("amount", UnitValueFromAmount(asset.nAmount, asset.units));
                    detail.pushKV("units", asset.units);
                    detail.pushKV("reissuable", asset.nReissuable);
                    detail.pushKV("has_ipfs", asset.nHasIPFS);
                    detail.pushKV("block_height", data.nHeight);
                    detail.pushKV("blockhash", data.blockHash.GetHex());
                    if (asset.nHasIPFS) {
                        if (asset.strIPFSHash.size() == 32) {
                            detail.pushKV("txid_hash", EncodeAssetData(asset.strIPFSHash));
                        } else {
                            detail.pushKV("ipfs_hash", EncodeAssetData(asset.strIPFSHash));
                        }
                    }
                    result.pushKV(asset.strName, detail);
                } else {
                    result.push_back(asset.strName);
                }
            }

            return result;
        },
    };
}

static RPCHelpMan getassetdata()
{
    return RPCHelpMan{
        "getassetdata",
        "Returns asset metadata if that asset exists.\n",
        {
            {"asset_name", RPCArg::Type::STR, RPCArg::Optional::NO, "the name of the asset"},
        },
        RPCResult{
            RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::STR, "name", "the asset name"},
                {RPCResult::Type::NUM, "amount", "the total amount issued"},
                {RPCResult::Type::NUM, "units", "the number of decimal places"},
                {RPCResult::Type::NUM, "reissuable", "1 if reissuable"},
                {RPCResult::Type::NUM, "has_ipfs", "1 if has IPFS data"},
                {RPCResult::Type::STR, "ipfs_hash", /*optional=*/true, "the IPFS hash (only if has_ipfs = 1)"},
                {RPCResult::Type::STR, "verifier_string", /*optional=*/true, "the verifier string for restricted assets"},
            }
        },
        RPCExamples{
            HelpExampleCli("getassetdata", "\"ASSET_NAME\"")
          + HelpExampleRpc("getassetdata", "\"ASSET_NAME\"")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            std::string asset_name = request.params[0].get_str();

            LOCK(cs_main);
            UniValue result(UniValue::VOBJ);

            if (passets) {
                CNewAsset asset;
                if (!passets->GetAssetMetaDataIfExists(asset_name, asset))
                    return UniValue::VNULL;

                result.pushKV("name", asset.strName);
                result.pushKV("amount", UnitValueFromAmount(asset.nAmount, asset.units));
                result.pushKV("units", asset.units);
                result.pushKV("reissuable", asset.nReissuable);
                result.pushKV("has_ipfs", asset.nHasIPFS);

                if (asset.nHasIPFS) {
                    if (asset.strIPFSHash.size() == 32) {
                        result.pushKV("txid_hash", EncodeAssetData(asset.strIPFSHash));
                    } else {
                        result.pushKV("ipfs_hash", EncodeAssetData(asset.strIPFSHash));
                    }
                }

                CNullAssetTxVerifierString verifier;
                if (passets->GetAssetVerifierStringIfExists(asset.strName, verifier)) {
                    result.pushKV("verifier_string", verifier.verifier_string);
                }

                return result;
            }

            return UniValue::VNULL;
        },
    };
}

static RPCHelpMan getcacheinfo()
{
    return RPCHelpMan{
        "getcacheinfo",
        "Returns information about the asset cache.\n",
        {},
        RPCResult{
            RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::NUM, "asset_total_cache_size", "total size of asset caches"},
                {RPCResult::Type::NUM, "asset_address_map_size", "size of address-to-asset amount map"},
            }
        },
        RPCExamples{
            HelpExampleCli("getcacheinfo", "")
          + HelpExampleRpc("getcacheinfo", "")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            LOCK(cs_main);
            UniValue result(UniValue::VOBJ);

            if (passets) {
                result.pushKV("asset_total_cache_size", (int)passets->DynamicMemoryUsage());
                result.pushKV("asset_address_map_size", (int)passets->mapAssetsAddressAmount.size());
            } else {
                result.pushKV("asset_total_cache_size", 0);
                result.pushKV("asset_address_map_size", 0);
            }

            return result;
        },
    };
}

static RPCHelpMan listassetbalancesbyaddress()
{
    return RPCHelpMan{
        "listassetbalancesbyaddress",
        "Returns a list of all asset balances for an address.\n",
        {
            {"address", RPCArg::Type::STR, RPCArg::Optional::NO, "a valid Avian address"},
            {"onlytotal", RPCArg::Type::BOOL, RPCArg::Default{false}, "when false result is just a list of assets balances -- when true only the number of assets is returned"},
            {"count", RPCArg::Type::NUM, RPCArg::DefaultHint{"all"}, "truncates results to include only the first count assets found"},
            {"start", RPCArg::Type::NUM, RPCArg::Default{0}, "results skip over the first start assets found"},
        },
        RPCResult{
            RPCResult::Type::OBJ_DYN, "", "",
            {
                {RPCResult::Type::NUM, "asset_name", "asset balance"},
            }
        },
        RPCExamples{
            HelpExampleCli("listassetbalancesbyaddress", "\"RXissueAssetXXXXXXXXXXXXXXXXZFGHWo\"")
          + HelpExampleRpc("listassetbalancesbyaddress", "\"RXissueAssetXXXXXXXXXXXXXXXXZFGHWo\"")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            if (!fAssetIndex)
                throw JSONRPCError(RPC_MISC_ERROR, "This rpc call is not functional unless -assetindex is enabled.");

            std::string address = request.params[0].get_str();

            bool onlytotal = false;
            if (!request.params[1].isNull())
                onlytotal = request.params[1].get_bool();

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

            LOCK(cs_main);

            if (!passets)
                throw JSONRPCError(RPC_INTERNAL_ERROR, "asset cache unavailable.");

            UniValue result(UniValue::VOBJ);

            if (onlytotal) {
                // Just count the number of assets at this address
                int count = 0;
                for (const auto& [pair, amount] : passets->mapAssetsAddressAmount) {
                    if (pair.second == address && amount > 0) {
                        count++;
                    }
                }
                result.pushKV("total", count);
            } else {
                size_t found = 0;
                long skipped = 0;
                for (const auto& [pair, amount] : passets->mapAssetsAddressAmount) {
                    if (pair.second == address && amount > 0) {
                        if (skipped < start) {
                            skipped++;
                            continue;
                        }
                        result.pushKV(pair.first, AssetUnitValueFromAmount(amount, pair.first));
                        found++;
                        if (found >= count)
                            break;
                    }
                }
            }

            return result;
        },
    };
}

static RPCHelpMan listaddressesbyasset()
{
    return RPCHelpMan{
        "listaddressesbyasset",
        "Returns a list of all addresses that hold the given asset.\n",
        {
            {"asset_name", RPCArg::Type::STR, RPCArg::Optional::NO, "name of the asset"},
            {"onlytotal", RPCArg::Type::BOOL, RPCArg::Default{false}, "when false result is just a list of addresses with balances -- when true only the number of addresses is returned"},
            {"count", RPCArg::Type::NUM, RPCArg::DefaultHint{"all"}, "truncates results to include only the first count addresses found"},
            {"start", RPCArg::Type::NUM, RPCArg::Default{0}, "results skip over the first start addresses found"},
        },
        RPCResult{
            RPCResult::Type::OBJ_DYN, "", "",
            {
                {RPCResult::Type::NUM, "address", "balance"},
            }
        },
        RPCExamples{
            HelpExampleCli("listaddressesbyasset", "\"ASSET_NAME\"")
          + HelpExampleRpc("listaddressesbyasset", "\"ASSET_NAME\"")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            if (!fAssetIndex)
                throw JSONRPCError(RPC_MISC_ERROR, "This rpc call is not functional unless -assetindex is enabled.");

            std::string assetName = request.params[0].get_str();

            bool onlytotal = false;
            if (!request.params[1].isNull())
                onlytotal = request.params[1].get_bool();

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

            LOCK(cs_main);

            if (!passets)
                throw JSONRPCError(RPC_INTERNAL_ERROR, "asset cache unavailable.");

            UniValue result(UniValue::VOBJ);

            // Check if asset exists
            CNewAsset asset;
            if (!passets->GetAssetMetaDataIfExists(assetName, asset))
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Asset not found: " + assetName);

            if (onlytotal) {
                int count = 0;
                for (const auto& [pair, amount] : passets->mapAssetsAddressAmount) {
                    if (pair.first == assetName && amount > 0) {
                        count++;
                    }
                }
                result.pushKV("total", count);
            } else {
                size_t found = 0;
                long skipped = 0;
                for (const auto& [pair, amount] : passets->mapAssetsAddressAmount) {
                    if (pair.first == assetName && amount > 0) {
                        if (skipped < start) {
                            skipped++;
                            continue;
                        }
                        result.pushKV(pair.second, AssetUnitValueFromAmount(amount, assetName));
                        found++;
                        if (found >= count)
                            break;
                    }
                }
            }

            return result;
        },
    };
}

static RPCHelpMan checkaddressrestriction()
{
    return RPCHelpMan{
        "checkaddressrestriction",
        "Checks to see if an address has been frozen by a restricted asset.\n",
        {
            {"address", RPCArg::Type::STR, RPCArg::Optional::NO, "the Avian address to search"},
            {"restricted_name", RPCArg::Type::STR, RPCArg::Optional::NO, "the restricted asset name to search"},
        },
        RPCResult{
            RPCResult::Type::BOOL, "", "true if the address is frozen"
        },
        RPCExamples{
            HelpExampleCli("checkaddressrestriction", "\"address\" \"$RESTRICTED_NAME\"")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            std::string address = request.params[0].get_str();
            std::string restricted_name = request.params[1].get_str();

            LOCK(cs_main);

            if (!passets)
                throw JSONRPCError(RPC_INTERNAL_ERROR, "asset cache unavailable.");

            return passets->CheckForAddressRestriction(restricted_name, address, true);
        },
    };
}

static RPCHelpMan checkglobalrestriction()
{
    return RPCHelpMan{
        "checkglobalrestriction",
        "Checks to see if a restricted asset is globally frozen.\n",
        {
            {"restricted_name", RPCArg::Type::STR, RPCArg::Optional::NO, "the restricted asset name to search"},
        },
        RPCResult{
            RPCResult::Type::BOOL, "", "true if the asset is globally frozen"
        },
        RPCExamples{
            HelpExampleCli("checkglobalrestriction", "\"$RESTRICTED_NAME\"")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            std::string restricted_name = request.params[0].get_str();

            LOCK(cs_main);

            if (!passets)
                throw JSONRPCError(RPC_INTERNAL_ERROR, "asset cache unavailable.");

            return passets->CheckForGlobalRestriction(restricted_name, true);
        },
    };
}

static UniValue ANSIDToObject(CAvianNameSystemID& ansID)
{
    UniValue obj(UniValue::VOBJ);
    obj.pushKV("id", ansID.to_string());
    obj.pushKV("type", (int)ansID.type());
    auto typePair = CAvianNameSystemID::enum_to_string(ansID.type());
    obj.pushKV("type_name", typePair.first);
    if (ansID.type() == CAvianNameSystemID::ADDR)
        obj.pushKV("address", ansID.addr());
    else if (ansID.type() == CAvianNameSystemID::IP)
        obj.pushKV("ip", ansID.ip());
    return obj;
}

static RPCHelpMan getansdata()
{
    return RPCHelpMan{
        "getansdata",
        "Returns ANS (Avian Name System) data for an asset if it has an ANS record.\n",
        {
            {"asset_name", RPCArg::Type::STR, RPCArg::Optional::NO, "the name of the asset"},
        },
        RPCResult{
            RPCResult::Type::OBJ, "", "ANS data object, or null if the asset has no ANS record",
            {
                {RPCResult::Type::STR, "id", "the ANS ID string"},
                {RPCResult::Type::NUM, "type", "the ANS type number"},
                {RPCResult::Type::STR, "type_name", "the ANS type description"},
                {RPCResult::Type::STR, "address", /*optional=*/true, "the Avian address (if type is ADDR)"},
                {RPCResult::Type::STR, "ip", /*optional=*/true, "the IP address (if type is IP)"},
            }
        },
        RPCExamples{
            HelpExampleCli("getansdata", "\"ASSET_NAME\"")
          + HelpExampleRpc("getansdata", "\"ASSET_NAME\"")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            std::string asset_name = request.params[0].get_str();

            LOCK(cs_main);

            if (!passets)
                throw JSONRPCError(RPC_INTERNAL_ERROR, "asset cache unavailable.");

            CNewAsset asset;
            if (!passets->GetAssetMetaDataIfExists(asset_name, asset))
                return UniValue::VNULL;

            if (!asset.nHasANS)
                return UniValue::VNULL;

            CAvianNameSystemID ansID(asset.strANSID);
            return ANSIDToObject(ansID);
        },
    };
}

static RPCHelpMan checkaddresstag()
{
    return RPCHelpMan{
        "checkaddresstag",
        "Checks to see if an address has a qualifier tag assigned to it.\n",
        {
            {"address", RPCArg::Type::STR, RPCArg::Optional::NO, "the Avian address to check"},
            {"tag_name", RPCArg::Type::STR, RPCArg::Optional::NO, "the qualifier tag name to search (e.g. \"#TAG\")"},
        },
        RPCResult{
            RPCResult::Type::BOOL, "", "true if the address has the tag assigned"
        },
        RPCExamples{
            HelpExampleCli("checkaddresstag", "\"address\" \"#TAG\"")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            std::string address = request.params[0].get_str();
            std::string tag_name = request.params[1].get_str();

            CTxDestination destination = DecodeDestination(address);
            if (!IsValidDestination(destination))
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid Avian address: " + address);

            if (!IsAssetNameAQualifier(tag_name))
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid qualifier name: " + tag_name + " (must start with '#')");

            LOCK(cs_main);

            if (!passets)
                throw JSONRPCError(RPC_INTERNAL_ERROR, "asset cache unavailable.");

            return passets->CheckForAddressQualifier(tag_name, address);
        },
    };
}

static RPCHelpMan listtagsforaddress()
{
    return RPCHelpMan{
        "listtagsforaddress",
        "Lists all qualifier tags assigned to an address.\n",
        {
            {"address", RPCArg::Type::STR, RPCArg::Optional::NO, "the Avian address to search"},
        },
        RPCResult{
            RPCResult::Type::ARR, "", "",
            {
                {RPCResult::Type::STR, "", "qualifier tag name"},
            }
        },
        RPCExamples{
            HelpExampleCli("listtagsforaddress", "\"address\"")
          + HelpExampleRpc("listtagsforaddress", "\"address\"")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            std::string address = request.params[0].get_str();

            CTxDestination destination = DecodeDestination(address);
            if (!IsValidDestination(destination))
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid Avian address: " + address);

            if (!prestricteddb)
                throw JSONRPCError(RPC_INTERNAL_ERROR, "restricted asset db unavailable.");

            std::vector<std::string> qualifiers;
            prestricteddb->GetAddressQualifiers(address, qualifiers);

            UniValue result(UniValue::VARR);
            for (const auto& qualifier : qualifiers) {
                result.push_back(qualifier);
            }

            return result;
        },
    };
}

static RPCHelpMan listaddressesfortag()
{
    return RPCHelpMan{
        "listaddressesfortag",
        "Lists all addresses that have a qualifier tag assigned to them.\n",
        {
            {"tag_name", RPCArg::Type::STR, RPCArg::Optional::NO, "the qualifier tag name (e.g. \"#TAG\")"},
        },
        RPCResult{
            RPCResult::Type::ARR, "", "",
            {
                {RPCResult::Type::STR, "", "Avian address"},
            }
        },
        RPCExamples{
            HelpExampleCli("listaddressesfortag", "\"#TAG\"")
          + HelpExampleRpc("listaddressesfortag", "\"#TAG\"")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            std::string tag_name = request.params[0].get_str();

            if (!IsAssetNameAQualifier(tag_name))
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid qualifier name: " + tag_name + " (must start with '#')");

            if (!prestricteddb)
                throw JSONRPCError(RPC_INTERNAL_ERROR, "restricted asset db unavailable.");

            std::vector<std::string> addresses;
            prestricteddb->GetQualifierAddresses(tag_name, addresses);

            UniValue result(UniValue::VARR);
            for (const auto& addr : addresses) {
                result.push_back(addr);
            }

            return result;
        },
    };
}

static RPCHelpMan listaddressrestrictions()
{
    return RPCHelpMan{
        "listaddressrestrictions",
        "Lists all restricted assets that have frozen the given address.\n",
        {
            {"address", RPCArg::Type::STR, RPCArg::Optional::NO, "the Avian address to search"},
        },
        RPCResult{
            RPCResult::Type::ARR, "", "",
            {
                {RPCResult::Type::STR, "", "restricted asset name"},
            }
        },
        RPCExamples{
            HelpExampleCli("listaddressrestrictions", "\"address\"")
          + HelpExampleRpc("listaddressrestrictions", "\"address\"")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            std::string address = request.params[0].get_str();

            CTxDestination destination = DecodeDestination(address);
            if (!IsValidDestination(destination))
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid Avian address: " + address);

            if (!prestricteddb)
                throw JSONRPCError(RPC_INTERNAL_ERROR, "restricted asset db unavailable.");

            std::vector<std::string> restrictions;
            prestricteddb->GetAddressRestrictions(address, restrictions);

            UniValue result(UniValue::VARR);
            for (const auto& restriction : restrictions) {
                result.push_back(restriction);
            }

            return result;
        },
    };
}

static RPCHelpMan listglobalrestrictions()
{
    return RPCHelpMan{
        "listglobalrestrictions",
        "Lists all globally frozen restricted assets.\n",
        {},
        RPCResult{
            RPCResult::Type::ARR, "", "",
            {
                {RPCResult::Type::STR, "", "restricted asset name"},
            }
        },
        RPCExamples{
            HelpExampleCli("listglobalrestrictions", "")
          + HelpExampleRpc("listglobalrestrictions", "")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            if (!prestricteddb)
                throw JSONRPCError(RPC_INTERNAL_ERROR, "restricted asset db unavailable.");

            std::vector<std::string> restrictions;
            prestricteddb->GetGlobalRestrictions(restrictions);

            UniValue result(UniValue::VARR);
            for (const auto& restriction : restrictions) {
                result.push_back(restriction);
            }

            return result;
        },
    };
}

static RPCHelpMan getverifierstring()
{
    return RPCHelpMan{
        "getverifierstring",
        "Returns the verifier string for a restricted asset.\n",
        {
            {"restricted_name", RPCArg::Type::STR, RPCArg::Optional::NO, "the restricted asset name (e.g. \"$RESTRICTED\")"},
        },
        RPCResult{
            RPCResult::Type::STR, "", "the verifier string"
        },
        RPCExamples{
            HelpExampleCli("getverifierstring", "\"$RESTRICTED_NAME\"")
          + HelpExampleRpc("getverifierstring", "\"$RESTRICTED_NAME\"")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            std::string asset_name = request.params[0].get_str();

            if (!IsAssetNameAnRestricted(asset_name))
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid restricted asset name: " + asset_name + " (must start with '$')");

            LOCK(cs_main);

            if (!passets)
                throw JSONRPCError(RPC_INTERNAL_ERROR, "asset cache unavailable.");

            CNullAssetTxVerifierString verifier;
            if (!passets->GetAssetVerifierStringIfExists(asset_name, verifier))
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Verifier string not found for asset: " + asset_name);

            return verifier.verifier_string;
        },
    };
}

static RPCHelpMan isvalidverifierstring()
{
    return RPCHelpMan{
        "isvalidverifierstring",
        "Checks to see if a verifier string is syntactically valid.\n",
        {
            {"verifier_string", RPCArg::Type::STR, RPCArg::Optional::NO, "the verifier string to validate"},
        },
        RPCResult{
            RPCResult::Type::BOOL, "", "true if the verifier string is valid"
        },
        RPCExamples{
            HelpExampleCli("isvalidverifierstring", "\"#TAG & #TAG2\"")
          + HelpExampleRpc("isvalidverifierstring", "\"#TAG & #TAG2\"")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            std::string verifier_string = request.params[0].get_str();

            LOCK(cs_main);

            if (!passets)
                throw JSONRPCError(RPC_INTERNAL_ERROR, "asset cache unavailable.");

            std::string strError;
            bool result = ContextualCheckVerifierString(passets, verifier_string, "", strError);

            return result;
        },
    };
}

static RPCHelpMan getsnapshot()
{
    return RPCHelpMan{
        "getsnapshot",
        "Returns an ownership snapshot of an asset at the given block height.\n",
        {
            {"asset_name", RPCArg::Type::STR, RPCArg::Optional::NO, "the name of the asset"},
            {"block_height", RPCArg::Type::NUM, RPCArg::Optional::NO, "the block height of the snapshot"},
        },
        RPCResult{
            RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::STR, "name", "the asset name"},
                {RPCResult::Type::NUM, "height", "the block height of the snapshot"},
                {RPCResult::Type::ARR, "owners", "",
                {
                    {RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR, "address", "owner address"},
                        {RPCResult::Type::NUM, "amount_owned", "amount held"},
                    }},
                }},
            }
        },
        RPCExamples{
            HelpExampleCli("getsnapshot", "\"ASSET_NAME\" 100")
          + HelpExampleRpc("getsnapshot", "\"ASSET_NAME\", 100")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            if (!pAssetSnapshotDb)
                throw JSONRPCError(RPC_INTERNAL_ERROR, "snapshot db unavailable.");

            std::string asset_name = request.params[0].get_str();
            int block_height = request.params[1].getInt<int>();

            CAssetSnapshotDBEntry snapshotEntry;
            if (!pAssetSnapshotDb->RetrieveOwnershipSnapshot(asset_name, block_height, snapshotEntry))
                throw JSONRPCError(RPC_INVALID_PARAMETER, "No snapshot found for asset '" + asset_name + "' at height " + std::to_string(block_height));

            UniValue result(UniValue::VOBJ);
            result.pushKV("name", snapshotEntry.assetName);
            result.pushKV("height", snapshotEntry.height);

            UniValue owners(UniValue::VARR);
            for (const auto& [address, amount] : snapshotEntry.ownersAndAmounts) {
                UniValue entry(UniValue::VOBJ);
                entry.pushKV("address", address);
                entry.pushKV("amount_owned", AssetUnitValueFromAmount(amount, asset_name));
                owners.push_back(entry);
            }
            result.pushKV("owners", owners);

            return result;
        },
    };
}

static RPCHelpMan purgesnapshot()
{
    return RPCHelpMan{
        "purgesnapshot",
        "Purges an ownership snapshot of an asset at the given block height.\n",
        {
            {"asset_name", RPCArg::Type::STR, RPCArg::Optional::NO, "the name of the asset"},
            {"block_height", RPCArg::Type::NUM, RPCArg::Optional::NO, "the block height of the snapshot to purge"},
        },
        RPCResult{
            RPCResult::Type::BOOL, "", "true if the snapshot was purged"
        },
        RPCExamples{
            HelpExampleCli("purgesnapshot", "\"ASSET_NAME\" 100")
          + HelpExampleRpc("purgesnapshot", "\"ASSET_NAME\", 100")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            if (!pAssetSnapshotDb)
                throw JSONRPCError(RPC_INTERNAL_ERROR, "snapshot db unavailable.");

            std::string asset_name = request.params[0].get_str();
            int block_height = request.params[1].getInt<int>();

            if (!pAssetSnapshotDb->RemoveOwnershipSnapshot(asset_name, block_height))
                throw JSONRPCError(RPC_INTERNAL_ERROR, "Failed to purge snapshot for asset '" + asset_name + "' at height " + std::to_string(block_height));

            return true;
        },
    };
}

static RPCHelpMan ansencode()
{
    return RPCHelpMan{
        "ansencode",
        "Encodes type and data into an ANS (Avian Name System) ID string.\n",
        {
            {"type", RPCArg::Type::STR, RPCArg::Optional::NO, "the ANS type: \"ADDR\" or \"IP\""},
            {"data", RPCArg::Type::STR, RPCArg::Optional::NO, "the data for the ANS record (Avian address or IP address)"},
        },
        RPCResult{
            RPCResult::Type::STR, "", "the encoded ANS ID string"
        },
        RPCExamples{
            HelpExampleCli("ansencode", "\"ADDR\" \"RXissueAssetXXXXXXXXXXXXXXXXZFGHWo\"")
          + HelpExampleCli("ansencode", "\"IP\" \"127.0.0.1\"")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            std::string type_str = request.params[0].get_str();
            std::string data = request.params[1].get_str();

            CAvianNameSystemID::Type type;
            if (type_str == "ADDR" || type_str == "addr" || type_str == "0")
                type = CAvianNameSystemID::ADDR;
            else if (type_str == "IP" || type_str == "ip" || type_str == "1")
                type = CAvianNameSystemID::IP;
            else
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid ANS type. Must be \"ADDR\" or \"IP\".");

            std::string error;
            std::string formatted = CAvianNameSystemID::FormatTypeData(type, data, error);
            if (!error.empty())
                throw JSONRPCError(RPC_INVALID_PARAMETER, error);

            CAvianNameSystemID ansID(type, formatted);
            return ansID.to_string();
        },
    };
}

static RPCHelpMan ansdecode()
{
    return RPCHelpMan{
        "ansdecode",
        "Decodes an ANS (Avian Name System) ID string and returns its components.\n",
        {
            {"ans_id", RPCArg::Type::STR, RPCArg::Optional::NO, "the ANS ID string to decode"},
        },
        RPCResult{
            RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::STR, "id", "the ANS ID string"},
                {RPCResult::Type::NUM, "type", "the ANS type number"},
                {RPCResult::Type::STR, "type_name", "the ANS type description"},
                {RPCResult::Type::STR, "address", /*optional=*/true, "the Avian address (if type is ADDR)"},
                {RPCResult::Type::STR, "ip", /*optional=*/true, "the IP address (if type is IP)"},
            }
        },
        RPCExamples{
            HelpExampleCli("ansdecode", "\"ANS0RXissueAssetXXXXXXXXXXXXXXXXZFGHWo\"")
          + HelpExampleRpc("ansdecode", "\"ANS0RXissueAssetXXXXXXXXXXXXXXXXZFGHWo\"")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            std::string ans_id = request.params[0].get_str();

            if (!CAvianNameSystemID::IsValidID(ans_id))
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid ANS ID: " + ans_id);

            CAvianNameSystemID ansID(ans_id);
            return ANSIDToObject(ansID);
        },
    };
}

void RegisterAssetRPCCommands(CRPCTable& t)
{
    static const CRPCCommand commands[]{
        {"assets", &listassets},
        {"assets", &getassetdata},
        {"assets", &getcacheinfo},
        {"assets", &listassetbalancesbyaddress},
        {"assets", &listaddressesbyasset},
        {"assets", &getansdata},
        {"assets", &getsnapshot},
        {"assets", &purgesnapshot},
        {"assets", &ansencode},
        {"assets", &ansdecode},
        {"restricted assets", &checkaddressrestriction},
        {"restricted assets", &checkglobalrestriction},
        {"restricted assets", &checkaddresstag},
        {"restricted assets", &listtagsforaddress},
        {"restricted assets", &listaddressesfortag},
        {"restricted assets", &listaddressrestrictions},
        {"restricted assets", &listglobalrestrictions},
        {"restricted assets", &getverifierstring},
        {"restricted assets", &isvalidverifierstring},
    };
    for (const auto& c : commands) {
        t.appendCommand(c.name, &c);
    }
}
