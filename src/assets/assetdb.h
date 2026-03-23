// Copyright (c) 2017-2019 The Raven Core developers
// Copyright (c) 2022 The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_ASSETS_ASSETDB_H
#define BITCOIN_ASSETS_ASSETDB_H

#include <dbwrapper.h>
#include <serialize.h>
#include <util/fs.h>
#include <consensus/amount.h>

#include <map>
#include <string>

const int8_t ASSET_UNDO_INCLUDES_VERIFIER_STRING = -1;

class CNewAsset;
class uint256;
class COutPoint;
class CDatabasedAssetData;

template <typename Key, typename Value>
class CLRUCache;

struct CBlockAssetUndo
{
    bool fChangedIPFS;
    bool fChangedANS;
    bool fChangedUnits;
    std::string strIPFS;
    std::string strANSID;
    int32_t nUnits;
    int8_t version;
    bool fChangedVerifierString;
    std::string verifierString;

    // Explicit Serialize/Unserialize because the read path uses s.empty()/s.size()
    // which only exist on certain stream types (DataStream), not all.
    template <typename Stream>
    void Serialize(Stream& s) const
    {
        ::Serialize(s, fChangedUnits);
        ::Serialize(s, fChangedIPFS);
        ::Serialize(s, fChangedANS);
        ::Serialize(s, strIPFS);
        ::Serialize(s, strANSID);
        ::Serialize(s, nUnits);
        ::Serialize(s, ASSET_UNDO_INCLUDES_VERIFIER_STRING);
        ::Serialize(s, fChangedVerifierString);
        ::Serialize(s, verifierString);
    }

    template <typename Stream>
    void Unserialize(Stream& s)
    {
        ::Unserialize(s, fChangedUnits);
        ::Unserialize(s, fChangedIPFS);
        ::Unserialize(s, fChangedANS);
        ::Unserialize(s, strIPFS);
        ::Unserialize(s, strANSID);
        ::Unserialize(s, nUnits);
        if (!s.empty() && s.size() >= 1) {
            int8_t nVersionCheck;
            ::Unserialize(s, nVersionCheck);

            if (nVersionCheck == ASSET_UNDO_INCLUDES_VERIFIER_STRING) {
                ::Unserialize(s, fChangedVerifierString);
                ::Unserialize(s, verifierString);
            }
            version = nVersionCheck;
        }
    }
};

/** Asset database (assets/) */
class CAssetsDB : public CDBWrapper
{
public:
    explicit CAssetsDB(const fs::path& datadir, size_t nCacheSize, bool fMemory = false, bool fWipe = false);

    CAssetsDB(const CAssetsDB&) = delete;
    CAssetsDB& operator=(const CAssetsDB&) = delete;

    // Write to database functions
    bool WriteAssetData(const CNewAsset& asset, const int nHeight, const uint256& blockHash);
    bool WriteAssetAddressQuantity(const std::string& assetName, const std::string& address, const CAmount& quantity);
    bool WriteAddressAssetQuantity( const std::string& address, const std::string& assetName, const CAmount& quantity);
    bool WriteBlockUndoAssetData(const uint256& blockhash, const std::vector<std::pair<std::string, CBlockAssetUndo> >& assetUndoData);
    bool WriteReissuedMempoolState(const std::map<std::string, uint256>& mapReissuedAssets);

    // Read from database functions
    bool ReadAssetData(const std::string& strName, CNewAsset& asset, int& nHeight, uint256& blockHash);
    bool ReadAssetAddressQuantity(const std::string& assetName, const std::string& address, CAmount& quantity);
    bool ReadAddressAssetQuantity(const std::string& address, const std::string& assetName, CAmount& quantity);
    bool ReadBlockUndoAssetData(const uint256& blockhash, std::vector<std::pair<std::string, CBlockAssetUndo> >& assetUndoData);
    bool ReadReissuedMempoolState(std::map<std::string, uint256>& mapReissuedAssets, std::map<uint256, std::string>& mapReissuedTx);

    // Best block tracking for asset DB consistency
    bool WriteBestBlock(const uint256& blockHash);
    bool ReadBestBlock(uint256& blockHash);

    // Erase from database functions
    bool EraseAssetData(const std::string& assetName);
    bool EraseMyAssetData(const std::string& assetName);
    bool EraseAssetAddressQuantity(const std::string &assetName, const std::string &address);
    bool EraseAddressAssetQuantity(const std::string &address, const std::string &assetName);

    // Helper functions
    bool LoadAssets(class CLRUCache<std::string, CDatabasedAssetData>& cache,
                    std::map<std::pair<std::string, std::string>, CAmount>* pMapAssetsAddressAmount,
                    bool fAssetIndex);
    bool AssetDir(std::vector<CDatabasedAssetData>& assets, const std::string filter, const size_t count, const long start);
    bool AssetDir(std::vector<CDatabasedAssetData>& assets);

    bool AddressDir(std::vector<std::pair<std::string, CAmount> >& vecAssetAmount, int& totalEntries, const bool& fGetTotal, const std::string& address, const size_t count, const long start);
    bool AssetAddressDir(std::vector<std::pair<std::string, CAmount> >& vecAddressAmount, int& totalEntries, const bool& fGetTotal, const std::string& assetName, const size_t count, const long start);
};


#endif // BITCOIN_ASSETS_ASSETDB_H
