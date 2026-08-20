// Copyright (c) 2026-present The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Regression tests for the asset transfer overflow exploit.
// The attack: an attacker holding X tokens can craft a transfer with
// nAmount exceeding MAX_MONEY, which without a bounds check passes
// the input==output equality check via integer overflow and creates
// tokens out of thin air. Forks inheriting this asset validation
// behavior may be affected.
//
// Fix: CheckTxAssets() now rejects any per-transfer nAmount outside MoneyRange
// and uses a pre-addition saturation check (nAmount > MAX_MONEY - current) on
// accumulated input/output totals to avoid signed integer overflow (UB in C++).
// Both checks are gated behind nAssetTransferOverflowFixHeight.
//
// ContextualCheckTransferAsset() unit tests (below) cover what that
// function still validates directly: nAmount <= 0 and name validity.
// The height-gated MAX_MONEY and overflow accumulation checks are
// exercised by functional tests that construct full transactions.

#include <assets/assets.h>
#include <assets/assettypes.h>
#include <addresstype.h>
#include <chainparams.h>
#include <coins.h>
#include <consensus/amount.h>
#include <consensus/tx_verify.h>
#include <consensus/validation.h>
#include <primitives/transaction.h>
#include <primitives/transaction_identifier.h>
#include <script/script.h>
#include <test/util/setup_common.h>
#include <uint256.h>

#include <limits>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(asset_transfer_overflow_tests, BasicTestingSetup)

static bool CheckTransfer(const std::string& name, CAmount amount, std::string& err)
{
    CAssetTransfer transfer;
    transfer.strName = name;
    transfer.nAmount = amount;
    transfer.nExpireTime = 0;
    return ContextualCheckTransferAsset(/*assetCache=*/nullptr, transfer, "dummyaddress", err);
}

BOOST_AUTO_TEST_CASE(transfer_amount_zero_rejected)
{
    std::string err;
    BOOST_CHECK(!CheckTransfer("TESTASSET", 0, err));
    BOOST_CHECK(!err.empty());
}

BOOST_AUTO_TEST_CASE(transfer_amount_negative_rejected)
{
    std::string err;
    BOOST_CHECK(!CheckTransfer("TESTASSET", -1, err));
    BOOST_CHECK(!CheckTransfer("TESTASSET", std::numeric_limits<CAmount>::min(), err));
}

BOOST_AUTO_TEST_CASE(transfer_positive_amount_passes_basic_check)
{
    // ContextualCheckTransferAsset only rejects <= 0; MAX_MONEY enforcement
    // is done in CheckTxAssets gated by nAssetTransferOverflowFixHeight.
    // A cache=nullptr will cause a lookup failure later, but the amount
    // check itself must pass for values in (0, MAX_MONEY].
    std::string err;
    CheckTransfer("TESTASSET", 1, err);
    BOOST_CHECK(err.find("equal to or less than zero") == std::string::npos);

    CheckTransfer("TESTASSET", MAX_MONEY, err);
    BOOST_CHECK(err.find("equal to or less than zero") == std::string::npos);
}


// ---------------------------------------------------------------------
// CheckTxAssets height-gated overflow tests
// ---------------------------------------------------------------------
//
// The three cases above exercise ContextualCheckTransferAsset. The gate
// itself — reject any amount outside MoneyRange, and guard the running
// total against exceeding MAX_MONEY, both only at/after
// nAssetTransferOverflowFixHeight — lives in CheckTxAssets and needs a
// full transaction plus a spent-coin view. BasicTestingSetup selects
// ChainType::MAIN, whose activation height is 5,270,000.

static CScript OverflowAssetScript(CAmount amount)
{
    CAssetTransfer a("OVERFLOWTEST", amount);
    // A plain P2PKH base; ConstructTransaction appends the asset portion.
    CScript s = CScript() << OP_DUP << OP_HASH160
                         << std::vector<unsigned char>(20, 0x11)
                         << OP_EQUALVERIFY << OP_CHECKSIG;
    a.ConstructTransaction(s);
    return s;
}

// 1-in/1-out transfer of `amount` units where the input coin holds the
// same amount, so the transaction is balanced (input == output).
static CTransaction BuildBalancedTransfer(CAmount amount, CCoinsViewCache& coins)
{
    CTxOut coinOut;
    coinOut.nValue = 0;
    coinOut.scriptPubKey = OverflowAssetScript(amount);

    uint256 hash = uint256{"bf50cb9a63be0019171456252989a459a7d0a5f494735278290079d22ab704a2"};
    COutPoint outpoint(Txid::FromUint256(hash), 1);
    coins.AddCoin(outpoint, Coin(coinOut, 10, false), true);

    CTxOut txOut;
    txOut.nValue = 0;
    txOut.scriptPubKey = OverflowAssetScript(amount);

    CMutableTransaction mutTx;
    CTxIn in;
    in.prevout = outpoint;
    mutTx.vin.emplace_back(in);
    mutTx.vout.emplace_back(txOut);
    return CTransaction(mutTx);
}

BOOST_AUTO_TEST_CASE(check_tx_assets_overflow_gate_flips_by_height)
{
    const int fix_height = Params().GetConsensus().nAssetTransferOverflowFixHeight;
    const CAmount evil = std::numeric_limits<int64_t>::max();

    // Below the activation height: legacy behaviour, the exploit tx passes.
    {
        CCoinsView view; CCoinsViewCache coins(&view);
        CTransaction tx = BuildBalancedTransfer(evil, coins);
        TxValidationState state;
        std::vector<std::pair<std::string, uint256>> vReissueAssets;
        BOOST_CHECK_MESSAGE(
            Consensus::CheckTxAssets(tx, state, coins, nullptr, nullptr, vReissueAssets,
                                     /*fRunningUnitTests=*/true, nullptr, 0, nullptr, fix_height - 1),
            "Pre-activation: overflow transfer should pass (legacy behaviour preserved)");
    }

    // At/after the activation height: the same tx must be rejected.
    {
        CCoinsView view; CCoinsViewCache coins(&view);
        CTransaction tx = BuildBalancedTransfer(evil, coins);
        TxValidationState state;
        std::vector<std::pair<std::string, uint256>> vReissueAssets;
        BOOST_CHECK_MESSAGE(
            !Consensus::CheckTxAssets(tx, state, coins, nullptr, nullptr, vReissueAssets,
                                      /*fRunningUnitTests=*/true, nullptr, 0, nullptr, fix_height),
            "Post-activation: overflow transfer must be rejected");
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-txns-asset-input-amount-out-of-range");
    }
}

BOOST_AUTO_TEST_CASE(check_tx_assets_output_accumulation_overflow)
{
    const int fix_height = Params().GetConsensus().nAssetTransferOverflowFixHeight;

    // One in-range input, two in-range outputs whose SUM exceeds MAX_MONEY.
    // Each amount passes MoneyRange individually, so only the running-total
    // guard on the output side can catch this.
    CCoinsView view; CCoinsViewCache coins(&view);

    CTxOut coinOut; coinOut.nValue = 0; coinOut.scriptPubKey = OverflowAssetScript(MAX_MONEY);
    uint256 hash = uint256{"bf50cb9a63be0019171456252989a459a7d0a5f494735278290079d22ab704a2"};
    COutPoint outpoint(Txid::FromUint256(hash), 1);
    coins.AddCoin(outpoint, Coin(coinOut, 10, false), true);

    CMutableTransaction mutTx;
    CTxIn in; in.prevout = outpoint;
    mutTx.vin.emplace_back(in);

    CTxOut o1; o1.nValue = 0; o1.scriptPubKey = OverflowAssetScript(MAX_MONEY);
    CTxOut o2; o2.nValue = 0; o2.scriptPubKey = OverflowAssetScript(1 * COIN);
    mutTx.vout.emplace_back(o1);
    mutTx.vout.emplace_back(o2);
    CTransaction tx(mutTx);

    TxValidationState state;
    std::vector<std::pair<std::string, uint256>> vReissueAssets;
    BOOST_CHECK_MESSAGE(
        !Consensus::CheckTxAssets(tx, state, coins, nullptr, nullptr, vReissueAssets,
                                  /*fRunningUnitTests=*/true, nullptr, 0, nullptr, fix_height),
        "Post-activation: output total exceeding MAX_MONEY must be rejected");
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-txns-asset-outputs-amount-overflow");
}

BOOST_AUTO_TEST_SUITE_END()
