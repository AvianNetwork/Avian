// Copyright (c) 2026-present The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Regression tests for CVE-style overflow in asset transfer amount validation.
// The exploit: an attacker holding X tokens could craft a transfer with
// nAmount = X + 2^64, which wraps to X via int64 overflow and passes the
// input==output equality check without a MAX_MONEY guard.

#include <assets/assets.h>
#include <assets/assettypes.h>
#include <consensus/amount.h>

#include <boost/test/unit_test.hpp>

BOOST_AUTO_TEST_SUITE(asset_transfer_overflow_tests)

// Helper: build a minimal valid-looking transfer and run ContextualCheckTransferAsset.
// The function validates name and amount before touching the cache, so nullptr is safe
// for tests that expect rejection on those grounds.
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

BOOST_AUTO_TEST_CASE(transfer_amount_above_max_money_rejected)
{
    std::string err;
    // MAX_MONEY + 1 must be rejected
    BOOST_CHECK(!CheckTransfer("TESTASSET", MAX_MONEY + 1, err));
    BOOST_CHECK(!err.empty());
}

BOOST_AUTO_TEST_CASE(transfer_amount_overflow_value_rejected)
{
    std::string err;
    // int64 max — the value an attacker would use to cause wrap-around
    BOOST_CHECK(!CheckTransfer("TESTASSET", std::numeric_limits<int64_t>::max(), err));
    BOOST_CHECK(!err.empty());
}

BOOST_AUTO_TEST_CASE(transfer_amount_max_money_accepted)
{
    // MAX_MONEY itself must pass the amount bounds (cache=nullptr will cause a
    // later failure on asset lookup, but the amount check must not block it).
    std::string err;
    // We can't complete the full check without a real cache, but we verify the
    // error is NOT the amount-range error when nAmount == MAX_MONEY.
    CheckTransfer("TESTASSET", MAX_MONEY, err);
    BOOST_CHECK(err.find("greater than max money") == std::string::npos);
    BOOST_CHECK(err.find("equal to or less than zero") == std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()
