// Copyright (c) 2024-present The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <index/addressindex.h>

#include <addressindex.h>
#include <assets/assets.h>
#include <common/args.h>
#include <crypto/hash.h>
#include <interfaces/chain.h>
#include <logging.h>
#include <node/blockstorage.h>
#include <script/script.h>
#include <script/standard.h>
#include <undo.h>

constexpr uint8_t DB_ADDRESSINDEX{'a'};
constexpr uint8_t DB_ADDRESSUNSPENTINDEX{'u'};

std::unique_ptr<AddressIndex> g_addressindex;

// ---------------------------------------------------------------------------
// Internal helper: resolve script -> (addressType, hashBytes, assetName, value)
// Returns addressType=0 if no known script pattern.
// ---------------------------------------------------------------------------
struct ScriptAddressInfo {
    unsigned int addressType{0};
    uint160 hashBytes;
    std::string assetName;
    CAmount value{0};
    bool isAsset{false};
};

static ScriptAddressInfo DecodeScript(const CScript& script, CAmount fallbackValue)
{
    ScriptAddressInfo info;
    info.value = fallbackValue;

    if (script.IsPayToScriptHash()) {
        info.addressType = 2;
        info.hashBytes = uint160(std::vector<unsigned char>(script.begin() + 2, script.begin() + 22));
    } else if (script.size() == 25 && script[0] == OP_DUP && script[1] == OP_HASH160 &&
               script[2] == 20 && script[23] == OP_EQUALVERIFY && script[24] == OP_CHECKSIG) {
        info.addressType = 1;
        info.hashBytes = uint160(std::vector<unsigned char>(script.begin() + 3, script.begin() + 23));
    } else if ((script.size() == 35 || script.size() == 67) && script[script.size() - 1] == OP_CHECKSIG) {
        info.addressType = 1;
        info.hashBytes = Hash160(std::vector<unsigned char>(script.begin() + 1, script.end() - 1));
    } else {
        int nType = 0;
        bool fIsOwner = false;
        if (script.IsAssetScript(nType, fIsOwner)) {
            CAssetOutputEntry assetData;
            if (GetAssetData(script, assetData)) {
                if (auto* keyID = std::get_if<PKHash>(&assetData.destination)) {
                    info.addressType = 1;
                    info.hashBytes = ToKeyID(*keyID);
                    info.assetName = assetData.assetName;
                    info.value = assetData.nAmount;
                    info.isAsset = true;
                }
            }
        }
    }
    return info;
}

// ---------------------------------------------------------------------------
// DB class
// ---------------------------------------------------------------------------
class AddressIndex::DB : public BaseIndex::DB
{
public:
    explicit DB(size_t n_cache_size, bool f_memory = false, bool f_wipe = false)
        : BaseIndex::DB(gArgs.GetDataDirNet() / "indexes" / "addressindex",
                        n_cache_size, f_memory, f_wipe) {}

    bool WriteAddressIndex(const std::vector<std::pair<CAddressIndexKey, CAmount>>& vect)
    {
        CDBBatch batch(*this);
        for (const auto& it : vect)
            batch.Write(std::make_pair(DB_ADDRESSINDEX, it.first), it.second);
        return WriteBatch(batch);
    }

    bool EraseAddressIndex(const std::vector<std::pair<CAddressIndexKey, CAmount>>& vect)
    {
        CDBBatch batch(*this);
        for (const auto& it : vect)
            batch.Erase(std::make_pair(DB_ADDRESSINDEX, it.first));
        return WriteBatch(batch);
    }

    bool UpdateAddressUnspentIndex(const std::vector<std::pair<CAddressUnspentKey, CAddressUnspentValue>>& vect)
    {
        CDBBatch batch(*this);
        for (const auto& it : vect) {
            if (it.second.IsNull())
                batch.Erase(std::make_pair(DB_ADDRESSUNSPENTINDEX, it.first));
            else
                batch.Write(std::make_pair(DB_ADDRESSUNSPENTINDEX, it.first), it.second);
        }
        return WriteBatch(batch);
    }

    bool ReadAddressIndex(uint160 addressHash, int type, std::string assetName,
                          std::vector<std::pair<CAddressIndexKey, CAmount>>& addressIndex,
                          int start, int end) const
    {
        std::unique_ptr<CDBIterator> pcursor(NewIterator());

        if (!assetName.empty() && start > 0)
            pcursor->Seek(std::make_pair(DB_ADDRESSINDEX, CAddressIndexIteratorHeightKey(type, addressHash, assetName, start)));
        else if (!assetName.empty())
            pcursor->Seek(std::make_pair(DB_ADDRESSINDEX, CAddressIndexIteratorAssetKey(type, addressHash, assetName)));
        else if (start > 0)
            pcursor->Seek(std::make_pair(DB_ADDRESSINDEX, CAddressIndexIteratorHeightKey(type, addressHash, start)));
        else
            pcursor->Seek(std::make_pair(DB_ADDRESSINDEX, CAddressIndexIteratorKey(type, addressHash)));

        while (pcursor->Valid()) {
            std::pair<uint8_t, CAddressIndexKey> key;
            if (pcursor->GetKey(key) && key.first == DB_ADDRESSINDEX && key.second.hashBytes == addressHash) {
                if (!assetName.empty() && key.second.asset != assetName) break;
                if (end > 0 && key.second.blockHeight > end) break;
                CAmount nValue;
                if (pcursor->GetValue(nValue)) {
                    addressIndex.push_back({key.second, nValue});
                    pcursor->Next();
                } else {
                    LogPrintf("%s: failed to get address index value\n", __func__);
                    return false;
                }
            } else {
                break;
            }
        }
        return true;
    }

    bool ReadAddressUnspentIndex(uint160 addressHash, int type, std::string assetName,
                                 std::vector<std::pair<CAddressUnspentKey, CAddressUnspentValue>>& vect) const
    {
        std::unique_ptr<CDBIterator> pcursor(NewIterator());

        if (assetName.empty())
            pcursor->Seek(std::make_pair(DB_ADDRESSUNSPENTINDEX, CAddressIndexIteratorKey(type, addressHash)));
        else
            pcursor->Seek(std::make_pair(DB_ADDRESSUNSPENTINDEX, CAddressIndexIteratorAssetKey(type, addressHash, assetName)));

        while (pcursor->Valid()) {
            std::pair<uint8_t, CAddressUnspentKey> key;
            if (pcursor->GetKey(key) && key.first == DB_ADDRESSUNSPENTINDEX && key.second.hashBytes == addressHash) {
                if (!assetName.empty() && key.second.asset != assetName) break;
                CAddressUnspentValue nValue;
                if (pcursor->GetValue(nValue)) {
                    vect.push_back({key.second, nValue});
                    pcursor->Next();
                } else {
                    LogPrintf("%s: failed to get address unspent value\n", __func__);
                    return false;
                }
            } else {
                break;
            }
        }
        return true;
    }
};

// ---------------------------------------------------------------------------
// AddressIndex
// ---------------------------------------------------------------------------
AddressIndex::AddressIndex(std::unique_ptr<interfaces::Chain> chain, size_t n_cache_size,
                           bool f_memory, bool f_wipe)
    : BaseIndex(std::move(chain), "addressindex"),
      m_db(std::make_unique<AddressIndex::DB>(n_cache_size, f_memory, f_wipe))
{}

AddressIndex::~AddressIndex() = default;

bool AddressIndex::CustomAppend(const interfaces::BlockInfo& block)
{
    // Genesis block has no spendable outputs — nothing to index on input side.
    if (block.height == 0) return true;

    assert(block.data);
    assert(block.undo_data);

    std::vector<std::pair<CAddressIndexKey, CAmount>>           addressIndex;
    std::vector<std::pair<CAddressUnspentKey, CAddressUnspentValue>> addressUnspentIndex;

    const CBlock& blk = *block.data;
    const CBlockUndo& blockUndo = *block.undo_data;

    for (int i = 0; i < (int)blk.vtx.size(); i++) {
        const CTransaction& tx = *blk.vtx[i];
        const uint256 txhash = tx.GetHash().ToUint256();

        // Inputs (spending side) — skip coinbase
        if (!tx.IsCoinBase()) {
            const CTxUndo& txundo = blockUndo.vtxundo[i - 1]; // vtxundo skips coinbase
            for (unsigned int j = 0; j < tx.vin.size(); j++) {
                const CTxOut& prevout = txundo.vprevout[j].out;
                auto info = DecodeScript(prevout.scriptPubKey, prevout.nValue);
                if (info.addressType == 0) continue;

                if (info.isAsset) {
                    addressIndex.push_back({
                        CAddressIndexKey(info.addressType, info.hashBytes, info.assetName,
                                         block.height, i, txhash, j, /*spending=*/true),
                        info.value * -1});
                    addressUnspentIndex.push_back({
                        CAddressUnspentKey(info.addressType, info.hashBytes, info.assetName,
                                           tx.vin[j].prevout.hash.ToUint256(), tx.vin[j].prevout.n),
                        CAddressUnspentValue()});
                } else {
                    addressIndex.push_back({
                        CAddressIndexKey(info.addressType, info.hashBytes,
                                         block.height, i, txhash, j, /*spending=*/true),
                        prevout.nValue * -1});
                    addressUnspentIndex.push_back({
                        CAddressUnspentKey(info.addressType, info.hashBytes,
                                           tx.vin[j].prevout.hash.ToUint256(), tx.vin[j].prevout.n),
                        CAddressUnspentValue()});
                }
            }
        }

        // Outputs (receiving side)
        for (unsigned int k = 0; k < tx.vout.size(); k++) {
            const CTxOut& out = tx.vout[k];
            auto info = DecodeScript(out.scriptPubKey, out.nValue);
            if (info.addressType == 0) continue;

            if (info.isAsset) {
                addressIndex.push_back({
                    CAddressIndexKey(info.addressType, info.hashBytes, info.assetName,
                                     block.height, i, txhash, k, /*spending=*/false),
                    info.value});
                addressUnspentIndex.push_back({
                    CAddressUnspentKey(info.addressType, info.hashBytes, info.assetName,
                                       txhash, k),
                    CAddressUnspentValue(info.value, out.scriptPubKey, block.height)});
            } else {
                addressIndex.push_back({
                    CAddressIndexKey(info.addressType, info.hashBytes,
                                     block.height, i, txhash, k, /*spending=*/false),
                    out.nValue});
                addressUnspentIndex.push_back({
                    CAddressUnspentKey(info.addressType, info.hashBytes, txhash, k),
                    CAddressUnspentValue(out.nValue, out.scriptPubKey, block.height)});
            }
        }
    }

    if (!m_db->WriteAddressIndex(addressIndex)) {
        LogError("%s: failed to write address index", __func__);
        return false;
    }
    if (!m_db->UpdateAddressUnspentIndex(addressUnspentIndex)) {
        LogError("%s: failed to update address unspent index", __func__);
        return false;
    }
    return true;
}

bool AddressIndex::CustomRemove(const interfaces::BlockInfo& block)
{
    if (block.height == 0) return true;

    assert(block.data);
    assert(block.undo_data);

    std::vector<std::pair<CAddressIndexKey, CAmount>>           addressIndex;
    std::vector<std::pair<CAddressUnspentKey, CAddressUnspentValue>> addressUnspentIndex;

    const CBlock& blk = *block.data;
    const CBlockUndo& blockUndo = *block.undo_data;

    for (int i = 0; i < (int)blk.vtx.size(); i++) {
        const CTransaction& tx = *blk.vtx[i];
        const uint256 txhash = tx.GetHash().ToUint256();

        // Undo outputs: erase receiving entries and unspent entries
        for (unsigned int o = 0; o < tx.vout.size(); o++) {
            auto info = DecodeScript(tx.vout[o].scriptPubKey, tx.vout[o].nValue);
            if (info.addressType == 0) continue;

            if (info.isAsset) {
                addressIndex.push_back({
                    CAddressIndexKey(info.addressType, info.hashBytes, info.assetName,
                                     block.height, i, txhash, o, /*spending=*/false),
                    info.value});
                addressUnspentIndex.push_back({
                    CAddressUnspentKey(info.addressType, info.hashBytes, info.assetName, txhash, o),
                    CAddressUnspentValue()});
            } else {
                addressIndex.push_back({
                    CAddressIndexKey(info.addressType, info.hashBytes,
                                     block.height, i, txhash, o, /*spending=*/false),
                    tx.vout[o].nValue});
                addressUnspentIndex.push_back({
                    CAddressUnspentKey(info.addressType, info.hashBytes, txhash, o),
                    CAddressUnspentValue()});
            }
        }

        // Undo inputs: restore unspent entries; erase spending entries
        if (!tx.IsCoinBase()) {
            const CTxUndo& txundo = blockUndo.vtxundo[i - 1];
            for (unsigned int j = 0; j < tx.vin.size(); j++) {
                const CTxOut& prevout = txundo.vprevout[j].out;
                int prevHeight = txundo.vprevout[j].nHeight;
                auto info = DecodeScript(prevout.scriptPubKey, prevout.nValue);
                if (info.addressType == 0) continue;

                // Erase the spending delta entry
                if (info.isAsset) {
                    addressIndex.push_back({
                        CAddressIndexKey(info.addressType, info.hashBytes, info.assetName,
                                         block.height, i, txhash, j, /*spending=*/true),
                        info.value * -1});
                    // Restore the unspent entry
                    addressUnspentIndex.push_back({
                        CAddressUnspentKey(info.addressType, info.hashBytes, info.assetName,
                                           tx.vin[j].prevout.hash.ToUint256(), tx.vin[j].prevout.n),
                        CAddressUnspentValue(info.value, prevout.scriptPubKey, prevHeight)});
                } else {
                    addressIndex.push_back({
                        CAddressIndexKey(info.addressType, info.hashBytes,
                                         block.height, i, txhash, j, /*spending=*/true),
                        prevout.nValue * -1});
                    addressUnspentIndex.push_back({
                        CAddressUnspentKey(info.addressType, info.hashBytes,
                                           tx.vin[j].prevout.hash.ToUint256(), tx.vin[j].prevout.n),
                        CAddressUnspentValue(prevout.nValue, prevout.scriptPubKey, prevHeight)});
                }
            }
        }
    }

    if (!m_db->EraseAddressIndex(addressIndex)) {
        LogError("%s: failed to erase address index", __func__);
        return false;
    }
    if (!m_db->UpdateAddressUnspentIndex(addressUnspentIndex)) {
        LogError("%s: failed to update address unspent index", __func__);
        return false;
    }
    return true;
}

BaseIndex::DB& AddressIndex::GetDB() const { return *m_db; }

bool AddressIndex::ReadAddressIndex(uint160 addressHash, int type, std::string assetName,
                                    std::vector<std::pair<CAddressIndexKey, CAmount>>& addressIndex,
                                    int start, int end) const
{
    return m_db->ReadAddressIndex(addressHash, type, assetName, addressIndex, start, end);
}

bool AddressIndex::ReadAddressIndex(uint160 addressHash, int type,
                                    std::vector<std::pair<CAddressIndexKey, CAmount>>& addressIndex,
                                    int start, int end) const
{
    return m_db->ReadAddressIndex(addressHash, type, "", addressIndex, start, end);
}

bool AddressIndex::ReadAddressUnspentIndex(uint160 addressHash, int type, std::string assetName,
                                           std::vector<std::pair<CAddressUnspentKey, CAddressUnspentValue>>& unspentOutputs) const
{
    return m_db->ReadAddressUnspentIndex(addressHash, type, assetName, unspentOutputs);
}

bool AddressIndex::ReadAddressUnspentIndex(uint160 addressHash, int type,
                                           std::vector<std::pair<CAddressUnspentKey, CAddressUnspentValue>>& unspentOutputs) const
{
    return m_db->ReadAddressUnspentIndex(addressHash, type, "", unspentOutputs);
}
