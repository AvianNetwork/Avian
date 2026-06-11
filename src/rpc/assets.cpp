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
#include <assets/cbor.h>
#include <assets/assetsnapshotdb.h>
#include <assets/snapshotrequestdb.h>
#include <key_io.h>
#include <util/strencodings.h>

#include <univalue.h>
#include <limits>

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
                        {RPCResult::Type::NUM, "has_ans", "1 if has ANS data"},
                        {RPCResult::Type::STR, "ans_id", /*optional=*/true, "the ANS ID (only if has_ans = 1)"},
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

            // Also include assets that are in the in-memory cache but not yet flushed to disk.
            // This mirrors the pattern used by listaddressesbyasset.
            if (passets) {
                LOCK(cs_main);
                std::set<std::string> inDbNames;
                for (const auto& d : assets)
                    inDbNames.insert(d.asset.strName);

                bool wc = !filter.empty() && filter.back() == '*';
                const std::string pfx = wc ? filter.substr(0, filter.size() - 1) : filter;

                for (const auto& entry : passets->setNewAssetsToAdd) {
                    const std::string& name = entry.asset.strName;
                    if (inDbNames.count(name))
                        continue;
                    bool matches = pfx.empty() ||
                                   (wc  && name.substr(0, pfx.size()) == pfx) ||
                                   (!wc && name == pfx);
                    if (matches)
                        assets.emplace_back(entry.asset, entry.blockHeight, entry.blockHash);
                }
            }

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
                    detail.pushKV("has_ans", asset.nHasANS);
                    detail.pushKV("block_height", data.nHeight);
                    detail.pushKV("blockhash", data.blockHash.GetHex());
                    if (asset.nHasIPFS) {
                        if (asset.strIPFSHash.size() == 32) {
                            detail.pushKV("txid_hash", EncodeAssetData(asset.strIPFSHash));
                        } else {
                            detail.pushKV("ipfs_hash", EncodeAssetData(asset.strIPFSHash));
                        }
                    }
                    if (asset.nHasANS)
                        detail.pushKV("ans_id", asset.strANSID);
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
                {RPCResult::Type::NUM, "has_ans", "1 if has ANS data"},
                {RPCResult::Type::STR, "ans_id", /*optional=*/true, "the ANS ID (only if has_ans = 1)"},
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

                result.pushKV("has_ans", asset.nHasANS);
                if (asset.nHasANS)
                    result.pushKV("ans_id", asset.strANSID);

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

            // Build combined map from DB + unsaved in-memory dirty entries.
            std::map<std::string, CAmount> combined;
            if (passetsdb) {
                std::vector<std::pair<std::string, CAmount>> vecDB;
                int dbTotal = 0;
                if (!passetsdb->AddressDir(vecDB, dbTotal, false, address, std::numeric_limits<size_t>::max(), 0))
                    throw JSONRPCError(RPC_INTERNAL_ERROR, "Failed to query address asset database");
                for (const auto& [assetName, amt] : vecDB)
                    combined[assetName] = amt;
            }
            // Overlay unsaved in-memory dirty entries
            for (const auto& [pair, amount] : passets->mapAssetsAddressAmount) {
                if (pair.second == address)
                    combined[pair.first] = amount;
            }

            if (onlytotal) {
                int nTotal = 0;
                for (const auto& [assetName, amt] : combined) {
                    if (amt > 0) nTotal++;
                }
                result.pushKV("total", nTotal);
            } else {
                size_t found = 0;
                long skipped = 0;
                for (const auto& [assetName, amt] : combined) {
                    if (amt > 0) {
                        if (skipped < start) {
                            skipped++;
                            continue;
                        }
                        result.pushKV(assetName, AssetUnitValueFromAmount(amt, assetName));
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
            // Owner tokens (ending with '!') have no CNewAsset metadata entry; verify via the base asset instead
            if (IsAssetNameAnOwner(assetName)) {
                std::string baseName = assetName.substr(0, assetName.size() - 1);
                CNewAsset baseAsset;
                if (!passets->GetAssetMetaDataIfExists(baseName, baseAsset))
                    throw JSONRPCError(RPC_INVALID_PARAMETER, "Asset not found: " + assetName);
            } else {
                CNewAsset asset;
                if (!passets->GetAssetMetaDataIfExists(assetName, asset))
                    throw JSONRPCError(RPC_INVALID_PARAMETER, "Asset not found: " + assetName);
            }

            // Build the complete address->amount map by combining persisted DB data with any
            // unsaved in-memory changes.
            std::map<std::string, CAmount> combined;
            if (passetsdb) {
                std::vector<std::pair<std::string, CAmount>> vecDB;
                int dbTotal = 0;
                if (!passetsdb->AssetAddressDir(vecDB, dbTotal, false, assetName, std::numeric_limits<size_t>::max(), 0))
                    throw JSONRPCError(RPC_INTERNAL_ERROR, "Failed to query asset address database");
                for (const auto& [addr, amt] : vecDB)
                    combined[addr] = amt;
            }
            // Overlay unsaved in-memory dirty entries (may add new addresses or update balances)
            for (const auto& [pair, amount] : passets->mapAssetsAddressAmount) {
                if (pair.first == assetName)
                    combined[pair.second] = amount;
            }

            if (onlytotal) {
                int nTotal = 0;
                for (const auto& [addr, amt] : combined) {
                    if (amt > 0) nTotal++;
                }
                result.pushKV("total", nTotal);
            } else {
                size_t found = 0;
                long skipped = 0;
                for (const auto& [addr, amt] : combined) {
                    if (amt > 0) {
                        if (skipped < start) {
                            skipped++;
                            continue;
                        }
                        result.pushKV(addr, AssetUnitValueFromAmount(amt, assetName));
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
    if (ansID.type() == CAvianNameSystemID::PROFILE) {
        const ANSProfileData& p = ansID.profile();
        if (!p.addr.empty())   obj.pushKV("address",      p.addr);
        if (!p.name.empty())   obj.pushKV("display_name", p.name);
        if (!p.avatar.empty()) obj.pushKV("avatar",       p.avatar_binary
                                            ? HexStr(p.avatar)  // binary: return as hex
                                            : p.avatar);
        if (!p.banner.empty()) obj.pushKV("banner",      p.banner_binary
                                            ? HexStr(p.banner)
                                            : p.banner);
        if (!p.url.empty())    obj.pushKV("url",          p.url);
    }
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
                {RPCResult::Type::STR, "id",           "the ANS ID string"},
                {RPCResult::Type::NUM, "type",         "the ANS type number"},
                {RPCResult::Type::STR, "type_name",    "the ANS type description"},
                {RPCResult::Type::STR, "address",      /*optional=*/true, "the Avian address (ADDR type, or PROFILE key 0)"},
                {RPCResult::Type::STR, "display_name", /*optional=*/true, "display name (PROFILE key 1)"},
                {RPCResult::Type::STR, "avatar",       /*optional=*/true, "avatar URL/CID or hex image bytes (PROFILE key 2)"},
                {RPCResult::Type::STR, "banner",       /*optional=*/true, "banner URL/CID or hex image bytes (PROFILE key 4)"},
                {RPCResult::Type::STR, "url",          /*optional=*/true, "website URL (PROFILE key 3)"},
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

static RPCHelpMan resolveavn()
{
    return RPCHelpMan{
        "resolveavn",
        "Resolves an AVN name to an address.\n"
        "If 'coin' is omitted, resolves to an Avian address using ANS ADDR/PROFILE record or owner-token fallback.\n"
        "If 'coin' is provided (e.g. \"BTC\"), resolves to an external address stored in the sub-asset NAME.AVN/COIN (AIP-0010).\n",
        {
            {"name", RPCArg::Type::STR, RPCArg::Optional::NO, "the AVN name to resolve (e.g. \"ALICE\" or \"ALICE.AVN\")"},
            {"coin", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "optional coin ticker for cross-chain lookup (e.g. \"BTC\", \"MEWC\")"},
        },
        RPCResult{
            RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::STR, "name",         "the resolved asset name"},
                {RPCResult::Type::STR, "address",      "the resolved address"},
                {RPCResult::Type::STR, "source",       "resolution source: \"ans_record\", \"ans_xaddr\", \"ans_profile\", or \"owner_token\""},
                {RPCResult::Type::STR, "display_name", /*optional=*/true, "display name from ANS PROFILE (ans_profile only)"},
                {RPCResult::Type::STR, "avatar",       /*optional=*/true, "avatar URL/CID from ANS PROFILE (ans_profile only)"},
                {RPCResult::Type::STR, "banner",       /*optional=*/true, "banner URL/CID from ANS PROFILE (ans_profile only)"},
                {RPCResult::Type::STR, "url",          /*optional=*/true, "website URL from ANS PROFILE (ans_profile only)"},
            }
        },
        RPCExamples{
            HelpExampleCli("resolveavn", "\"ALICE\"")
          + HelpExampleCli("resolveavn", "\"BOB\" \"BTC\"")
          + HelpExampleRpc("resolveavn", "\"ALICE\"")
          + HelpExampleRpc("resolveavn", "\"BOB\", \"MEWC\"")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            std::string name = request.params[0].get_str();

            // Normalise to uppercase and strip ".AVN" suffix if present
            for (auto& c : name) c = toupper(c);
            if (name.size() > 4 && name.substr(name.size() - 4) == ".AVN")
                name = name.substr(0, name.size() - 4);

            if (name.empty())
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Name must not be empty.");

            LOCK(cs_main);

            if (!passets)
                throw JSONRPCError(RPC_INTERNAL_ERROR, "asset cache unavailable.");

            // AIP-0010: cross-chain resolution via sub-asset NAME.AVN/COIN
            if (!request.params[1].isNull()) {
                std::string coin = request.params[1].get_str();
                for (auto& c : coin) c = toupper(c);
                if (coin.empty())
                    throw JSONRPCError(RPC_INVALID_PARAMETER, "Coin ticker must not be empty.");

                std::string subAssetName = name + CAvianNameSystemID::domain + "/" + coin; // "NAME.AVN/COIN"

                CNewAsset subAsset;
                if (!passets->GetAssetMetaDataIfExists(subAssetName, subAsset))
                    throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY,
                        "Cross-chain sub-asset not found: " + subAssetName);

                if (!subAsset.nHasANS || subAsset.strANSID.empty())
                    throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY,
                        "Sub-asset has no ANS record: " + subAssetName);

                CAvianNameSystemID ansID(subAsset.strANSID);
                if (ansID.type() != CAvianNameSystemID::XADDR || ansID.addr().empty())
                    throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY,
                        "Sub-asset ANS record is not an external address (XADDR): " + subAssetName);

                UniValue result(UniValue::VOBJ);
                result.pushKV("name",    subAssetName);
                result.pushKV("address", ansID.addr());
                result.pushKV("source",  "ans_xaddr");
                return result;
            }

            std::string assetName = name + CAvianNameSystemID::domain; // "<NAME>.AVN"

            CNewAsset asset;
            if (!passets->GetAssetMetaDataIfExists(assetName, asset))
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "AVN name not found: " + assetName);

            // Tier 1a/1b: ANS record lookup
            if (asset.nHasANS && !asset.strANSID.empty()) {
                CAvianNameSystemID ansID(asset.strANSID);
                if (ansID.type() == CAvianNameSystemID::ADDR && !ansID.addr().empty()) {
                    UniValue result(UniValue::VOBJ);
                    result.pushKV("name", assetName);
                    result.pushKV("address", ansID.addr());
                    result.pushKV("source", "ans_record");
                    return result;
                }
                // Tier 1b: ANS PROFILE record — use the addr field (CBOR key 0) if present
                if (ansID.type() == CAvianNameSystemID::PROFILE && !ansID.profile().addr.empty()) {
                    const ANSProfileData& pd = ansID.profile();
                    UniValue result(UniValue::VOBJ);
                    result.pushKV("name",    assetName);
                    result.pushKV("address", pd.addr);
                    result.pushKV("source",  "ans_profile");
                    if (!pd.name.empty())   result.pushKV("display_name", pd.name);
                    if (!pd.avatar.empty()) result.pushKV("avatar", pd.avatar_binary ? HexStr(pd.avatar) : pd.avatar);
                    if (!pd.banner.empty()) result.pushKV("banner", pd.banner_binary ? HexStr(pd.banner) : pd.banner);
                    if (!pd.url.empty())    result.pushKV("url", pd.url);
                    return result;
                }
            }

            // Tier 2: owner token fallback (requires assetindex)
            if (!fAssetIndex)
                throw JSONRPCError(RPC_MISC_ERROR,
                    "Enable -assetindex for owner token fallback resolution of '" + assetName + "'.");

            if (!passetsdb)
                throw JSONRPCError(RPC_INTERNAL_ERROR, "asset db unavailable.");

            std::string ownerToken = assetName + "!";
            std::vector<std::pair<std::string, CAmount>> ownerAddrs;
            int dbTotal = 0;
            if (!passetsdb->AssetAddressDir(ownerAddrs, dbTotal, false, ownerToken, 1, 0) || ownerAddrs.empty())
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY,
                    "No ANS record and no owner found for: " + assetName);

            UniValue result(UniValue::VOBJ);
            result.pushKV("name", assetName);
            result.pushKV("address", ownerAddrs[0].first);
            result.pushKV("source", "owner_token");
            return result;
        },
    };
}

static RPCHelpMan ansencode()
{
    return RPCHelpMan{
        "ansencode",
        "Encodes type and data into an ANS (Avian Name System) ID string.\n"
        "For PROFILE type, 'data' can be a JSON object with named fields or raw hex-encoded CBOR bytes.\n"
        "JSON fields: \"name\" (display name), \"addr\" (Avian address), \"avatar\" (URL/CID), \"url\" (website).\n",
        {
            {"type", RPCArg::Type::STR, RPCArg::Optional::NO, "the ANS type: \"ADDR\" or \"PROFILE\""},
            {"data", RPCArg::Type::STR, RPCArg::Optional::NO,
                "for ADDR: an Avian address; "
                "for PROFILE: a JSON object e.g. {\"name\":\"Bob\",\"avatar\":\"ipfs://Qm...\"} "
                "or raw hex-encoded CBOR bytes"},
        },
        RPCResult{
            RPCResult::Type::STR, "", "the encoded ANS ID string"
        },
        RPCExamples{
            HelpExampleCli("ansencode", "\"ADDR\" \"RXissueAssetXXXXXXXXXXXXXXXXZFGHWo\"")
          + HelpExampleCli("ansencode", "\"PROFILE\" \"{\\\"name\\\":\\\"Bob\\\",\\\"avatar\\\":\\\"ipfs://QmTest\\\"}\"")
          + HelpExampleCli("ansencode", "\"PROFILE\" \"{\\\"name\\\":\\\"Bob\\\",\\\"addr\\\":\\\"RXissueAssetXXXXXXXXXXXXXXXXZFGHWo\\\",\\\"url\\\":\\\"https://example.com\\\"}\"")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            std::string type_str = request.params[0].get_str();
            std::string data     = request.params[1].get_str();

            CAvianNameSystemID::Type type;
            if (type_str == "ADDR" || type_str == "addr" || type_str == "0")
                type = CAvianNameSystemID::ADDR;
            else if (type_str == "XADDR" || type_str == "xaddr" || type_str == "1")
                type = CAvianNameSystemID::XADDR;
            else if (type_str == "PROFILE" || type_str == "profile" || type_str == "2")
                type = CAvianNameSystemID::PROFILE;
            else
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid ANS type. Supported types: \"ADDR\" (0), \"XADDR\" (1), \"PROFILE\" (2).");

            if (type == CAvianNameSystemID::PROFILE) {
                std::string trimmed = data;
                trimmed.erase(0, trimmed.find_first_not_of(" \t\n\r"));

                if (!trimmed.empty() && trimmed[0] == '{') {
                    // JSON object input: build CBOR from named fields
                    UniValue profileJson;
                    if (!profileJson.read(trimmed))
                        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid JSON for PROFILE data. Expected object with fields: name, addr, avatar, url.");
                    if (!profileJson.isObject())
                        throw JSONRPCError(RPC_INVALID_PARAMETER, "PROFILE JSON data must be an object.");

                    ANSProfileData p;
                    if (profileJson.exists("name"))   p.name   = profileJson["name"].get_str();
                    if (profileJson.exists("addr"))   p.addr   = profileJson["addr"].get_str();
                    if (profileJson.exists("avatar")) { p.avatar = profileJson["avatar"].get_str(); p.avatar_binary = false; }
                    if (profileJson.exists("url"))    p.url    = profileJson["url"].get_str();

                    if (p.name.empty() && p.addr.empty() && p.avatar.empty() && p.url.empty())
                        throw JSONRPCError(RPC_INVALID_PARAMETER, "PROFILE JSON must have at least one of: name, addr, avatar, url.");

                    data = ANS_CBOR::EncodeProfile(p);
                } else {
                    // Hex CBOR input (power user / programmatic)
                    if (!IsHex(data))
                        throw JSONRPCError(RPC_INVALID_PARAMETER, "For PROFILE type, data must be a JSON object or hex-encoded CBOR bytes.");
                    auto bytes = ParseHex(data);
                    data = std::string(bytes.begin(), bytes.end());
                }
            }

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
        "Decodes an ANS (Avian Name System) ID string and returns its components.\n"
        "For PROFILE records, CBOR binary fields (avatar) are returned as hex.\n",
        {
            {"ans_id", RPCArg::Type::STR, RPCArg::Optional::NO, "the ANS ID string to decode"},
        },
        RPCResult{
            RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::STR, "id",           "the ANS ID string"},
                {RPCResult::Type::NUM, "type",         "the ANS type number"},
                {RPCResult::Type::STR, "type_name",    "the ANS type description"},
                {RPCResult::Type::STR, "address",      /*optional=*/true, "the Avian address (ADDR type, or PROFILE key 0)"},
                {RPCResult::Type::STR, "display_name", /*optional=*/true, "display name (PROFILE key 1)"},
                {RPCResult::Type::STR, "avatar",       /*optional=*/true, "avatar URL/CID or hex image bytes (PROFILE key 2)"},
                {RPCResult::Type::STR, "banner",       /*optional=*/true, "banner URL/CID or hex image bytes (PROFILE key 4)"},
                {RPCResult::Type::STR, "url",          /*optional=*/true, "website URL (PROFILE key 3)"},
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

static RPCHelpMan whoisavn()
{
    return RPCHelpMan{
        "whoisavn",
        "Reverse ANS lookup: given an Avian address, returns which ANS names are owned at,\n"
        "registered to, or held as tokens at that address.\n"
        "Requires -assetindex.\n",
        {
            {"address", RPCArg::Type::STR, RPCArg::Optional::NO, "the Avian address to look up"},
        },
        RPCResult{
            RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::STR, "address",       "the queried address"},
                {RPCResult::Type::ARR, "owner_of",      "ANS names whose owner token (X.AVN!) is held at this address",
                    {{RPCResult::Type::STR, "", "ANS name"}}},
                {RPCResult::Type::ARR, "registered_as", "ANS names whose ANS record points to this address (bech32/legacy normalised)",
                    {{RPCResult::Type::STR, "", "ANS name"}}},
                {RPCResult::Type::ARR, "holds",         "ANS names whose regular token (X.AVN) is held at this address",
                    {{RPCResult::Type::STR, "", "ANS name"}}},
            }
        },
        RPCExamples{
            HelpExampleCli("whoisavn", "\"RXissueAssetXXXXXXXXXXXXXXXXXhhZGt\"")
          + HelpExampleRpc("whoisavn", "\"RXissueAssetXXXXXXXXXXXXXXXXXhhZGt\"")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            if (!fAssetIndex)
                throw JSONRPCError(RPC_MISC_ERROR, "whoisavn requires -assetindex.");

            if (!passetsdb || !passets)
                throw JSONRPCError(RPC_INTERNAL_ERROR, "asset db unavailable.");

            std::string address = request.params[0].get_str();

            LOCK(cs_main);

            const std::string& domain = CAvianNameSystemID::domain; // ".AVN"

            // Normalise an address string to its underlying hash160 for cross-format
            // comparison (P2PKH legacy ↔ P2WPKH bech32 share the same hash160).
            auto addrHash = [](const std::string& addr) -> uint160 {
                CTxDestination dest = DecodeDestination(addr);
                if (auto* p = std::get_if<PKHash>(&dest))           return uint160(*p);
                if (auto* p = std::get_if<WitnessV0KeyHash>(&dest)) return uint160(*p);
                return uint160();
            };
            const uint160 queryHash = addrHash(address);

            // Build the combined balance map once; shared by owner_of and holds.
            std::map<std::string, CAmount> combined;
            {
                std::vector<std::pair<std::string, CAmount>> vecDB;
                int dbTotal = 0;
                passetsdb->AddressDir(vecDB, dbTotal, false, address, std::numeric_limits<size_t>::max(), 0);
                for (const auto& [name, amt] : vecDB) combined[name] = amt;
                for (const auto& [pair, amt] : passets->mapAssetsAddressAmount)
                    if (pair.second == address) combined[pair.first] = amt;
            }

            // --- owner_of: X.AVN! owner tokens at this address ---
            UniValue ownerOf(UniValue::VARR);
            for (const auto& [name, amt] : combined) {
                if (amt <= 0) continue;
                if (name.size() > domain.size() + 1 && name.back() == '!') {
                    std::string base = name.substr(0, name.size() - 1);
                    if (base.size() > domain.size() &&
                        base.substr(base.size() - domain.size()) == domain)
                        ownerOf.push_back(base);
                }
            }

            // --- registered_as: .AVN assets whose ANS addr matches (hash160-normalised) ---
            UniValue registeredAs(UniValue::VARR);
            {
                std::vector<CDatabasedAssetData> ansAssets;
                passetsdb->AssetDir(ansAssets, "*", std::numeric_limits<size_t>::max(), 0);
                for (const auto& data : ansAssets) {
                    const CNewAsset& a = data.asset;
                    if (a.strName.size() <= domain.size() ||
                        a.strName.substr(a.strName.size() - domain.size()) != domain)
                        continue;
                    if (!a.nHasANS || a.strANSID.empty()) continue;
                    if (!CAvianNameSystemID::IsValidID(a.strANSID)) continue;
                    CAvianNameSystemID ans(a.strANSID);
                    std::string ansAddr;
                    if (ans.type() == CAvianNameSystemID::ADDR)
                        ansAddr = ans.addr();
                    else if (ans.type() == CAvianNameSystemID::PROFILE)
                        ansAddr = ans.profile().addr;
                    if (!ansAddr.empty() && !queryHash.IsNull() &&
                        addrHash(ansAddr) == queryHash)
                        registeredAs.push_back(a.strName);
                }
            }

            // --- holds: regular X.AVN tokens (not owner tokens) at this address ---
            UniValue holds(UniValue::VARR);
            for (const auto& [name, amt] : combined) {
                if (amt <= 0) continue;
                if (name.back() == '!') continue; // skip owner tokens
                if (name.size() > domain.size() &&
                    name.substr(name.size() - domain.size()) == domain)
                    holds.push_back(name);
            }

            UniValue result(UniValue::VOBJ);
            result.pushKV("address",       address);
            result.pushKV("owner_of",      ownerOf);
            result.pushKV("registered_as", registeredAs);
            result.pushKV("holds",         holds);
            return result;
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
        {"assets", &getsnapshot},
        {"assets", &purgesnapshot},
        {"avian name system", &getansdata},
        {"avian name system", &resolveavn},
        {"avian name system", &whoisavn},
        {"avian name system", &ansencode},
        {"avian name system", &ansdecode},
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
