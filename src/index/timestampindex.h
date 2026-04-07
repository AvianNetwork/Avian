// Copyright (c) 2024-present The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_INDEX_TIMESTAMPINDEX_H
#define BITCOIN_INDEX_TIMESTAMPINDEX_H

#include <index/base.h>
#include <timestampindex.h>

static constexpr bool DEFAULT_TIMESTAMPINDEX{false};

/**
 * TimestampIndex maps block timestamps to block hashes, enabling time-range
 * queries via the getblockhashes RPC.
 *
 * The on-disk DB lives under indexes/timestampindex/ and contains two key spaces:
 *   's' (DB_TIMESTAMPINDEX)  — CTimestampIndexKey       -> 0
 *   'z' (DB_BLOCKHASHINDEX)  — CTimestampBlockIndexKey  -> CTimestampBlockIndexValue
 */
class TimestampIndex final : public BaseIndex
{
protected:
    class DB;

private:
    const std::unique_ptr<DB> m_db;

    bool AllowPrune() const override { return false; }

protected:
    bool CustomAppend(const interfaces::BlockInfo& block) override;

    BaseIndex::DB& GetDB() const override;

public:
    explicit TimestampIndex(std::unique_ptr<interfaces::Chain> chain, size_t n_cache_size,
                            bool f_memory = false, bool f_wipe = false);
    virtual ~TimestampIndex() override;

    /// Read block hashes within a timestamp range [low, high).
    bool ReadTimestampIndex(unsigned int high, unsigned int low,
                            std::vector<std::pair<uint256, unsigned int>>& hashes);

    /// Read the logical timestamp for a specific block hash.
    bool ReadTimestampBlockIndex(const uint256& hash, unsigned int& logicalTS) const;
};

/// Global timestamp index instance. Null when -timestampindex is not enabled.
extern std::unique_ptr<TimestampIndex> g_timestampindex;

#endif // BITCOIN_INDEX_TIMESTAMPINDEX_H
