#include "founder_payment.h"

#include "base58.h"
#include "chainparams.h"
#include "util.h"
#include <boost/foreach.hpp>

CAmount FounderPayment::getFounderPaymentAmount(int blockHeight, CAmount blockReward)
{
    if (blockHeight <= startBlock) {
        return 0;
    }

    for (int i = 0; i < rewardStructures.size(); i++) {
        FounderRewardStructure rewardStructure = rewardStructures[i];
        if (rewardStructure.blockHeight == INT_MAX || blockHeight <= rewardStructure.blockHeight) {
            return blockReward * rewardStructure.rewardPercentage / 100;
        }
    }
    return 0;
}

void FounderPayment::FillFounderPayment(CMutableTransaction& txNew, int nBlockHeight, CAmount blockReward, CTxOut& txoutFounderRet)
{
    // Make sure it's not filled yet
    CAmount founderPayment = getFounderPaymentAmount(nBlockHeight, blockReward);
    txoutFounderRet = CTxOut();
    CScript payee;

    // Fill payee with the founder address
    CAvianAddress cbAddress(founderAddress);
    payee = GetScriptForDestination(cbAddress.Get());

    // Split reward between miner
    txNew.vout[0].nValue -= founderPayment;
    txoutFounderRet = CTxOut(founderPayment, payee);
    txNew.vout.push_back(txoutFounderRet);
}

bool FounderPayment::IsBlockPayeeValid(const CTransaction& txNew, const int height, const CAmount blockReward)
{
    CScript payee;
    // Fill payee with the founder address
    payee = GetScriptForDestination(CAvianAddress(founderAddress).Get());
    const CAmount founderReward = getFounderPaymentAmount(height, blockReward);
    BOOST_FOREACH (const CTxOut& out, txNew.vout) {
        if (out.scriptPubKey == payee && out.nValue >= founderReward) {
            return true;
        }
    }

    return false;
}
