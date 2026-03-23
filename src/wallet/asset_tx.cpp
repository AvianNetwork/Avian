// Copyright (c) 2017-2019 The Raven Core developers
// Copyright (c) 2020-2024 The Avian developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <wallet/asset_tx.h>

#include <addresstype.h>
#include <assets/assets.h>
#include <assets/assettypes.h>
#include <key_io.h>
#include <rpc/protocol.h>
#include <script/script.h>
#include <util/moneystr.h>
#include <validation.h>
#include <wallet/coincontrol.h>
#include <wallet/receive.h>
#include <wallet/spend.h>
#include <wallet/wallet.h>

namespace wallet {

//! Generate an OP_AVN_ASSET <hash> script for null asset data (qualifier tags, restrictions)
static CScript GetScriptForNullAssetDataDestination(const CTxDestination& dest)
{
    CScript script;
    if (auto* pkh = std::get_if<PKHash>(&dest)) {
        script << OP_AVN_ASSET << ToByteVector(*pkh);
    } else if (auto* sh = std::get_if<ScriptHash>(&dest)) {
        script << OP_AVN_ASSET << ToByteVector(*sh);
    }
    return script;
}

bool VerifyWalletHasAsset(const CWallet& wallet, const std::string& asset_name, std::pair<int, std::string>& error)
    EXCLUSIVE_LOCKS_REQUIRED(wallet.cs_wallet)
{
    AssertLockHeld(wallet.cs_wallet);

    CoinFilterParams coin_params;
    coin_params.min_amount = 0;
    CoinsResult available = AvailableCoinsWithAssets(wallet, nullptr, std::nullopt, coin_params);

    if (available.mapAssetCoins.count(asset_name))
        return true;

    error = std::make_pair(RPC_INVALID_REQUEST, strprintf("Wallet doesn't have asset: %s", asset_name));
    return false;
}

//! Helper: generate a change address from the wallet
static bool GetChangeAddress(CWallet& wallet, CTxDestination& dest, std::string& strError)
{
    auto op_dest = wallet.GetNewDestination(OutputType::LEGACY, "");
    if (!op_dest) {
        strError = util::ErrorString(op_dest).original;
        return false;
    }
    dest = *op_dest;
    return true;
}

bool CreateAssetTransaction(
    CWallet& wallet,
    CCoinControl& coinControl,
    const CNewAsset& asset,
    const std::string& address,
    std::pair<int, std::string>& error,
    CTransactionRef& txRef,
    CAmount& nFeeRequired,
    std::string* verifier_string)
{
    std::vector<CNewAsset> assets;
    assets.push_back(asset);
    return CreateAssetTransaction(wallet, coinControl, assets, address, error, txRef, nFeeRequired, verifier_string);
}

bool CreateAssetTransaction(
    CWallet& wallet,
    CCoinControl& coinControl,
    const std::vector<CNewAsset>& assets,
    const std::string& address,
    std::pair<int, std::string>& error,
    CTransactionRef& txRef,
    CAmount& nFeeRequired,
    std::string* verifier_string)
{
    std::string change_address;
    if (!std::get_if<CNoDestination>(&coinControl.destChange)) {
        change_address = EncodeDestination(coinControl.destChange);
    }

    // Validate the assets data
    std::string strError;
    for (const auto& asset : assets) {
        if (!ContextualCheckNewAsset(passets, asset, strError)) {
            error = std::make_pair(RPC_INVALID_PARAMETER, strError);
            return false;
        }
    }

    if (!change_address.empty()) {
        CTxDestination destination = DecodeDestination(change_address);
        if (!IsValidDestination(destination)) {
            error = std::make_pair(RPC_INVALID_ADDRESS_OR_KEY, std::string("Invalid Avian address: ") + change_address);
            return false;
        }
    } else {
        CTxDestination changeDest;
        std::string strFailReason;
        if (!GetChangeAddress(wallet, changeDest, strFailReason)) {
            error = std::make_pair(RPC_WALLET_KEYPOOL_RAN_OUT, strFailReason);
            return false;
        }
        change_address = EncodeDestination(changeDest);
        coinControl.destChange = changeDest;
    }

    AssetType assetType{AssetType::ROOT};
    std::string parentName;
    for (const auto& asset : assets) {
        if (!IsAssetNameValid(asset.strName, assetType)) {
            error = std::make_pair(RPC_INVALID_PARAMETER, "Asset name not valid");
            return false;
        }
        if (assets.size() > 1 && assetType != AssetType::UNIQUE) {
            error = std::make_pair(RPC_INVALID_PARAMETER, "Only unique assets can be issued in bulk.");
            return false;
        }
        std::string parent = GetParentName(asset.strName);
        if (parentName.empty())
            parentName = parent;
        if (parentName != parent) {
            error = std::make_pair(RPC_INVALID_PARAMETER, "All assets must have the same parent.");
            return false;
        }
    }

    // Burn amount and address depend on asset type
    CAmount burnAmount = GetBurnAmount(assetType) * assets.size();
    CScript scriptPubKeyBurn = GetScriptForDestination(DecodeDestination(GetBurnAddress(assetType)));

    // Check wallet balance
    Balance bal = GetBalance(wallet);
    if (bal.m_mine_trusted < burnAmount) {
        error = std::make_pair(RPC_WALLET_INSUFFICIENT_FUNDS, "Insufficient funds");
        return false;
    }

    LOCK2(cs_main, wallet.cs_wallet);

    // Build recipient list
    std::vector<CRecipient> vecSend;

    // 1) Burn output
    CRecipient burnRecipient;
    burnRecipient.dest = CNoDestination();
    burnRecipient.nAmount = burnAmount;
    burnRecipient.fSubtractFeeFromAmount = false;
    burnRecipient.scriptOverride = scriptPubKeyBurn;
    vecSend.push_back(burnRecipient);

    // 2) For subassets/unique/msgchannel: transfer owner token back to change address
    if (assetType == AssetType::SUB || assetType == AssetType::UNIQUE || assetType == AssetType::MSGCHANNEL) {
        if (!VerifyWalletHasAsset(wallet, parentName + OWNER_TAG, error))
            return false;

        CScript scriptTransferOwner = GetScriptForDestination(DecodeDestination(change_address));
        CAssetTransfer ownerTransfer(parentName + OWNER_TAG, OWNER_ASSET_AMOUNT);
        ownerTransfer.ConstructTransaction(scriptTransferOwner);

        CRecipient rec;
        rec.dest = CNoDestination();
        rec.nAmount = 0;
        rec.fSubtractFeeFromAmount = false;
        rec.scriptOverride = scriptTransferOwner;
        vecSend.push_back(rec);
    }

    // 3) For sub-qualifiers: transfer parent qualifier back to change address
    if (assetType == AssetType::SUB_QUALIFIER) {
        if (!VerifyWalletHasAsset(wallet, parentName, error))
            return false;

        CScript scriptTransferQualifier = GetScriptForDestination(DecodeDestination(change_address));
        CAssetTransfer qualifierTransfer(parentName, OWNER_ASSET_AMOUNT);
        qualifierTransfer.ConstructTransaction(scriptTransferQualifier);

        CRecipient rec;
        rec.dest = CNoDestination();
        rec.nAmount = 0;
        rec.fSubtractFeeFromAmount = false;
        rec.scriptOverride = scriptTransferQualifier;
        vecSend.push_back(rec);
    }

    // 4) For restricted assets: transfer root owner token + verifier string
    if (assetType == AssetType::RESTRICTED) {
        std::string strStripped = parentName.substr(1, parentName.size() - 1);
        if (!VerifyWalletHasAsset(wallet, strStripped + OWNER_TAG, error))
            return false;

        CScript scriptTransferOwner = GetScriptForDestination(DecodeDestination(change_address));
        CAssetTransfer ownerTransfer(strStripped + OWNER_TAG, OWNER_ASSET_AMOUNT);
        ownerTransfer.ConstructTransaction(scriptTransferOwner);

        CRecipient ownerRec;
        ownerRec.dest = CNoDestination();
        ownerRec.nAmount = 0;
        ownerRec.fSubtractFeeFromAmount = false;
        ownerRec.scriptOverride = scriptTransferOwner;
        vecSend.push_back(ownerRec);

        if (!verifier_string) {
            error = std::make_pair(RPC_INVALID_PARAMETER, "Error: Verifier string not found");
            return false;
        }

        CScript verifierScript;
        CNullAssetTxVerifierString verifier(*verifier_string);
        verifier.ConstructTransaction(verifierScript);

        CRecipient verifierRec;
        verifierRec.dest = CNoDestination();
        verifierRec.nAmount = 0;
        verifierRec.fSubtractFeeFromAmount = false;
        verifierRec.scriptOverride = verifierScript;
        vecSend.push_back(verifierRec);
    }

    // 5) New asset output(s) — the actual asset scripts sent to the destination address
    CTxDestination assetDest = DecodeDestination(address);
    for (const auto& asset : assets) {
        CScript scriptAssetNew = GetScriptForDestination(assetDest);
        asset.ConstructTransaction(scriptAssetNew);

        CRecipient assetRec;
        assetRec.dest = CNoDestination();
        assetRec.nAmount = 0;
        assetRec.fSubtractFeeFromAmount = false;
        assetRec.scriptOverride = scriptAssetNew;
        vecSend.push_back(assetRec);

        // Owner token output
        CScript scriptOwnerNew = GetScriptForDestination(assetDest);
        asset.ConstructOwnerTransaction(scriptOwnerNew);

        CRecipient ownerRec;
        ownerRec.dest = CNoDestination();
        ownerRec.nAmount = 0;
        ownerRec.fSubtractFeeFromAmount = false;
        ownerRec.scriptOverride = scriptOwnerNew;
        vecSend.push_back(ownerRec);
    }

    // Pre-select owner token UTXOs if needed for subassets/unique/msgchannel/restricted
    if (assetType == AssetType::SUB || assetType == AssetType::UNIQUE || assetType == AssetType::MSGCHANNEL) {
        CoinFilterParams coin_params;
        coin_params.min_amount = 0;
        CoinsResult available = AvailableCoinsWithAssets(wallet, nullptr, std::nullopt, coin_params);
        auto it = available.mapAssetCoins.find(parentName + OWNER_TAG);
        if (it != available.mapAssetCoins.end() && !it->second.empty()) {
            coinControl.Select(it->second[0].outpoint);
        }
    } else if (assetType == AssetType::SUB_QUALIFIER) {
        CoinFilterParams coin_params;
        coin_params.min_amount = 0;
        CoinsResult available = AvailableCoinsWithAssets(wallet, nullptr, std::nullopt, coin_params);
        auto it = available.mapAssetCoins.find(parentName);
        if (it != available.mapAssetCoins.end() && !it->second.empty()) {
            coinControl.Select(it->second[0].outpoint);
        }
    } else if (assetType == AssetType::RESTRICTED) {
        std::string strStripped = parentName.substr(1, parentName.size() - 1);
        CoinFilterParams coin_params;
        coin_params.min_amount = 0;
        CoinsResult available = AvailableCoinsWithAssets(wallet, nullptr, std::nullopt, coin_params);
        auto it = available.mapAssetCoins.find(strStripped + OWNER_TAG);
        if (it != available.mapAssetCoins.end() && !it->second.empty()) {
            coinControl.Select(it->second[0].outpoint);
        }
    }

    // Allow the wallet to select additional AVN inputs for fees
    coinControl.m_allow_other_inputs = true;

    // Create the transaction
    auto res = CreateTransaction(wallet, vecSend, std::nullopt, coinControl, true);
    if (!res) {
        error = std::make_pair(RPC_WALLET_ERROR, util::ErrorString(res).original);
        return false;
    }

    txRef = res->tx;
    nFeeRequired = res->fee;
    return true;
}

bool CreateTransferAssetTransaction(
    CWallet& wallet,
    const CCoinControl& coinControl,
    const std::vector<std::pair<CAssetTransfer, std::string>>& vTransfers,
    const std::string& changeAddress,
    std::pair<int, std::string>& error,
    CTransactionRef& txRef,
    CAmount& nFeeRequired,
    std::vector<std::pair<CNullAssetTxData, std::string>>* nullAssetTxData,
    std::vector<CNullAssetTxData>* nullGlobalRestrictionData)
{
    std::vector<CRecipient> vecSend;

    // Check for a balance before processing
    Balance bal = GetBalance(wallet);
    if (bal.m_mine_trusted == 0) {
        error = std::make_pair(RPC_WALLET_INSUFFICIENT_FUNDS, std::string("This wallet doesn't contain any AVN, transferring an asset requires a network fee"));
        return false;
    }

    LOCK2(cs_main, wallet.cs_wallet);

    // Collect required asset amounts and build transfer outputs
    std::map<std::string, CAmount> mapAssetTargets;

    for (const auto& transfer : vTransfers) {
        const std::string& address = transfer.second;
        const std::string& asset_name = transfer.first.strName;
        CAmount nAmount = transfer.first.nAmount;

        if (!IsValidDestinationString(address)) {
            error = std::make_pair(RPC_INVALID_ADDRESS_OR_KEY, std::string("Invalid Avian address: ") + address);
            return false;
        }

        if (!VerifyWalletHasAsset(wallet, asset_name, error))
            return false;

        // Ownership tokens must transfer exactly 1
        if (IsAssetNameAnOwner(asset_name)) {
            if (nAmount != OWNER_ASSET_AMOUNT) {
                error = std::make_pair(RPC_INVALID_PARAMS, std::string("When transferring an 'Ownership Asset' the amount must always be 1. Please try again with the amount of 1"));
                return false;
            }
        }

        // Check restricted asset rules
        if (IsAssetNameAnRestricted(asset_name)) {
            if (passets->CheckForGlobalRestriction(asset_name, true)) {
                error = std::make_pair(RPC_INVALID_PARAMETER, "Unable to transfer restricted asset, this restricted asset has been globally frozen");
                return false;
            }

            std::string strError;
            if (!transfer.first.ContextualCheckAgainstVerifyString(passets, address, strError)) {
                error = std::make_pair(RPC_INVALID_PARAMETER, strError);
                return false;
            }
        }

        // Build the asset transfer output script
        CScript scriptPubKey = GetScriptForDestination(DecodeDestination(address));
        CAssetTransfer assetTransfer(asset_name, nAmount, transfer.first.message, transfer.first.nExpireTime);
        assetTransfer.ConstructTransaction(scriptPubKey);

        CRecipient recipient;
        recipient.dest = CNoDestination();
        recipient.nAmount = 0;
        recipient.fSubtractFeeFromAmount = false;
        recipient.scriptOverride = scriptPubKey;
        vecSend.push_back(recipient);

        // Accumulate required amounts
        mapAssetTargets[asset_name] += nAmount;
    }

    // Handle null asset data (qualifier tags, address restrictions)
    if (nullAssetTxData) {
        int nAddTagCount = 0;
        for (auto& pair : *nullAssetTxData) {
            std::string strError;
            if (IsAssetNameAQualifier(pair.first.asset_name)) {
                if (!VerifyQualifierChange(*passets, pair.first, pair.second, strError)) {
                    error = std::make_pair(RPC_INVALID_REQUEST, strError);
                    return false;
                }
                if (pair.first.flag == (int)QualifierType::ADD_QUALIFIER)
                    nAddTagCount++;
            } else if (IsAssetNameAnRestricted(pair.first.asset_name)) {
                if (!VerifyRestrictedAddressChange(*passets, pair.first, pair.second, strError)) {
                    error = std::make_pair(RPC_INVALID_REQUEST, strError);
                    return false;
                }
            }

            CScript dataScript = GetScriptForNullAssetDataDestination(DecodeDestination(pair.second));
            pair.first.ConstructTransaction(dataScript);

            CRecipient recipient;
            recipient.dest = CNoDestination();
            recipient.nAmount = 0;
            recipient.fSubtractFeeFromAmount = false;
            recipient.scriptOverride = dataScript;
            vecSend.push_back(recipient);
        }

        // Burn for adding tags
        if (nAddTagCount) {
            CScript addTagBurnScript = GetScriptForDestination(DecodeDestination(GetBurnAddress(AssetType::NULL_ADD_QUALIFIER)));
            CRecipient addTagRec;
            addTagRec.dest = CNoDestination();
            addTagRec.nAmount = GetBurnAmount(AssetType::NULL_ADD_QUALIFIER) * nAddTagCount;
            addTagRec.fSubtractFeeFromAmount = false;
            addTagRec.scriptOverride = addTagBurnScript;
            vecSend.push_back(addTagRec);
        }
    }

    // Handle global restriction data
    if (nullGlobalRestrictionData) {
        for (auto& dataObject : *nullGlobalRestrictionData) {
            std::string strError;
            if (!VerifyGlobalRestrictedChange(*passets, dataObject, strError)) {
                error = std::make_pair(RPC_INVALID_REQUEST, strError);
                return false;
            }

            CScript dataScript;
            dataObject.ConstructGlobalRestrictionTransaction(dataScript);

            CRecipient recipient;
            recipient.dest = CNoDestination();
            recipient.nAmount = 0;
            recipient.fSubtractFeeFromAmount = false;
            recipient.scriptOverride = dataScript;
            vecSend.push_back(recipient);
        }
    }

    // Select asset UTXOs and create asset change outputs
    CoinFilterParams coin_params;
    coin_params.min_amount = 0;
    CoinsResult available = AvailableCoinsWithAssets(wallet, nullptr, std::nullopt, coin_params);

    std::set<COutput> setAssetCoins;
    std::map<std::string, CAmount> mapSelectedValues;
    if (!SelectAssets(wallet, available.mapAssetCoins, mapAssetTargets, setAssetCoins, mapSelectedValues)) {
        error = std::make_pair(RPC_WALLET_INSUFFICIENT_FUNDS, "Insufficient asset funds");
        return false;
    }

    // Pre-select asset UTXOs and create change outputs
    CCoinControl coinControlCopy = coinControl;
    coinControlCopy.m_allow_other_inputs = true;

    // Determine asset change address
    std::string assetChangeAddr;
    if (!std::get_if<CNoDestination>(&coinControlCopy.destAssetChange)) {
        assetChangeAddr = EncodeDestination(coinControlCopy.destAssetChange);
    } else if (!changeAddress.empty()) {
        assetChangeAddr = changeAddress;
    } else {
        CTxDestination changeDest;
        std::string strFailReason;
        if (!GetChangeAddress(wallet, changeDest, strFailReason)) {
            error = std::make_pair(RPC_WALLET_KEYPOOL_RAN_OUT, strFailReason);
            return false;
        }
        assetChangeAddr = EncodeDestination(changeDest);
    }

    for (const auto& coin : setAssetCoins) {
        coinControlCopy.Select(coin.outpoint);
    }

    // Create asset change outputs for any excess
    for (const auto& [assetName, selectedAmount] : mapSelectedValues) {
        CAmount targetAmount = mapAssetTargets[assetName];
        if (selectedAmount > targetAmount) {
            CAmount changeAmount = selectedAmount - targetAmount;
            CScript scriptChange = GetScriptForDestination(DecodeDestination(assetChangeAddr));
            CAssetTransfer changeTransfer(assetName, changeAmount);
            changeTransfer.ConstructTransaction(scriptChange);

            CRecipient changeRec;
            changeRec.dest = CNoDestination();
            changeRec.nAmount = 0;
            changeRec.fSubtractFeeFromAmount = false;
            changeRec.scriptOverride = scriptChange;
            vecSend.push_back(changeRec);
        }
    }

    // Create the transaction
    auto res = CreateTransaction(wallet, vecSend, std::nullopt, coinControlCopy, true);
    if (!res) {
        error = std::make_pair(RPC_TRANSACTION_ERROR, util::ErrorString(res).original);
        return false;
    }

    txRef = res->tx;
    nFeeRequired = res->fee;
    return true;
}

bool CreateReissueAssetTransaction(
    CWallet& wallet,
    CCoinControl& coinControl,
    const CReissueAsset& reissueAsset,
    const std::string& address,
    std::pair<int, std::string>& error,
    CTransactionRef& txRef,
    CAmount& nFeeRequired,
    std::string* verifier_string)
{
    std::vector<CRecipient> vecSend;
    std::string asset_name = reissueAsset.strName;

    // Get the asset type
    AssetType asset_type = AssetType::INVALID;
    IsAssetNameValid(asset_name, asset_type);

    // Validate destination address
    if (!IsValidDestinationString(address)) {
        error = std::make_pair(RPC_INVALID_ADDRESS_OR_KEY, std::string("Invalid Avian address: ") + address);
        return false;
    }

    // Build change address
    std::string change_address;
    if (!std::get_if<CNoDestination>(&coinControl.destChange)) {
        change_address = EncodeDestination(coinControl.destChange);
    }

    if (!change_address.empty()) {
        CTxDestination destination = DecodeDestination(change_address);
        if (!IsValidDestination(destination)) {
            error = std::make_pair(RPC_INVALID_ADDRESS_OR_KEY, std::string("Invalid Avian address: ") + change_address);
            return false;
        }
    } else {
        CTxDestination changeDest;
        std::string strFailReason;
        if (!GetChangeAddress(wallet, changeDest, strFailReason)) {
            error = std::make_pair(RPC_WALLET_KEYPOOL_RAN_OUT, strFailReason);
            return false;
        }
        change_address = EncodeDestination(changeDest);
        coinControl.destChange = changeDest;
    }

    // Cannot reissue owner tokens
    if (IsAssetNameAnOwner(asset_name)) {
        error = std::make_pair(RPC_INVALID_PARAMS, std::string("Owner Assets are not able to be reissued"));
        return false;
    }

    // Validate reissue data
    std::string strError;
    if (!ContextualCheckReissueAsset(passets, reissueAsset, strError)) {
        error = std::make_pair(RPC_VERIFY_ERROR, std::string("Failed to create reissue asset object. Error: ") + strError);
        return false;
    }

    LOCK2(cs_main, wallet.cs_wallet);

    // Verify ownership
    std::string stripped_asset_name = asset_name.substr(1, asset_name.size() - 1);
    if (asset_type == AssetType::RESTRICTED) {
        if (!VerifyWalletHasAsset(wallet, stripped_asset_name + OWNER_TAG, error))
            return false;
    } else {
        if (!VerifyWalletHasAsset(wallet, asset_name + OWNER_TAG, error))
            return false;
    }

    // Check wallet balance
    CAmount burnAmount = GetBurnAmount(AssetType::REISSUE);
    Balance bal = GetBalance(wallet);
    if (bal.m_mine_trusted < burnAmount) {
        error = std::make_pair(RPC_WALLET_INSUFFICIENT_FUNDS, "Insufficient funds");
        return false;
    }

    // Owner token transfer back to change address
    CScript scriptTransferOwner = GetScriptForDestination(DecodeDestination(change_address));
    if (asset_type == AssetType::RESTRICTED) {
        CAssetTransfer ownerTransfer(stripped_asset_name + OWNER_TAG, OWNER_ASSET_AMOUNT);
        ownerTransfer.ConstructTransaction(scriptTransferOwner);
    } else {
        CAssetTransfer ownerTransfer(asset_name + OWNER_TAG, OWNER_ASSET_AMOUNT);
        ownerTransfer.ConstructTransaction(scriptTransferOwner);
    }

    // Handle restricted asset verifier string
    if (asset_type == AssetType::RESTRICTED) {
        if (verifier_string) {
            if (reissueAsset.nAmount > 0) {
                std::string strErr;
                if (!ContextualCheckVerifierString(passets, *verifier_string, address, strErr)) {
                    error = std::make_pair(RPC_INVALID_PARAMETER, strErr);
                    return false;
                }
            } else {
                std::string strErr;
                if (!ContextualCheckVerifierString(passets, *verifier_string, "", strErr)) {
                    error = std::make_pair(RPC_INVALID_PARAMETER, strErr);
                    return false;
                }
            }

            CScript verifierScript;
            CNullAssetTxVerifierString verifier(*verifier_string);
            verifier.ConstructTransaction(verifierScript);

            CRecipient verifierRec;
            verifierRec.dest = CNoDestination();
            verifierRec.nAmount = 0;
            verifierRec.fSubtractFeeFromAmount = false;
            verifierRec.scriptOverride = verifierScript;
            vecSend.push_back(verifierRec);
        } else {
            // If reissuing more supply, validate against existing verifier string
            if (reissueAsset.nAmount > 0) {
                CNullAssetTxVerifierString verifier;
                if (passets->GetAssetVerifierStringIfExists(reissueAsset.strName, verifier)) {
                    std::string strErr;
                    if (!ContextualCheckVerifierString(passets, verifier.verifier_string, address, strErr)) {
                        error = std::make_pair(RPC_INVALID_PARAMETER, strErr);
                        return false;
                    }
                }
            }
        }
    }

    // Burn output
    CScript scriptPubKeyBurn = GetScriptForDestination(DecodeDestination(GetBurnAddress(AssetType::REISSUE)));
    CRecipient burnRec;
    burnRec.dest = CNoDestination();
    burnRec.nAmount = burnAmount;
    burnRec.fSubtractFeeFromAmount = false;
    burnRec.scriptOverride = scriptPubKeyBurn;
    vecSend.push_back(burnRec);

    // Owner token transfer output
    CRecipient ownerRec;
    ownerRec.dest = CNoDestination();
    ownerRec.nAmount = 0;
    ownerRec.fSubtractFeeFromAmount = false;
    ownerRec.scriptOverride = scriptTransferOwner;
    vecSend.push_back(ownerRec);

    // Reissue asset output
    CScript scriptReissue = GetScriptForDestination(DecodeDestination(address));
    reissueAsset.ConstructTransaction(scriptReissue);

    CRecipient reissueRec;
    reissueRec.dest = CNoDestination();
    reissueRec.nAmount = 0;
    reissueRec.fSubtractFeeFromAmount = false;
    reissueRec.scriptOverride = scriptReissue;
    vecSend.push_back(reissueRec);

    // Pre-select owner token UTXO
    CoinFilterParams coin_params;
    coin_params.min_amount = 0;
    CoinsResult available = AvailableCoinsWithAssets(wallet, nullptr, std::nullopt, coin_params);

    std::string ownerTokenName;
    if (asset_type == AssetType::RESTRICTED) {
        ownerTokenName = stripped_asset_name + OWNER_TAG;
    } else {
        ownerTokenName = asset_name + OWNER_TAG;
    }

    auto it = available.mapAssetCoins.find(ownerTokenName);
    if (it != available.mapAssetCoins.end() && !it->second.empty()) {
        coinControl.Select(it->second[0].outpoint);
    }

    coinControl.m_allow_other_inputs = true;

    // Create the transaction
    auto res = CreateTransaction(wallet, vecSend, std::nullopt, coinControl, true);
    if (!res) {
        error = std::make_pair(RPC_WALLET_ERROR, util::ErrorString(res).original);
        return false;
    }

    txRef = res->tx;
    nFeeRequired = res->fee;
    return true;
}

bool SendAssetTransaction(
    CWallet& wallet,
    CTransactionRef& tx,
    std::pair<int, std::string>& error,
    std::string& txid)
{
    wallet.CommitTransaction(tx, {}, {});
    txid = tx->GetHash().GetHex();
    return true;
}

} // namespace wallet
