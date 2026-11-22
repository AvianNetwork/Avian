// Copyright (c) 2026 The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chain.h>
#include <consensus/params.h>
#include <consensus/validation.h>
#include <pow.h>
#include <primitives/block.h>
#include <sync.h>
#include <test/util/setup_common.h>
#include <util/check.h>
#include <util/time.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(maxreorg_tests, TestChain100Setup)

//! A header that forks from the active chain at fork_height, with valid proof of work.
static CBlockHeader ForkHeader(ChainstateManager& chainman, int fork_height, uint32_t salt)
{
    LOCK(cs_main);
    CBlockHeader header{Assert(chainman.ActiveChain()[fork_height + 1])->GetBlockHeader()};
    header.hashMerkleRoot = uint256{static_cast<uint8_t>(salt)};
    header.nNonce = 0;
    header.m_hasPoWHash = false;
    while (!CheckProofOfWork(header, chainman.GetConsensus())) {
        header.m_hasPoWHash = false;
        ++header.nNonce;
    }
    return header;
}

BOOST_AUTO_TEST_CASE(deep_fork_is_refused_without_blaming_the_peer)
{
    ChainstateManager& chainman{*Assert(m_node.chainman)};
    const Consensus::Params& consensus{chainman.GetConsensus()};
    const CBlockIndex* tip{WITH_LOCK(cs_main, return chainman.ActiveChain().Tip())};
    const int fork_height{tip->nHeight - consensus.nMaxReorganizationDepth};
    BOOST_REQUIRE(fork_height > 0);

    // The rule applies to a node that is caught up and has peers.
    SetMockTime(tip->GetBlockTime());
    int peers{consensus.nMinReorganizationPeers};
    chainman.get_peer_count = [&peers] { return peers; };

    {
        const CBlockHeader header{ForkHeader(chainman, fork_height, 1)};
        BlockValidationState state;
        BOOST_CHECK(!chainman.ProcessNewBlockHeaders({{header}}, /*min_pow_checked=*/true, state));
        BOOST_CHECK(state.IsInvalid());
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-fork-prior-to-maxreorgdepth");
        // The answer comes from this node's own tip and peers, so it is not the result
        // net_processing punishes a peer for.
        BOOST_CHECK(state.GetResult() == BlockValidationResult::BLOCK_MAX_REORG_DEPTH);
    }

    // One block shallower is an ordinary fork.
    {
        const CBlockHeader header{ForkHeader(chainman, fork_height + 1, 2)};
        BlockValidationState state;
        BOOST_CHECK(chainman.ProcessNewBlockHeaders({{header}}, /*min_pow_checked=*/true, state));
        BOOST_CHECK(state.IsValid());
    }

    // With too few peers the rule is off, as in 4.x.
    {
        peers = consensus.nMinReorganizationPeers - 1;
        const CBlockHeader header{ForkHeader(chainman, fork_height, 3)};
        BlockValidationState state;
        BOOST_CHECK(chainman.ProcessNewBlockHeaders({{header}}, /*min_pow_checked=*/true, state));
        BOOST_CHECK(state.IsValid());
    }

    chainman.get_peer_count = nullptr;
}

BOOST_AUTO_TEST_SUITE_END()
