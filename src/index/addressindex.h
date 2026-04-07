// Copyright (c) 2024-present The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_INDEX_ADDRESSINDEX_H
#define BITCOIN_INDEX_ADDRESSINDEX_H

#include <addressindex.h>
#include <index/base.h>
#include <primitives/transaction.h>

static constexpr bool DEFAULT_ADDRESSINDEX{false};

/**
 * AddressIndex maintains a full address index (balance, txids, UTXOs).
 *
 * The on-disk DB lives under indexes/addressindex/ and contains two key spaces:
 *   'a' (DB_ADDRESSINDEX)       — CAddressIndexKey   -> CAmount (all deltas)
 *   'u' (DB_ADDRESSUNSPENTINDEX)— CAddressUnspentKey -> CAddressUnspentValue
 */
class AddressIndex final : public BaseIndex
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
                .disconnect_data      = true,
                .disconnect_undo_data = true};
    }

    bool CustomAppend(const interfaces::BlockInfo& block) override;
    bool CustomRemove(const interfaces::BlockInfo& block) override;

    BaseIndex::DB& GetDB() const override;

public:
    explicit AddressIndex(std::unique_ptr<interfaces::Chain> chain, size_t n_cache_size,
                          bool f_memory = false, bool f_wipe = false);
    virtual ~AddressIndex() override;

    /// Read all deltas for an address (optionally filtered by asset and/or block range).
    bool ReadAddressIndex(uint160 addressHash, int type, std::string assetName,
                          std::vector<std::pair<CAddressIndexKey, CAmount>>& addressIndex,
                          int start = 0, int end = 0) const;
    bool ReadAddressIndex(uint160 addressHash, int type,
                          std::vector<std::pair<CAddressIndexKey, CAmount>>& addressIndex,
                          int start = 0, int end = 0) const;

    /// Read unspent outputs for an address (optionally filtered by asset).
    bool ReadAddressUnspentIndex(uint160 addressHash, int type, std::string assetName,
                                 std::vector<std::pair<CAddressUnspentKey, CAddressUnspentValue>>& unspentOutputs) const;
    bool ReadAddressUnspentIndex(uint160 addressHash, int type,
                                 std::vector<std::pair<CAddressUnspentKey, CAddressUnspentValue>>& unspentOutputs) const;
};

/// Global address index instance. Null when -addressindex is not enabled.
extern std::unique_ptr<AddressIndex> g_addressindex;

#endif // BITCOIN_INDEX_ADDRESSINDEX_H
