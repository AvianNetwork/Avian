// Copyright (c) 2026-present The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// What the asset database records about itself, and what the node does when a
// record in it cannot be read.

#include <addresstype.h>
#include <assets/assetdb.h>
#include <assets/assets.h>
#include <assets/assettypes.h>
#include <assets/restricteddb.h>
#include <coins.h>
#include <consensus/amount.h>
#include <key_io.h>
#include <node/blockstorage.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <util/strencodings.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace {
CTxDestination Holder(uint8_t tag)
{
    return PKHash{uint160{std::vector<unsigned char>(20, tag)}};
}

Coin AssetCoin(const std::string& name, CAmount amount, uint8_t holder)
{
    CScript script{GetScriptForDestination(Holder(holder))};
    CAssetTransfer{name, amount}.ConstructTransaction(script);
    return Coin{CTxOut{0, script}, /*nHeightIn=*/1, /*fCoinBaseIn=*/false};
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(asset_db_tests, TestChain100Setup)

// A block with no undo record is a block without reissues. A record that is there
// and cannot be read is not: the disconnect has to fail, as it does on 4.x.
BOOST_AUTO_TEST_CASE(disconnect_fails_on_an_unreadable_undo_record)
{
    LOCK(::cs_main);
    BOOST_REQUIRE(AreAssetsDeployed());
    Chainstate& chainstate{m_node.chainman->ActiveChainstate()};
    const CBlockIndex* tip{chainstate.m_chain.Tip()};
    CBlock block;
    BOOST_REQUIRE(m_node.chainman->m_blockman.ReadBlock(block, *tip));

    const auto disconnect = [&] {
        CCoinsViewCache view{&chainstate.CoinsTip()};
        CAssetsCache cache;
        return chainstate.DisconnectBlock(block, tip, view, &cache);
    };
    BOOST_CHECK(disconnect() == DISCONNECT_OK);

    // One entry is announced and its name is cut short
    const auto key{std::make_pair(uint8_t{'U'}, tip->GetBlockHash())};
    BOOST_REQUIRE(passetsdb->Write(key, std::vector<unsigned char>{0x05}));
    BOOST_CHECK(disconnect() == DISCONNECT_FAILED);

    BOOST_REQUIRE(passetsdb->Erase(key));
    BOOST_CHECK(disconnect() == DISCONNECT_OK);
}

BOOST_AUTO_TEST_CASE(the_asset_index_setting_is_recorded)
{
    bool kept{true};
    BOOST_CHECK(!passetsdb->ReadAssetIndexKept(kept));
    BOOST_CHECK(passetsdb->WriteAssetIndexKept(false));
    BOOST_CHECK(passetsdb->ReadAssetIndexKept(kept));
    BOOST_CHECK(!kept);
    BOOST_CHECK(passetsdb->WriteAssetIndexKept(true));
    BOOST_CHECK(passetsdb->ReadAssetIndexKept(kept));
    BOOST_CHECK(kept);
}

// The balances a node did not keep while -assetindex was off are read from the
// UTXO set, and what the database held from before is dropped.
BOOST_AUTO_TEST_CASE(balances_are_rebuilt_from_the_utxo_set)
{
    LOCK(::cs_main);
    Chainstate& chainstate{m_node.chainman->ActiveChainstate()};
    const std::string first{EncodeDestination(Holder(0x11))};
    const std::string second{EncodeDestination(Holder(0x22))};
    const std::string emptied{EncodeDestination(Holder(0x33))};

    CCoinsViewCache& coins{chainstate.CoinsTip()};
    coins.AddCoin(COutPoint{Txid::FromUint256(uint256{1}), 0}, AssetCoin("AVIANTEST", 7 * COIN, 0x11), false);
    coins.AddCoin(COutPoint{Txid::FromUint256(uint256{1}), 1}, AssetCoin("AVIANTEST", 5 * COIN, 0x11), false);
    coins.AddCoin(COutPoint{Txid::FromUint256(uint256{2}), 0}, AssetCoin("AVIANTEST", 3 * COIN, 0x22), false);
    coins.AddCoin(COutPoint{Txid::FromUint256(uint256{3}), 0}, AssetCoin("OTHER", COIN, 0x22), false);

    // An address that has since sent everything away
    BOOST_REQUIRE(passetsdb->WriteAssetAddressQuantity("AVIANTEST", emptied, 9 * COIN));
    BOOST_REQUIRE(passetsdb->WriteAddressAssetQuantity(emptied, "AVIANTEST", 9 * COIN));
    BOOST_REQUIRE(passetsdb->WriteAssetIndexKept(false));

    BOOST_REQUIRE(RebuildAssetBalances(*m_node.chainman));

    const auto balance = [](const std::string& name, const std::string& address) {
        CAmount by_asset{-1}, by_address{-2};
        const bool a{passetsdb->ReadAssetAddressQuantity(name, address, by_asset)};
        const bool b{passetsdb->ReadAddressAssetQuantity(address, name, by_address)};
        BOOST_CHECK_EQUAL(a, b);
        if (a && b) BOOST_CHECK_EQUAL(by_asset, by_address);
        return a ? by_asset : CAmount{0};
    };
    BOOST_CHECK_EQUAL(balance("AVIANTEST", first), 12 * COIN);
    BOOST_CHECK_EQUAL(balance("AVIANTEST", second), 3 * COIN);
    BOOST_CHECK_EQUAL(balance("OTHER", second), COIN);
    BOOST_CHECK_EQUAL(balance("OTHER", first), 0);
    BOOST_CHECK_EQUAL(balance("AVIANTEST", emptied), 0);

    bool kept{false};
    BOOST_CHECK(passetsdb->ReadAssetIndexKept(kept));
    BOOST_CHECK(kept);
}

// The addresses of a tag are kept only with -assetindex, the tags of an address
// always. The first are written again from the second.
BOOST_AUTO_TEST_CASE(tag_addresses_are_rebuilt_from_address_tags)
{
    CRestrictedDB db{m_args.GetDataDirNet(), 1 << 20, /*fMemory=*/true, /*fWipe=*/true};
    BOOST_REQUIRE(db.WriteAddressQualifier("first", "#KYC"));
    BOOST_REQUIRE(db.WriteAddressQualifier("second", "#KYC"));
    BOOST_REQUIRE(db.WriteAddressQualifier("first", "#AML"));
    // A tag removed while the entries were not kept
    BOOST_REQUIRE(db.WriteQualifierAddress("third", "#KYC"));
    BOOST_REQUIRE(db.WriteQualifierAddress("third", "#GONE"));

    BOOST_REQUIRE(db.RebuildQualifierAddresses());

    const auto addresses = [&](std::string tag) {
        std::vector<std::string> found;
        BOOST_CHECK(db.GetQualifierAddresses(tag, found));
        std::sort(found.begin(), found.end());
        return found;
    };
    BOOST_CHECK(addresses("#KYC") == (std::vector<std::string>{"first", "second"}));
    BOOST_CHECK(addresses("#AML") == std::vector<std::string>{"first"});
    BOOST_CHECK(addresses("#GONE").empty());
}

BOOST_AUTO_TEST_SUITE_END()
