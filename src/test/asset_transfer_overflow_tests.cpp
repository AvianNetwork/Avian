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
#include <consensus/amount.h>

#include <boost/test/unit_test.hpp>

BOOST_AUTO_TEST_SUITE(asset_transfer_overflow_tests)

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

BOOST_AUTO_TEST_SUITE_END()
