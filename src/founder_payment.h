#ifndef AVN_FOUNDER_PAYMENT_H
#define AVN_FOUNDER_PAYMENT_H

#include "amount.h"
#include "primitives/transaction.h"
#include "script/standard.h"
#include <limits.h>
#include <string>

using namespace std;

static const string DEFAULT_FOUNDER_ADDRESS = "rPC7kPCNPAVnUvQs4fWEvnFwJ4yfKvArXM";

struct FounderRewardStructure {
    string founderAddress;
    int startBlock;
    int blockHeight;
    int rewardPercentage;
};

class FounderPayment
{
public:
    FounderPayment(vector<FounderRewardStructure> rewardStructures = {})
    {
        this->rewardStructures = rewardStructures;
    }
    ~FounderPayment(){};
    CAmount getFounderPaymentAmount(int blockHeight, CAmount blockReward);
    string getFounderPaymentAddress(int blockHeight);
    void FillFounderPayment(CMutableTransaction& txNew, int nBlockHeight, CAmount blockReward, CTxOut& txoutFounderRet);
    bool IsBlockPayeeValid(const CTransaction& txNew, const int height, const CAmount blockReward);
    bool IsFounderPaymentsStarted(int blockHeight);

private:
    vector<FounderRewardStructure> rewardStructures;
};


#endif /* AVN_FOUNDER_PAYMENT_H */
