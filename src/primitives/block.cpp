// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2016 The Bitcoin Core developers
// Copyright (c) 2017 The Raven Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "versionbits.h"

#include <cstdint>

#include "chainparams.h"

#include "primitives/block.h"

#include "algo/hash_algos.h"
#include "tinyformat.h"
#include "utilstrencodings.h"
#include "crypto/common.h"

#include "consensus/consensus.h"

#define TIME_MASK 0xffffff80

static const uint32_t MAINNET_X16RT_ACTIVATIONTIME = 2000000000;
static const uint32_t TESTNET_X16RT_ACTIVATIONTIME = 1634101200;
static const uint32_t REGTEST_X16RT_ACTIVATIONTIME = 1629951212;

BlockNetwork bNetwork = BlockNetwork();

BlockNetwork::BlockNetwork()
{
    fOnTestnet = false;
    fOnRegtest = false;
}

void BlockNetwork::SetNetwork(const std::string& net)
{
    if (net == "test") {
        fOnTestnet = true;
    } else if (net == "regtest") {
        fOnRegtest = true;
    }
}

uint256 CBlockHeader::GetHash() const
{
    uint256 thash;
    unsigned int profile = 0x0;
    uint32_t nTimeToUse = MAINNET_X16RT_ACTIVATIONTIME;

    if (bNetwork.fOnTestnet)
        nTimeToUse = TESTNET_X16RT_ACTIVATIONTIME;
    else if (bNetwork.fOnRegtest)
        nTimeToUse = REGTEST_X16RT_ACTIVATIONTIME;
    else
        nTimeToUse = MAINNET_X16RT_ACTIVATIONTIME;

    if (nTime > nTimeToUse) {
        // x16rt
        int32_t nTimeX16r = nTime&TIME_MASK;
        uint256 hashTime = Hash(BEGIN(nTimeX16r), END(nTimeX16r));
        thash = HashX16R(BEGIN(nVersion), END(nNonce), hashTime);
    }
    else {
        thash = HashX16R(BEGIN(nVersion), END(nNonce), hashPrevBlock);
    }

    return thash;
}

uint256 CBlockHeader::GetX16RHash() const
{
    return HashX16R(BEGIN(nVersion), END(nNonce), hashPrevBlock);
}

std::string CBlock::ToString() const
{
    std::stringstream s;
    s << strprintf("CBlock(hash=%s, ver=0x%08x, hashPrevBlock=%s, hashMerkleRoot=%s, nTime=%u, nBits=%08x, nNonce=%u, vtx=%u)\n",
        GetHash().ToString(),
        nVersion,
        hashPrevBlock.ToString(),
        hashMerkleRoot.ToString(),
        nTime, nBits, nNonce,
        vtx.size());
    for (const auto& tx : vtx) {
        s << "  " << tx->ToString() << "\n";
    }
    return s.str();
}

/// Used to test algo switching between X16R and X16RV2

//uint256 CBlockHeader::TestTiger() const
//{
//    return HashTestTiger(BEGIN(nVersion), END(nNonce), hashPrevBlock);
//}
//
//uint256 CBlockHeader::TestSha512() const
//{
//    return HashTestSha512(BEGIN(nVersion), END(nNonce), hashPrevBlock);
//}
//
//uint256 CBlockHeader::TestGost512() const
//{
//    return HashTestGost512(BEGIN(nVersion), END(nNonce), hashPrevBlock);
//}

//CBlock block = ConsensusParams().GenesisBlock();
//int64_t nStart = GetTimeMillis();
//LogPrintf("Starting Tiger %dms\n", nStart);
//block.TestTiger();
//LogPrintf("Tiger Finished %dms\n", GetTimeMillis() - nStart);
//
//nStart = GetTimeMillis();
//LogPrintf("Starting Sha512 %dms\n", nStart);
//block.TestSha512();
//LogPrintf("Sha512 Finished %dms\n", GetTimeMillis() - nStart);
//
//nStart = GetTimeMillis();
//LogPrintf("Starting Gost512 %dms\n", nStart);
//block.TestGost512();
//LogPrintf("Gost512 Finished %dms\n", GetTimeMillis() - nStart);
