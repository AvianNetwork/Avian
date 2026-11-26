// Copyright (c) 2026-present The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// A mempool transaction that fails the asset rules in a block must not stop the
// node from building templates, and the names a transaction issues are tracked
// so that it is refused or evicted when another transaction issues one first.

#include <addresstype.h>
#include <assets/assets.h>
#include <assets/assettypes.h>
#include <consensus/amount.h>
#include <node/miner.h>
#include <policy/packages.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <test/util/setup_common.h>
#include <test/util/txmempool.h>
#include <txmempool.h>
#include <uint256.h>
#include <util/strencodings.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using node::BlockAssembler;
using node::CBlockTemplate;

namespace {
const CAmount FEE{10000};

CScript Holder()
{
    return GetScriptForDestination(PKHash{uint160{ParseHex("00112233445566778899aabbccddeeff00112233")}});
}

// Spends a coinbase to move an asset it does not hold. Its inputs exist, so it
// is in order for the mempool map, and it fails CheckTxAssets in a block.
CMutableTransaction MovesAssetItDoesNotHold(const CTransactionRef& coinbase)
{
    CMutableTransaction tx;
    tx.vin.emplace_back(COutPoint{coinbase->GetHash(), 0});
    tx.vout.emplace_back(coinbase->vout[0].nValue - FEE, CScript{} << OP_TRUE);
    CScript transfer{Holder()};
    CAssetTransfer{"AVIANTEST", COIN}.ConstructTransaction(transfer);
    tx.vout.emplace_back(0, transfer);
    return tx;
}

CMutableTransaction Issues(const std::vector<std::string>& names, uint8_t input)
{
    CMutableTransaction tx;
    tx.vin.emplace_back(COutPoint{Txid::FromUint256(uint256{input}), 0});
    for (const std::string& name : names) {
        CScript script{Holder()};
        if (name.back() == '!') {
            CNewAsset{name.substr(0, name.size() - 1), COIN}.ConstructOwnerTransaction(script);
        } else {
            CNewAsset{name, UNIQUE_ASSET_AMOUNT, UNIQUE_ASSET_UNITS, UNIQUE_ASSETS_REISSUABLE, /*nHasIPFS=*/0, /*strIPFSHash=*/""}.ConstructTransaction(script);
        }
        tx.vout.emplace_back(0, script);
    }
    return tx;
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(asset_template_tests, TestChain100Setup)

BOOST_AUTO_TEST_CASE(template_drops_a_transaction_that_fails_the_asset_rules)
{
    mineBlocks(15); // so that the first coinbases can be spent
    CTxMemPool& pool{*Assert(m_node.mempool)};
    BlockAssembler::Options options;
    options.coinbase_output_script = CScript{} << OP_TRUE;

    const CMutableTransaction good{CreateValidMempoolTransaction(m_coinbase_txns[0], 0, /*input_height=*/1, coinbaseKey, Holder())};
    const CMutableTransaction bad{MovesAssetItDoesNotHold(m_coinbase_txns[1])};
    CMutableTransaction child;
    child.vin.emplace_back(COutPoint{bad.GetHash(), 0});
    child.vout.emplace_back(bad.vout[0].nValue - FEE, CScript{} << OP_TRUE);
    {
        LOCK2(::cs_main, pool.cs);
        TestMemPoolEntryHelper entry;
        AddToMempool(pool, entry.Fee(FEE).SpendsCoinbase(true).FromTx(bad));
        AddToMempool(pool, entry.Fee(FEE).SpendsCoinbase(false).FromTx(child));
        BOOST_REQUIRE_EQUAL(pool.size(), 3U);
    }

    const std::unique_ptr<CBlockTemplate> block_template{BlockAssembler{m_node.chainman->ActiveChainstate(), &pool, options}.CreateNewBlock()};
    BOOST_REQUIRE(block_template);
    BOOST_REQUIRE_EQUAL(block_template->block.vtx.size(), 2U);
    BOOST_CHECK(block_template->block.vtx[1]->GetHash() == good.GetHash());

    BOOST_CHECK(pool.exists(good.GetHash()));
    BOOST_CHECK(!pool.exists(bad.GetHash()));
    BOOST_CHECK(!pool.exists(child.GetHash()));
}

// One call removes a bounded number of transactions, so that a mempool being
// refilled cannot hold the caller for ever. The next call carries on.
BOOST_AUTO_TEST_CASE(template_eviction_is_bounded_per_call)
{
    mineBlocks(15); // so that the first coinbases can be spent
    CTxMemPool& pool{*Assert(m_node.mempool)};
    BlockAssembler::Options options;
    options.coinbase_output_script = CScript{} << OP_TRUE;
    {
        LOCK2(::cs_main, pool.cs);
        TestMemPoolEntryHelper entry;
        for (int i{0}; i < 12; ++i) {
            AddToMempool(pool, entry.Fee(FEE).SpendsCoinbase(true).FromTx(MovesAssetItDoesNotHold(m_coinbase_txns[i])));
        }
    }

    BlockAssembler assembler{m_node.chainman->ActiveChainstate(), &pool, options};
    BOOST_CHECK_THROW(assembler.CreateNewBlock(), std::runtime_error);
    BOOST_CHECK_EQUAL(pool.size(), 2U);

    const std::unique_ptr<CBlockTemplate> block_template{assembler.CreateNewBlock()};
    BOOST_REQUIRE(block_template);
    BOOST_CHECK_EQUAL(block_template->block.vtx.size(), 1U);
    BOOST_CHECK_EQUAL(pool.size(), 0U);
}

// The asset checks of mempool acceptance cannot see the other members of a
// package, so a package with an asset transaction in it is not evaluated.
BOOST_AUTO_TEST_CASE(asset_transactions_are_not_accepted_as_a_package)
{
    mineBlocks(15);
    CTxMemPool& pool{*Assert(m_node.mempool)};
    const CTransactionRef parent{MakeTransactionRef(CreateValidMempoolTransaction(
        m_coinbase_txns[0], 0, /*input_height=*/1, coinbaseKey, Holder(),
        m_coinbase_txns[0]->vout[0].nValue - FEE, /*submit=*/false))};

    CMutableTransaction plain_child;
    plain_child.vin.emplace_back(COutPoint{parent->GetHash(), 0});
    plain_child.vout.emplace_back(parent->vout[0].nValue - FEE, Holder());
    CMutableTransaction asset_child{plain_child};
    CScript transfer{Holder()};
    CAssetTransfer{"AVIANTEST", COIN}.ConstructTransaction(transfer);
    asset_child.vout.emplace_back(0, transfer);

    const auto reason = [&](const CMutableTransaction& child) {
        const Package package{parent, MakeTransactionRef(child)};
        LOCK(::cs_main);
        return ProcessNewPackage(m_node.chainman->ActiveChainstate(), pool, package, /*test_accept=*/true, /*client_maxfeerate=*/{}).m_state.GetRejectReason();
    };
    BOOST_CHECK_EQUAL(reason(asset_child), "package-asset-transaction");
    BOOST_CHECK(reason(plain_child) != "package-asset-transaction");
}

// A unique-asset issuance creates one name per output. Each is tracked, the
// owner token it carries is not, and the names are let go with the transaction.
BOOST_AUTO_TEST_CASE(every_issued_name_is_tracked)
{
    CTxMemPool& pool{*Assert(m_node.mempool)};
    LOCK2(::cs_main, pool.cs);
    TestMemPoolEntryHelper entry;

    const CMutableTransaction first{Issues({"AVIANTEST#ONE", "AVIANTEST#TWO", "AVIANTEST!"}, 1)};
    const uint256 first_hash{first.GetHash().ToUint256()};
    AddToMempool(pool, entry.FromTx(first));
    BOOST_CHECK_EQUAL(pool.mapAssetToHash.size(), 2U);
    BOOST_CHECK(pool.mapAssetToHash.at("AVIANTEST#ONE") == first_hash);
    BOOST_CHECK(pool.mapAssetToHash.at("AVIANTEST#TWO") == first_hash);
    BOOST_CHECK_EQUAL(pool.mapHashToAsset.at(first_hash).size(), 2U);

    // The mempool is asked about a unique name as about any other
    std::string error;
    const CTransaction again{Issues({"AVIANTEST#TWO"}, 2)};
    BOOST_CHECK(ContextualCheckUniqueAssetTx(GetCurrentAssetCache(), error, again));
    BOOST_CHECK(!ContextualCheckUniqueAssetTx(GetCurrentAssetCache(), error, again, &pool));
    BOOST_CHECK(error.find("already being created in the mempool") != std::string::npos);

    // A block that issues one of the names evicts the transaction
    ConnectedBlockAssetData data;
    data.newAssetsToAdd.emplace(CNewAsset{"AVIANTEST#TWO", UNIQUE_ASSET_AMOUNT}, "address", 101, uint256::ONE);
    pool.removeForBlock({}, /*nBlockHeight=*/101, data);
    BOOST_CHECK_EQUAL(pool.size(), 0U);
    BOOST_CHECK(pool.mapAssetToHash.empty());
    BOOST_CHECK(pool.mapHashToAsset.empty());

    // When two transactions issue a name, removing one leaves the other's claim
    const CMutableTransaction second{Issues({"AVIANTEST#ONE"}, 3)};
    AddToMempool(pool, entry.FromTx(first));
    AddToMempool(pool, entry.FromTx(second));
    BOOST_CHECK(pool.mapAssetToHash.at("AVIANTEST#ONE") == second.GetHash().ToUint256());
    pool.removeRecursive(CTransaction{first}, MemPoolRemovalReason::EXPIRY);
    BOOST_CHECK_EQUAL(pool.mapAssetToHash.size(), 1U);
    BOOST_CHECK(pool.mapAssetToHash.at("AVIANTEST#ONE") == second.GetHash().ToUint256());
    pool.removeRecursive(CTransaction{second}, MemPoolRemovalReason::EXPIRY);
    BOOST_CHECK(pool.mapAssetToHash.empty());
}

BOOST_AUTO_TEST_SUITE_END()
