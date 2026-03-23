// Copyright (c) 2017-2019 The Raven Core developers
// Copyright (c) 2020-2024 The Avian developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WALLET_ASSET_TX_H
#define BITCOIN_WALLET_ASSET_TX_H

#include <assets/assettypes.h>
#include <consensus/amount.h>
#include <sync.h>

#include <string>
#include <utility>
#include <vector>

class CScript;
class uint256;

namespace wallet {

class CWallet;
class CCoinControl;

//! Check if the wallet owns a given asset (has available UTXOs for it)
//! Caller must hold wallet.cs_wallet (enforced via AssertLockHeld in the definition).
bool VerifyWalletHasAsset(const CWallet& wallet, const std::string& asset_name, std::pair<int, std::string>& error);

//! Create a new asset issuance transaction
bool CreateAssetTransaction(
    CWallet& wallet,
    CCoinControl& coinControl,
    const CNewAsset& asset,
    const std::string& address,
    std::pair<int, std::string>& error,
    CTransactionRef& txRef,
    CAmount& nFeeRequired,
    std::string* verifier_string = nullptr);

//! Create a new asset issuance transaction (batch of unique assets)
bool CreateAssetTransaction(
    CWallet& wallet,
    CCoinControl& coinControl,
    const std::vector<CNewAsset>& assets,
    const std::string& address,
    std::pair<int, std::string>& error,
    CTransactionRef& txRef,
    CAmount& nFeeRequired,
    std::string* verifier_string = nullptr);

//! Create an asset transfer transaction
bool CreateTransferAssetTransaction(
    CWallet& wallet,
    const CCoinControl& coinControl,
    const std::vector<std::pair<CAssetTransfer, std::string>>& vTransfers,
    const std::string& changeAddress,
    std::pair<int, std::string>& error,
    CTransactionRef& txRef,
    CAmount& nFeeRequired,
    std::vector<std::pair<CNullAssetTxData, std::string>>* nullAssetTxData = nullptr,
    std::vector<CNullAssetTxData>* nullGlobalRestrictionData = nullptr);

//! Create an asset reissue transaction
bool CreateReissueAssetTransaction(
    CWallet& wallet,
    CCoinControl& coinControl,
    const CReissueAsset& reissueAsset,
    const std::string& address,
    std::pair<int, std::string>& error,
    CTransactionRef& txRef,
    CAmount& nFeeRequired,
    std::string* verifier_string = nullptr);

//! Commit an asset transaction to the wallet and broadcast
bool SendAssetTransaction(
    CWallet& wallet,
    CTransactionRef& tx,
    std::pair<int, std::string>& error,
    std::string& txid);

} // namespace wallet

#endif // BITCOIN_WALLET_ASSET_TX_H
