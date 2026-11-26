// Copyright (c) 2026-present The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// What a connected block does to the asset transactions left in the mempool,
// and what the mempool is allowed to say about a block.

#include <addresstype.h>
#include <assets/assets.h>
#include <assets/assettypes.h>
#include <coins.h>
#include <consensus/tx_verify.h>
#include <consensus/validation.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <test/util/setup_common.h>
#include <test/util/txmempool.h>
#include <txmempool.h>
#include <uint256.h>
#include <util/strencodings.h>

#include <boost/test/unit_test.hpp>

#include <string>
#include <utility>
#include <vector>

namespace {
CMutableTransaction Spend(const COutPoint& prevout)
{
    CMutableTransaction tx;
    tx.vin.resize(1);
    tx.vin[0].prevout = prevout;
    tx.vin[0].scriptSig = CScript{} << OP_11;
    tx.vout.resize(1);
    tx.vout[0].scriptPubKey = CScript{} << OP_11 << OP_EQUAL;
    tx.vout[0].nValue = 10000;
    return tx;
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(asset_mempool_tests, TestingSetup)

// Every map a block can evict from, with two transactions in the same set and a
// child of one of them that no map knows about.
BOOST_AUTO_TEST_CASE(block_evicts_tracked_transactions_with_descendants)
{
    CTxMemPool& pool{*Assert(m_node.mempool)};
    LOCK2(::cs_main, pool.cs);
    TestMemPoolEntryHelper entry;

    const std::string asset{"$TOKEN"};
    const std::string address{"address"};
    const auto frozen_key{std::make_pair(address, asset)};

    const auto fill = [&](const auto& track) EXCLUSIVE_LOCKS_REQUIRED(::cs_main, pool.cs) {
        const CMutableTransaction first{Spend(COutPoint{Txid::FromUint256(uint256{1}), 0})};
        const CMutableTransaction second{Spend(COutPoint{Txid::FromUint256(uint256{2}), 0})};
        const CMutableTransaction child{Spend(COutPoint{first.GetHash(), 0})};
        const CMutableTransaction unrelated{Spend(COutPoint{Txid::FromUint256(uint256{3}), 0})};
        for (const auto& tx : {first, second, child, unrelated}) AddToMempool(pool, entry.FromTx(tx));
        track(first.GetHash().ToUint256());
        track(second.GetHash().ToUint256());
        BOOST_REQUIRE_EQUAL(pool.size(), 4U);
        return unrelated.GetHash();
    };
    const auto connect = [&](ConnectedBlockAssetData& data, const Txid& unrelated) EXCLUSIVE_LOCKS_REQUIRED(::cs_main, pool.cs) {
        pool.removeForBlock({}, /*nBlockHeight=*/1, data);
        BOOST_CHECK_EQUAL(pool.size(), 1U);
        BOOST_CHECK(pool.exists(unrelated));
        pool.removeRecursive(*Assert(pool.get(unrelated)), MemPoolRemovalReason::EXPIRY);
        BOOST_REQUIRE_EQUAL(pool.size(), 0U);
    };

    {
        const Txid unrelated{fill([&](const uint256& hash) EXCLUSIVE_LOCKS_REQUIRED(pool.cs) {
            pool.mapAssetVerifierChanged[asset].insert(hash);
            pool.mapHashVerifierChanged[hash].insert(asset);
        })};
        ConnectedBlockAssetData data;
        data.newVerifiersToAdd.emplace(asset, "KYC");
        connect(data, unrelated);
        BOOST_CHECK(pool.mapHashVerifierChanged.empty());
    }
    {
        const Txid unrelated{fill([&](const uint256& hash) EXCLUSIVE_LOCKS_REQUIRED(pool.cs) {
            pool.mapAddressesQualifiersChanged[address].insert(hash);
            pool.mapHashQualifiersChanged[hash].insert(address);
        })};
        ConnectedBlockAssetData data;
        data.newQualifiersToAdd.emplace("#KYC", address, QualifierType::ADD_QUALIFIER);
        connect(data, unrelated);
        BOOST_CHECK(pool.mapHashQualifiersChanged.empty());
    }
    {
        const Txid unrelated{fill([&](const uint256& hash) EXCLUSIVE_LOCKS_REQUIRED(pool.cs) {
            pool.mapAssetMarkedGlobalFrozen[asset].insert(hash);
            pool.mapHashMarkedGlobalFrozen[hash].insert(asset);
        })};
        ConnectedBlockAssetData data;
        data.newGlobalRestrictionsToAdd.emplace(asset, RestrictedType::GLOBAL_FREEZE);
        connect(data, unrelated);
        BOOST_CHECK(pool.mapHashMarkedGlobalFrozen.empty());
    }
    {
        // removeUnchecked() drops the whole map entry once its set is empty
        const Txid unrelated{fill([&](const uint256& hash) EXCLUSIVE_LOCKS_REQUIRED(pool.cs) {
            pool.mapGlobalFreezingAssetTransactions[asset].insert(hash);
            pool.mapHashGlobalFreezingAssetTransactions[hash].insert(asset);
        })};
        ConnectedBlockAssetData data;
        data.newGlobalRestrictionsToAdd.emplace(asset, RestrictedType::GLOBAL_FREEZE);
        connect(data, unrelated);
        BOOST_CHECK(pool.mapGlobalFreezingAssetTransactions.empty());
    }
    {
        const Txid unrelated{fill([&](const uint256& hash) EXCLUSIVE_LOCKS_REQUIRED(pool.cs) {
            pool.mapGlobalUnFreezingAssetTransactions[asset].insert(hash);
            pool.mapHashGlobalUnFreezingAssetTransactions[hash].insert(asset);
        })};
        ConnectedBlockAssetData data;
        data.newGlobalRestrictionsToAdd.emplace(asset, RestrictedType::GLOBAL_UNFREEZE);
        connect(data, unrelated);
        BOOST_CHECK(pool.mapGlobalUnFreezingAssetTransactions.empty());
    }
    {
        const Txid unrelated{fill([&](const uint256& hash) EXCLUSIVE_LOCKS_REQUIRED(pool.cs) {
            pool.mapAddressesMarkedFrozen[frozen_key].insert(hash);
            pool.mapHashToAddressMarkedFrozen[hash].insert(frozen_key);
        })};
        ConnectedBlockAssetData data;
        data.newAddressRestrictionsToAdd.emplace(asset, address, RestrictedType::FREEZE_ADDRESS);
        connect(data, unrelated);
        BOOST_CHECK(pool.mapHashToAddressMarkedFrozen.empty());
    }
}

// One unconfirmed reissue per asset is what the mempool allows itself. It says
// nothing about a block: the node that holds a different reissue of the asset
// would otherwise refuse a block every other node accepts.
BOOST_AUTO_TEST_CASE(reissue_lock_is_mempool_only)
{
    CTxMemPool& pool{*Assert(m_node.mempool)};
    LOCK2(::cs_main, pool.cs);

    const std::string name{"AVIANTEST"};
    const COutPoint prevout{Txid::FromUint256(uint256{1}), 0};
    CCoinsView base;
    CCoinsViewCache view{&base};
    view.AddCoin(prevout, Coin{CTxOut{COIN, CScript{} << OP_11 << OP_EQUAL}, 1, false}, false);

    CMutableTransaction mtx;
    mtx.vin.emplace_back(prevout);
    CScript script{GetScriptForDestination(PKHash{uint160{ParseHex("00112233445566778899aabbccddeeff00112233")}})};
    CReissueAsset{name, COIN, /*nUnits=*/-1, /*nReissuable=*/1, /*strIPFSHash=*/"", /*strANSID=*/""}.ConstructTransaction(script);
    mtx.vout.emplace_back(0, script);
    const CTransaction tx{mtx};

    const auto check = [&](const CTxMemPool* mempool) EXCLUSIVE_LOCKS_REQUIRED(::cs_main) {
        TxValidationState state;
        std::vector<std::pair<std::string, uint256>> reissues;
        const bool valid{Consensus::CheckTxAssets(tx, state, view, /*assetCache=*/nullptr, mempool, reissues)};
        return valid ? std::string{} : state.GetRejectReason();
    };

    // Nothing holds the asset
    BOOST_CHECK_EQUAL(check(&pool), "");
    BOOST_CHECK_EQUAL(check(nullptr), "");

    // Another unconfirmed reissue of it does
    mapReissuedAssets[name] = uint256::ONE;
    BOOST_CHECK_EQUAL(check(&pool), "bad-tx-reissue-chaining-not-allowed");
    BOOST_CHECK_EQUAL(check(nullptr), "");

    // This one does
    mapReissuedAssets[name] = tx.GetHash().ToUint256();
    BOOST_CHECK_EQUAL(check(&pool), "");
    BOOST_CHECK_EQUAL(check(nullptr), "");

    mapReissuedAssets.erase(name);
}

BOOST_AUTO_TEST_SUITE_END()
