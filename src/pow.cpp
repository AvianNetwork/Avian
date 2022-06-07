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

//    LogPrintf("--- diff --- %d: %d\n", pindexLast->nHeight, bnNew.GetCompact());

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
// Modified LWMA-3 / LWMA-1
// Copyright (c) 2017-2021 The Bitcoin Gold developers, Zawy, iamstenman (Microbitcoin), The Litecoin Cash developers, The Avian developers
// MIT License
// Algorithm by Zawy, a modification of WT-144 by Tom Harding
// For updates see
// https://github.com/zawy12/difficulty-algorithms/issues/3#issuecomment-442129791

unsigned int GetNextWorkRequiredLWMA(const CBlockIndex* pindexLast, const CBlockHeader *pblock, const Consensus::ConsensusParams& params, const POW_TYPE powType)
{
    if (!pindexLast || pindexLast->nHeight < params.diffRetargetFix)
        return GetNextWorkRequiredLWMA1(pindexLast, pblock, params, powType);
    else if (pindexLast->nHeight >= params.diffRetargetFix && pindexLast->GetBlockTime() < params.diffRetargetTake2)
        return GetNextWorkRequiredLWMA2(pindexLast, pblock, params, powType);
    else
        return GetNextWorkRequiredLWMA3(pindexLast, pblock, params, powType);
}

unsigned int GetNextWorkRequiredLWMA1(const CBlockIndex* pindexLast, const CBlockHeader *pblock, const Consensus::ConsensusParams& params, const POW_TYPE powType) {
    const bool verbose = LogAcceptCategory(BCLog::CROW);
    const arith_uint256 powLimit = UintToArith256(params.powLimit);                 // Minimum diff, 00000ffff... for both algos
    const int64_t T = params.nPowTargetSpacing * 2;                                 // Target freq
    const int64_t N = 90;                                                           // Window size
    const int64_t k = N * (N + 1) * T / 2;                                          // Constant for proper averaging after weighting solvetimes
    const int64_t height = pindexLast->nHeight;                                     // Block height

    // TESTNET ONLY: Allow minimum difficulty blocks if we haven't seen a block for ostensibly 10 blocks worth of time.
    // ***** THIS IS NOT SAFE TO DO ON YOUR MAINNET! *****
    if (params.fPowAllowMinDifficultyBlocks && pblock->GetBlockTime() > pindexLast->GetBlockTime() + T * 10) {
        if (verbose) LogPrintf("* GetNextWorkRequiredLWMA1: Allowing %s pow limit (apparent testnet stall)\n", POW_TYPE_NAMES[powType]);
        return powLimit.GetCompact();
    }

    // Not enough blocks on chain? Return limit
    if (height < N) {
        if (verbose) LogPrintf("* GetNextWorkRequiredLWMA1: Allowing %s pow limit (short chain)\n", POW_TYPE_NAMES[powType]);
        return powLimit.GetCompact();
    }

    arith_uint256 avgTarget, nextTarget;
    int64_t thisTimestamp, previousTimestamp;
    int64_t sumWeightedSolvetimes = 0, j = 0, blocksFound = 0;

    // Find previousTimestamp (N blocks of this blocktype back) 
    const CBlockIndex* blockPreviousTimestamp = pindexLast;
    while (blocksFound < N) {
        // Reached forkpoint before finding N blocks of correct powtype? Return min
        if (blockPreviousTimestamp->GetBlockHeader().nTime < params.powForkTime) {
            if (verbose) LogPrintf("* GetNextWorkRequiredLWMA1: Allowing %s pow limit (previousTime calc reached forkpoint at height %i)\n", POW_TYPE_NAMES[powType], blockPreviousTimestamp->nHeight);
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
    if (verbose) LogPrintf("* GetNextWorkRequiredLWMA1: previousTime: First in period is %s at height %i\n", blockPreviousTimestamp->GetBlockHeader().GetHash().ToString().c_str(), blockPreviousTimestamp->nHeight);

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
        if (verbose) LogPrintf("* GetNextWorkRequiredLWMA1: Allowing %s pow limit (target too high)\n", POW_TYPE_NAMES[powType]);
        return powLimit.GetCompact();
    }

    return nextTarget.GetCompact();
}

unsigned int GetNextWorkRequiredLWMA2(const CBlockIndex* pindexLast, const CBlockHeader *pblock, const Consensus::ConsensusParams& params, const POW_TYPE powType)
{
    const bool verbose = LogAcceptCategory(BCLog::CROW);
    const int64_t N = params.lwmaAveragingWindow;                                   // Window size
    const int64_t k = 1277;                                                         // Constant for proper averaging after weighting solvetimes (k=(N+1)/2*TargetSolvetime*0.998)
    const arith_uint256 powLimit = UintToArith256(params.powLimit);                 // Minimum diff, 00000ffff... for both algos
    const int height = pindexLast->nHeight + 1;                                     // Block height
    assert(height > N);

    // TESTNET ONLY: Allow minimum difficulty blocks if we haven't seen a block for ostensibly 10 blocks worth of time.
    // ***** THIS IS NOT SAFE TO DO ON YOUR MAINNET! *****
    if (params.fPowAllowMinDifficultyBlocks &&
        pblock->GetBlockTime() > pindexLast->GetBlockTime() + params.nPowTargetSpacing * 2) {
        if (verbose) LogPrintf("* GetNextWorkRequiredLWMA2: Allowing %s pow limit (apparent testnet stall)\n", POW_TYPE_NAMES[powType]);
        return powLimit.GetCompact();
    }

    if (params.fPowNoRetargeting) {
        return pindexLast->nBits;
    }

    arith_uint256 sum_target;
    int t = 0, j = 0, blocksFound = 0;
    
    // Loop through N most recent blocks.
    for (int i = height - N; i < height; i++) {

        if (pindexLast->GetBlockHeader().GetPoWType() != powType) {
            if (verbose) LogPrintf("* GetNextWorkRequiredLWMA2: Height %i: Skipping %s (wrong blocktype)\n", pindexLast->nHeight, pindexLast->GetBlockHeader().GetHash().ToString().c_str());
            assert (pindexLast->pprev);
            pindexLast = pindexLast->pprev;
            continue;
        } else {
            blocksFound++;
        }

        const CBlockIndex* block = pindexLast->GetAncestor(i);
        const CBlockIndex* block_Prev = block->GetAncestor(i - 1);

        if(block == nullptr || block_Prev == nullptr) {
            assert (pindexLast->pprev);
            pindexLast = pindexLast->pprev;
            continue;   
        }

        int64_t solvetime = block->GetBlockTime() - block_Prev->GetBlockTime();

        j++;
        t += solvetime * j; // Weighted solvetime sum.

        // Target sum divided by a factor, (k N^2).
        // The factor is a part of the final equation. However we divide sum_target here to avoid
        // potential overflow.
        arith_uint256 target;
        target.SetCompact(block->nBits);
        sum_target += target / (k * N * N);
    }


    // Keep t reasonable in case strange solvetimes occurred.
    if (t < N * k / 3) {
        t = N * k / 3;
    }


    arith_uint256 next_target = t * sum_target;
    if (next_target > powLimit) {
        if (verbose) LogPrintf("* GetNextWorkRequiredLWMA2: Allowing %s pow limit (target too high)\n", POW_TYPE_NAMES[powType]);
        next_target = powLimit;
    }

    // If no blocks of correct POW_TYPE was found within the block window then return powLimit
    if(blocksFound == 0){
        if (verbose) LogPrintf("* GetNextWorkRequiredLWMA2: Allowing %s pow limit (blocksFound returned 0)\n", POW_TYPE_NAMES[powType]);
        return powLimit.GetCompact();
    }

    return next_target.GetCompact();
}

unsigned int GetNextWorkRequiredLWMA3(const CBlockIndex* pindexLast, const CBlockHeader *pblock, const Consensus::ConsensusParams& params, const POW_TYPE powType)
{
	// Originally from XVG repository - modified by AVN team

	// LWMA for BTC clones
	// Algorithm by zawy, LWMA idea by Tom Harding
	// Code by h4x3rotab of BTC Gold, modified/updated by zawy
	// https://github.com/zawy12/difficulty-algorithms/issues/3#issuecomment-388386175
	//  FTL must be changed to about N*T/20 = 360 for T=120 and N=60 coins.
	//  FTL is MAX_FUTURE_BLOCK_TIME in chain.h.
	//  FTL in Ignition, Numus, and others can be found in main.h as DRIFT.
	//  Some coins took out a variable, and need to change the 2*60*60 here:
	//  if (block.GetBlockTime() > nAdjustedTime + 2 * 60 * 60)

	const arith_uint256 powLimit = UintToArith256(params.powTypeLimits[powType]);   // Minimum diff, 00000ffff... for x16rt, 000fffff... for minotaurx
	const int64_t T = params.nPowTargetSpacing * 2;                                 // Target freq 30s x 2 algos
	const int64_t N = 60;                                                           // Window size - 60 as per zawy12 graphs
	const int64_t k = N * (N + 1) * T / 2;                                          // Constant for proper averaging after weighting solvetimes == 109800
	const int64_t height = pindexLast->nHeight + 1;                                 // Block height, last + 1, since we are finding next block diff

	arith_uint256 sum_target;
	int64_t t = 0, j = 0;
	int64_t solvetime = 0;

	std::vector<const CBlockIndex*> SameAlgoBlocks;
	for (int c = height-1; SameAlgoBlocks.size() < (N + 1); c--){
		const CBlockIndex* block = pindexLast->GetAncestor(c); // -1 after execution
		if (block->GetBlockHeader().GetPoWType() == powType){
			SameAlgoBlocks.push_back(block);
		}

		if (c < 100){ // If there are not enough blocks with this algo, return powLimit until dataset is big enough
			return powLimit.GetCompact();
		}
	}
	// Creates vector with {block1000, block997, block993}, so we start at the back

	// Loop through N most recent blocks. starting with the lowest blockheight
	for (int i = N; i > 0; i--) {
		const CBlockIndex* block = SameAlgoBlocks[i-1];
		const CBlockIndex* block_Prev = SameAlgoBlocks[i];

		solvetime = block->GetBlockTime() - block_Prev->GetBlockTime();
		// solvetime is always min 1 second, max 360s to avoid huge variances and negative timestamps
		solvetime = std::min(solvetime, 6*T);
		if (solvetime < 1)
			solvetime = 1;

		j++;
		t += solvetime * j;  // Weighted solvetime sum.

		// Target sum divided by a factor, (k N^2).
		// The factor is a part of the final equation. However we divide sum_target here to avoid
		// potential overflow.
		arith_uint256 target;
		target.SetCompact(block->nBits);
		sum_target += target / N / k;
	}

	arith_uint256 next_target = t * sum_target;

	if (next_target > powLimit) {
		next_target = powLimit;
	}

	return next_target.GetCompact();
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

bool CheckProofOfWorkCrow(uint256 hash, unsigned int nBits, const Consensus::ConsensusParams& params, const POW_TYPE powType)
{
	// crow active with LWMA3 as retargetter and different powtargets/algo
	bool fNegative;
	bool fOverflow;
	arith_uint256 bnTarget;

	bnTarget.SetCompact(nBits, &fNegative, &fOverflow);

	// Check range
	if (fNegative || bnTarget == 0 || fOverflow || bnTarget > UintToArith256(params.powTypeLimits[powType]))
		return false;

	// Check proof of work matches claimed amount
	if (UintToArith256(hash) > bnTarget)
		return false;

	return true;
}

bool CheckProofOfWork(uint256 hash, unsigned int nBits, const Consensus::ConsensusParams& params)
{
    bool fNegative;
    bool fOverflow;
    arith_uint256 bnTarget;

    bnTarget.SetCompact(nBits, &fNegative, &fOverflow);

    // Check range
    if (fNegative || bnTarget == 0 || fOverflow || bnTarget > UintToArith256(params.powLimit))
        return false;

    // Check proof of work matches claimed amount
    if (UintToArith256(hash) > bnTarget)
        return false;

    return true;
}

bool CheckProofOfWork(const CBlockHeader& blockheader, const Consensus::ConsensusParams& params)
{
	if (blockheader.GetBlockTime() > params.diffRetargetTake2)
		return CheckProofOfWorkCrow(blockheader.GetHash(), blockheader.nBits, params, blockheader.GetPoWType());
	else
		return CheckProofOfWork(blockheader.GetHash(), blockheader.nBits, params);
}
