// Copyright (c) 2019 The Raven Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_ASSETS_ASSETSNAPSHOTDB_H
#define BITCOIN_ASSETS_ASSETSNAPSHOTDB_H

#include <set>

#include <dbwrapper.h>
#include <consensus/amount.h>
#include <util/fs.h>

class CAssetSnapshotDBEntry
{
public:
    int height;
    std::string assetName;
    std::set<std::pair<std::string, CAmount>> ownersAndAmounts;

    //  Used as the DB key for the snapshot
    std::string heightAndName;

    CAssetSnapshotDBEntry();
    CAssetSnapshotDBEntry(
        const std::string & p_assetName, const int p_snapshotHeight,
        const std::set<std::pair<std::string, CAmount>> & p_ownersAndAmounts
    );

    void SetNull()
    {
        height = 0;
        assetName = "";
        ownersAndAmounts.clear();

        heightAndName = "";
    }

    bool operator<(const CAssetSnapshotDBEntry &rhs) const
    {
        return heightAndName < rhs.heightAndName;
    }

    // Serialization methods
    SERIALIZE_METHODS(CAssetSnapshotDBEntry, obj)
    {
        READWRITE(obj.height);
        READWRITE(obj.assetName);
        READWRITE(obj.ownersAndAmounts);
        READWRITE(obj.heightAndName);
    }
};

class CAssetSnapshotDB  : public CDBWrapper {
public:
    explicit CAssetSnapshotDB(const fs::path& datadir, size_t nCacheSize, bool fMemory = false, bool fWipe = false);

    CAssetSnapshotDB(const CAssetSnapshotDB&) = delete;
    CAssetSnapshotDB& operator=(const CAssetSnapshotDB&) = delete;

    //  Add an entry to the snapshot at the specified height
    bool AddAssetOwnershipSnapshot(
        const std::string & p_assetName, int p_height);

    //  Read all of the entries at a specified height
    bool RetrieveOwnershipSnapshot(
        const std::string & p_assetName, int p_height,
        CAssetSnapshotDBEntry & p_snapshotEntry);

    //  Remove the asset snapshot at the specified height
    bool RemoveOwnershipSnapshot(
        const std::string & p_assetName, int p_height);
};


#endif // BITCOIN_ASSETS_ASSETSNAPSHOTDB_H
