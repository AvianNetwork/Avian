#ifndef AVN_FOUNDER_PAYMENT_H
#define AVN_FOUNDER_PAYMENT_H

#include "amount.h"
#include "primitives/transaction.h"
#include "script/standard.h"
#include <limits.h>
#include <string>

using namespace std;

static const string DEFAULT_FOUNDER_ADDRESS = "RDs4A4sDHp4otDHQQuFSaPDYEg2xx3hbdN";

struct FounderRewardStructure {
    int blockHeight;
    int rewardPercentage;
};

class FounderPayment
{
public:
    FounderPayment(vector<FounderRewardStructure> rewardStructures = {}, int startBlock = 0, const string& address = DEFAULT_FOUNDER_ADDRESS)
    {
        this->founderAddress = address;
        this->startBlock = startBlock;
        this->rewardStructures = rewardStructures;
    }
    ~FounderPayment(){};
    CAmount getFounderPaymentAmount(int blockHeight, CAmount blockReward);
    void FillFounderPayment(CMutableTransaction& txNew, int nBlockHeight, CAmount blockReward, CTxOut& txoutFounderRet);
    bool IsBlockPayeeValid(const CTransaction& txNew, const int height, const CAmount blockReward);
    int getStartBlock() { return this->startBlock; }

private:
    string founderAddress;
    int startBlock;
    vector<FounderRewardStructure> rewardStructures;
};


#endif /* AVN_FOUNDER_PAYMENT_H */
