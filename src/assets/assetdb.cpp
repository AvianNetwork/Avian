// Copyright (c) 2017-2019 The Raven Core developers
// Copyright (c) 2022 The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <assets/assetdb.h>
#include <assets/assettypes.h>
#include <logging.h>
#include <serialize.h>

static const uint8_t ASSET_FLAG = 'A';
static const uint8_t ASSET_ADDRESS_QUANTITY_FLAG = 'B';
static const uint8_t ADDRESS_ASSET_QUANTITY_FLAG = 'C';
static const uint8_t MY_ASSET_FLAG = 'M';
static const uint8_t BLOCK_ASSET_UNDO_DATA = 'U';
static const uint8_t MEMPOOL_REISSUED_TX = 'Z';
static const uint8_t ASSET_BEST_BLOCK_FLAG = 'T'; // Tip tracking

[[maybe_unused]] static size_t MAX_DATABASE_RESULTS = 50000;

CAssetsDB::CAssetsDB(const fs::path& datadir, size_t nCacheSize, bool fMemory, bool fWipe)
    : CDBWrapper(DBParams{
          .path = datadir / "assets",
          .cache_bytes = nCacheSize,
          .memory_only = fMemory,
          .wipe_data = fWipe})
{
}

bool CAssetsDB::WriteAssetData(const CNewAsset &asset, const int nHeight, const uint256& blockHash)
{
    CDatabasedAssetData data(asset, nHeight, blockHash);
    return Write(std::make_pair(ASSET_FLAG, asset.strName), data);
}

bool CAssetsDB::WriteAssetAddressQuantity(const std::string &assetName, const std::string &address, const CAmount &quantity)
{
    return Write(std::make_pair(ASSET_ADDRESS_QUANTITY_FLAG, std::make_pair(assetName, address)), quantity);
}

bool CAssetsDB::WriteAddressAssetQuantity(const std::string &address, const std::string &assetName, const CAmount& quantity) {
    return Write(std::make_pair(ADDRESS_ASSET_QUANTITY_FLAG, std::make_pair(address, assetName)), quantity);
}

bool CAssetsDB::ReadAssetData(const std::string& strName, CNewAsset& asset, int& nHeight, uint256& blockHash)
{
    CDatabasedAssetData data;
    bool ret = Read(std::make_pair(ASSET_FLAG, strName), data);

    if (ret) {
        asset = data.asset;
        nHeight = data.nHeight;
        blockHash = data.blockHash;
    }

    return ret;
}

bool CAssetsDB::ReadAssetAddressQuantity(const std::string& assetName, const std::string& address, CAmount& quantity)
{
    return Read(std::make_pair(ASSET_ADDRESS_QUANTITY_FLAG, std::make_pair(assetName, address)), quantity);
}

bool CAssetsDB::WriteBestBlock(const uint256& blockHash)
{
    return Write(ASSET_BEST_BLOCK_FLAG, blockHash);
}

bool CAssetsDB::ReadBestBlock(uint256& blockHash)
{
    return Read(ASSET_BEST_BLOCK_FLAG, blockHash);
}

bool CAssetsDB::ReadAddressAssetQuantity(const std::string &address, const std::string &assetName, CAmount& quantity) {
    return Read(std::make_pair(ADDRESS_ASSET_QUANTITY_FLAG, std::make_pair(address, assetName)), quantity);
}

bool CAssetsDB::EraseAssetData(const std::string& assetName)
{
    return Erase(std::make_pair(ASSET_FLAG, assetName));
}

bool CAssetsDB::EraseMyAssetData(const std::string& assetName)
{
    return Erase(std::make_pair(MY_ASSET_FLAG, assetName));
}

bool CAssetsDB::EraseAssetAddressQuantity(const std::string &assetName, const std::string &address) {
    return Erase(std::make_pair(ASSET_ADDRESS_QUANTITY_FLAG, std::make_pair(assetName, address)));
}

bool CAssetsDB::EraseAddressAssetQuantity(const std::string &address, const std::string &assetName) {
    return Erase(std::make_pair(ADDRESS_ASSET_QUANTITY_FLAG, std::make_pair(address, assetName)));
}

bool CAssetsDB::WriteBlockUndoAssetData(const uint256& blockhash, const std::vector<std::pair<std::string, CBlockAssetUndo> >& assetUndoData)
{
    return Write(std::make_pair(BLOCK_ASSET_UNDO_DATA, blockhash), assetUndoData);
}

bool CAssetsDB::ReadBlockUndoAssetData(const uint256 &blockhash, std::vector<std::pair<std::string, CBlockAssetUndo> > &assetUndoData)
{
    // If it exists, return the read value.
    if (Exists(std::make_pair(BLOCK_ASSET_UNDO_DATA, blockhash)))
           return Read(std::make_pair(BLOCK_ASSET_UNDO_DATA, blockhash), assetUndoData);

    // If it doesn't exist, we just return true because we don't want to fail just because it didn't exist in the db
    return true;
}

bool CAssetsDB::WriteReissuedMempoolState(const std::map<std::string, uint256>& mapReissuedAssets)
{
    return Write(MEMPOOL_REISSUED_TX, mapReissuedAssets);
}

bool CAssetsDB::ReadReissuedMempoolState(std::map<std::string, uint256>& mapReissuedAssets, std::map<uint256, std::string>& mapReissuedTx)
{
    mapReissuedAssets.clear();
    mapReissuedTx.clear();
    // If it exists, return the read value.
    bool rv = Read(MEMPOOL_REISSUED_TX, mapReissuedAssets);
    if (rv) {
        for (const auto& pair : mapReissuedAssets)
            mapReissuedTx.insert(std::make_pair(pair.second, pair.first));
    }
    return rv;
}

bool CAssetsDB::LoadAssets(CLRUCache<std::string, CDatabasedAssetData>& cache,
                           std::map<std::pair<std::string, std::string>, CAmount>* pMapAssetsAddressAmount,
                           bool fAssetIndex)
{
    std::unique_ptr<CDBIterator> pcursor(NewIterator());

    pcursor->Seek(std::make_pair(ASSET_FLAG, std::string()));

    // Load assets
    while (pcursor->Valid()) {
        std::pair<uint8_t, std::string> key;
        if (pcursor->GetKey(key) && key.first == ASSET_FLAG) {
            CDatabasedAssetData data;
            if (pcursor->GetValue(data)) {
                cache.Put(data.asset.strName, data);
                pcursor->Next();

                // Loaded enough from database to have in memory.
                // No need to load everything if it is just going to be removed from the cache
                if (cache.Size() == (cache.MaxSize() / 2))
                    break;
            } else {
                LogError("%s: failed to read asset\n", __func__);
                return false;
            }
        } else {
            break;
        }
    }

    if (fAssetIndex && pMapAssetsAddressAmount) {
        std::unique_ptr<CDBIterator> pcursor3(NewIterator());
        pcursor3->Seek(std::make_pair(ASSET_ADDRESS_QUANTITY_FLAG, std::make_pair(std::string(), std::string())));

        // Load mapAssetAddressAmount
        while (pcursor3->Valid()) {
            std::pair<uint8_t, std::pair<std::string, std::string> > key; // <Asset Name, Address> -> Quantity
            if (pcursor3->GetKey(key) && key.first == ASSET_ADDRESS_QUANTITY_FLAG) {
                CAmount value;
                if (pcursor3->GetValue(value)) {
                    pMapAssetsAddressAmount->insert(
                            std::make_pair(std::make_pair(key.second.first, key.second.second), value));
                    if (pMapAssetsAddressAmount->size() > MAX_DATABASE_RESULTS)
                        break;
                    pcursor3->Next();
                } else {
                    LogError("%s: failed to read address quantity from database\n", __func__);
                    return false;
                }
            } else {
                break;
            }
        }
    }

    return true;
}

bool CAssetsDB::AssetDir(std::vector<CDatabasedAssetData>& assets, const std::string filter, const size_t count, const long start)
{
    std::unique_ptr<CDBIterator> pcursor(NewIterator());
    pcursor->Seek(std::make_pair(ASSET_FLAG, std::string()));

    auto prefix = filter;
    bool wildcard = prefix.back() == '*';
    if (wildcard)
        prefix.pop_back();

    size_t skip = 0;
    if (start >= 0) {
        skip = start;
    }
    else {
        // compute table size for backwards offset
        long table_size = 0;
        while (pcursor->Valid()) {
            std::pair<uint8_t, std::string> key;
            if (pcursor->GetKey(key) && key.first == ASSET_FLAG) {
                if (prefix == "" ||
                    (wildcard && key.second.find(prefix) == 0) ||
                    (!wildcard && key.second == prefix)) {
                    table_size += 1;
                }
            }
            pcursor->Next();
        }
        skip = table_size + start;
        pcursor->SeekToFirst();
    }

    size_t loaded = 0;
    size_t offset = 0;

    // Load assets
    while (pcursor->Valid() && loaded < count) {
        std::pair<uint8_t, std::string> key;
        if (pcursor->GetKey(key) && key.first == ASSET_FLAG) {
            if (prefix == "" ||
                    (wildcard && key.second.find(prefix) == 0) ||
                    (!wildcard && key.second == prefix)) {
                if (offset < skip) {
                    offset += 1;
                }
                else {
                    CDatabasedAssetData data;
                    if (pcursor->GetValue(data)) {
                        assets.push_back(data);
                        loaded += 1;
                    } else {
                        LogError("%s: failed to read asset\n", __func__);
                        return false;
                    }
                }
            }
            pcursor->Next();
        } else {
            break;
        }
    }

    return true;
}

bool CAssetsDB::AddressDir(std::vector<std::pair<std::string, CAmount> >& vecAssetAmount, int& totalEntries, const bool& fGetTotal, const std::string& address, const size_t count, const long start)
{
    std::unique_ptr<CDBIterator> pcursor(NewIterator());
    pcursor->Seek(std::make_pair(ADDRESS_ASSET_QUANTITY_FLAG, std::make_pair(address, std::string())));

    if (fGetTotal) {
        totalEntries = 0;
        while (pcursor->Valid()) {
            std::pair<uint8_t, std::pair<std::string, std::string> > key;
            if (pcursor->GetKey(key) && key.first == ADDRESS_ASSET_QUANTITY_FLAG && key.second.first == address) {
                totalEntries++;
            }
            pcursor->Next();
        }
        return true;
    }

    size_t skip = 0;
    if (start >= 0) {
        skip = start;
    }
    else {
        // compute table size for backwards offset
        long table_size = 0;
        while (pcursor->Valid()) {
            std::pair<uint8_t, std::pair<std::string, std::string> > key;
            if (pcursor->GetKey(key) && key.first == ADDRESS_ASSET_QUANTITY_FLAG && key.second.first == address) {
                table_size += 1;
            }
            pcursor->Next();
        }
        skip = table_size + start;
        pcursor->SeekToFirst();
    }

    size_t loaded = 0;
    size_t offset = 0;

    // Load assets
    while (pcursor->Valid() && loaded < count && loaded < MAX_DATABASE_RESULTS) {
        std::pair<uint8_t, std::pair<std::string, std::string> > key;
        if (pcursor->GetKey(key) && key.first == ADDRESS_ASSET_QUANTITY_FLAG && key.second.first == address) {
                if (offset < skip) {
                    offset += 1;
                }
                else {
                    CAmount amount;
                    if (pcursor->GetValue(amount)) {
                        vecAssetAmount.emplace_back(std::make_pair(key.second.second, amount));
                        loaded += 1;
                    } else {
                        LogError("%s: failed to read address asset quantity\n", __func__);
                        return false;
                    }
                }
            pcursor->Next();
        } else {
            break;
        }
    }

    return true;
}

// Can get to total count of addresses that belong to a certain asset_name, or get you the list of all addresses that belong to a certain asset_name
bool CAssetsDB::AssetAddressDir(std::vector<std::pair<std::string, CAmount> >& vecAddressAmount, int& totalEntries, const bool& fGetTotal, const std::string& assetName, const size_t count, const long start)
{
    std::unique_ptr<CDBIterator> pcursor(NewIterator());
    pcursor->Seek(std::make_pair(ASSET_ADDRESS_QUANTITY_FLAG, std::make_pair(assetName, std::string())));

    if (fGetTotal) {
        totalEntries = 0;
        while (pcursor->Valid()) {
            std::pair<uint8_t, std::pair<std::string, std::string> > key;
            if (pcursor->GetKey(key) && key.first == ASSET_ADDRESS_QUANTITY_FLAG && key.second.first == assetName) {
                totalEntries += 1;
            }
            pcursor->Next();
        }
        return true;
    }

    size_t skip = 0;
    if (start >= 0) {
        skip = start;
    }
    else {
        // compute table size for backwards offset
        long table_size = 0;
        while (pcursor->Valid()) {
            std::pair<uint8_t, std::pair<std::string, std::string> > key;
            if (pcursor->GetKey(key) && key.first == ASSET_ADDRESS_QUANTITY_FLAG && key.second.first == assetName) {
                table_size += 1;
            }
            pcursor->Next();
        }
        skip = table_size + start;
        pcursor->SeekToFirst();
    }

    size_t loaded = 0;
    size_t offset = 0;

    // Load assets
    while (pcursor->Valid() && loaded < count && loaded < MAX_DATABASE_RESULTS) {
        std::pair<uint8_t, std::pair<std::string, std::string> > key;
        if (pcursor->GetKey(key) && key.first == ASSET_ADDRESS_QUANTITY_FLAG && key.second.first == assetName) {
            if (offset < skip) {
                offset += 1;
            }
            else {
                CAmount amount;
                if (pcursor->GetValue(amount)) {
                    vecAddressAmount.emplace_back(std::make_pair(key.second.second, amount));
                    loaded += 1;
                } else {
                    LogError("%s: failed to read asset address quantity\n", __func__);
                    return false;
                }
            }
            pcursor->Next();
        } else {
            break;
        }
    }

    return true;
}

bool CAssetsDB::AssetDir(std::vector<CDatabasedAssetData>& assets)
{
    return CAssetsDB::AssetDir(assets, "*", MAX_SIZE, 0);
}
