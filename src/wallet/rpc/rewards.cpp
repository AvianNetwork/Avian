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
#include <util/moneystr.h>
#include <util/strencodings.h>
#include <util/string.h>
#include <validation.h>

#include <wallet/asset_tx.h>
#include <wallet/coincontrol.h>
#include <wallet/context.h>
#include <wallet/rewards.h>
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

//! One distribution is worked on at a time: the RPC and the scheduler would otherwise
//! both find the same batch unsent, and both send it.
static GlobalMutex g_reward_distribution_mutex;

bool ContinueRewardDistribution(CWallet& wallet, const uint256& hash, CRewardSnapshot& snapshot,
                                const std::string& change_address, std::vector<std::string>& sent_txids,
                                std::pair<int, std::string>& error)
{
    LOCK(g_reward_distribution_mutex);

    const auto stop = [&](int status, int code, const std::string& why) {
        snapshot.nStatus = status;
        pDistributeSnapshotDb->OverrideDistributeSnapshot(hash, snapshot);
        error = {code, why};
        return false;
    };

    std::vector<OwnerAndAmount> payments;
    if (!GenerateDistributionList(snapshot, payments)) {
        return stop(CRewardSnapshot::REWARD_ERROR, RPC_INVALID_REQUEST, "Failed to generate the distribution list");
    }
    if (payments.empty()) {
        return stop(CRewardSnapshot::LOW_REWARDS, RPC_INVALID_REQUEST, "Distribution list is empty. All reward amounts round to zero.");
    }
    if (wallet.IsLocked()) {
        // Not a failure of the distribution: its status stays, and it is paid once the wallet is unlocked.
        error = {RPC_WALLET_UNLOCK_NEEDED, "Error: Please enter the wallet passphrase with walletpassphrase first."};
        return false;
    }

    // The batches this wallet has already made for this distribution. A batch carries the
    // distribution and its number in the wallet, written with the transaction itself, so
    // one that went out just before the node stopped is found here although the record of
    // it below was never written. Without this it would be sent a second time.
    std::map<int, uint256> tagged;
    {
        LOCK(wallet.cs_wallet);
        for (const auto& [wtxid, wtx] : wallet.mapWallet) {
            const auto distribution = wtx.mapValue.find("reward_distribution");
            const auto number = wtx.mapValue.find("reward_batch");
            if (distribution == wtx.mapValue.end() || number == wtx.mapValue.end()) continue;
            if (distribution->second != hash.GetHex()) continue;
            if (const auto n{ToIntegral<int>(number->second)}) tagged.emplace(*n, wtxid.ToUint256());
        }
    }

    const bool is_avn{snapshot.strDistributionAsset == "AVN"};
    int batch{0};
    for (size_t first = 0; first < payments.size(); first += MAX_PAYMENTS_PER_TRANSACTION, ++batch) {
        const size_t last{std::min(first + MAX_PAYMENTS_PER_TRANSACTION, payments.size())};

        uint256 recorded;
        bool sent_before{pDistributeSnapshotDb->GetDistributeTransaction(hash, batch, recorded)};
        if (!sent_before) {
            if (const auto it{tagged.find(batch)}; it != tagged.end()) {
                recorded = it->second;
                pDistributeSnapshotDb->AddDistributeTransaction(hash, batch, recorded);
                sent_before = true;
            }
        }
        if (sent_before) {
            // The next batch goes out only if that transaction is this wallet's and still
            // stands. One that was abandoned, or that something in the mempool or in a block
            // has displaced, pays nobody, and has a depth of zero like any unconfirmed one.
            std::string fault;
            {
                LOCK(wallet.cs_wallet);
                const CWalletTx* wtx{wallet.GetWalletTx(Txid::FromUint256(recorded))};
                if (!wtx) {
                    fault = "which this wallet does not have";
                } else if (wtx->isAbandoned()) {
                    fault = "which was abandoned";
                } else if (wtx->isMempoolConflicted() || wtx->isBlockConflicted() || wallet.GetTxDepthInMainChain(*wtx) < 0) {
                    fault = "which conflicts with another transaction";
                }
            }
            if (!fault.empty()) {
                return stop(CRewardSnapshot::STUCK_TX, RPC_WALLET_ERROR,
                            strprintf("Batch %d was sent as %s, %s. It has not paid, and nothing more is sent", batch, recorded.GetHex(), fault));
            }
            continue;
        }

        mapValue_t map_value;
        map_value["reward_distribution"] = hash.GetHex();
        map_value["reward_batch"] = util::ToString(batch);

        std::string txid;
        if (is_avn) {
            std::vector<CRecipient> recipients;
            for (size_t i = first; i < last; ++i) {
                const CTxDestination dest{DecodeDestination(payments[i].address)};
                if (!IsValidDestination(dest)) {
                    wallet.WalletLogPrintf("Reward distribution of %s to holders of %s at height %d: '%s' is not a valid address and is not paid its %s\n",
                                           snapshot.strDistributionAsset, snapshot.strOwnershipAsset, snapshot.nHeight,
                                           payments[i].address, FormatMoney(payments[i].amount));
                    continue;
                }
                recipients.push_back({dest, payments[i].amount, false, CScript()});
            }
            if (recipients.empty()) continue;

            CCoinControl coin_control;
            if (!change_address.empty()) coin_control.destChange = DecodeDestination(change_address);

            try {
                txid = SendMoney(wallet, coin_control, recipients, std::move(map_value), false).get_str();
            } catch (const UniValue& rpc_error) {
                // SendMoney reports as an RPC does
                const std::string why{rpc_error.isObject() && rpc_error["message"].isStr() ? rpc_error["message"].get_str() : rpc_error.write()};
                const int code{rpc_error.isObject() && rpc_error["code"].isNum() ? rpc_error["code"].getInt<int>() : int{RPC_WALLET_ERROR}};
                return stop(code == RPC_WALLET_INSUFFICIENT_FUNDS ? CRewardSnapshot::LOW_FUNDS : CRewardSnapshot::FAILED_CREATE_TRANSACTION, code, why);
            } catch (const std::exception& e) {
                return stop(CRewardSnapshot::FAILED_CREATE_TRANSACTION, RPC_WALLET_ERROR, e.what());
            }
        } else {
            std::vector<std::pair<CAssetTransfer, std::string>> transfers;
            for (size_t i = first; i < last; ++i) {
                transfers.emplace_back(CAssetTransfer(snapshot.strDistributionAsset, payments[i].amount), payments[i].address);
            }

            CCoinControl coin_control;
            if (!change_address.empty()) {
                coin_control.destChange = DecodeDestination(change_address);
                coin_control.destAssetChange = DecodeDestination(change_address);
            }

            CTransactionRef tx;
            CAmount fee_required;
            std::pair<int, std::string> asset_error;
            if (!CreateTransferAssetTransaction(wallet, coin_control, transfers, "", asset_error, tx, fee_required)) {
                return stop(CRewardSnapshot::FAILED_CREATE_TRANSACTION, asset_error.first,
                            "Failed to create distribution transaction: " + asset_error.second);
            }
            if (!SendAssetTransaction(wallet, tx, asset_error, txid, std::move(map_value))) {
                return stop(CRewardSnapshot::FAILED_COMMIT_TRANSACTION, asset_error.first,
                            "Failed to send distribution transaction: " + asset_error.second);
            }
        }

        sent_txids.push_back(txid);
        if (HasTestOption(gArgs, "reward_stop_before_record")) {
            error = {RPC_MISC_ERROR, "Stopped before recording the batch (-test=reward_stop_before_record)"};
            return false;
        }
        pDistributeSnapshotDb->AddDistributeTransaction(hash, batch, uint256::FromHex(txid).value());
    }

    snapshot.nStatus = CRewardSnapshot::COMPLETE;
    pDistributeSnapshotDb->OverrideDistributeSnapshot(hash, snapshot);
    return true;
}

void MaybeDistributeRewards(WalletContext& context)
{
    if (!pDistributeSnapshotDb || !pAssetSnapshotDb || !passets) return;

    // The height each wallet was last looked at, and what was last logged about each
    // distribution: a locked wallet would otherwise say the same thing at every block.
    // Only the scheduler thread comes here.
    static std::map<std::string, int> last_height;
    static std::map<uint256, std::string> last_said;

    std::map<uint256, CRewardSnapshot> snapshots;
    bool listed{false};

    for (const std::shared_ptr<CWallet>& pwallet : GetWallets(context)) {
        if (pwallet->chain().isInitialBlockDownload()) continue;
        const int height{WITH_LOCK(pwallet->cs_wallet, return pwallet->GetLastBlockHeight())};
        auto [it, first_look] = last_height.try_emplace(pwallet->GetName(), height);
        if (!first_look && it->second == height) continue;
        it->second = height;

        if (!listed) {
            pDistributeSnapshotDb->ListDistributeSnapshots(snapshots);
            listed = true;
        }
        for (auto& [hash, snapshot] : snapshots) {
            // These three end a distribution. Anything else is tried again.
            if (snapshot.nStatus == CRewardSnapshot::COMPLETE || snapshot.nStatus == CRewardSnapshot::REWARD_ERROR ||
                snapshot.nStatus == CRewardSnapshot::LOW_REWARDS || snapshot.nStatus == CRewardSnapshot::STUCK_TX) {
                continue;
            }
            // Only the wallet it was asked of pays. One made before that was recorded is left alone.
            std::string owner;
            if (!pDistributeSnapshotDb->GetDistributeWallet(hash, owner) || owner != pwallet->GetName()) continue;

            std::string change_address;
            pDistributeSnapshotDb->GetDistributeChangeAddress(hash, change_address);

            std::vector<std::string> sent;
            std::pair<int, std::string> error;
            if (ContinueRewardDistribution(*pwallet, hash, snapshot, change_address, sent, error)) {
                pwallet->WalletLogPrintf("Reward distribution of %s to holders of %s at height %d is complete, %d transaction(s) sent now\n",
                                         snapshot.strDistributionAsset, snapshot.strOwnershipAsset, snapshot.nHeight, sent.size());
                last_said.erase(hash);
            } else {
                const std::string say{strprintf("status %d: %s", snapshot.nStatus, error.second)};
                if (last_said[hash] != say) {
                    pwallet->WalletLogPrintf("Reward distribution of %s to holders of %s at height %d is not complete (%s)%s\n",
                                             snapshot.strDistributionAsset, snapshot.strOwnershipAsset, snapshot.nHeight, say,
                                             sent.empty() ? "" : strprintf(", %d transaction(s) sent now", sent.size()));
                    last_said[hash] = say;
                }
            }
        }
    }
}

RPCHelpMan distributereward()
{
    return RPCHelpMan{
        "distributereward",
        "Splits the specified amount of the distribution asset to all owners of asset_name that are not in the optional exclusion_addresses.\n"
        "Requires wallet passphrase to be set with walletpassphrase call if wallet is encrypted.\n"
        "A distribution that cannot be sent in full now, because the wallet is locked or short of funds, stays\n"
        "registered: this wallet tries again each time it sees a new block, until every batch has been sent or\n"
        "canceldistribution withdraws it.\n",
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
            pDistributeSnapshotDb->SetDistributeWallet(snapshotHash, pwallet->GetName());
            pDistributeSnapshotDb->SetDistributeChangeAddress(snapshotHash, change_address);

            // Everything is sent now if it can be. What cannot be is tried again each time
            // this wallet sees a new block, as 4.x did, until the distribution is complete.
            std::vector<std::string> sent;
            std::pair<int, std::string> error;
            if (!ContinueRewardDistribution(*pwallet, snapshotHash, distribRewardSnapshotData, change_address, sent, error)) {
                const bool retried{distribRewardSnapshotData.nStatus != CRewardSnapshot::REWARD_ERROR &&
                                   distribRewardSnapshotData.nStatus != CRewardSnapshot::LOW_REWARDS &&
                                   distribRewardSnapshotData.nStatus != CRewardSnapshot::STUCK_TX};
                throw JSONRPCError(error.first, error.second +
                                   (retried ? ". The distribution is registered, and is tried again with each new block." : ""));
            }

            UniValue txids(UniValue::VARR);
            for (const std::string& txid : sent) txids.push_back(txid);
            return txids;
        },
    };
}

RPCHelpMan canceldistribution()
{
    return RPCHelpMan{
        "canceldistribution",
        "Withdraws a reward distribution that distributereward registered, so that nothing more is sent for it.\n"
        "The batches it has already sent stay sent, and are listed. They are remembered: if the same\n"
        "distribution is asked for again, only the batches that were never sent go out.\n"
        "The arguments are those the distribution was made with.\n",
        {
            {"asset_name", RPCArg::Type::STR, RPCArg::Optional::NO, "the asset whose owners the reward goes to"},
            {"snapshot_height", RPCArg::Type::NUM, RPCArg::Optional::NO, "the block height of the ownership snapshot"},
            {"distribution_asset_name", RPCArg::Type::STR, RPCArg::Optional::NO, "the name of the asset that is distributed, or AVN"},
            {"gross_distribution_amount", RPCArg::Type::NUM, RPCArg::Optional::NO, "the amount of the distribution asset that is split amongst all owners"},
            {"exception_addresses", RPCArg::Type::STR, RPCArg::Default{""}, "comma-separated list of ownership addresses that are excluded"},
        },
        RPCResult{
            RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::STR, "request_status", "\"Removed\""},
                {RPCResult::Type::NUM, "status", "the status code the distribution had"},
                {RPCResult::Type::ARR, "sent", "the transactions already sent for it, a batch each",
                {
                    {RPCResult::Type::STR_HEX, "", "txid of a distribution batch"},
                }},
            }
        },
        RPCExamples{
            HelpExampleCli("canceldistribution", "\"ASSET_NAME\" 12345 \"AVN\" 1000")
          + HelpExampleRpc("canceldistribution", "\"ASSET_NAME\", 34987, \"DIVIDENDS\", 100000")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            const std::shared_ptr<CWallet> pwallet = GetWalletForJSONRPCRequest(request);
            if (!pwallet) throw JSONRPCError(RPC_WALLET_NOT_FOUND, "Wallet not found");

            if (!pDistributeSnapshotDb)
                throw JSONRPCError(RPC_DATABASE_ERROR, std::string("Distribute snapshot database is not setup. Please restart wallet to try again"));

            const std::string exception_addresses{request.params[4].isNull() ? "" : request.params[4].get_str()};
            const CRewardSnapshot asked(request.params[0].get_str(), request.params[2].get_str(), exception_addresses,
                                        AmountFromValue(request.params[3]), request.params[1].getInt<int>());
            const uint256 hash{asked.GetHash()};

            // Held so that a batch is not sent, or the distribution written back, while it is removed
            LOCK(g_reward_distribution_mutex);

            CRewardSnapshot stored;
            if (!pDistributeSnapshotDb->RetrieveDistributeSnapshotRequest(hash, stored))
                throw JSONRPCError(RPC_MISC_ERROR, std::string("Distribution not found"));

            std::string owner;
            if (pDistributeSnapshotDb->GetDistributeWallet(hash, owner) && owner != pwallet->GetName())
                throw JSONRPCError(RPC_WALLET_ERROR, strprintf("This distribution belongs to the wallet \"%s\". Withdraw it from there", owner));

            UniValue sent(UniValue::VARR);
            uint256 txid;
            for (int batch = 0; pDistributeSnapshotDb->GetDistributeTransaction(hash, batch, txid); ++batch) {
                sent.push_back(txid.GetHex());
            }

            if (!pDistributeSnapshotDb->RemoveDistributeSnapshotRequest(hash))
                throw JSONRPCError(RPC_DATABASE_ERROR, std::string("Failed to remove the distribution"));
            mapRewardSnapshots.erase(hash);

            UniValue result(UniValue::VOBJ);
            result.pushKV("request_status", "Removed");
            result.pushKV("status", stored.nStatus);
            result.pushKV("sent", std::move(sent));
            return result;
        },
    };
}

} // namespace wallet
