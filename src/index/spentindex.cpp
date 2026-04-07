// Copyright (c) 2024-present The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <index/spentindex.h>

#include <assets/assets.h>
#include <common/args.h>
#include <crypto/hash.h>
#include <interfaces/chain.h>
#include <logging.h>
#include <script/script.h>
#include <script/standard.h>
#include <undo.h>

constexpr uint8_t DB_SPENTINDEX{'p'};

std::unique_ptr<SpentIndex> g_spentindex;

// ---------------------------------------------------------------------------
// DB class
// ---------------------------------------------------------------------------
class SpentIndex::DB : public BaseIndex::DB
{
public:
    explicit DB(size_t n_cache_size, bool f_memory = false, bool f_wipe = false)
        : BaseIndex::DB(gArgs.GetDataDirNet() / "indexes" / "spentindex",
                        n_cache_size, f_memory, f_wipe) {}

    bool ReadSpentIndex(CSpentIndexKey& key, CSpentIndexValue& value) const
    {
        return Read(std::make_pair(DB_SPENTINDEX, key), value);
    }

    bool UpdateSpentIndex(const std::vector<std::pair<CSpentIndexKey, CSpentIndexValue>>& vect)
    {
        CDBBatch batch(*this);
        for (const auto& it : vect) {
            if (it.second.IsNull())
                batch.Erase(std::make_pair(DB_SPENTINDEX, it.first));
            else
                batch.Write(std::make_pair(DB_SPENTINDEX, it.first), it.second);
        }
        return WriteBatch(batch);
    }
};

// ---------------------------------------------------------------------------
// SpentIndex
// ---------------------------------------------------------------------------
SpentIndex::SpentIndex(std::unique_ptr<interfaces::Chain> chain, size_t n_cache_size,
                       bool f_memory, bool f_wipe)
    : BaseIndex(std::move(chain), "spentindex"),
      m_db(std::make_unique<SpentIndex::DB>(n_cache_size, f_memory, f_wipe))
{}

SpentIndex::~SpentIndex() = default;

// Resolve a script to (addressType, hashBytes) for the spent index value.
// Returns addressType=0 if unknown.
static std::pair<int, uint160> DecodeScriptForSpent(const CScript& script)
{
    if (script.IsPayToScriptHash()) {
        return {2, uint160(std::vector<unsigned char>(script.begin() + 2, script.begin() + 22))};
    } else if (script.size() == 25 && script[0] == OP_DUP && script[1] == OP_HASH160 &&
               script[2] == 20 && script[23] == OP_EQUALVERIFY && script[24] == OP_CHECKSIG) {
        return {1, uint160(std::vector<unsigned char>(script.begin() + 3, script.begin() + 23))};
    } else if ((script.size() == 35 || script.size() == 67) && script[script.size() - 1] == OP_CHECKSIG) {
        return {1, Hash160(std::vector<unsigned char>(script.begin() + 1, script.end() - 1))};
    }
    return {0, uint160()};
}

bool SpentIndex::CustomAppend(const interfaces::BlockInfo& block)
{
    if (block.height == 0) return true;

    assert(block.data);
    assert(block.undo_data);

    std::vector<std::pair<CSpentIndexKey, CSpentIndexValue>> spentIndex;

    const CBlock& blk = *block.data;
    const CBlockUndo& blockUndo = *block.undo_data;

    for (int i = 1; i < (int)blk.vtx.size(); i++) { // skip coinbase
        const CTransaction& tx = *blk.vtx[i];
        const CTxUndo& txundo = blockUndo.vtxundo[i - 1];

        for (unsigned int j = 0; j < tx.vin.size(); j++) {
            const CTxOut& prevout = txundo.vprevout[j].out;
            auto [addressType, hashBytes] = DecodeScriptForSpent(prevout.scriptPubKey);

            spentIndex.push_back({
                CSpentIndexKey(tx.vin[j].prevout.hash.ToUint256(), tx.vin[j].prevout.n),
                CSpentIndexValue(tx.GetHash().ToUint256(), j, block.height,
                                 prevout.nValue, addressType, hashBytes)});
        }
    }

    if (!m_db->UpdateSpentIndex(spentIndex)) {
        LogError("%s: failed to write spent index", __func__);
        return false;
    }
    return true;
}

bool SpentIndex::CustomRemove(const interfaces::BlockInfo& block)
{
    if (block.height == 0) return true;

    assert(block.data);
    assert(block.undo_data);

    std::vector<std::pair<CSpentIndexKey, CSpentIndexValue>> spentIndex;

    const CBlock& blk = *block.data;
    const CBlockUndo& blockUndo = *block.undo_data;

    for (int i = 1; i < (int)blk.vtx.size(); i++) { // skip coinbase
        const CTransaction& tx = *blk.vtx[i];

        for (unsigned int j = 0; j < tx.vin.size(); j++) {
            spentIndex.push_back({
                CSpentIndexKey(tx.vin[j].prevout.hash.ToUint256(), tx.vin[j].prevout.n),
                CSpentIndexValue()});  // IsNull() → erase
        }
    }

    if (!m_db->UpdateSpentIndex(spentIndex)) {
        LogError("%s: failed to erase spent index", __func__);
        return false;
    }
    return true;
}

BaseIndex::DB& SpentIndex::GetDB() const { return *m_db; }

bool SpentIndex::ReadSpentIndex(CSpentIndexKey& key, CSpentIndexValue& value) const
{
    return m_db->ReadSpentIndex(key, value);
}
