// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2016 The Bitcoin Core developers
// Copyright (c) 2017 The Raven Core developers
// Copyright (c) 2022 The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "versionbits.h"

#include <cstdint>

#include "chainparams.h"

#include "algo/minotaurx/minotaurx.h"             // Minotaurx Algo
#include "algo/x16r/x16r.h"

#include "primitives/block.h"
#include "primitives/powcache.h"

#include "tinyformat.h"
#include "utilstrencodings.h"
#include "crypto/common.h"
#include "util.h"
#include "sync.h"

#include "consensus/consensus.h"

#define TIME_MASK 0xffffff80

static const uint32_t MAINNET_X16RT_ACTIVATIONTIME = 1638847406;
static const uint32_t TESTNET_X16RT_ACTIVATIONTIME = 1634101200;
static const uint32_t REGTEST_X16RT_ACTIVATIONTIME = 1629951212;

static const uint32_t MAINNET_CROW_MULTI_ACTIVATIONTIME = 1638847407;
static const uint32_t TESTNET_CROW_MULTI_ACTIVATIONTIME = 1639005225;
static const uint32_t REGTEST_CROW_MULTI_ACTIVATIONTIME = 1629951212;

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

uint256 CBlockHeader::GetSHA256Hash() const
{
    return SerializeHash(*this);
}

uint256 CBlockHeader::ComputePoWHash() const
{
    uint256 thash;
    unsigned int profile = 0x0;
    uint32_t nTimeToUse = MAINNET_X16RT_ACTIVATIONTIME;
    uint32_t nCrowTimeToUse = MAINNET_CROW_MULTI_ACTIVATIONTIME;

    if (bNetwork.fOnTestnet) {
        nTimeToUse = TESTNET_X16RT_ACTIVATIONTIME;
        nCrowTimeToUse = TESTNET_CROW_MULTI_ACTIVATIONTIME;
    } else if (bNetwork.fOnRegtest) {
        nTimeToUse = REGTEST_X16RT_ACTIVATIONTIME;
        nCrowTimeToUse = REGTEST_CROW_MULTI_ACTIVATIONTIME;
    } else {
        nTimeToUse = MAINNET_X16RT_ACTIVATIONTIME;
        nCrowTimeToUse = MAINNET_CROW_MULTI_ACTIVATIONTIME;
    }

    if (nTime > nTimeToUse) {
        if(nTime > nCrowTimeToUse) {
            // Mutli algo (x16rt + new Crow algo)
            switch (GetPoWType()) {
            case POW_TYPE_X16RT: {
                int32_t nTimeX16r = nTime & TIME_MASK;
                uint256 hashTime = Hash(BEGIN(nTimeX16r), END(nTimeX16r));
                thash = HashX16R(BEGIN(nVersion), END(nNonce), hashTime);
                break;
            }
            case POW_TYPE_MINOTAURX: {
                return Minotaurx(BEGIN(nVersion), END(nNonce), true);
                break;
            }
            default: // Don't crash the client on invalid blockType, just return a bad hash
                return HIGH_HASH;
            }
        } else {
            // x16rt before dual-algo
            int32_t nTimeX16r = nTime&TIME_MASK;
            uint256 hashTime = Hash(BEGIN(nTimeX16r), END(nTimeX16r));
            thash = HashX16R(BEGIN(nVersion), END(nNonce), hashTime);
        }
    }
    else {
        // x16r
        thash = HashX16R(BEGIN(nVersion), END(nNonce), hashPrevBlock);
    }

    return thash;
}

uint256 CBlockHeader::GetHash(bool readCache) const
{
    LOCK(cs_pow);
    CPowCache& cache(CPowCache::Instance());

    uint256 headerHash = GetSHA256Hash();
    uint256 powHash;
    bool found = false;

    if (readCache) {
        found = cache.get(headerHash, powHash);
    }

    if (!found || cache.IsValidate()) {
        uint256 powHash2 = ComputePoWHash();
        if (found && powHash2 != powHash) {
           LogPrintf("PowCache failure: headerHash: %s, from cache: %s, computed: %s, correcting\n", headerHash.ToString(), powHash.ToString(), powHash2.ToString());
        }
        powHash = powHash2;
        cache.erase(headerHash); // If it exists, replace it.
        cache.insert(headerHash, powHash2);
    }
    return powHash;
}

// Minotaurx algo
uint256 CBlockHeader::CrowHashArbitrary(const char* data) {
    return Minotaurx(data, data + strlen(data), true);
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
