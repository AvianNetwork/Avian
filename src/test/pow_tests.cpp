// Copyright (c) 2015 The Bitcoin Core developers
// Copyright (c) 2017 The Raven Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "chain.h"
#include "chainparams.h"
#include "pow.h"
#include "random.h"
#include "util.h"
#include "test/test_raven.h"

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(pow_tests, BasicTestingSetup)

    /* Test calculation of next difficulty target with no constraints applying */
    BOOST_AUTO_TEST_CASE(get_next_work_test)
    {
        BOOST_TEST_MESSAGE("Running Get Next Work Test");

        const auto chainParams = CreateChainParams(CBaseChainParams::MAIN);
        int64_t nLastRetargetTime = 1261130161; // Block #30240
        CBlockIndex pindexLast;
        pindexLast.nHeight = 32255;
        pindexLast.nTime = 1262152739;  // Block #32255
        pindexLast.nBits = 0x1e00ffff;
        BOOST_CHECK_EQUAL(CalculateNextWorkRequired(&pindexLast, nLastRetargetTime, chainParams->GetConsensus()), 0x1e03fffc);
    }

    /* Test the constraint on the upper bound for next work */
    BOOST_AUTO_TEST_CASE(get_next_work_pow_limit_test)
    {
        BOOST_TEST_MESSAGE("Running Get Next Work POW Limit Test");

        const auto chainParams = CreateChainParams(CBaseChainParams::MAIN);
        int64_t nLastRetargetTime = 1231006505; // Block #0
        CBlockIndex pindexLast;
        pindexLast.nHeight = 2015;
        pindexLast.nTime = 1233061996;  // Block #2015
        pindexLast.nBits = 0x1e00ffff;
        BOOST_CHECK_EQUAL(CalculateNextWorkRequired(&pindexLast, nLastRetargetTime, chainParams->GetConsensus()), 0x1e03fffc);
    }

    /* Test the constraint on the lower bound for actual time taken */
    BOOST_AUTO_TEST_CASE(get_next_work_lower_limit_actual_test)
    {
        BOOST_TEST_MESSAGE("Running Get Next Work Lower Limit Actual Test");

        const auto chainParams = CreateChainParams(CBaseChainParams::MAIN);
        int64_t nLastRetargetTime = 1279008237; // Block #66528
        CBlockIndex pindexLast;
        pindexLast.nHeight = 68543;
        pindexLast.nTime = 1279297671;  // Block #68543
        pindexLast.nBits = 0x1e00ffff;
        BOOST_CHECK_EQUAL(CalculateNextWorkRequired(&pindexLast, nLastRetargetTime, chainParams->GetConsensus()), 0x1e02648c);
    }

    /* Test the constraint on the upper bound for actual time taken */
    BOOST_AUTO_TEST_CASE(get_next_work_upper_limit_actual_test)
    {
        BOOST_TEST_MESSAGE("Running Get Next Work Upper Limit Actual  Test");

        const auto chainParams = CreateChainParams(CBaseChainParams::MAIN);
        int64_t nLastRetargetTime = 1263163443; // NOTE: Not an actual block time
        CBlockIndex pindexLast;
        pindexLast.nHeight = 46367;
        pindexLast.nTime = 1269211443;  // Block #46367
        pindexLast.nBits = 0x1e00ffff;
        BOOST_CHECK_EQUAL(CalculateNextWorkRequired(&pindexLast, nLastRetargetTime, chainParams->GetConsensus()), 0x1e03fffc);
    }

    BOOST_AUTO_TEST_CASE(get_block_proof_equivalent_time_test)
    {
        BOOST_TEST_MESSAGE("Running Get Block Proof Equivalent Time Test");

        const auto chainParams = CreateChainParams(CBaseChainParams::MAIN);
        std::vector<CBlockIndex> blocks(10000);
        for (int i = 0; i < 10000; i++)
        {
            blocks[i].pprev = i ? &blocks[i - 1] : nullptr;
            blocks[i].nHeight = i;
            blocks[i].nTime = 1269211443 + i * chainParams->GetConsensus().nPowTargetSpacing;
            blocks[i].nBits = 0x207fffff; /* target 0x7fffff000... */
            blocks[i].nChainWork = i ? blocks[i - 1].nChainWork + GetBlockProof(blocks[i - 1]) : arith_uint256(0);
        }

        for (int j = 0; j < 1000; j++)
        {
            CBlockIndex *p1 = &blocks[InsecureRandRange(10000)];
            CBlockIndex *p2 = &blocks[InsecureRandRange(10000)];
            CBlockIndex *p3 = &blocks[InsecureRandRange(10000)];

            int64_t tdiff = GetBlockProofEquivalentTime(*p1, *p2, *p3, chainParams->GetConsensus());
            BOOST_CHECK_EQUAL(tdiff, p1->GetBlockTime() - p2->GetBlockTime());
        }
    }

BOOST_AUTO_TEST_SUITE_END()
