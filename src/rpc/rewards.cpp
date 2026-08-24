// Copyright (c) 2019-2020 The Raven Core developers
// Copyright (c) 2020-2024 The Avian developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <util/string.h>
#include <rpc/server.h>
#include <rpc/server_util.h>
#include <rpc/util.h>

#include <assets/assets.h>
#include <assets/assetdb.h>
#include <assets/rewards.h>
#include <assets/snapshotrequestdb.h>
#include <assets/assetsnapshotdb.h>
#include <common/args.h>
#include <core_io.h>
#include <key_io.h>
#include <node/context.h>
#include <validation.h>

#include <univalue.h>

using node::NodeContext;

extern CSnapshotRequestDB* pSnapshotRequestDb;
extern CAssetSnapshotDB* pAssetSnapshotDb;
extern CDistributeSnapshotRequestDB* pDistributeSnapshotDb;

static RPCHelpMan requestsnapshot()
{
    return RPCHelpMan{
        "requestsnapshot",
        "Schedules a snapshot of the specified asset at the specified block height.\n",
        {
            {"asset_name", RPCArg::Type::STR, RPCArg::Optional::NO, "the asset name for which the snapshot will be taken"},
            {"block_height", RPCArg::Type::NUM, RPCArg::Optional::NO, "the block height at which the snapshot will be taken"},
        },
        RPCResult{
            RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::STR, "request_status", "status of the request (e.g. \"Added\")"},
            }
        },
        RPCExamples{
            HelpExampleCli("requestsnapshot", "\"ASSET_NAME\" 12345")
          + HelpExampleRpc("requestsnapshot", "\"ASSET_NAME\", 12345")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            if (!fAssetIndex)
                throw JSONRPCError(RPC_INVALID_REQUEST, "This RPC call is not functional unless -assetindex is enabled. To enable, please run the wallet with -assetindex, this will require a reindex to occur");

            std::string asset_name = request.params[0].get_str();
            int block_height = request.params[1].getInt<int>();

            AssetType ownershipAssetType;
            std::string assetError;
            if (!IsAssetNameValid(asset_name, ownershipAssetType, assetError))
                throw JSONRPCError(RPC_INVALID_PARAMETER, std::string("Invalid asset_name: Please use a valid asset name"));

            if (ownershipAssetType == AssetType::UNIQUE || ownershipAssetType == AssetType::OWNER || ownershipAssetType == AssetType::MSGCHANNEL)
                throw JSONRPCError(RPC_INVALID_PARAMETER, std::string("Invalid asset_name: OWNER, UNIQUE, MSGCHANNEL assets are not allowed for this call"));

            if (!passets)
                throw JSONRPCError(RPC_DATABASE_ERROR, std::string("Asset cache not setup. Please restart wallet to try again"));

            CNewAsset asset;
            if (!passets->GetAssetMetaDataIfExists(asset_name, asset))
                throw JSONRPCError(RPC_INVALID_PARAMETER, std::string("Invalid asset_name: asset does not exist."));

            NodeContext& node = EnsureAnyNodeContext(request.context);
            ChainstateManager& chainman = EnsureChainman(node);
            int nHeight;
            {
                LOCK(cs_main);
                nHeight = chainman.ActiveChain().Height();
            }

            if (block_height <= nHeight)
                throw JSONRPCError(RPC_INVALID_PARAMETER, std::string("Invalid block_height: block height should be greater than current active chain height"));

            if (!pSnapshotRequestDb)
                throw JSONRPCError(RPC_DATABASE_ERROR, std::string("Snapshot Request database is not setup. Please restart wallet to try again"));

            if (pSnapshotRequestDb->ScheduleSnapshot(asset_name, block_height)) {
                UniValue obj(UniValue::VOBJ);
                obj.pushKV("request_status", "Added");
                return obj;
            }

            throw JSONRPCError(RPC_MISC_ERROR, std::string("Failed to add requested snapshot to database"));
        },
    };
}

static RPCHelpMan getsnapshotrequest()
{
    return RPCHelpMan{
        "getsnapshotrequest",
        "Retrieves the specified snapshot request details.\n",
        {
            {"asset_name", RPCArg::Type::STR, RPCArg::Optional::NO, "the asset name for which the snapshot will be taken"},
            {"block_height", RPCArg::Type::NUM, RPCArg::Optional::NO, "the block height at which the snapshot will be taken"},
        },
        RPCResult{
            RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::STR, "asset_name", "the asset name"},
                {RPCResult::Type::NUM, "block_height", "the block height of the snapshot"},
            }
        },
        RPCExamples{
            HelpExampleCli("getsnapshotrequest", "\"ASSET_NAME\" 12345")
          + HelpExampleRpc("getsnapshotrequest", "\"ASSET_NAME\", 12345")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            if (!fAssetIndex)
                throw JSONRPCError(RPC_INVALID_REQUEST, "This RPC call is not functional unless -assetindex is enabled. To enable, please run the wallet with -assetindex, this will require a reindex to occur");

            std::string asset_name = request.params[0].get_str();
            int block_height = request.params[1].getInt<int>();

            if (!pSnapshotRequestDb)
                throw JSONRPCError(RPC_DATABASE_ERROR, std::string("Snapshot Request database is not setup. Please restart wallet to try again"));

            CSnapshotRequestDBEntry snapshotRequest;
            if (pSnapshotRequestDb->RetrieveSnapshotRequest(asset_name, block_height, snapshotRequest)) {
                UniValue obj(UniValue::VOBJ);
                obj.pushKV("asset_name", snapshotRequest.assetName);
                obj.pushKV("block_height", snapshotRequest.heightForSnapshot);
                return obj;
            }

            throw JSONRPCError(RPC_MISC_ERROR, std::string("Failed to retrieve specified snapshot request"));
        },
    };
}

static RPCHelpMan listsnapshotrequests()
{
    return RPCHelpMan{
        "listsnapshotrequests",
        "List snapshot request details.\n",
        {
            {"asset_name", RPCArg::Type::STR, RPCArg::Default{""}, "list only requests for a specific asset (default is all)"},
            {"block_height", RPCArg::Type::NUM, RPCArg::Default{0}, "list only requests for a particular block height (default is all)"},
        },
        RPCResult{
            RPCResult::Type::ARR, "", "",
            {
                {RPCResult::Type::OBJ, "", "",
                {
                    {RPCResult::Type::STR, "asset_name", "the asset name"},
                    {RPCResult::Type::NUM, "block_height", "the block height of the snapshot"},
                }},
            }
        },
        RPCExamples{
            HelpExampleCli("listsnapshotrequests", "")
          + HelpExampleCli("listsnapshotrequests", "\"ASSET_NAME\"")
          + HelpExampleRpc("listsnapshotrequests", "\"ASSET_NAME\", 345333")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            if (!fAssetIndex)
                throw JSONRPCError(RPC_INVALID_REQUEST, "This RPC call is not functional unless -assetindex is enabled. To enable, please run the wallet with -assetindex, this will require a reindex to occur");

            std::string asset_name;
            int block_height = 0;
            if (!request.params[0].isNull())
                asset_name = request.params[0].get_str();
            if (!request.params[1].isNull())
                block_height = request.params[1].getInt<int>();

            if (!pSnapshotRequestDb)
                throw JSONRPCError(RPC_DATABASE_ERROR, std::string("Snapshot Request database is not setup. Please restart wallet to try again"));

            UniValue result(UniValue::VARR);
            std::set<CSnapshotRequestDBEntry> entries;
            if (pSnapshotRequestDb->RetrieveSnapshotRequestsForHeight(asset_name, block_height, entries)) {
                for (const auto& entry : entries) {
                    UniValue item(UniValue::VOBJ);
                    item.pushKV("asset_name", entry.assetName);
                    item.pushKV("block_height", entry.heightForSnapshot);
                    result.push_back(item);
                }
                return result;
            }

            return UniValue::VNULL;
        },
    };
}

static RPCHelpMan cancelsnapshotrequest()
{
    return RPCHelpMan{
        "cancelsnapshotrequest",
        "Cancels the specified snapshot request.\n",
        {
            {"asset_name", RPCArg::Type::STR, RPCArg::Optional::NO, "the asset name for which the snapshot request will be cancelled"},
            {"block_height", RPCArg::Type::NUM, RPCArg::Optional::NO, "the block height at which the snapshot was scheduled"},
        },
        RPCResult{
            RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::STR, "request_status", "status of the request (e.g. \"Removed\")"},
            }
        },
        RPCExamples{
            HelpExampleCli("cancelsnapshotrequest", "\"ASSET_NAME\" 12345")
          + HelpExampleRpc("cancelsnapshotrequest", "\"ASSET_NAME\", 12345")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            if (!fAssetIndex)
                throw JSONRPCError(RPC_INVALID_REQUEST, "This RPC call is not functional unless -assetindex is enabled. To enable, please run the wallet with -assetindex, this will require a reindex to occur");

            std::string asset_name = request.params[0].get_str();
            int block_height = request.params[1].getInt<int>();

            if (!pSnapshotRequestDb)
                throw JSONRPCError(RPC_DATABASE_ERROR, std::string("Snapshot Request database is not setup. Please restart wallet to try again"));

            if (pSnapshotRequestDb->RemoveSnapshotRequest(asset_name, block_height)) {
                UniValue obj(UniValue::VOBJ);
                obj.pushKV("request_status", "Removed");
                return obj;
            }

            throw JSONRPCError(RPC_MISC_ERROR, std::string("Failed to remove specified snapshot request"));
        },
    };
}

static RPCHelpMan getdistributestatus()
{
    return RPCHelpMan{
        "getdistributestatus",
        "Give information about the status of a reward distribution.\n",
        {
            {"asset_name", RPCArg::Type::STR, RPCArg::Optional::NO, "the reward will be distributed to all owners of this asset"},
            {"snapshot_height", RPCArg::Type::NUM, RPCArg::Optional::NO, "the block height of the ownership snapshot"},
            {"distribution_asset_name", RPCArg::Type::STR, RPCArg::Optional::NO, "the name of the asset that will be distributed, or AVN"},
            {"gross_distribution_amount", RPCArg::Type::NUM, RPCArg::Optional::NO, "the amount of the distribution asset that will be split amongst all owners"},
            {"exception_addresses", RPCArg::Type::STR, RPCArg::Default{""}, "comma-separated list of ownership addresses that should be excluded"},
        },
        RPCResult{
            RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::STR, "Asset Name", "the ownership asset name"},
                {RPCResult::Type::STR, "Height", "the snapshot height"},
                {RPCResult::Type::STR, "Distribution Name", "the distribution asset name"},
                {RPCResult::Type::NUM, "Distribution Amount", "the distribution amount"},
                {RPCResult::Type::NUM, "Status", "the distribution status code"},
            }
        },
        RPCExamples{
            HelpExampleCli("getdistributestatus", "\"ASSET_NAME\" 12345 \"AVN\" 1000")
          + HelpExampleCli("getdistributestatus", "\"ASSET_NAME\" 12345 \"DIVIDENDS\" 1000 \"addr1,addr2\"")
          + HelpExampleRpc("getdistributestatus", "\"ASSET_NAME\", 34987, \"DIVIDENDS\", 100000")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            if (!fAssetIndex)
                throw JSONRPCError(RPC_INVALID_REQUEST, "This RPC call is not functional unless -assetindex is enabled. To enable, please run the wallet with -assetindex, this will require a reindex to occur");

            std::string asset_name = request.params[0].get_str();
            int snapshot_height = request.params[1].getInt<int>();
            std::string distribution_asset_name = request.params[2].get_str();
            CAmount distribution_amount = AmountFromValue(request.params[3]);
            std::string exception_addresses;
            if (!request.params[4].isNull())
                exception_addresses = request.params[4].get_str();

            if (!pDistributeSnapshotDb)
                throw JSONRPCError(RPC_INVALID_REQUEST, std::string("Distribute snapshot database is not setup. Please restart wallet to try again"));

            CRewardSnapshot distribRewardSnapshotData(asset_name, distribution_asset_name, exception_addresses, distribution_amount, snapshot_height);
            auto hash = distribRewardSnapshotData.GetHash();

            CRewardSnapshot temp;
            if (!pDistributeSnapshotDb->RetrieveDistributeSnapshotRequest(hash, temp))
                throw JSONRPCError(RPC_MISC_ERROR, std::string("Distribution not found"));

            UniValue responseObj(UniValue::VOBJ);
            responseObj.pushKV("Asset Name", temp.strOwnershipAsset);
            responseObj.pushKV("Height", util::ToString(temp.nHeight));
            responseObj.pushKV("Distribution Name", temp.strDistributionAsset);
            responseObj.pushKV("Distribution Amount", ValueFromAmount(temp.nDistributionAmount));
            responseObj.pushKV("Status", temp.nStatus);

            return responseObj;
        },
    };
}

void RegisterRewardsRPCCommands(CRPCTable& t)
{
    static const CRPCCommand commands[]{
        {"rewards", &requestsnapshot},
        {"rewards", &getsnapshotrequest},
        {"rewards", &listsnapshotrequests},
        {"rewards", &cancelsnapshotrequest},
        {"rewards", &getdistributestatus},
    };
    for (const auto& c : commands) {
        t.appendCommand(c.name, &c);
    }
}
