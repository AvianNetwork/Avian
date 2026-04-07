// Copyright (c) 2024-present The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <index/timestampindex.h>

#include <common/args.h>
#include <interfaces/chain.h>
#include <logging.h>
#include <timestampindex.h>

constexpr uint8_t DB_TIMESTAMPINDEX{'s'};
constexpr uint8_t DB_BLOCKHASHINDEX{'z'};

std::unique_ptr<TimestampIndex> g_timestampindex;

// ---------------------------------------------------------------------------
// DB class
// ---------------------------------------------------------------------------
class TimestampIndex::DB : public BaseIndex::DB
{
public:
    explicit DB(size_t n_cache_size, bool f_memory = false, bool f_wipe = false)
        : BaseIndex::DB(gArgs.GetDataDirNet() / "indexes" / "timestampindex",
                        n_cache_size, f_memory, f_wipe) {}

    bool WriteTimestampIndex(const CTimestampIndexKey& timestampIndex)
    {
        CDBBatch batch(*this);
        batch.Write(std::make_pair(DB_TIMESTAMPINDEX, timestampIndex), 0);
        return WriteBatch(batch);
    }

    bool WriteTimestampBlockIndex(const CTimestampBlockIndexKey& blockhashIndex,
                                  const CTimestampBlockIndexValue& logicalts)
    {
        CDBBatch batch(*this);
        batch.Write(std::make_pair(DB_BLOCKHASHINDEX, blockhashIndex), logicalts);
        return WriteBatch(batch);
    }

    bool ReadTimestampIndex(unsigned int high, unsigned int low,
                            std::vector<std::pair<uint256, unsigned int>>& hashes)
    {
        std::unique_ptr<CDBIterator> pcursor(NewIterator());
        pcursor->Seek(std::make_pair(DB_TIMESTAMPINDEX, CTimestampIndexIteratorKey(low)));

        while (pcursor->Valid()) {
            std::pair<uint8_t, CTimestampIndexKey> key;
            if (pcursor->GetKey(key) && key.first == DB_TIMESTAMPINDEX && key.second.timestamp < high) {
                hashes.push_back({key.second.blockHash, key.second.timestamp});
                pcursor->Next();
            } else {
                break;
            }
        }
        return true;
    }

    bool ReadTimestampBlockIndex(const uint256& hash, unsigned int& logicalTS) const
    {
        CTimestampBlockIndexValue lts;
        if (!Read(std::make_pair(DB_BLOCKHASHINDEX, CTimestampBlockIndexKey(hash)), lts))
            return false;
        logicalTS = lts.ltimestamp;
        return true;
    }
};

// ---------------------------------------------------------------------------
// TimestampIndex
// ---------------------------------------------------------------------------
TimestampIndex::TimestampIndex(std::unique_ptr<interfaces::Chain> chain, size_t n_cache_size,
                               bool f_memory, bool f_wipe)
    : BaseIndex(std::move(chain), "timestampindex"),
      m_db(std::make_unique<TimestampIndex::DB>(n_cache_size, f_memory, f_wipe))
{}

TimestampIndex::~TimestampIndex() = default;

bool TimestampIndex::CustomAppend(const interfaces::BlockInfo& block)
{
    // Compute monotonically increasing logical timestamp
    unsigned int logicalTS = block.chain_time_max;  // block.chain_time_max is nTime for this block
    unsigned int prevLogicalTS = 0;

    if (block.prev_hash) {
        m_db->ReadTimestampBlockIndex(*block.prev_hash, prevLogicalTS);
    }
    if (logicalTS <= prevLogicalTS)
        logicalTS = prevLogicalTS + 1;

    if (!m_db->WriteTimestampIndex(CTimestampIndexKey(logicalTS, block.hash))) {
        LogError("%s: failed to write timestamp index", __func__);
        return false;
    }
    if (!m_db->WriteTimestampBlockIndex(CTimestampBlockIndexKey(block.hash),
                                        CTimestampBlockIndexValue(logicalTS))) {
        LogError("%s: failed to write timestamp block index", __func__);
        return false;
    }
    return true;
}

BaseIndex::DB& TimestampIndex::GetDB() const { return *m_db; }

bool TimestampIndex::ReadTimestampIndex(unsigned int high, unsigned int low,
                                        std::vector<std::pair<uint256, unsigned int>>& hashes)
{
    return m_db->ReadTimestampIndex(high, low, hashes);
}

bool TimestampIndex::ReadTimestampBlockIndex(const uint256& hash, unsigned int& logicalTS) const
{
    return m_db->ReadTimestampBlockIndex(hash, logicalTS);
}
