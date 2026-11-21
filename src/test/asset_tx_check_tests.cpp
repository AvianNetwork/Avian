// Copyright (c) 2026-present The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// The asset rules that need no context, which 4.x enforced in CheckTransaction()
// and 5.0.x did not enforce at all. Each rejection below is a transaction that
// 4.x refuses and 5.0.4 accepted.

#include <addresstype.h>
#include <assets/assets.h>
#include <assets/assettypes.h>
#include <consensus/amount.h>
#include <consensus/tx_verify.h>
#include <consensus/validation.h>
#include <key_io.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <test/util/setup_common.h>
#include <uint256.h>

#include <boost/test/unit_test.hpp>

#include <string>
#include <vector>

namespace {
PKHash Addr(unsigned char fill)
{
    return PKHash{uint160{std::vector<unsigned char>(20, fill)}};
}

CTxOut Pay(const PKHash& to, CAmount value = COIN)
{
    return CTxOut{value, GetScriptForDestination(to)};
}

CTxOut Transfer(const std::string& name, CAmount amount, const PKHash& to = Addr(0x11), CAmount value = 0)
{
    CScript script{GetScriptForDestination(to)};
    CAssetTransfer{name, amount}.ConstructTransaction(script);
    return CTxOut{value, script};
}

/** Qualifier tag (#NAME) or address restriction ($NAME) applied to an address. */
CTxOut AddressData(const std::string& name, int8_t flag, const PKHash& address = Addr(0x22))
{
    CScript script{CScript{} << OP_AVN_ASSET << ToByteVector(address)};
    CNullAssetTxData{name, flag}.ConstructTransaction(script);
    return CTxOut{0, script};
}

CTxOut GlobalRestriction(const std::string& name, int8_t flag)
{
    CScript script;
    CNullAssetTxData{name, flag}.ConstructGlobalRestrictionTransaction(script);
    return CTxOut{0, script};
}

CTxOut Verifier(const std::string& verifier)
{
    CScript script;
    CNullAssetTxVerifierString{verifier}.ConstructTransaction(script);
    return CTxOut{0, script};
}

CTxOut TagFee(int count)
{
    const CTxDestination dest{DecodeDestination(GetBurnAddress(AssetType::NULL_ADD_QUALIFIER))};
    return CTxOut{GetBurnAmount(AssetType::NULL_ADD_QUALIFIER) * count, GetScriptForDestination(dest)};
}

CTxOut IssueAsset(const std::string& name, const PKHash& to, CAmount value = 0)
{
    CScript script{GetScriptForDestination(to)};
    CNewAsset{name, 21000 * COIN, /*units=*/0, /*nReissuable=*/1, /*nHasIPFS=*/0, /*strIPFSHash=*/""}.ConstructTransaction(script);
    return CTxOut{value, script};
}

CTxOut IssueOwner(const std::string& name, const PKHash& to)
{
    CScript script{GetScriptForDestination(to)};
    CNewAsset{name, 21000 * COIN}.ConstructOwnerTransaction(script);
    return CTxOut{0, script};
}

CTxOut Reissue(const std::string& name, const PKHash& to, CAmount value = 0)
{
    CScript script{GetScriptForDestination(to)};
    CReissueAsset{name, 100 * COIN, /*nUnits=*/-1, /*nReissuable=*/1, /*strIPFSHash=*/"", /*strANSID=*/""}.ConstructTransaction(script);
    return CTxOut{value, script};
}

CMutableTransaction Tx(std::vector<CTxOut> outputs, bool coinbase = false)
{
    CMutableTransaction mtx;
    mtx.vin.resize(1);
    if (coinbase) {
        mtx.vin[0].prevout.SetNull();
        mtx.vin[0].scriptSig = CScript{} << 1 << OP_0;
    } else {
        mtx.vin[0].prevout = COutPoint{Txid::FromUint256(uint256::ONE), 0};
    }
    mtx.vout = std::move(outputs);
    return mtx;
}

/** Empty when the transaction passes, otherwise the reject reason. */
std::string Check(const CMutableTransaction& mtx)
{
    TxValidationState state;
    if (Consensus::CheckTxAssetStructure(CTransaction{mtx}, state)) return "";
    BOOST_CHECK(state.GetResult() == TxValidationResult::TX_CONSENSUS);
    return state.GetRejectReason();
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(asset_tx_check_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(transfers)
{
    BOOST_CHECK_EQUAL(Check(Tx({Pay(Addr(0x01))})), "");
    BOOST_CHECK_EQUAL(Check(Tx({Pay(Addr(0x01)), Transfer("AVIANTEST", 5 * COIN)})), "");
    BOOST_CHECK_EQUAL(Check(Tx({Transfer("AVIANTEST!", 1 * COIN)})), "");
    BOOST_CHECK_EQUAL(Check(Tx({Transfer("AVIANTEST#CARD", 1 * COIN)})), "");
    BOOST_CHECK_EQUAL(Check(Tx({Transfer("#KYC", 1 * COIN)})), "");
    BOOST_CHECK_EQUAL(Check(Tx({Transfer("#KYC", 10 * COIN)})), "");

    // Asset outputs never carry AVN
    BOOST_CHECK_EQUAL(Check(Tx({Transfer("AVIANTEST", 5 * COIN, Addr(0x11), /*value=*/1)})),
                      "bad-txns-asset-transfer-amount-isn't-zero");

    BOOST_CHECK_EQUAL(Check(Tx({Transfer("AVIANTEST!", 2 * COIN)})), "bad-txns-transfer-owner-amount-was-not-1");
    BOOST_CHECK_EQUAL(Check(Tx({Transfer("AVIANTEST#CARD", 2 * COIN)})), "bad-txns-transfer-unique-amount-was-not-1");
    BOOST_CHECK_EQUAL(Check(Tx({Transfer("#KYC", 11 * COIN)})), "bad-txns-transfer-qualifier-amount-must be between 1 - 100");
    BOOST_CHECK_EQUAL(Check(Tx({Transfer("#KYC", 0)})), "bad-txns-transfer-qualifier-amount-must be between 1 - 100");
    BOOST_CHECK_EQUAL(Check(Tx({Transfer("not a name", 1 * COIN)})), "bad-txns-transfer-asset-name-invalid");
}

BOOST_AUTO_TEST_CASE(coinbase_carries_no_assets)
{
    BOOST_CHECK_EQUAL(Check(Tx({Pay(Addr(0x01))}, /*coinbase=*/true)), "");

    // A coinbase has no inputs, so an asset output in it would be assets from nothing.
    // CheckTxAssets() is not run for a coinbase, and ConnectBlock() applies its outputs.
    BOOST_CHECK_EQUAL(Check(Tx({Pay(Addr(0x01)), Transfer("AVIANTEST", 5 * COIN)}, /*coinbase=*/true)),
                      "bad-txns-coinbase-contains-asset-txes");
    BOOST_CHECK_EQUAL(Check(Tx({Pay(Addr(0x01)), Transfer("AVIANTEST!", 1 * COIN)}, /*coinbase=*/true)),
                      "bad-txns-coinbase-contains-asset-txes");
    BOOST_CHECK_EQUAL(Check(Tx({Pay(Addr(0x01)), Verifier("KYC")}, /*coinbase=*/true)),
                      "bad-txns-coinbase-contains-asset-txes");
    // The same output is fine in an ordinary transaction, where it is balanced against inputs.
    BOOST_CHECK_EQUAL(Check(Tx({Pay(Addr(0x01)), Transfer("AVIANTEST", 5 * COIN)})), "");
}

BOOST_AUTO_TEST_CASE(issuance)
{
    const PKHash issuer{Addr(0x31)};
    const PKHash other{Addr(0x32)};

    // fee, owner token, asset: the layout IsNewAsset() recognises
    BOOST_CHECK_EQUAL(Check(Tx({Pay(Addr(0x01)), IssueOwner("AVIANTEST", issuer), IssueAsset("AVIANTEST", issuer)})), "");

    BOOST_CHECK_EQUAL(Check(Tx({Pay(Addr(0x01)), IssueOwner("AVIANTEST", other), IssueAsset("AVIANTEST", issuer)})),
                      "bad-txns-owner-address-mismatch");
    BOOST_CHECK_EQUAL(Check(Tx({Pay(Addr(0x01)), IssueOwner("OTHERNAME", issuer), IssueAsset("AVIANTEST", issuer)})),
                      "bad-txns-owner-name-mismatch");
    BOOST_CHECK_EQUAL(Check(Tx({Pay(Addr(0x01)), IssueOwner("AVIANTEST", issuer), IssueAsset("AVIANTEST", issuer, /*value=*/1)})),
                      "bad-txns-asset-issued-amount-isn't-zero");

    // A new-asset output outside an issuance layout
    BOOST_CHECK_EQUAL(Check(Tx({Pay(Addr(0x01)), IssueAsset("AVIANTEST", issuer)})), "bad-txns-bad-asset-transaction");

    // A reissue output carries no AVN either
    BOOST_CHECK_EQUAL(Check(Tx({Pay(Addr(0x01)), Transfer("AVIANTEST!", 1 * COIN), Reissue("AVIANTEST", issuer, /*value=*/1)})),
                      "bad-txns-asset-reissued-amount-isn't-zero");

    // OP_AVN_ASSET outside an asset or null asset data script
    BOOST_CHECK_EQUAL(Check(Tx({CTxOut{0, CScript{} << OP_1 << OP_AVN_ASSET}})),
                      "bad-txns-op-avn-asset-not-in-right-script-location");
}

BOOST_AUTO_TEST_CASE(qualifier_tags)
{
    // Adding a tag: transfer the qualifier, name the address, pay the fee
    BOOST_CHECK_EQUAL(Check(Tx({Transfer("#KYC", 1 * COIN), AddressData("#KYC", 1), TagFee(1)})), "");
    // Removing a tag is free
    BOOST_CHECK_EQUAL(Check(Tx({Transfer("#KYC", 1 * COIN), AddressData("#KYC", 0)})), "");
    // Two addresses, two fees
    BOOST_CHECK_EQUAL(Check(Tx({Transfer("#KYC", 1 * COIN), AddressData("#KYC", 1, Addr(0x22)), AddressData("#KYC", 1, Addr(0x23)), TagFee(2)})), "");

    BOOST_CHECK_EQUAL(Check(Tx({Transfer("#KYC", 1 * COIN), AddressData("#KYC", 1)})),
                      "bad-txns-tx-doesn't-contain-required-burn-fee-for-adding-tags");
    BOOST_CHECK_EQUAL(Check(Tx({Transfer("#KYC", 1 * COIN), AddressData("#KYC", 1, Addr(0x22)), AddressData("#KYC", 1, Addr(0x23)), TagFee(1)})),
                      "bad-txns-tx-doesn't-contain-required-burn-fee-for-adding-tags");
    BOOST_CHECK_EQUAL(Check(Tx({AddressData("#KYC", 1), TagFee(1)})),
                      "bad-txns-tx-contains-qualifier-asset-null-tx-without-asset-transfer");
    // One change per asset and address. This needs the address a tag names, which
    // ExtractDestination() did not give in 5.0.x: every tag then counted as one address.
    BOOST_CHECK_EQUAL(Check(Tx({Transfer("#KYC", 1 * COIN), AddressData("#KYC", 1), AddressData("#KYC", 0), TagFee(1)})),
                      "bad-txns-null-data-only-one-change-per-asset-address");
    BOOST_CHECK_EQUAL(Check(Tx({Transfer("#KYC", 1 * COIN), AddressData("#KYC", 2)})),
                      "bad-txns-null-data-flag-must-be-0-or-1");
}

BOOST_AUTO_TEST_CASE(restrictions_and_verifiers)
{
    // Freezing an address or the whole asset needs the owner token (TOKEN!) in the same transaction
    BOOST_CHECK_EQUAL(Check(Tx({Transfer("TOKEN!", 1 * COIN), AddressData("$TOKEN", 1)})), "");
    BOOST_CHECK_EQUAL(Check(Tx({AddressData("$TOKEN", 1)})),
                      "bad-txns-tx-contains-restricted-asset-null-tx-without-asset-transfer");
    BOOST_CHECK_EQUAL(Check(Tx({Transfer("$TOKEN", 1 * COIN), AddressData("$TOKEN", 1)})),
                      "bad-txns-tx-contains-restricted-asset-null-tx-without-asset-transfer");

    BOOST_CHECK_EQUAL(Check(Tx({Transfer("TOKEN!", 1 * COIN), GlobalRestriction("$TOKEN", 1)})), "");
    BOOST_CHECK_EQUAL(Check(Tx({GlobalRestriction("$TOKEN", 1)})),
                      "bad-txns-tx-contains-global-asset-null-tx-without-asset-transfer");
    BOOST_CHECK_EQUAL(Check(Tx({Transfer("TOKEN!", 1 * COIN), GlobalRestriction("$TOKEN", 1), GlobalRestriction("$TOKEN", 0)})),
                      "bad-txns-null-data-only-one-global-change-per-asset-name");

    // A verifier string belongs to a restricted asset issuance or reissuance, one per transaction
    BOOST_CHECK_EQUAL(Check(Tx({Pay(Addr(0x01)), Verifier("KYC")})),
                      "bad-txns-tx-cointains-verifier-string-without-restricted-asset-issuance-or-reissuance");
    BOOST_CHECK_EQUAL(Check(Tx({Pay(Addr(0x01)), Verifier("KYC"), Verifier("KYC")})),
                      "bad-txns-null-data-only-one-verifier-per-tx");
    BOOST_CHECK_EQUAL(Check(Tx({Pay(Addr(0x01)), Verifier("KYC & AML")})),
                      "bad-txns-null-verifier-data-contained-whitespaces");
}

BOOST_AUTO_TEST_SUITE_END()
