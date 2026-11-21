// Copyright (c) 2017-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <consensus/tx_verify.h>

#include <chain.h>
#include <coins.h>
#include <consensus/amount.h>
#include <consensus/consensus.h>
#include <consensus/validation.h>
#include <primitives/transaction.h>
#include <script/interpreter.h>
#include <util/check.h>
#include <util/moneystr.h>

// AVN: Asset system includes
#include <assets/assets.h>
#include <assets/assettypes.h>
#include <assets/messages.h>
#include <chainparams.h>
#include <key_io.h>
#include <logging.h>
#include <tinyformat.h>
#include <util/time.h>

#include <map>
#include <set>
#include <string>
#include <utility>

bool IsFinalTx(const CTransaction &tx, int nBlockHeight, int64_t nBlockTime)
{
    if (tx.nLockTime == 0)
        return true;
    if ((int64_t)tx.nLockTime < ((int64_t)tx.nLockTime < LOCKTIME_THRESHOLD ? (int64_t)nBlockHeight : nBlockTime))
        return true;

    // Even if tx.nLockTime isn't satisfied by nBlockHeight/nBlockTime, a
    // transaction is still considered final if all inputs' nSequence ==
    // SEQUENCE_FINAL (0xffffffff), in which case nLockTime is ignored.
    //
    // Because of this behavior OP_CHECKLOCKTIMEVERIFY/CheckLockTime() will
    // also check that the spending input's nSequence != SEQUENCE_FINAL,
    // ensuring that an unsatisfied nLockTime value will actually cause
    // IsFinalTx() to return false here:
    for (const auto& txin : tx.vin) {
        if (!(txin.nSequence == CTxIn::SEQUENCE_FINAL))
            return false;
    }
    return true;
}

std::pair<int, int64_t> CalculateSequenceLocks(const CTransaction &tx, int flags, std::vector<int>& prevHeights, const CBlockIndex& block)
{
    assert(prevHeights.size() == tx.vin.size());

    // Will be set to the equivalent height- and time-based nLockTime
    // values that would be necessary to satisfy all relative lock-
    // time constraints given our view of block chain history.
    // The semantics of nLockTime are the last invalid height/time, so
    // use -1 to have the effect of any height or time being valid.
    int nMinHeight = -1;
    int64_t nMinTime = -1;

    bool fEnforceBIP68 = tx.version >= 2 && flags & LOCKTIME_VERIFY_SEQUENCE;

    // Do not enforce sequence numbers as a relative lock time
    // unless we have been instructed to
    if (!fEnforceBIP68) {
        return std::make_pair(nMinHeight, nMinTime);
    }

    for (size_t txinIndex = 0; txinIndex < tx.vin.size(); txinIndex++) {
        const CTxIn& txin = tx.vin[txinIndex];

        // Sequence numbers with the most significant bit set are not
        // treated as relative lock-times, nor are they given any
        // consensus-enforced meaning at this point.
        if (txin.nSequence & CTxIn::SEQUENCE_LOCKTIME_DISABLE_FLAG) {
            // The height of this input is not relevant for sequence locks
            prevHeights[txinIndex] = 0;
            continue;
        }

        int nCoinHeight = prevHeights[txinIndex];

        if (txin.nSequence & CTxIn::SEQUENCE_LOCKTIME_TYPE_FLAG) {
            const int64_t nCoinTime{Assert(block.GetAncestor(std::max(nCoinHeight - 1, 0)))->GetMedianTimePast()};
            // NOTE: Subtract 1 to maintain nLockTime semantics
            // BIP 68 relative lock times have the semantics of calculating
            // the first block or time at which the transaction would be
            // valid. When calculating the effective block time or height
            // for the entire transaction, we switch to using the
            // semantics of nLockTime which is the last invalid block
            // time or height.  Thus we subtract 1 from the calculated
            // time or height.

            // Time-based relative lock-times are measured from the
            // smallest allowed timestamp of the block containing the
            // txout being spent, which is the median time past of the
            // block prior.
            nMinTime = std::max(nMinTime, nCoinTime + (int64_t)((txin.nSequence & CTxIn::SEQUENCE_LOCKTIME_MASK) << CTxIn::SEQUENCE_LOCKTIME_GRANULARITY) - 1);
        } else {
            nMinHeight = std::max(nMinHeight, nCoinHeight + (int)(txin.nSequence & CTxIn::SEQUENCE_LOCKTIME_MASK) - 1);
        }
    }

    return std::make_pair(nMinHeight, nMinTime);
}

bool EvaluateSequenceLocks(const CBlockIndex& block, std::pair<int, int64_t> lockPair)
{
    assert(block.pprev);
    int64_t nBlockTime = block.pprev->GetMedianTimePast();
    if (lockPair.first >= block.nHeight || lockPair.second >= nBlockTime)
        return false;

    return true;
}

bool SequenceLocks(const CTransaction &tx, int flags, std::vector<int>& prevHeights, const CBlockIndex& block)
{
    return EvaluateSequenceLocks(block, CalculateSequenceLocks(tx, flags, prevHeights, block));
}

unsigned int GetLegacySigOpCount(const CTransaction& tx)
{
    unsigned int nSigOps = 0;
    for (const auto& txin : tx.vin)
    {
        nSigOps += txin.scriptSig.GetSigOpCount(false);
    }
    for (const auto& txout : tx.vout)
    {
        nSigOps += txout.scriptPubKey.GetSigOpCount(false);
    }
    return nSigOps;
}

unsigned int GetP2SHSigOpCount(const CTransaction& tx, const CCoinsViewCache& inputs)
{
    if (tx.IsCoinBase())
        return 0;

    unsigned int nSigOps = 0;
    for (unsigned int i = 0; i < tx.vin.size(); i++)
    {
        const Coin& coin = inputs.AccessCoin(tx.vin[i].prevout);
        assert(!coin.IsSpent());
        const CTxOut &prevout = coin.out;
        if (prevout.scriptPubKey.IsPayToScriptHash())
            nSigOps += prevout.scriptPubKey.GetSigOpCount(tx.vin[i].scriptSig);
    }
    return nSigOps;
}

int64_t GetTransactionSigOpCost(const CTransaction& tx, const CCoinsViewCache& inputs, uint32_t flags)
{
    int64_t nSigOps = GetLegacySigOpCount(tx) * WITNESS_SCALE_FACTOR;

    if (tx.IsCoinBase())
        return nSigOps;

    if (flags & SCRIPT_VERIFY_P2SH) {
        nSigOps += GetP2SHSigOpCount(tx, inputs) * WITNESS_SCALE_FACTOR;
    }

    for (unsigned int i = 0; i < tx.vin.size(); i++)
    {
        const Coin& coin = inputs.AccessCoin(tx.vin[i].prevout);
        assert(!coin.IsSpent());
        const CTxOut &prevout = coin.out;
        nSigOps += CountWitnessSigOps(tx.vin[i].scriptSig, prevout.scriptPubKey, &tx.vin[i].scriptWitness, flags);
    }
    return nSigOps;
}

bool Consensus::CheckTxInputs(const CTransaction& tx, TxValidationState& state, const CCoinsViewCache& inputs, int nSpendHeight, CAmount& txfee)
{
    // are the actual inputs available?
    if (!inputs.HaveInputs(tx)) {
        return state.Invalid(TxValidationResult::TX_MISSING_INPUTS, "bad-txns-inputs-missingorspent",
                         strprintf("%s: inputs missing/spent", __func__));
    }

    CAmount nValueIn = 0;
    for (unsigned int i = 0; i < tx.vin.size(); ++i) {
        const COutPoint &prevout = tx.vin[i].prevout;
        const Coin& coin = inputs.AccessCoin(prevout);
        assert(!coin.IsSpent());

        // AVN: an unspendable script never becomes a coin, so there is nothing here to spend.
        // A node that ran 5.0.x still has the tag, restriction, verifier and zero-amount asset
        // outputs it stored as coins before IsUnspendable() covered them. Without this it would
        // accept a spend of one, which a 4.x node and a freshly synced node both refuse.
        if (coin.out.scriptPubKey.IsUnspendable()) {
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-inputs-missingorspent",
                                 strprintf("%s: input %s has an unspendable script", __func__, prevout.ToString()));
        }

        // If prev is coinbase, check that it's matured
        if (coin.IsCoinBase() && nSpendHeight - coin.nHeight < COINBASE_MATURITY) {
            return state.Invalid(TxValidationResult::TX_PREMATURE_SPEND, "bad-txns-premature-spend-of-coinbase",
                strprintf("tried to spend coinbase at depth %d", nSpendHeight - coin.nHeight));
        }

        // Check for negative or overflow input values
        nValueIn += coin.out.nValue;
        if (!MoneyRange(coin.out.nValue) || !MoneyRange(nValueIn)) {
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-inputvalues-outofrange");
        }
    }

    const CAmount value_out = tx.GetValueOut();
    if (nValueIn < value_out) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-in-belowout",
            strprintf("value in (%s) < value out (%s)", FormatMoney(nValueIn), FormatMoney(value_out)));
    }

    // Tally transaction fees
    const CAmount txfee_aux = nValueIn - value_out;
    if (!MoneyRange(txfee_aux)) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-fee-outofrange");
    }

    txfee = txfee_aux;
    return true;
}

// AVN: fMessaging is defined in init.cpp
extern bool fMessaging;

namespace {
//! Whether OP_AVN_ASSET appears as an opcode, not as pushed data, anywhere in the script.
bool HasAssetOpcode(const CScript& script)
{
    CScript::const_iterator pc = script.begin();
    opcodetype opcode;
    while (pc < script.end()) {
        if (!script.GetOp(pc, opcode)) break;
        if (opcode == OP_AVN_ASSET) return true;
    }
    return false;
}
} // namespace

bool Consensus::CheckTxAssetStructure(const CTransaction& tx, TxValidationState& state)
{
    const auto invalid = [&state](const std::string& reason) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, reason);
    };

    // CheckTransaction() rejects this first; the per-type helpers below index tx.vout.
    if (tx.vout.empty()) return invalid("bad-txns-vout-empty");

    std::set<std::string> setAssetTransferNames;
    std::map<std::pair<std::string, std::string>, int> mapNullDataTxCount; // (asset name, address) -> count
    std::set<std::string> setNullGlobalAssetChanges;
    bool fContainsNewRestrictedAsset = false;
    bool fContainsRestrictedAssetReissue = false;
    bool fContainsNullAssetVerifierTx = false;
    int nCountAddTagOuts = 0;

    for (const auto& txout : tx.vout) {
        // Qualifier tags, address restrictions, global restrictions and verifier strings
        if (txout.scriptPubKey.IsNullAsset()) {
            CNullAssetTxData data;
            std::string address;
            std::string strError;

            if (txout.scriptPubKey.IsNullAssetTxDataScript()) {
                if (!AssetNullDataFromScript(txout.scriptPubKey, data, address))
                    return invalid("bad-txns-null-asset-data-serialization");

                if (!VerifyNullAssetDataFlag(data.flag, strError))
                    return invalid(strError);

                if (++mapNullDataTxCount[std::make_pair(data.asset_name, address)] > 1)
                    return invalid("bad-txns-null-data-only-one-change-per-asset-address");

                // Each qualifier that is added costs a burn fee
                if (IsAssetNameAQualifier(data.asset_name) && data.flag == (int)QualifierType::ADD_QUALIFIER) {
                    nCountAddTagOuts++;
                }
            } else if (txout.scriptPubKey.IsNullGlobalRestrictionAssetTxDataScript()) {
                if (!GlobalAssetNullDataFromScript(txout.scriptPubKey, data))
                    return invalid("bad-txns-null-global-asset-data-serialization");

                if (!VerifyNullAssetDataFlag(data.flag, strError))
                    return invalid(strError);

                if (!setNullGlobalAssetChanges.insert(data.asset_name).second)
                    return invalid("bad-txns-null-data-only-one-global-change-per-asset-name");
            } else if (txout.scriptPubKey.IsNullAssetVerifierTxDataScript()) {
                if (!CheckVerifierAssetTxOut(txout, strError))
                    return invalid(strError);

                if (fContainsNullAssetVerifierTx)
                    return invalid("bad-txns-null-data-only-one-verifier-per-tx");

                fContainsNullAssetVerifierTx = true;
            }
        }

        int nType = 0;
        bool fIsOwner = false;
        if (!txout.scriptPubKey.IsAssetScript(nType, fIsOwner)) continue;

        if (nType == TX_TRANSFER_ASSET) {
            CAssetTransfer transfer;
            std::string address;
            if (!TransferAssetFromScript(txout.scriptPubKey, transfer, address))
                return invalid("bad-txns-transfer-asset-bad-deserialize");

            // Remembered so that null asset data can be matched to a transfer below
            setAssetTransferNames.insert(transfer.strName);

            AssetType assetType;
            if (!IsAssetNameValid(transfer.strName, assetType))
                return invalid("bad-txns-transfer-asset-name-invalid");

            if (IsAssetNameAnOwner(transfer.strName) && transfer.nAmount != OWNER_ASSET_AMOUNT)
                return invalid("bad-txns-transfer-owner-amount-was-not-1");

            if (assetType == AssetType::UNIQUE && transfer.nAmount != UNIQUE_ASSET_AMOUNT)
                return invalid("bad-txns-transfer-unique-amount-was-not-1");

            if ((assetType == AssetType::QUALIFIER || assetType == AssetType::SUB_QUALIFIER) &&
                (transfer.nAmount < QUALIFIER_ASSET_MIN_AMOUNT || transfer.nAmount > QUALIFIER_ASSET_MAX_AMOUNT))
                return invalid("bad-txns-transfer-qualifier-amount-must be between 1 - 100");

            // Asset outputs never carry AVN
            if (txout.nValue != 0)
                return invalid("bad-txns-asset-transfer-amount-isn't-zero");
        } else if (nType == TX_NEW_ASSET) {
            if (txout.nValue != 0)
                return invalid("bad-txns-asset-issued-amount-isn't-zero");
        } else if (nType == TX_REISSUE_ASSET) {
            if (txout.nValue != 0)
                return invalid("bad-txns-asset-reissued-amount-isn't-zero");
        } else {
            return invalid("bad-asset-type-not-any-of-the-main-three");
        }
    }

    if (nCountAddTagOuts && !CheckAddingTagBurnFee(tx, nCountAddTagOuts))
        return invalid("bad-txns-tx-doesn't-contain-required-burn-fee-for-adding-tags");

    // Tagging or restricting an address is authorised by transferring the controlling asset
    // in the same transaction: the owner token for $RESTRICTED, the qualifier itself for #TAG.
    for (const auto& [key, count] : mapNullDataTxCount) {
        const std::string& name = key.first;
        if (!name.empty() && name.front() == RESTRICTED_CHAR) {
            if (!setAssetTransferNames.count(name.substr(1) + OWNER_TAG)) // $TOKEN -> TOKEN!
                return invalid("bad-txns-tx-contains-restricted-asset-null-tx-without-asset-transfer");
        } else {
            if (!setAssetTransferNames.count(name))
                return invalid("bad-txns-tx-contains-qualifier-asset-null-tx-without-asset-transfer");
        }
    }

    for (const auto& name : setNullGlobalAssetChanges) {
        if (name.empty())
            return invalid("bad-txns-tx-contains-global-asset-null-tx-with-null-asset-name");

        if (!setAssetTransferNames.count(name.substr(1) + OWNER_TAG)) // $TOKEN -> TOKEN!
            return invalid("bad-txns-tx-contains-global-asset-null-tx-without-asset-transfer");
    }

    if (tx.IsCoinBase()) {
        for (const auto& vout : tx.vout) {
            if (vout.scriptPubKey.IsAssetScript() || vout.scriptPubKey.IsNullAsset())
                return invalid("bad-txns-coinbase-contains-asset-txes");
        }
    }

    // The burn and layout checks for each type (VerifyNewAsset() and friends) are in CheckTxAssets().
    if (IsNewAsset(tx)) {
        std::string strError;
        CNewAsset asset;
        std::string strAddress;
        if (!AssetFromTransaction(tx, asset, strAddress))
            return invalid("bad-txns-issue-asset-from-transaction");

        if (!IsNewOwnerTxValid(tx, asset.strName, strAddress, strError))
            return invalid(strError);

        if (!CheckNewAsset(asset, strError))
            return invalid(strError);
    } else if (IsReissueAsset(tx)) {
        std::string strError;
        CReissueAsset reissue;
        std::string strAddress;
        if (!ReissueAssetFromTransaction(tx, reissue, strAddress))
            return invalid("bad-txns-reissue-asset");

        if (!CheckReissueAsset(reissue, strError))
            return invalid(strError);

        AssetType type;
        IsAssetNameValid(reissue.strName, type);
        if (type == AssetType::RESTRICTED) {
            // A restricted reissue may change the verifier string; it need not carry one
            CNullAssetTxVerifierString new_verifier;
            bool fNotFound = false;
            if (!GetVerifierStringFromTx(tx, new_verifier, strError, fNotFound) && !fNotFound)
                return invalid("bad-txns-reissue-restricted-verifier-" + strError);

            fContainsRestrictedAssetReissue = true;
        }
    } else if (IsNewUniqueAsset(tx)) {
        std::string strError;
        for (const auto& out : tx.vout) {
            if (!IsScriptNewUniqueAsset(out.scriptPubKey)) continue;

            CNewAsset asset;
            std::string strAddress;
            if (!AssetFromScript(out.scriptPubKey, asset, strAddress))
                return invalid("bad-txns-check-transaction-issue-unique-asset-serialization");

            if (!CheckNewAsset(asset, strError))
                return invalid("bad-txns-issue-unique" + strError);
        }
    } else if (IsNewMsgChannelAsset(tx)) {
        std::string strError;
        CNewAsset asset;
        std::string strAddress;
        if (!MsgChannelAssetFromTransaction(tx, asset, strAddress))
            return invalid("bad-txns-issue-msgchannel-from-transaction");

        if (!CheckNewAsset(asset, strError))
            return invalid("bad-txns-issue-msgchannel" + strError);
    } else if (IsNewQualifierAsset(tx)) {
        std::string strError;
        CNewAsset asset;
        std::string strAddress;
        if (!QualifierAssetFromTransaction(tx, asset, strAddress))
            return invalid("bad-txns-issue-qualifier-from-transaction");

        if (!CheckNewAsset(asset, strError))
            return invalid("bad-txns-issue-qualfier" + strError);
    } else if (IsNewRestrictedAsset(tx)) {
        std::string strError;
        CNewAsset asset;
        std::string strAddress;
        if (!RestrictedAssetFromTransaction(tx, asset, strAddress))
            return invalid("bad-txns-issue-restricted-from-transaction");

        if (!CheckNewAsset(asset, strError))
            return invalid("bad-txns-issue-restricted" + strError);

        CNullAssetTxVerifierString verifier;
        if (!GetVerifierStringFromTx(tx, verifier, strError))
            return invalid("bad-txns-issue-restricted-verifier-search-" + strError);

        fContainsNewRestrictedAsset = true;
    } else {
        // Anything else may only contain asset transfers, and OP_AVN_ASSET only where a
        // null asset data script puts it.
        for (const auto& out : tx.vout) {
            int nType = 0;
            bool fIsOwner = false;
            if (out.scriptPubKey.IsAssetScript(nType, fIsOwner)) {
                if (nType != TX_TRANSFER_ASSET)
                    return invalid("bad-txns-bad-asset-transaction");
            } else if (HasAssetOpcode(out.scriptPubKey) && out.scriptPubKey[0] != OP_AVN_ASSET) {
                return invalid("bad-txns-op-avn-asset-not-in-right-script-location");
            }
        }
    }

    // A verifier string only makes sense alongside a restricted asset issuance or reissuance
    if (fContainsNullAssetVerifierTx && !fContainsRestrictedAssetReissue && !fContainsNewRestrictedAsset)
        return invalid("bad-txns-tx-cointains-verifier-string-without-restricted-asset-issuance-or-reissuance");

    // ...and a restricted asset issuance must come with one
    if (fContainsNewRestrictedAsset && !fContainsNullAssetVerifierTx)
        return invalid("bad-txns-tx-cointains-restricted-asset-issuance-without-verifier");

    return true;
}

//! Check that asset inputs and outputs balance, and validate asset operations.
bool Consensus::CheckTxAssets(const CTransaction& tx, TxValidationState& state, const CCoinsViewCache& inputs,
                              CAssetsCache* assetCache, const CTxMemPool* mempool,
                              std::vector<std::pair<std::string, uint256>>& vPairReissueAssets,
                              const bool fRunningUnitTests, std::set<CMessage>* setMessages,
                              int64_t nBlocktime,
                              std::vector<std::pair<std::string, CNullAssetTxData>>* myNullAssetData,
                              int nSpendHeight)
{
    // are the actual inputs available?
    if (!inputs.HaveInputs(tx)) {
        return state.Invalid(TxValidationResult::TX_MISSING_INPUTS, "bad-txns-inputs-missing-or-spent",
                             strprintf("%s: inputs missing/spent", __func__));
    }

    // Create map that stores the amount of an asset transaction input. Used to verify no assets are burned
    std::map<std::string, CAmount> totalInputs;
    std::map<std::string, std::string> mapAddresses;

    for (unsigned int i = 0; i < tx.vin.size(); ++i) {
        const COutPoint &prevout = tx.vin[i].prevout;
        const Coin& coin = inputs.AccessCoin(prevout);
        assert(!coin.IsSpent());

        if (coin.out.scriptPubKey.IsAssetScript()) {
            CAssetOutputEntry data;
            if (!GetAssetData(coin.out.scriptPubKey, data))
                return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-failed-to-get-asset-from-script");

            // Add to the total value of assets in the inputs
            if (nSpendHeight >= ::Params().GetConsensus().nAssetTransferOverflowFixHeight) {
                if (!MoneyRange(data.nAmount))
                    return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-asset-input-amount-out-of-range");
                CAmount current = totalInputs[data.assetName];
                if (data.nAmount > MAX_MONEY - current)
                    return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-asset-inputs-amount-overflow");
                totalInputs[data.assetName] = current + data.nAmount;
            } else {
                if (totalInputs.count(data.assetName))
                    totalInputs.at(data.assetName) += data.nAmount;
                else
                    totalInputs.insert(make_pair(data.assetName, data.nAmount));
            }

            if (AreMessagesDeployed()) {
                mapAddresses.insert(make_pair(data.assetName, EncodeDestination(data.destination)));
            }

            if (IsAssetNameAnRestricted(data.assetName)) {
                if (assetCache && assetCache->CheckForAddressRestriction(data.assetName, EncodeDestination(data.destination), true)) {
                    return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-restricted-asset-transfer-from-frozen-address");
                }
            }
        }
    }

    // Create map that stores the amount of an asset transaction output. Used to verify no assets are burned
    std::map<std::string, CAmount> totalOutputs;
    int index = 0;
    int64_t currentTime = TicksSinceEpoch<std::chrono::seconds>(NodeClock::now());
    std::string strError = "";
    for (const auto& txout : tx.vout) {
        bool fIsAsset = false;
        int nType = 0;
        bool fIsOwner = false;
        if (txout.scriptPubKey.IsAssetScript(nType, fIsOwner))
            fIsAsset = true;

        if (assetCache) {
            if (fIsAsset && !AreAssetsDeployed())
                return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-is-asset-and-asset-not-active");

            if (txout.scriptPubKey.IsNullAsset()) {
                if (!AreRestrictedAssetsDeployed())
                    return state.Invalid(TxValidationResult::TX_CONSENSUS,
                                         "bad-tx-null-asset-data-before-restricted-assets-activated");

                if (txout.scriptPubKey.IsNullAssetTxDataScript()) {
                    if (!ContextualCheckNullAssetTxOut(txout, assetCache, strError, myNullAssetData))
                        return state.Invalid(TxValidationResult::TX_CONSENSUS, strError);
                } else if (txout.scriptPubKey.IsNullGlobalRestrictionAssetTxDataScript()) {
                    if (!ContextualCheckGlobalAssetTxOut(txout, assetCache, strError))
                        return state.Invalid(TxValidationResult::TX_CONSENSUS, strError);
                } else if (txout.scriptPubKey.IsNullAssetVerifierTxDataScript()) {
                    if (!ContextualCheckVerifierAssetTxOut(txout, assetCache, strError))
                        return state.Invalid(TxValidationResult::TX_CONSENSUS, strError);
                } else {
                    return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-tx-null-asset-data-unknown-type");
                }
            }
        }

        if (nType == TX_TRANSFER_ASSET) {
            CAssetTransfer transfer;
            std::string address = "";
            if (!TransferAssetFromScript(txout.scriptPubKey, transfer, address))
                return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-tx-asset-transfer-bad-deserialize");

            if (!ContextualCheckTransferAsset(assetCache, transfer, address, strError))
                return state.Invalid(TxValidationResult::TX_CONSENSUS, strError);

            // Add to the total value of assets in the outputs
            if (nSpendHeight >= ::Params().GetConsensus().nAssetTransferOverflowFixHeight) {
                if (!MoneyRange(transfer.nAmount))
                    return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-asset-transfer-amount-out-of-range");
                CAmount current = totalOutputs[transfer.strName];
                if (transfer.nAmount > MAX_MONEY - current)
                    return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-asset-outputs-amount-overflow");
                totalOutputs[transfer.strName] = current + transfer.nAmount;
            } else {
                if (totalOutputs.count(transfer.strName))
                    totalOutputs.at(transfer.strName) += transfer.nAmount;
                else
                    totalOutputs.insert(make_pair(transfer.strName, transfer.nAmount));
            }

            if (!fRunningUnitTests) {
                if (IsAssetNameAnOwner(transfer.strName)) {
                    if (transfer.nAmount != OWNER_ASSET_AMOUNT)
                        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-transfer-owner-amount-was-not-1");
                } else {
                    // For all other types of assets, make sure they are sending the right type of units
                    CNewAsset asset;
                    if (!assetCache->GetAssetMetaDataIfExists(transfer.strName, asset))
                        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-transfer-asset-not-exist");

                    if (asset.strName != transfer.strName)
                        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-asset-database-corrupted");

                    if (!CheckAmountWithUnits(transfer.nAmount, asset.units))
                        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-transfer-asset-amount-not-match-units");
                }
            }

            // Get messages from the transaction, only used when getting called from ConnectBlock
            if (AreMessagesDeployed() && fMessaging && setMessages) {
                if (IsAssetNameAnOwner(transfer.strName) || IsAssetNameAnMsgChannel(transfer.strName)) {
                    if (!transfer.message.empty()) {
                        if (transfer.nExpireTime == 0 || transfer.nExpireTime > currentTime) {
                            if (mapAddresses.count(transfer.strName)) {
                                if (mapAddresses.at(transfer.strName) == address) {
                                    COutPoint out(tx.GetHash(), index);
                                    CMessage message(out, transfer.strName, transfer.message,
                                                     transfer.nExpireTime, nBlocktime);
                                    setMessages->insert(message);
                                }
                            }
                        }
                    }
                }
            }
        } else if (nType == TX_REISSUE_ASSET) {
            CReissueAsset reissue;
            std::string address;
            if (!ReissueAssetFromScript(txout.scriptPubKey, reissue, address))
                return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-tx-asset-reissue-bad-deserialize");

            if (mapReissuedAssets.count(reissue.strName)) {
                if (mapReissuedAssets.at(reissue.strName) != tx.GetHash().ToUint256())
                    return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-tx-reissue-chaining-not-allowed");
            } else {
                vPairReissueAssets.emplace_back(std::make_pair(reissue.strName, tx.GetHash().ToUint256()));
            }
        }
        index++;
    }

    if (assetCache) {
        if (IsNewAsset(tx)) {
            // Restore the structural validation layer that 4.2.0 runs in CheckTransaction but the
            // Bitcoin Core 30.2 rewrite left as dead code (VerifyNewAsset had no call sites in 5.x).
            // It enforces the issuance burn, that the owner-token name matches the asset, the output
            // counts, and — for sub-assets — that the parent ROOT! owner token is held. Without it,
            // 5.x accepted issuances with no burn / a mismatched owner / no parent owner that 4.2.0
            // rejects, which both splits the chain and allows free/unauthorized issuance.
            if (!VerifyNewAsset(tx, strError))
                return state.Invalid(TxValidationResult::TX_CONSENSUS, strError);

            CNewAsset asset;
            std::string address;
            if (!AssetFromScript(tx.vout[tx.vout.size() - 1].scriptPubKey, asset, address)) {
                LogError("%s : Failed to get new asset from transaction: %s", __func__, tx.GetHash().GetHex());
                return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-issue-serialzation-failed");
            }

            AssetType assetType;
            IsAssetNameValid(asset.strName, assetType);

            if (!ContextualCheckNewAsset(assetCache, asset, strError, mempool))
                return state.Invalid(TxValidationResult::TX_CONSENSUS, strError);

        } else if (IsReissueAsset(tx)) {
            // Structural check (restored from 4.2.0): requires the NAME! owner-token transfer and the
            // reissue burn output, and enforces the reissue output counts. Without it, 5.x let anyone
            // reissue (mint more of) any reissuable asset they do NOT own, paying no burn — a
            // consensus split and an unauthorized-inflation bug.
            if (!VerifyReissueAsset(tx, strError))
                return state.Invalid(TxValidationResult::TX_CONSENSUS, strError);

            CReissueAsset reissue_asset;
            std::string address;
            if (!ReissueAssetFromScript(tx.vout[tx.vout.size() - 1].scriptPubKey, reissue_asset, address)) {
                LogError("%s : Failed to get reissue asset from transaction: %s", __func__, tx.GetHash().GetHex());
                return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-reissue-serialzation-failed");
            }
            if (!ContextualCheckReissueAsset(assetCache, reissue_asset, strError, tx))
                return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-reissue-contextual-" + strError);
        } else if (IsNewUniqueAsset(tx)) {
            // Structural check (restored from 4.2.0): requires the 5-AVN unique burn and that the
            // parent ROOT! owner token is held, and rejects any owner-token creation output. This
            // subsumes the explicit owner-output guard below, which is kept for defence in depth.
            if (!VerifyNewUniqueAsset(tx, strError))
                return state.Invalid(TxValidationResult::TX_CONSENSUS, strError);

            // Unique asset creation txs must NOT contain an owner token creation output
            // (TX_NEW_ASSET, fIsOwner=true). A unique asset has no owner token of its own; it
            // inherits authority from its parent root ROOT! owner, which is *transferred* (not newly
            // created) in the transaction. Any tx with an extra NAME#unique! creation output is
            // malformed. This mirrors the restricted-asset guard below and restores the rule that
            // VerifyNewUniqueAsset enforced (nOwners > 0 -> reject); that validator was left
            // disconnected from consensus during the Bitcoin Core rewrite, and its absence let 5.0.x
            // accept a unique issuance carrying an owner token that 4.2.0 rejects — splitting the chain.
            for (const auto& vout : tx.vout) {
                int nOutType = 0;
                bool fOutIsOwner = false;
                if (vout.scriptPubKey.IsAssetScript(nOutType, fOutIsOwner)) {
                    if (nOutType == TX_NEW_ASSET && fOutIsOwner) {
                        return state.Invalid(TxValidationResult::TX_CONSENSUS,
                                             "bad-txns-issue-unique-has-owner-creation-output");
                    }
                }
            }

            if (!ContextualCheckUniqueAssetTx(assetCache, strError, tx))
                return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-issue-unique-contextual-" + strError);
        } else if (IsNewMsgChannelAsset(tx)) {
            if (!AreMessagesDeployed())
                return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-issue-msgchannel-before-messaging-is-active");

            // Structural check (restored from 4.2.0): burn output present and output counts.
            if (!VerifyNewMsgChannelAsset(tx, strError))
                return state.Invalid(TxValidationResult::TX_CONSENSUS, strError);

            CNewAsset asset;
            std::string strAddress;
            if (!MsgChannelAssetFromTransaction(tx, asset, strAddress))
                return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-issue-msgchannel-serialzation-failed");

            if (!ContextualCheckNewAsset(assetCache, asset, strError, mempool))
                return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-issue-msgchannel-contextual-" + strError);
        } else if (IsNewQualifierAsset(tx)) {
            if (!AreRestrictedAssetsDeployed())
                return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-issue-qualifier-before-it-is-active");

            // Structural check (restored from 4.2.0): burn output present and output counts.
            if (!VerifyNewQualfierAsset(tx, strError))
                return state.Invalid(TxValidationResult::TX_CONSENSUS, strError);

            CNewAsset asset;
            std::string strAddress;
            if (!QualifierAssetFromTransaction(tx, asset, strAddress))
                return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-issue-qualifier-serialzation-failed");

            if (!ContextualCheckNewAsset(assetCache, asset, strError, mempool))
                return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-issue-qualfier-contextual" + strError);

        } else if (IsNewRestrictedAsset(tx)) {
            if (!AreRestrictedAssetsDeployed())
                return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-issue-restricted-before-it-is-active");

            // Structural check (restored from 4.2.0): burn output present, output counts, and the
            // root TOKEN! owner transfer. Subsumes the explicit owner-output guard below, which is
            // kept for defence in depth.
            if (!VerifyNewRestrictedAsset(tx, strError))
                return state.Invalid(TxValidationResult::TX_CONSENSUS, strError);

            // Restricted asset creation txs must NOT contain an owner token creation output (TX_NEW_ASSET, fIsOwner=true).
            // The root TOKEN! owner is transferred (not newly created) in these transactions. Any tx with an
            // extra $TOKEN! creation output is malformed — reject it so it cannot enter or persist in the mempool.
            for (const auto& vout : tx.vout) {
                int nOutType = 0;
                bool fOutIsOwner = false;
                if (vout.scriptPubKey.IsAssetScript(nOutType, fOutIsOwner)) {
                    if (nOutType == TX_NEW_ASSET && fOutIsOwner) {
                        return state.Invalid(TxValidationResult::TX_CONSENSUS,
                                             "bad-txns-issue-restricted-has-owner-creation-output");
                    }
                }
            }

            CNewAsset asset;
            std::string strAddress;
            if (!RestrictedAssetFromTransaction(tx, asset, strAddress))
                return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-issue-restricted-serialzation-failed");

            if (!ContextualCheckNewAsset(assetCache, asset, strError, mempool))
                return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-issue-restricted-contextual" + strError);

            // Get verifier string
            CNullAssetTxVerifierString verifier;
            if (!GetVerifierStringFromTx(tx, verifier, strError))
                return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-issue-restricted-verifier-search-" + strError);

            // Check the verifier string against the destination address
            if (!ContextualCheckVerifierString(assetCache, verifier.verifier_string, strAddress, strError))
                return state.Invalid(TxValidationResult::TX_CONSENSUS, strError);

        } else {
            // For transactions that are not asset issuance/reissuance, ensure only transfers are present
            for (const auto& out : tx.vout) {
                int nType;
                bool _isOwner;
                if (out.scriptPubKey.IsAssetScript(nType, _isOwner)) {
                    if (nType != TX_TRANSFER_ASSET) {
                        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-bad-asset-transaction");
                    }
                } else {
                    // Check if OP_AVN_ASSET appears in script but not as recognized asset script
                    bool hasAvnAssetOp = false;
                    CScript::const_iterator pc = out.scriptPubKey.begin();
                    opcodetype opcode;
                    while (pc < out.scriptPubKey.end()) {
                        if (!out.scriptPubKey.GetOp(pc, opcode))
                            break;
                        if (opcode == OP_AVN_ASSET) {
                            hasAvnAssetOp = true;
                            break;
                        }
                    }
                    if (hasAvnAssetOp) {
                        if (AreRestrictedAssetsDeployed()) {
                            if (out.scriptPubKey[0] != OP_AVN_ASSET) {
                                return state.Invalid(TxValidationResult::TX_CONSENSUS,
                                                     "bad-txns-op-avn-asset-not-in-right-script-location");
                            }
                        } else {
                            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-bad-asset-script");
                        }
                    }
                }
            }
        }
    }

    // Verify that input and output asset amounts match exactly
    for (const auto& outValue : totalOutputs) {
        if (!totalInputs.count(outValue.first)) {
            std::string errorMsg = strprintf("Bad Transaction - Trying to create outpoint for asset that you don't have: %s", outValue.first);
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-tx-inputs-outputs-mismatch " + errorMsg);
        }

        if (totalInputs.at(outValue.first) != outValue.second) {
            std::string errorMsg = strprintf("Bad Transaction - Assets would be burnt %s", outValue.first);
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-tx-inputs-outputs-mismatch " + errorMsg);
        }
    }

    // Check the input size and the output size
    if (totalOutputs.size() != totalInputs.size()) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-tx-asset-inputs-size-does-not-match-outputs-size");
    }

    return true;
}
