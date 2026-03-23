// Copyright (c) 2019 The Raven Core developers
// Copyright (c) 2022 The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_ASSETS_ASSETTYPES_H
#define BITCOIN_ASSETS_ASSETTYPES_H

#include <addresstype.h>
#include <consensus/amount.h>
#include <primitives/transaction.h>
#include <serialize.h>
#include <uint256.h>

#include <cstdint>
#include <list>
#include <sstream>
#include <string>
#include <unordered_map>

#define MAX_UNIT 8
#define MIN_UNIT 0

class CAssetsCache;

enum class AssetType
{
    ROOT = 0,
    SUB = 1,
    UNIQUE = 2,
    MSGCHANNEL = 3,
    QUALIFIER = 4,
    SUB_QUALIFIER = 5,
    RESTRICTED = 6,
    VOTE = 7,
    REISSUE = 8,
    OWNER = 9,
    NULL_ADD_QUALIFIER = 10,
    INVALID = 11
};

enum class QualifierType
{
    REMOVE_QUALIFIER = 0,
    ADD_QUALIFIER = 1
};

enum class RestrictedType
{
    UNFREEZE_ADDRESS = 0,
    FREEZE_ADDRESS = 1,
    GLOBAL_UNFREEZE = 2,
    GLOBAL_FREEZE = 3
};

int IntFromAssetType(AssetType type);
AssetType AssetTypeFromInt(int nType);

// Integer constants matching IsAssetScript() return values.
// These correspond to TxoutType enum class values in script/solver.h but are used
// as plain ints by the asset subsystem's IsAssetScript() which predates enum class.
static constexpr int TX_NEW_ASSET = 8;
static constexpr int TX_REISSUE_ASSET = 9;
static constexpr int TX_TRANSFER_ASSET = 10;
static constexpr int TX_RESTRICTED_ASSET_DATA = 11;

// Output entry describing an asset found in a transaction output
struct CAssetOutputEntry
{
    int type;               // TX_NEW_ASSET, TX_TRANSFER_ASSET, TX_REISSUE_ASSET
    std::string assetName;
    CTxDestination destination;
    CAmount nAmount;
    std::string message;    // for transfers with attached message
    int64_t expireTime{0};  // for transfers with expiration
};

static constexpr int8_t IPFS_SHA2_256 = 0x12;
static constexpr int8_t TXID_NOTIFIER = 0x54;
static constexpr int8_t IPFS_SHA2_256_LEN = 0x20;

// Helper: serialize an IPFS/TXID hash to a stream
template <typename Stream>
bool SerializeIPFSHash(Stream& s, const std::string& strIPFSHash)
{
    if (strIPFSHash.length() == 34) {
        int8_t type = IPFS_SHA2_256;
        ::Serialize(s, type);
        std::string hashData = strIPFSHash.substr(2);
        ::Serialize(s, hashData);
        return true;
    } else if (strIPFSHash.length() == 32) {
        int8_t type = TXID_NOTIFIER;
        ::Serialize(s, type);
        ::Serialize(s, strIPFSHash);
        return true;
    }
    return false;
}

// Helper: unserialize an IPFS/TXID hash from a stream
// Requires the stream to support size() and empty() (e.g., DataStream).
template <typename Stream>
bool UnserializeIPFSHash(Stream& s, std::string& strIPFSHash)
{
    strIPFSHash.clear();
    if (!s.empty() && s.size() >= 33) {
        int8_t type;
        ::Unserialize(s, type);
        std::string hash;
        ::Unserialize(s, hash);

        if (type == IPFS_SHA2_256) {
            strIPFSHash += char(IPFS_SHA2_256);
            strIPFSHash += char(IPFS_SHA2_256_LEN);
        }
        strIPFSHash.append(hash, 0, 32);
        return true;
    }
    return false;
}

class CNewAsset
{
public:
    std::string strName; // MAX 31 Bytes
    CAmount nAmount;     // 8 Bytes
    int8_t units;        // 1 Byte
    int8_t nReissuable;  // 1 Byte
    int8_t nHasIPFS;     // 1 Byte
    int8_t nHasANS;      // 1 Byte
    std::string strIPFSHash; // MAX 40 Bytes
    std::string strANSID;    // MAX 40 Bytes

    CNewAsset()
    {
        SetNull();
    }

    CNewAsset(const std::string& strName, const CAmount& nAmount, const int& units, const int& nReissuable, const int& nHasIPFS, const std::string& strIPFSHash);
    CNewAsset(const std::string& strName, const CAmount& nAmount, const int& units, const int& nReissuable, const int& nHasIPFS, const std::string& strIPFSHash, const int& nHasANS, const std::string& strANSID);
    CNewAsset(const std::string& strName, const CAmount& nAmount);

    CNewAsset(const CNewAsset& asset);
    CNewAsset& operator=(const CNewAsset& asset);

    void SetNull()
    {
        strName = "";
        nAmount = 0;
        units = int8_t(MAX_UNIT);
        nReissuable = int8_t(0);
        nHasIPFS = int8_t(0);
        strIPFSHash = "";
        nHasANS = int8_t(0);
        strANSID = "";
    }

    bool IsNull() const;
    std::string ToString();

    void ConstructTransaction(CScript& script) const;
    void ConstructOwnerTransaction(CScript& script) const;

    // Explicit Serialize/Unserialize for custom IPFS hash handling.
    // Binary format must match Avian exactly for consensus compatibility.
    template <typename Stream>
    void Serialize(Stream& s) const
    {
        ::Serialize(s, strName);
        ::Serialize(s, nAmount);
        ::Serialize(s, units);
        ::Serialize(s, nReissuable);
        ::Serialize(s, nHasIPFS);
        if (nHasIPFS == 1) {
            SerializeIPFSHash(s, strIPFSHash);
        }
        ::Serialize(s, nHasANS);
        if (nHasANS == 1) {
            ::Serialize(s, strANSID);
        }
    }

    template <typename Stream>
    void Unserialize(Stream& s)
    {
        ::Unserialize(s, strName);
        ::Unserialize(s, nAmount);
        ::Unserialize(s, units);
        ::Unserialize(s, nReissuable);
        ::Unserialize(s, nHasIPFS);
        if (nHasIPFS == 1) {
            UnserializeIPFSHash(s, strIPFSHash);
        }
        ::Unserialize(s, nHasANS);
        if (nHasANS == 1) {
            ::Unserialize(s, strANSID);
        }
    }
};

class AssetComparator
{
public:
    bool operator()(const CNewAsset& s1, const CNewAsset& s2) const
    {
        return s1.strName < s2.strName;
    }
};

class CDatabasedAssetData
{
public:
    CNewAsset asset;
    int nHeight;
    uint256 blockHash;

    CDatabasedAssetData(const CNewAsset& asset, const int& nHeight, const uint256& blockHash);
    CDatabasedAssetData();

    void SetNull()
    {
        asset.SetNull();
        nHeight = -1;
        blockHash = uint256();
    }

    SERIALIZE_METHODS(CDatabasedAssetData, obj)
    {
        READWRITE(obj.asset, obj.nHeight, obj.blockHash);
    }
};

class CAssetTransfer
{
public:
    std::string strName;
    CAmount nAmount;
    std::string message;
    int64_t nExpireTime;

    CAssetTransfer()
    {
        SetNull();
    }

    void SetNull()
    {
        nAmount = 0;
        strName = "";
        message = "";
        nExpireTime = 0;
    }

    // Explicit Serialize/Unserialize for custom IPFS hash + optional expire time.
    template <typename Stream>
    void Serialize(Stream& s) const
    {
        ::Serialize(s, strName);
        ::Serialize(s, nAmount);
        bool validIPFS = SerializeIPFSHash(s, message);
        if (validIPFS && nExpireTime != 0) {
            ::Serialize(s, nExpireTime);
        }
    }

    template <typename Stream>
    void Unserialize(Stream& s)
    {
        ::Unserialize(s, strName);
        ::Unserialize(s, nAmount);
        bool validIPFS = UnserializeIPFSHash(s, message);
        if (validIPFS) {
            if (!s.empty() && s.size() >= sizeof(int64_t)) {
                ::Unserialize(s, nExpireTime);
            }
        }
    }

    CAssetTransfer(const std::string& strAssetName, const CAmount& nAmount, const std::string& message = "", const int64_t& nExpireTime = 0);
    bool IsValid(std::string& strError) const;
    void ConstructTransaction(CScript& script) const;
    bool ContextualCheckAgainstVerifyString(CAssetsCache *assetCache, const std::string& address, std::string& strError) const;
};

class CReissueAsset
{
public:
    std::string strName;
    CAmount nAmount;
    int8_t nUnits;
    int8_t nReissuable;
    std::string strIPFSHash;
    std::string strANSID;

    CReissueAsset()
    {
        SetNull();
    }

    void SetNull()
    {
        nAmount = 0;
        strName = "";
        nUnits = 0;
        nReissuable = 1;
        strIPFSHash = "";
        strANSID = "";
    }

    // Explicit Serialize/Unserialize for custom IPFS hash handling.
    template <typename Stream>
    void Serialize(Stream& s) const
    {
        ::Serialize(s, strName);
        ::Serialize(s, nAmount);
        ::Serialize(s, nUnits);
        ::Serialize(s, nReissuable);
        SerializeIPFSHash(s, strIPFSHash);
        ::Serialize(s, strANSID);
    }

    template <typename Stream>
    void Unserialize(Stream& s)
    {
        ::Unserialize(s, strName);
        ::Unserialize(s, nAmount);
        ::Unserialize(s, nUnits);
        ::Unserialize(s, nReissuable);
        UnserializeIPFSHash(s, strIPFSHash);
        ::Unserialize(s, strANSID);
    }

    CReissueAsset(const std::string& strAssetName, const CAmount& nAmount, const int& nUnits, const int& nReissuable, const std::string& strIPFSHash, const std::string& strANSID);
    void ConstructTransaction(CScript& script) const;
    bool IsNull() const;
};

class CNullAssetTxData {
public:
    std::string asset_name;
    int8_t flag; // on/off but could be used to determine multiple options later on

    CNullAssetTxData()
    {
        SetNull();
    }

    void SetNull()
    {
        flag = -1;
        asset_name = "";
    }

    SERIALIZE_METHODS(CNullAssetTxData, obj)
    {
        READWRITE(obj.asset_name, obj.flag);
    }

    CNullAssetTxData(const std::string& strAssetname, const int8_t& nFlag);
    bool IsValid(std::string& strError, CAssetsCache& assetCache, bool fForceCheckPrimaryAssetExists) const;
    void ConstructTransaction(CScript& script) const;
    void ConstructGlobalRestrictionTransaction(CScript &script) const;
};

class CNullAssetTxVerifierString {

public:
    std::string verifier_string;

    CNullAssetTxVerifierString()
    {
        SetNull();
    }

    void SetNull()
    {
        verifier_string = "";
    }

    SERIALIZE_METHODS(CNullAssetTxVerifierString, obj)
    {
        READWRITE(obj.verifier_string);
    }

    CNullAssetTxVerifierString(const std::string& verifier);
    void ConstructTransaction(CScript& script) const;
};

/** THESE ARE ONLY TO BE USED WHEN ADDING THINGS TO THE CACHE DURING CONNECT AND DISCONNECT BLOCK */
struct CAssetCacheNewAsset
{
    CNewAsset asset;
    std::string address;
    uint256 blockHash;
    int blockHeight;

    CAssetCacheNewAsset(const CNewAsset& asset, const std::string& address, const int& blockHeight, const uint256& blockHash)
    {
        this->asset = asset;
        this->address = address;
        this->blockHash = blockHash;
        this->blockHeight = blockHeight;
    }

    bool operator<(const CAssetCacheNewAsset& rhs) const
    {
        return asset.strName < rhs.asset.strName;
    }
};

struct CAssetCacheReissueAsset
{
    CReissueAsset reissue;
    std::string address;
    COutPoint out;
    uint256 blockHash;
    int blockHeight;

    CAssetCacheReissueAsset(const CReissueAsset& reissue, const std::string& address, const COutPoint& out, const int& blockHeight, const uint256& blockHash)
    {
        this->reissue = reissue;
        this->address = address;
        this->out = out;
        this->blockHash = blockHash;
        this->blockHeight = blockHeight;
    }

    bool operator<(const CAssetCacheReissueAsset& rhs) const
    {
        return out < rhs.out;
    }
};

struct CAssetCacheNewTransfer
{
    CAssetTransfer transfer;
    std::string address;
    COutPoint out;

    CAssetCacheNewTransfer(const CAssetTransfer& transfer, const std::string& address, const COutPoint& out)
    {
        this->transfer = transfer;
        this->address = address;
        this->out = out;
    }

    bool operator<(const CAssetCacheNewTransfer& rhs) const
    {
        return out < rhs.out;
    }
};

struct CAssetCacheNewOwner
{
    std::string assetName;
    std::string address;

    CAssetCacheNewOwner(const std::string& assetName, const std::string& address)
    {
        this->assetName = assetName;
        this->address = address;
    }

    bool operator<(const CAssetCacheNewOwner& rhs) const
    {
        return assetName < rhs.assetName;
    }
};

struct CAssetCacheUndoAssetAmount
{
    std::string assetName;
    std::string address;
    CAmount nAmount;

    CAssetCacheUndoAssetAmount(const std::string& assetName, const std::string& address, const CAmount& nAmount)
    {
        this->assetName = assetName;
        this->address = address;
        this->nAmount = nAmount;
    }
};

struct CAssetCacheSpendAsset
{
    std::string assetName;
    std::string address;
    CAmount nAmount;

    CAssetCacheSpendAsset(const std::string& assetName, const std::string& address, const CAmount& nAmount)
    {
        this->assetName = assetName;
        this->address = address;
        this->nAmount = nAmount;
    }
};

struct CAssetCacheQualifierAddress {
    std::string assetName;
    std::string address;
    QualifierType type;

    CAssetCacheQualifierAddress(const std::string &assetName, const std::string &address, const QualifierType &type) {
        this->assetName = assetName;
        this->address = address;
        this->type = type;
    }

    bool operator<(const CAssetCacheQualifierAddress &rhs) const {
        return assetName < rhs.assetName || (assetName == rhs.assetName && address < rhs.address);
    }

    uint256 GetHash();
};

struct CAssetCacheRootQualifierChecker {
    std::string rootAssetName;
    std::string address;

    CAssetCacheRootQualifierChecker(const std::string &assetName, const std::string &address) {
        this->rootAssetName = assetName;
        this->address = address;
    }

    bool operator<(const CAssetCacheRootQualifierChecker &rhs) const {
        return rootAssetName < rhs.rootAssetName || (rootAssetName == rhs.rootAssetName && address < rhs.address);
    }

    uint256 GetHash();
};

struct CAssetCacheRestrictedAddress
{
    std::string assetName;
    std::string address;
    RestrictedType type;

    CAssetCacheRestrictedAddress(const std::string& assetName, const std::string& address, const RestrictedType& type)
    {
        this->assetName = assetName;
        this->address = address;
        this->type = type;
    }

    bool operator<(const CAssetCacheRestrictedAddress& rhs) const
    {
        return assetName < rhs.assetName || (assetName == rhs.assetName && address < rhs.address);
    }

    uint256 GetHash();
};

struct CAssetCacheRestrictedGlobal
{
    std::string assetName;
    RestrictedType type;

    CAssetCacheRestrictedGlobal(const std::string& assetName, const RestrictedType& type)
    {
        this->assetName = assetName;
        this->type = type;
    }

    bool operator<(const CAssetCacheRestrictedGlobal& rhs) const
    {
        return assetName < rhs.assetName;
    }
};

struct CAssetCacheRestrictedVerifiers
{
    std::string assetName;
    std::string verifier;
    bool fUndoingRessiue;

    CAssetCacheRestrictedVerifiers(const std::string& assetName, const std::string& verifier)
    {
        this->assetName = assetName;
        this->verifier = verifier;
        fUndoingRessiue = false;
    }

    bool operator<(const CAssetCacheRestrictedVerifiers& rhs) const
    {
        return assetName < rhs.assetName;
    }
};

// Least Recently Used Cache
template<typename cache_key_t, typename cache_value_t>
class CLRUCache
{
public:
    typedef typename std::pair<cache_key_t, cache_value_t> key_value_pair_t;
    typedef typename std::list<key_value_pair_t>::iterator list_iterator_t;

    CLRUCache(size_t max_size) : maxSize(max_size)
    {
    }
    CLRUCache()
    {
        SetNull();
    }

    void Put(const cache_key_t& key, const cache_value_t& value)
    {
        auto it = cacheItemsMap.find(key);
        cacheItemsList.push_front(key_value_pair_t(key, value));
        if (it != cacheItemsMap.end())
        {
            cacheItemsList.erase(it->second);
            cacheItemsMap.erase(it);
        }
        cacheItemsMap[key] = cacheItemsList.begin();

        if (cacheItemsMap.size() > maxSize)
        {
            auto last = cacheItemsList.end();
            last--;
            cacheItemsMap.erase(last->first);
            cacheItemsList.pop_back();
        }
    }

    void Erase(const cache_key_t& key)
    {
        auto it = cacheItemsMap.find(key);
        if (it != cacheItemsMap.end())
        {
            cacheItemsList.erase(it->second);
            cacheItemsMap.erase(it);
        }
    }

    const cache_value_t& Get(const cache_key_t& key)
    {
        auto it = cacheItemsMap.find(key);
        if (it == cacheItemsMap.end())
        {
            throw std::range_error("There is no such key in cache");
        }
        else
        {
            cacheItemsList.splice(cacheItemsList.begin(), cacheItemsList, it->second);
            return it->second->second;
        }
    }

    bool Exists(const cache_key_t& key) const
    {
        return cacheItemsMap.find(key) != cacheItemsMap.end();
    }

    size_t Size() const
    {
        return cacheItemsMap.size();
    }

    void Clear()
    {
        cacheItemsMap.clear();
        cacheItemsList.clear();
    }

    void SetNull()
    {
        maxSize = 0;
        Clear();
    }

    size_t MaxSize() const
    {
        return maxSize;
    }

    void SetSize(const size_t size)
    {
        maxSize = size;
    }

    const std::unordered_map<cache_key_t, list_iterator_t>& GetItemsMap()
    {
        return cacheItemsMap;
    }

    const std::list<key_value_pair_t>& GetItemsList()
    {
        return cacheItemsList;
    }

    CLRUCache(const CLRUCache& cache)
    {
        this->cacheItemsList = cache.cacheItemsList;
        this->cacheItemsMap = cache.cacheItemsMap;
        this->maxSize = cache.maxSize;
    }

private:
    std::list<key_value_pair_t> cacheItemsList;
    std::unordered_map<cache_key_t, list_iterator_t> cacheItemsMap;
    size_t maxSize;
};

#endif // BITCOIN_ASSETS_ASSETTYPES_H
