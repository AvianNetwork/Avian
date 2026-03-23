// Copyright (c) 2017-2019 The Raven Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_ASSETS_REWARDS_H
#define BITCOIN_ASSETS_REWARDS_H

#include <consensus/amount.h>
#include <tinyformat.h>
#include <assets/assettypes.h>

#include <string>
#include <set>
#include <map>
#include <unordered_map>
#include <list>


class CRewardSnapshot;
class CRewardSnapshot;

extern std::map<uint256, CRewardSnapshot> mapRewardSnapshots;

//  Addresses are delimited by commas
static const std::string ADDRESS_COMMA_DELIMITER = ",";
const int MAX_PAYMENTS_PER_TRANSACTION = 1000;

//  Individual payment record
struct OwnerAndAmount
{
    std::string address;
    CAmount amount;

    OwnerAndAmount(
            const std::string & p_address,
            CAmount p_rewardAmt
    )
    {
        address = p_address;
        amount = p_rewardAmt;
    }

    bool operator<(const OwnerAndAmount &rhs) const
    {
        return address < rhs.address;
    }
};

class CRewardTransaction {
    uint256 txid;
    int nHeight;

    CRewardTransaction() {
        SetNull();
    }

    CRewardTransaction(const uint256& p_txid, const int& p_nBatch) {
        SetNull();
        txid = p_txid;

    }

    void SetNull() {
        txid.SetNull();
        nHeight = 0;
    }

    SERIALIZE_METHODS(CRewardTransaction, obj)
    {
        READWRITE(obj.nHeight);
        READWRITE(obj.txid);
    }


};

class CRewardSnapshot {
public:
    enum {
        REWARD_ERROR = 0,
        PROCESSING = 1,
        COMPLETE = 2,
        LOW_FUNDS = 3,
        NOT_ENOUGH_FEE = 4,
        LOW_REWARDS = 5,
        STUCK_TX = 6,
        NETWORK_ERROR = 7,
        FAILED_CREATE_TRANSACTION = 8,
        FAILED_COMMIT_TRANSACTION = 9
    };

    std::string strOwnershipAsset;
    std::string strDistributionAsset;
    std::string strExceptionAddresses;
    CAmount nDistributionAmount;
    uint32_t nHeight;
    int nStatus;

    CRewardSnapshot() {
        SetNull();
    }

    CRewardSnapshot(const std::string& p_strOwnershipAsset, const std::string& p_strDistributionAsset, const std::string& p_strExceptionAddresses, const CAmount& p_nDistributionAmount, const uint32_t& p_nHeight) {
        SetNull();
        strOwnershipAsset = p_strOwnershipAsset;
        strDistributionAsset = p_strDistributionAsset;
        strExceptionAddresses = p_strExceptionAddresses;
        nDistributionAmount = p_nDistributionAmount;
        nHeight = p_nHeight;
        nStatus = PROCESSING;
    }

    void SetNull() {
        strOwnershipAsset = "";
        strDistributionAsset = "";
        strExceptionAddresses = "";
        nDistributionAmount = 0;
        nHeight = 0;
        nStatus = REWARD_ERROR;
    }

    uint256 GetHash() const;

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        ::Serialize(s, strOwnershipAsset);
        ::Serialize(s, strDistributionAsset);
        ::Serialize(s, strExceptionAddresses);
        ::Serialize(s, nDistributionAmount);
        ::Serialize(s, nHeight);
        ::Serialize(s, nStatus);
    }

    template <typename Stream>
    void Unserialize(Stream& s)
    {
        ::Unserialize(s, strOwnershipAsset);
        ::Unserialize(s, strDistributionAsset);
        ::Unserialize(s, strExceptionAddresses);
        ::Unserialize(s, nDistributionAmount);
        ::Unserialize(s, nHeight);
        ::Unserialize(s, nStatus);
    }
};

bool GenerateDistributionList(const CRewardSnapshot& p_rewardSnapshot, std::vector<OwnerAndAmount>& vecDistributionList);
bool AddDistributeRewardSnapshot(CRewardSnapshot& p_rewardSnapshot);








#endif // BITCOIN_ASSETS_REWARDS_H
