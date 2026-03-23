// Copyright (c) 2021 The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_FOUNDER_PAYMENT_H
#define BITCOIN_FOUNDER_PAYMENT_H

#include <consensus/amount.h>
#include <consensus/params.h>
#include <primitives/transaction.h>
#include <script/script.h>

#include <string>
#include <vector>

static const std::string DEFAULT_FOUNDER_ADDRESS = "rPC7kPCNPAVnUvQs4fWEvnFwJ4yfKvArXM";

class FounderPayment
{
public:
    FounderPayment() = default;
    FounderPayment(const std::vector<Consensus::FounderRewardStructure>& rewardStructures, int startBlock, const std::string& address)
        : founderAddress(address), startBlock(startBlock), rewardStructures(rewardStructures) {}

    /** Construct from consensus params */
    explicit FounderPayment(const Consensus::Params& params)
        : founderAddress(params.founderAddress), startBlock(params.founderStartBlock), rewardStructures(params.founderRewardStructures) {}

    CAmount getFounderPaymentAmount(int blockHeight, CAmount blockReward) const;
    void FillFounderPayment(CMutableTransaction& txNew, int nBlockHeight, CAmount blockReward, CTxOut& txoutFounderRet) const;
    bool IsBlockPayeeValid(const CTransaction& txNew, const int height, const CAmount blockReward) const;
    int getStartBlock() const { return startBlock; }

private:
    std::string founderAddress{DEFAULT_FOUNDER_ADDRESS};
    int startBlock{0};
    std::vector<Consensus::FounderRewardStructure> rewardStructures;
};

#endif // BITCOIN_FOUNDER_PAYMENT_H
