// Copyright (c) 2019-2020 The Raven Core developers
// Copyright (c) 2020-2024 The Avian developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <rpc/server.h>
#include <rpc/util.h>

#include <assets/assets.h>
#include <assets/assetdb.h>
#include <assets/rewards.h>
#include <assets/snapshotrequestdb.h>
#include <assets/assetsnapshotdb.h>
#include <common/args.h>
#include <key_io.h>
#include <sync.h>
#include <validation.h>

#include <wallet/asset_tx.h>
#include <wallet/coincontrol.h>
#include <wallet/rpc/util.h>
#include <wallet/transaction.h>
#include <wallet/wallet.h>

#include <univalue.h>

extern CSnapshotRequestDB* pSnapshotRequestDb;
extern CAssetSnapshotDB* pAssetSnapshotDb;
extern CDistributeSnapshotRequestDB* pDistributeSnapshotDb;

namespace wallet {

// Forward declaration from spend.cpp
UniValue SendMoney(CWallet& wallet, const CCoinControl &coin_control, std::vector<CRecipient> &recipients, mapValue_t map_value, bool verbose);

RPCHelpMan distributereward()
{
    return RPCHelpMan{
        "distributereward",
        "Splits the specified amount of the distribution asset to all owners of asset_name that are not in the optional exclusion_addresses.\n"
        "Requires wallet passphrase to be set with walletpassphrase call if wallet is encrypted.\n",
        {
            {"asset_name", RPCArg::Type::STR, RPCArg::Optional::NO, "the reward will be distributed to all owners of this asset"},
            {"snapshot_height", RPCArg::Type::NUM, RPCArg::Optional::NO, "the block height of the ownership snapshot"},
            {"distribution_asset_name", RPCArg::Type::STR, RPCArg::Optional::NO, "the name of the asset that will be distributed, or AVN"},
            {"gross_distribution_amount", RPCArg::Type::NUM, RPCArg::Optional::NO, "the amount of the distribution asset that will be split amongst all owners"},
            {"exception_addresses", RPCArg::Type::STR, RPCArg::Default{""}, "comma-separated list of ownership addresses that should be excluded"},
            {"change_address", RPCArg::Type::STR, RPCArg::Default{""}, "if the rewards can't be fully distributed, the change will be sent to this address"},
        },
        RPCResult{
            RPCResult::Type::ARR, "", "list of transaction IDs for each batch",
            {
                {RPCResult::Type::STR_HEX, "", "txid of a distribution batch"},
            }
        },
        RPCExamples{
            HelpExampleCli("distributereward", "\"ASSET_NAME\" 12345 \"AVN\" 1000")
          + HelpExampleCli("distributereward", "\"ASSET_NAME\" 12345 \"DIVIDENDS\" 1000 \"addr1,addr2\"")
          + HelpExampleRpc("distributereward", "\"ASSET_NAME\", 34987, \"DIVIDENDS\", 100000")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            const std::shared_ptr<CWallet> pwallet = GetWalletForJSONRPCRequest(request);
            if (!pwallet) throw JSONRPCError(RPC_WALLET_NOT_FOUND, "Wallet not found");

            if (!fAssetIndex)
                throw JSONRPCError(RPC_INVALID_REQUEST, "This RPC call is not functional unless -assetindex is enabled. To enable, please run the wallet with -assetindex, this will require a reindex to occur");

            std::string asset_name = request.params[0].get_str();
            int snapshot_height = request.params[1].getInt<int>();
            std::string distribution_asset_name = request.params[2].get_str();
            CAmount distribution_amount = AmountFromValue(request.params[3]);
            std::string exception_addresses;
            if (!request.params[4].isNull())
                exception_addresses = request.params[4].get_str();

            std::string change_address;
            if (!request.params[5].isNull()) {
                change_address = request.params[5].get_str();
                if (!change_address.empty() && !IsValidDestinationString(change_address))
                    throw JSONRPCError(RPC_INVALID_PARAMETER, std::string("Invalid change address: Use a valid AVN address"));
            }

            AssetType ownershipAssetType;
            std::string assetError;
            if (!IsAssetNameValid(asset_name, ownershipAssetType, assetError))
                throw JSONRPCError(RPC_INVALID_PARAMETER, std::string("Invalid asset_name: Please use a valid asset name"));

            if (ownershipAssetType == AssetType::UNIQUE || ownershipAssetType == AssetType::OWNER || ownershipAssetType == AssetType::MSGCHANNEL)
                throw JSONRPCError(RPC_INVALID_PARAMETER, std::string("Invalid asset_name: OWNER, UNIQUE, MSGCHANNEL assets are not allowed for this call"));

            const int nHeight = WITH_LOCK(pwallet->cs_wallet, return pwallet->GetLastBlockHeight());

            if (snapshot_height > nHeight)
                throw JSONRPCError(RPC_INVALID_PARAMETER, std::string("Invalid snapshot_height: block height should be less than or equal to the current active chain height"));

            if (distribution_asset_name != "AVN") {
                AssetType distributionAssetType;
                std::string distError;
                if (!IsAssetNameValid(distribution_asset_name, distributionAssetType, distError))
                    throw JSONRPCError(RPC_INVALID_PARAMETER, std::string("Invalid distribution_asset_name: Please use a valid asset name"));

                if (distributionAssetType == AssetType::UNIQUE || distributionAssetType == AssetType::OWNER || distributionAssetType == AssetType::MSGCHANNEL)
                    throw JSONRPCError(RPC_INVALID_PARAMETER, std::string("Invalid distribution_asset_name: OWNER, UNIQUE, MSGCHANNEL assets are not allowed for this call"));
            }

            if (nHeight - snapshot_height < (int)gArgs.GetIntArg("-minrewardheight", MINIMUM_REWARDS_PAYOUT_HEIGHT))
                throw JSONRPCError(RPC_INVALID_REQUEST, std::string("For security of the rewards payout, it is recommended to wait until chain is 60 blocks ahead of the snapshot height. You can modify this by using the -minrewardheight."));

            if (!passets)
                throw JSONRPCError(RPC_DATABASE_ERROR, std::string("Asset cache not setup. Please restart wallet to try again"));

            CNewAsset assetMetaData;
            if (!passets->GetAssetMetaDataIfExists(asset_name, assetMetaData))
                throw JSONRPCError(RPC_INVALID_REQUEST, std::string("The asset hasn't been created: ") + asset_name);

            if (!passetsdb)
                throw JSONRPCError(RPC_DATABASE_ERROR, std::string("Assets database is not setup. Please restart wallet to try again"));
            if (!pAssetSnapshotDb)
                throw JSONRPCError(RPC_DATABASE_ERROR, std::string("Asset Snapshot database is not setup. Please restart wallet to try again"));
            if (!pSnapshotRequestDb)
                throw JSONRPCError(RPC_DATABASE_ERROR, std::string("Snapshot Request database is not setup. Please restart wallet to try again"));

            if (!pSnapshotRequestDb->ContainsSnapshotRequest(asset_name, snapshot_height))
                throw JSONRPCError(RPC_INVALID_REQUEST, std::string("Snapshot request not found"));

            // Create and store the distribution snapshot
            CRewardSnapshot distribRewardSnapshotData(asset_name, distribution_asset_name, exception_addresses, distribution_amount, snapshot_height);
            if (!AddDistributeRewardSnapshot(distribRewardSnapshotData))
                throw JSONRPCError(RPC_INVALID_REQUEST, std::string("Distribution of reward has already been created. You must remove the distribution before creating another one"));

            auto snapshotHash = distribRewardSnapshotData.GetHash();

            // Generate the distribution list from the snapshot
            std::vector<OwnerAndAmount> vecDistributionList;
            if (!GenerateDistributionList(distribRewardSnapshotData, vecDistributionList)) {
                distribRewardSnapshotData.nStatus = CRewardSnapshot::REWARD_ERROR;
                pDistributeSnapshotDb->OverrideDistributeSnapshot(snapshotHash, distribRewardSnapshotData);
                throw JSONRPCError(RPC_INVALID_REQUEST, std::string("Failed to generate the distribution list"));
            }

            if (vecDistributionList.empty()) {
                distribRewardSnapshotData.nStatus = CRewardSnapshot::LOW_REWARDS;
                pDistributeSnapshotDb->OverrideDistributeSnapshot(snapshotHash, distribRewardSnapshotData);
                throw JSONRPCError(RPC_INVALID_REQUEST, std::string("Distribution list is empty. All reward amounts round to zero."));
            }

            EnsureWalletIsUnlocked(*pwallet);

            UniValue txids(UniValue::VARR);
            bool isAVN = (distribution_asset_name == "AVN");
            int batchNumber = 0;

            // Process in batches of MAX_PAYMENTS_PER_TRANSACTION
            for (size_t i = 0; i < vecDistributionList.size(); i += MAX_PAYMENTS_PER_TRANSACTION) {
                size_t batchEnd = std::min(i + MAX_PAYMENTS_PER_TRANSACTION, vecDistributionList.size());

                if (isAVN) {
                    // AVN distribution: use standard SendMoney
                    std::vector<CRecipient> recipients;
                    for (size_t j = i; j < batchEnd; j++) {
                        CTxDestination dest = DecodeDestination(vecDistributionList[j].address);
                        if (!IsValidDestination(dest)) continue;
                        recipients.push_back({dest, vecDistributionList[j].amount, false, CScript()});
                    }

                    if (recipients.empty()) continue;

                    CCoinControl coinControl;
                    if (!change_address.empty())
                        coinControl.destChange = DecodeDestination(change_address);

                    mapValue_t mapValue;
                    mapValue["reward_distribution"] = snapshotHash.GetHex();

                    UniValue result = SendMoney(*pwallet, coinControl, recipients, std::move(mapValue), false);
                    std::string txid = result.get_str();
                    txids.push_back(txid);

                    pDistributeSnapshotDb->AddDistributeTransaction(snapshotHash, batchNumber, uint256::FromHex(txid).value());
                } else {
                    // Asset distribution: use CreateTransferAssetTransaction
                    std::vector<std::pair<CAssetTransfer, std::string>> vTransfers;
                    for (size_t j = i; j < batchEnd; j++) {
                        CAssetTransfer transfer(distribution_asset_name, vecDistributionList[j].amount);
                        vTransfers.emplace_back(std::make_pair(transfer, vecDistributionList[j].address));
                    }

                    if (vTransfers.empty()) continue;

                    CCoinControl ctrl;
                    if (!change_address.empty()) {
                        ctrl.destChange = DecodeDestination(change_address);
                        ctrl.destAssetChange = DecodeDestination(change_address);
                    }

                    CTransactionRef tx;
                    CAmount nFeeRequired;
                    std::pair<int, std::string> error;

                    if (!CreateTransferAssetTransaction(*pwallet, ctrl, vTransfers, "", error, tx, nFeeRequired)) {
                        distribRewardSnapshotData.nStatus = CRewardSnapshot::FAILED_CREATE_TRANSACTION;
                        pDistributeSnapshotDb->OverrideDistributeSnapshot(snapshotHash, distribRewardSnapshotData);
                        throw JSONRPCError(error.first, "Failed to create distribution transaction: " + error.second);
                    }

                    std::string txid;
                    if (!SendAssetTransaction(*pwallet, tx, error, txid)) {
                        distribRewardSnapshotData.nStatus = CRewardSnapshot::FAILED_COMMIT_TRANSACTION;
                        pDistributeSnapshotDb->OverrideDistributeSnapshot(snapshotHash, distribRewardSnapshotData);
                        throw JSONRPCError(error.first, "Failed to send distribution transaction: " + error.second);
                    }

                    txids.push_back(txid);
                    pDistributeSnapshotDb->AddDistributeTransaction(snapshotHash, batchNumber, uint256::FromHex(txid).value());
                }
                batchNumber++;
            }

            // Mark distribution as complete
            distribRewardSnapshotData.nStatus = CRewardSnapshot::COMPLETE;
            pDistributeSnapshotDb->OverrideDistributeSnapshot(snapshotHash, distribRewardSnapshotData);

            return txids;
        },
    };
}

} // namespace wallet
