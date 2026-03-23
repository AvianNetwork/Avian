// Copyright (c) 2019 The Raven Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_ASSETS_SNAPSHOTREQUESTDB_H
#define BITCOIN_ASSETS_SNAPSHOTREQUESTDB_H

#include <dbwrapper.h>

#include <string>
#include <vector>
#include <set>

#include <consensus/amount.h>
#include <assets/rewards.h>
#include <util/fs.h>

class CSnapshotRequestDBEntry
{
public:
    std::string assetName;
    int heightForSnapshot;

    //  Used as the DB key for the snapshot request
    std::string heightAndName;

    CSnapshotRequestDBEntry();
    CSnapshotRequestDBEntry(
        const std::string & p_assetName, int p_heightForSnapshot
    );

    void SetNull()
    {
        assetName = "";
        heightForSnapshot = 0;

        heightAndName = "";
    }

    bool operator<(const CSnapshotRequestDBEntry &rhs) const
    {
        return heightAndName < rhs.heightAndName;
    }

    // Serialization methods
    SERIALIZE_METHODS(CSnapshotRequestDBEntry, obj)
    {
        READWRITE(obj.assetName);
        READWRITE(obj.heightForSnapshot);
        READWRITE(obj.heightAndName);
    }
};

class CSnapshotRequestDB  : public CDBWrapper
{
public:
    explicit CSnapshotRequestDB(const fs::path& datadir, size_t nCacheSize, bool fMemory = false, bool fWipe = false);

    CSnapshotRequestDB(const CSnapshotRequestDB&) = delete;
    CSnapshotRequestDB& operator=(const CSnapshotRequestDB&) = delete;

    // Schedule a asset snapshot
    bool ScheduleSnapshot(
        const std::string & p_assetName, int p_heightForSnapshot
    );

    //  Find a snapshot request using its ID
    bool RetrieveSnapshotRequest(
        const std::string & p_assetName, int p_heightForSnapshot,
        CSnapshotRequestDBEntry & p_snapshotRequest
    );

    bool ContainsSnapshotRequest(const std::string & p_assetName, int p_heightForSnapshot);

    // Remove a snapshot request
    bool RemoveSnapshotRequest(const std::string & p_assetName, int p_heightForSnapshot);

    //  Retrieve all snapshot requests at the provided block height
    //      (if the block height is zero, retrieve requests at ALL HEIGHTS),
    //      limited to the specified asset name (if provided)
    bool RetrieveSnapshotRequestsForHeight(
        const std::string & p_assetName, int p_blockHeight,
        std::set<CSnapshotRequestDBEntry> & p_assetsToSnapshot
    );
};

class CDistributeSnapshotRequestDB  : public CDBWrapper
{
public:
    explicit CDistributeSnapshotRequestDB(const fs::path& datadir, size_t nCacheSize, bool fMemory = false, bool fWipe = false);

    CDistributeSnapshotRequestDB(const CDistributeSnapshotRequestDB&) = delete;
    CDistributeSnapshotRequestDB& operator=(const CDistributeSnapshotRequestDB&) = delete;

    // Schedule a asset snapshot
    bool AddDistributeSnapshot(const uint256& hash, const CRewardSnapshot& p_rewardSnapshot);
    bool OverrideDistributeSnapshot(const uint256& hash, const CRewardSnapshot& p_rewardSnapshot);

    //  Find a snapshot request using its ID
    bool RetrieveDistributeSnapshotRequest(const uint256& hash, CRewardSnapshot& p_rewardSnapshot);

    // Remove a snapshot request
    bool RemoveDistributeSnapshotRequest(const uint256& hash);

    bool AddDistributeTransaction(const uint256& hash, const int& nBatchNumber, const uint256& txid);
    bool GetDistributeTransaction(const uint256& hash, const int& nBatchNumber, uint256& txid);

    void LoadAllDistributeSnapshot(std::map<uint256, CRewardSnapshot>& mapRewardSnapshots);


};



#endif // BITCOIN_ASSETS_SNAPSHOTREQUESTDB_H
