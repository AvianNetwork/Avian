// Copyright (c) 2021 The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <founder_payment.h>

#include <key_io.h>
#include <script/solver.h>

CAmount FounderPayment::getFounderPaymentAmount(int blockHeight, CAmount blockReward) const
{
    if (blockHeight <= startBlock) {
        return 0;
    }

    for (size_t i = 0; i < rewardStructures.size(); i++) {
        const Consensus::FounderRewardStructure& rewardStructure = rewardStructures[i];
        if (rewardStructure.blockHeight == INT_MAX || blockHeight <= rewardStructure.blockHeight) {
            return blockReward * rewardStructure.rewardPercentage / 100;
        }
    }
    return 0;
}

void FounderPayment::FillFounderPayment(CMutableTransaction& txNew, int nBlockHeight, CAmount blockReward, CTxOut& txoutFounderRet) const
{
    CAmount founderPayment = getFounderPaymentAmount(nBlockHeight, blockReward);
    txoutFounderRet = CTxOut();

    if (founderPayment == 0) {
        return;
    }

    // Decode founder address and build payee script
    CTxDestination dest = DecodeDestination(founderAddress);
    CScript payee = GetScriptForDestination(dest);

    // Reduce miner reward and add founder output
    txNew.vout[0].nValue -= founderPayment;
    txoutFounderRet = CTxOut(founderPayment, payee);
    txNew.vout.push_back(txoutFounderRet);
}

bool FounderPayment::IsBlockPayeeValid(const CTransaction& txNew, const int height, const CAmount blockReward) const
{
    const CAmount founderReward = getFounderPaymentAmount(height, blockReward);
    if (founderReward == 0) {
        return true;
    }

    // Decode founder address and build expected payee script
    CTxDestination dest = DecodeDestination(founderAddress);
    CScript payee = GetScriptForDestination(dest);

    for (const CTxOut& out : txNew.vout) {
        if (out.scriptPubKey == payee && out.nValue >= founderReward) {
            return true;
        }
    }

    return false;
}
