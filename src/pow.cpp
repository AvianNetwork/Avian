// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2016 The Bitcoin Core developers
// Copyright (c) 2017 The Raven Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "pow.h"

#include "arith_uint256.h"
#include "chain.h"
#include "primitives/block.h"
#include "uint256.h"
#include "util.h"
#include "validation.h"
#include "chainparams.h"
#include "tinyformat.h"

unsigned int static DarkGravityWave(const CBlockIndex* pindexLast, const CBlockHeader *pblock, const Consensus::ConsensusParams& params) {
    /* current difficulty formula, dash - DarkGravity v3, written by Evan Duffield - evan@dash.org */
    assert(pindexLast != nullptr);

    unsigned int nProofOfWorkLimit = UintToArith256(params.powLimit).GetCompact();
    const arith_uint256 bnPowLimit = UintToArith256(params.powLimit);
    int64_t nPastBlocks = 180; // ~3hr

    // make sure we have at least (nPastBlocks + 1) blocks, otherwise just return powLimit
    if (!pindexLast || pindexLast->nHeight < nPastBlocks) {
        return bnPowLimit.GetCompact();
    }

    if (params.fPowAllowMinDifficultyBlocks && params.fPowNoRetargeting) {
        // Special difficulty rule:
        // If the new block's timestamp is more than 2 * 1 minutes
        // then allow mining of a min-difficulty block.
        if (pblock->GetBlockTime() > pindexLast->GetBlockTime() + params.nPowTargetSpacing * 2)
            return nProofOfWorkLimit;
        else {
            // Return the last non-special-min-difficulty-rules-block
            const CBlockIndex *pindex = pindexLast;
            while (pindex->pprev && pindex->nHeight % params.DifficultyAdjustmentInterval() != 0 &&
                   pindex->nBits == nProofOfWorkLimit)
                pindex = pindex->pprev;
            return pindex->nBits;
        }
    }

    const CBlockIndex *pindex = pindexLast;
    arith_uint256 bnPastTargetAvg;

    for (unsigned int nCountBlocks = 1; nCountBlocks <= nPastBlocks; nCountBlocks++) {
        arith_uint256 bnTarget = arith_uint256().SetCompact(pindex->nBits);
        if (nCountBlocks == 1) {
            bnPastTargetAvg = bnTarget;
        } else {
            // NOTE: that's not an average really...
            bnPastTargetAvg = (bnPastTargetAvg * nCountBlocks + bnTarget) / (nCountBlocks + 1);
        }

        if(nCountBlocks != nPastBlocks) {
            assert(pindex->pprev); // should never fail
            pindex = pindex->pprev;
        }
    }

    arith_uint256 bnNew(bnPastTargetAvg);

    int64_t nActualTimespan = pindexLast->GetBlockTime() - pindex->GetBlockTime();
    // NOTE: is this accurate? nActualTimespan counts it for (nPastBlocks - 1) blocks only...
    int64_t nTargetTimespan = nPastBlocks * params.nPowTargetSpacing;

    if (nActualTimespan < nTargetTimespan/3)
        nActualTimespan = nTargetTimespan/3;
    if (nActualTimespan > nTargetTimespan*3)
        nActualTimespan = nTargetTimespan*3;

    // Retarget
    bnNew *= nActualTimespan;
    bnNew /= nTargetTimespan;

    if (bnNew > bnPowLimit) {
        bnNew = bnPowLimit;
    }

    LogPrintf("--- diff --- %d: %d\n", pindexLast->nHeight, bnNew.GetCompact());

    return bnNew.GetCompact();
}

unsigned int GetNextWorkRequiredBTC(const CBlockIndex* pindexLast, const CBlockHeader *pblock, const Consensus::ConsensusParams& params)
{
    assert(pindexLast != nullptr);
    unsigned int nProofOfWorkLimit = UintToArith256(params.powLimit).GetCompact();

    // Only change once per difficulty adjustment interval
    if ((pindexLast->nHeight+1) % params.DifficultyAdjustmentInterval() != 0)
    {
        if (params.fPowAllowMinDifficultyBlocks)
        {
            // Special difficulty rule for testnet:
            // If the new block's timestamp is more than 2* 10 minutes
            // then allow mining of a min-difficulty block.
            if (pblock->GetBlockTime() > pindexLast->GetBlockTime() + params.nPowTargetSpacing*2)
                return nProofOfWorkLimit;
            else
            {
                // Return the last non-special-min-difficulty-rules-block
                const CBlockIndex* pindex = pindexLast;
                while (pindex->pprev && pindex->nHeight % params.DifficultyAdjustmentInterval() != 0 && pindex->nBits == nProofOfWorkLimit)
                    pindex = pindex->pprev;
                return pindex->nBits;
            }
        }
        return pindexLast->nBits;
    }

    // Go back by what we want to be 14 days worth of blocks
    int nHeightFirst = pindexLast->nHeight - (params.DifficultyAdjustmentInterval()-1);
    assert(nHeightFirst >= 0);
    const CBlockIndex* pindexFirst = pindexLast->GetAncestor(nHeightFirst);
    assert(pindexFirst);

    return CalculateNextWorkRequired(pindexLast, pindexFirst->GetBlockTime(), params);
}

bool IsTransitioningToX16rt(const CBlockIndex* pindexLast, const CBlockHeader *pblock, const Consensus::ConsensusParams& params)
{
    if (pblock->nTime <= params.nX16rtTimestamp)
        return false;
        
    int64_t dgwWindow = 0; // RVL does not have DGWPastBlocks so no need to check.

    const CBlockIndex* pindex = pindexLast;
    
    while (pindex->pprev && dgwWindow > 0) {
        pindex = pindex->pprev;
        dgwWindow--;
    }
    
    return pindex->nTime <= params.nX16rtTimestamp;
}

// Crow Algo: Diff adjustment for pow algos (post-Crow activation)
// Modified LWMA-3
// Copyright (c) 2017-2021 The Bitcoin Gold developers, Zawy, iamstenman (Microbitcoin), The Litecoin Cash developers, The Ravencoin Lite developers
// MIT License
// Algorithm by Zawy, a modification of WT-144 by Tom Harding
// For updates see
// https://github.com/zawy12/difficulty-algorithms/issues/3#issuecomment-442129791
unsigned int GetNextWorkRequiredLWMA(const CBlockIndex* pindexLast, const CBlockHeader *pblock, const Consensus::ConsensusParams& params, const POW_TYPE powType) {
    const bool verbose = LogAcceptCategory(BCLog::CROW);
    const arith_uint256 powLimit = UintToArith256(params.powTypeLimits[powType]);   // Max target limit (easiest diff)
    const int64_t T = params.nPowTargetSpacing * 2;                                 // Target freq
    const int64_t N = params.lwmaAveragingWindow;                                   // Window size
    const int64_t k = N * (N + 1) * T / 2;                                          // Constant for proper averaging after weighting solvetimes
    const int64_t height = pindexLast->nHeight;                                     // Block height

    // TESTNET ONLY: Allow minimum difficulty blocks if we haven't seen a block for ostensibly 10 blocks worth of time.
    // ***** THIS IS NOT SAFE TO DO ON YOUR MAINNET! *****
    if (params.fPowAllowMinDifficultyBlocks && pblock->GetBlockTime() > pindexLast->GetBlockTime() + T * 10) {
        if (verbose) LogPrintf("* GetNextWorkRequiredLWMA: Allowing %s pow limit (apparent testnet stall)\n", POW_TYPE_NAMES[powType]);
        return powLimit.GetCompact();
    }

    // Not enough blocks on chain? Return limit
    if (height < N) {
        if (verbose) LogPrintf("* GetNextWorkRequiredLWMA: Allowing %s pow limit (short chain)\n", POW_TYPE_NAMES[powType]);
        return powLimit.GetCompact();
    }

    arith_uint256 avgTarget, nextTarget;
    int64_t thisTimestamp, previousTimestamp;
    int64_t sumWeightedSolvetimes = 0, j = 0, blocksFound = 0;

    // Find previousTimestamp (N blocks of this blocktype back) 
    const CBlockIndex* blockPreviousTimestamp = pindexLast;
    while (blocksFound < N) {
        // Reached forkpoint before finding N blocks of correct powtype? Return min
        if (blockPreviousTimestamp->GetBlockHeader().nTime > params.powForkTime) {
            if (verbose) LogPrintf("* GetNextWorkRequiredLWMA: Allowing %s pow limit (previousTime calc reached forkpoint at height %i)\n", POW_TYPE_NAMES[powType], blockPreviousTimestamp->nHeight);
            return powLimit.GetCompact();
        }
        
        // Wrong block type? Skip
        if (blockPreviousTimestamp->GetBlockHeader().GetPoWType() != powType) {
            assert (blockPreviousTimestamp->pprev);
            blockPreviousTimestamp = blockPreviousTimestamp->pprev;
            continue;
        }

        blocksFound++;
        if (blocksFound == N)   // Don't step to next one if we're at the one we want
            break;

        assert (blockPreviousTimestamp->pprev);
        blockPreviousTimestamp = blockPreviousTimestamp->pprev;
    }
    previousTimestamp = blockPreviousTimestamp->GetBlockTime();
    if (verbose) LogPrintf("* GetNextWorkRequiredLWMA: previousTime: First in period is %s at height %i\n", blockPreviousTimestamp->GetBlockHeader().GetHash().ToString().c_str(), blockPreviousTimestamp->nHeight);

    // Find N most recent blocks of wanted type
    blocksFound = 0;
    while (blocksFound < N) {
        // Wrong block type? Skip
        if (pindexLast->GetBlockHeader().GetPoWType() != powType) {
            //if (verbose) LogPrintf("* GetNextWorkRequiredLWMA: Height %i: Skipping %s (wrong blocktype)\n", pindexLast->nHeight, pindexLast->GetBlockHeader().GetHash().ToString().c_str());
            assert (pindexLast->pprev);
            pindexLast = pindexLast->pprev;
            continue;
        }

        const CBlockIndex* block = pindexLast;
        blocksFound++;
        //if (verbose) LogPrintf("* GetNextWorkRequiredLWMA: Height %i: Counting %s. Total %s blocks found now: %i.\n", pindexLast->nHeight, pindexLast->GetBlockHeader().GetHash().ToString().c_str(), POW_TYPE_NAMES[powType], blocksFound);

        // Prevent solvetimes from being negative in a safe way. It must be done like this. 
        // Do not attempt anything like  if (solvetime < 1) {solvetime=1;}
        // The +1 ensures short chains do not calculate nextTarget = 0.
        thisTimestamp = (block->GetBlockTime() > previousTimestamp) ? block->GetBlockTime() : previousTimestamp + 1;

        // 6*T limit prevents large drops in diff from long solvetimes which would cause oscillations.
        int64_t solvetime = std::min(6 * T, thisTimestamp - previousTimestamp);

        // The following is part of "preventing negative solvetimes". 
        previousTimestamp = thisTimestamp;

        // Give linearly higher weight to more recent solvetimes.
        j++;
        sumWeightedSolvetimes += solvetime * j; 

        arith_uint256 target;
        target.SetCompact(block->nBits);
        avgTarget += target / N / k; // Dividing by k here prevents an overflow below.

        // Now step!
        assert (pindexLast->pprev);
        pindexLast = pindexLast->pprev;            
    }
    nextTarget = avgTarget * sumWeightedSolvetimes; 

    if (nextTarget > powLimit) {
        if (verbose) LogPrintf("* GetNextWorkRequiredLWMA: Allowing %s pow limit (target too high)\n", POW_TYPE_NAMES[powType]);
        return powLimit.GetCompact();
    }

    return nextTarget.GetCompact();
}

// Call correct diff adjust for blocks prior to Crow Algo
unsigned int GetNextWorkRequired(const CBlockIndex* pindexLast, const CBlockHeader *pblock, const Consensus::ConsensusParams& params)
{
    int dgw = DarkGravityWave(pindexLast, pblock, params);
    int btc = GetNextWorkRequiredBTC(pindexLast, pblock, params);
    int64_t nPrevBlockTime = (pindexLast->pprev ? pindexLast->pprev->GetBlockTime() : pindexLast->GetBlockTime());

    if (IsDGWActive(pindexLast->nHeight + 1)) {
        LogPrint(BCLog::NET, "Block %s - version: %s: found next work required using DGW: [%s] (BTC would have been [%s]\t(%+d)\t(%0.3f%%)\t(%s sec))\n",
                 pindexLast->nHeight + 1, pblock->nVersion, dgw, btc, btc - dgw, (float)(btc - dgw) * 100.0 / (float)dgw, pindexLast->GetBlockTime() - nPrevBlockTime);
        return dgw;
    }
    else {
        LogPrint(BCLog::NET, "Block %s - version: %s: found next work required using BTC: [%s] (DGW would have been [%s]\t(%+d)\t(%0.3f%%)\t(%s sec))\n",
                  pindexLast->nHeight + 1, pblock->nVersion, btc, dgw, dgw - btc, (float)(dgw - btc) * 100.0 / (float)btc, pindexLast->GetBlockTime() - nPrevBlockTime);
        return btc;
    }
}

unsigned int CalculateNextWorkRequired(const CBlockIndex* pindexLast, int64_t nFirstBlockTime, const Consensus::ConsensusParams& params)
{
    if (params.fPowNoRetargeting)
        return pindexLast->nBits;

    // Limit adjustment step
    int64_t nActualTimespan = pindexLast->GetBlockTime() - nFirstBlockTime;
    if (nActualTimespan < params.nPowTargetTimespan/4)
        nActualTimespan = params.nPowTargetTimespan/4;
    if (nActualTimespan > params.nPowTargetTimespan*4)
        nActualTimespan = params.nPowTargetTimespan*4;

    // Retarget
    const arith_uint256 bnPowLimit = UintToArith256(params.powLimit);
    arith_uint256 bnNew;
    bnNew.SetCompact(pindexLast->nBits);
    bnNew *= nActualTimespan;
    bnNew /= params.nPowTargetTimespan;

    if (bnNew > bnPowLimit)
        bnNew = bnPowLimit;

    return bnNew.GetCompact();
}

bool CheckProofOfWork(uint256 hash, unsigned int nBits, const Consensus::ConsensusParams& params)
{
    bool fNegative;
    bool fOverflow;
    arith_uint256 bnTarget;

    bnTarget.SetCompact(nBits, &fNegative, &fOverflow);

    // Crow: Use highest pow limit for limit check
    arith_uint256 powLimit = 0;
    for (int i = 0; i < NUM_BLOCK_TYPES; i++)
        if (UintToArith256(params.powTypeLimits[i]) > powLimit)
            powLimit = UintToArith256(params.powTypeLimits[i]);

    // Check range
    if (fNegative || bnTarget == 0 || fOverflow || bnTarget > powLimit)
        return false;

    // Check proof of work matches claimed amount
    if (UintToArith256(hash) > bnTarget)
        return false;

    return true;
}
