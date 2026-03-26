// Copyright (c) 2024-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <assets/assets.h>
#include <consensus/amount.h>

#include <boost/test/unit_test.hpp>

BOOST_AUTO_TEST_SUITE(assets_amount_tests)

BOOST_AUTO_TEST_CASE(UnitValueFromAmountFormatting)
{
    // Regression: an asset issued with amount=10 and units=0 is stored as 10 * COIN,
    // but should be displayed as "10" (not 10 * COIN).
    BOOST_CHECK_EQUAL(UnitValueFromAmount(10 * COIN, /*units=*/0).getValStr(), "10");

    // Full precision (units=8) matches base-unit representation.
    BOOST_CHECK_EQUAL(UnitValueFromAmount(10 * COIN, /*units=*/8).getValStr(), "10.00000000");
    BOOST_CHECK_EQUAL(UnitValueFromAmount(1 * COIN + 12345678, /*units=*/8).getValStr(), "1.12345678");

    // Truncation to fewer decimals.
    BOOST_CHECK_EQUAL(UnitValueFromAmount(1 * COIN + 12345678, /*units=*/2).getValStr(), "1.12");

    // Integer display truncates remainder.
    BOOST_CHECK_EQUAL(UnitValueFromAmount(1 * COIN + 999, /*units=*/0).getValStr(), "1");

    // Negative values preserve sign.
    BOOST_CHECK_EQUAL(UnitValueFromAmount(-(1 * COIN + 50000000), /*units=*/2).getValStr(), "-1.50");

    // Units clamping.
    BOOST_CHECK_EQUAL(UnitValueFromAmount(1 * COIN + 1, /*units=*/100).getValStr(), "1.00000001");
    BOOST_CHECK_EQUAL(UnitValueFromAmount(1 * COIN + 1, /*units=*/-3).getValStr(), "1");
}

BOOST_AUTO_TEST_SUITE_END()
