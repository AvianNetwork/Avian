// Copyright (c) 2024-present The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_INDEX_SPENTINDEX_H
#define BITCOIN_INDEX_SPENTINDEX_H

#include <index/base.h>
#include <spentindex.h>

static constexpr bool DEFAULT_SPENTINDEX{false};

/**
 * SpentIndex maps outpoints (txid:n) to the spending transaction.
 *
 * The on-disk DB lives under indexes/spentindex/ and contains one key space:
 *   'p' (DB_SPENTINDEX) — CSpentIndexKey -> CSpentIndexValue
 */
class SpentIndex final : public BaseIndex
{
protected:
    class DB;

private:
    const std::unique_ptr<DB> m_db;

    bool AllowPrune() const override { return false; }

protected:
    interfaces::Chain::NotifyOptions CustomOptions() override
    {
        return {.connect_undo_data    = true,
                .disconnect_undo_data = true};
    }

    bool CustomAppend(const interfaces::BlockInfo& block) override;
    bool CustomRemove(const interfaces::BlockInfo& block) override;

    BaseIndex::DB& GetDB() const override;

public:
    explicit SpentIndex(std::unique_ptr<interfaces::Chain> chain, size_t n_cache_size,
                        bool f_memory = false, bool f_wipe = false);
    virtual ~SpentIndex() override;

    /// Look up spending transaction for an outpoint.
    bool ReadSpentIndex(CSpentIndexKey& key, CSpentIndexValue& value) const;
};

/// Global spent index instance. Null when -spentindex is not enabled.
extern std::unique_ptr<SpentIndex> g_spentindex;

#endif // BITCOIN_INDEX_SPENTINDEX_H
