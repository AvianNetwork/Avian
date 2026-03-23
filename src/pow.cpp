// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2022 The Bitcoin Core developers
// Copyright (c) 2017 The Raven Core developers
// Copyright (c) 2021 The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pow.h>

#include <arith_uint256.h>
#include <chain.h>
#include <logging.h>
#include <primitives/block.h>
#include <uint256.h>
#include <util/check.h>

#include <algorithm>
#include <vector>

// ========================================================================
// Dual-algo helper
// ========================================================================

bool IsDualAlgoEnabled(const CBlockIndex* pindexPrev, const Consensus::Params& params)
{
    if (pindexPrev != nullptr) {
        return (pindexPrev->nTime > params.powForkTime);
    }
    return false;
}

// ========================================================================
// DarkGravityWave v3 — pre-dual-algo difficulty adjustment
// Originally from Dash, adapted by Avian (always active from block 0)
// ========================================================================

static unsigned int DarkGravityWave(const CBlockIndex* pindexLast, const CBlockHeader* pblock, const Consensus::Params& params)
{
    assert(pindexLast != nullptr);

    unsigned int nProofOfWorkLimit = UintToArith256(params.powLimit).GetCompact();
    const arith_uint256 bnPowLimit = UintToArith256(params.powLimit);
    int64_t nPastBlocks = 180; // ~3hr at 60s blocks, ~1.5hr at 30s

    // Make sure we have at least (nPastBlocks + 1) blocks, otherwise just return powLimit
    if (!pindexLast || pindexLast->nHeight < nPastBlocks) {
        return bnPowLimit.GetCompact();
    }

    if (params.fPowAllowMinDifficultyBlocks && params.fPowNoRetargeting) {
        // Special difficulty rule:
        // If the new block's timestamp is more than 2 * target spacing
        // then allow mining of a min-difficulty block.
        if (pblock->GetBlockTime() > pindexLast->GetBlockTime() + params.nPowTargetSpacing * 2)
            return nProofOfWorkLimit;
        else {
            // Return the last non-special-min-difficulty-rules-block
            const CBlockIndex* pindex = pindexLast;
            while (pindex->pprev && pindex->nHeight % params.DifficultyAdjustmentInterval() != 0 &&
                   pindex->nBits == nProofOfWorkLimit)
                pindex = pindex->pprev;
            return pindex->nBits;
        }
    }

    const CBlockIndex* pindex = pindexLast;
    arith_uint256 bnPastTargetAvg;

    for (unsigned int nCountBlocks = 1; nCountBlocks <= (unsigned int)nPastBlocks; nCountBlocks++) {
        arith_uint256 bnTarget = arith_uint256().SetCompact(pindex->nBits);
        if (nCountBlocks == 1) {
            bnPastTargetAvg = bnTarget;
        } else {
            // NOTE: that's not an average really...
            bnPastTargetAvg = (bnPastTargetAvg * nCountBlocks + bnTarget) / (nCountBlocks + 1);
        }

        if (nCountBlocks != (unsigned int)nPastBlocks) {
            assert(pindex->pprev); // should never fail
            pindex = pindex->pprev;
        }
    }

    arith_uint256 bnNew(bnPastTargetAvg);

    int64_t nActualTimespan = pindexLast->GetBlockTime() - pindex->GetBlockTime();
    // NOTE: is this accurate? nActualTimespan counts it for (nPastBlocks - 1) blocks only...
    int64_t nTargetTimespan = nPastBlocks * params.nPowTargetSpacing;

    if (nActualTimespan < nTargetTimespan / 3)
        nActualTimespan = nTargetTimespan / 3;
    if (nActualTimespan > nTargetTimespan * 3)
        nActualTimespan = nTargetTimespan * 3;

    // Retarget
    bnNew *= nActualTimespan;
    bnNew /= nTargetTimespan;

    if (bnNew > bnPowLimit) {
        bnNew = bnPowLimit;
    }

    return bnNew.GetCompact();
}

// ========================================================================
// LWMA difficulty adjustment — post dual-algo activation
// Modified LWMA-3 / LWMA-1
// Copyright (c) 2017-2021 The Bitcoin Gold developers, Zawy, iamstenman
//   (Microbitcoin), The Litecoin Cash developers, The Avian developers
// MIT License
// Algorithm by Zawy, a modification of WT-144 by Tom Harding
// ========================================================================

unsigned int GetNextWorkRequiredLWMA(const CBlockIndex* pindexLast, const CBlockHeader* pblock, const Consensus::Params& params, const POW_TYPE powType)
{
    if (!pindexLast || pindexLast->nHeight < params.diffRetargetFix)
        return GetNextWorkRequiredLWMA1(pindexLast, pblock, params, powType);
    else if (pindexLast->nHeight >= params.diffRetargetFix && pindexLast->GetBlockTime() < params.diffRetargetTake2)
        return GetNextWorkRequiredLWMA2(pindexLast, pblock, params, powType);
    else
        return GetNextWorkRequiredLWMA3(pindexLast, pblock, params, powType);
}

unsigned int GetNextWorkRequiredLWMA1(const CBlockIndex* pindexLast, const CBlockHeader* pblock, const Consensus::Params& params, const POW_TYPE powType)
{
    const arith_uint256 powLimit = UintToArith256(params.powLimit); // Minimum diff
    const int64_t T = params.nPowTargetSpacing * 2;                 // Target freq (2x for dual algo)
    const int64_t N = 90;                                           // Window size
    const int64_t k = N * (N + 1) * T / 2;                          // Constant for proper averaging
    const int64_t height = pindexLast->nHeight;                     // Block height

    // TESTNET ONLY: Allow minimum difficulty blocks if we haven't seen a block for 10 blocks worth of time.
    if (params.fPowAllowMinDifficultyBlocks && pblock->GetBlockTime() > pindexLast->GetBlockTime() + T * 10) {
        LogDebug(BCLog::VALIDATION, "GetNextWorkRequiredLWMA1: Allowing %s pow limit (apparent testnet stall)\n", POW_TYPE_NAMES[powType]);
        return powLimit.GetCompact();
    }

    // Not enough blocks on chain? Return limit
    if (height < N) {
        LogDebug(BCLog::VALIDATION, "GetNextWorkRequiredLWMA1: Allowing %s pow limit (short chain)\n", POW_TYPE_NAMES[powType]);
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
            LogDebug(BCLog::VALIDATION, "GetNextWorkRequiredLWMA1: Allowing %s pow limit (previousTime calc reached forkpoint at height %i)\n",
                     POW_TYPE_NAMES[powType], blockPreviousTimestamp->nHeight);
            return powLimit.GetCompact();
        }

        // Wrong block type? Skip
        if (blockPreviousTimestamp->GetBlockHeader().GetPoWType() != powType) {
            assert(blockPreviousTimestamp->pprev);
            blockPreviousTimestamp = blockPreviousTimestamp->pprev;
            continue;
        }

        blocksFound++;
        if (blocksFound == N) // Don't step to next one if we're at the one we want
            break;

        assert(blockPreviousTimestamp->pprev);
        blockPreviousTimestamp = blockPreviousTimestamp->pprev;
    }
    previousTimestamp = blockPreviousTimestamp->GetBlockTime();
    LogDebug(BCLog::VALIDATION, "GetNextWorkRequiredLWMA1: previousTime: First in period is %s at height %i\n",
             blockPreviousTimestamp->GetBlockHeader().GetHash().ToString().c_str(), blockPreviousTimestamp->nHeight);

    // Find N most recent blocks of wanted type
    blocksFound = 0;
    const CBlockIndex* pindexWork = pindexLast; // Use a separate pointer to avoid modifying pindexLast
    while (blocksFound < N) {
        // Wrong block type? Skip
        if (pindexWork->GetBlockHeader().GetPoWType() != powType) {
            assert(pindexWork->pprev);
            pindexWork = pindexWork->pprev;
            continue;
        }

        const CBlockIndex* block = pindexWork;
        blocksFound++;

        // Prevent solvetimes from being negative in a safe way.
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
        assert(pindexWork->pprev);
        pindexWork = pindexWork->pprev;
    }
    nextTarget = avgTarget * sumWeightedSolvetimes;

    if (nextTarget > powLimit) {
        LogDebug(BCLog::VALIDATION, "GetNextWorkRequiredLWMA1: Allowing %s pow limit (target too high)\n", POW_TYPE_NAMES[powType]);
        return powLimit.GetCompact();
    }

    return nextTarget.GetCompact();
}

unsigned int GetNextWorkRequiredLWMA2(const CBlockIndex* pindexLast, const CBlockHeader* pblock, const Consensus::Params& params, const POW_TYPE powType)
{
    const int64_t N = params.lwmaAveragingWindow;                   // Window size (45)
    const int64_t k = 1277;                                         // Constant (k=(N+1)/2*TargetSolvetime*0.998)
    const arith_uint256 powLimit = UintToArith256(params.powLimit); // Minimum diff
    const int height = pindexLast->nHeight + 1;                     // Block height
    assert(height > N);

    // TESTNET ONLY: Allow minimum difficulty blocks
    if (params.fPowAllowMinDifficultyBlocks &&
        pblock->GetBlockTime() > pindexLast->GetBlockTime() + params.nPowTargetSpacing * 2) {
        LogDebug(BCLog::VALIDATION, "GetNextWorkRequiredLWMA2: Allowing %s pow limit (apparent testnet stall)\n", POW_TYPE_NAMES[powType]);
        return powLimit.GetCompact();
    }

    if (params.fPowNoRetargeting) {
        return pindexLast->nBits;
    }

    arith_uint256 sum_target;
    int t = 0, j = 0, blocksFound = 0;

    // Loop through N most recent blocks.
    // NOTE: This matches Avian's exact logic which uses both height-based iteration
    // and pindexLast walking. Consensus-critical — do not "fix".
    const CBlockIndex* pindexWork = pindexLast;
    for (int i = height - N; i < height; i++) {
        if (pindexWork->GetBlockHeader().GetPoWType() != powType) {
            LogDebug(BCLog::VALIDATION, "GetNextWorkRequiredLWMA2: Height %i: Skipping %s (wrong blocktype)\n",
                     pindexWork->nHeight, pindexWork->GetBlockHeader().GetHash().ToString().c_str());
            assert(pindexWork->pprev);
            pindexWork = pindexWork->pprev;
            continue;
        } else {
            blocksFound++;
        }

        const CBlockIndex* block = pindexWork->GetAncestor(i);
        const CBlockIndex* block_Prev = block->GetAncestor(i - 1);

        if (block == nullptr || block_Prev == nullptr) {
            assert(pindexWork->pprev);
            pindexWork = pindexWork->pprev;
            continue;
        }

        int64_t solvetime = block->GetBlockTime() - block_Prev->GetBlockTime();

        j++;
        t += solvetime * j; // Weighted solvetime sum.

        // Target sum divided by a factor, (k N^2).
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
        LogDebug(BCLog::VALIDATION, "GetNextWorkRequiredLWMA2: Allowing %s pow limit (target too high)\n", POW_TYPE_NAMES[powType]);
        next_target = powLimit;
    }

    // If no blocks of correct POW_TYPE was found within the block window then return powLimit
    if (blocksFound == 0) {
        LogDebug(BCLog::VALIDATION, "GetNextWorkRequiredLWMA2: Allowing %s pow limit (blocksFound returned 0)\n", POW_TYPE_NAMES[powType]);
        return powLimit.GetCompact();
    }

    return next_target.GetCompact();
}

unsigned int GetNextWorkRequiredLWMA3(const CBlockIndex* pindexLast, const CBlockHeader* pblock, const Consensus::Params& params, const POW_TYPE powType)
{
    // LWMA for BTC clones
    // Algorithm by zawy, LWMA idea by Tom Harding
    // Code by h4x3rotab of BTC Gold, modified/updated by zawy
    // https://github.com/zawy12/difficulty-algorithms/issues/3#issuecomment-388386175

    const arith_uint256 powLimit = UintToArith256(params.powTypeLimits[powType]); // Per-algorithm limit
    const int64_t T = params.nPowTargetSpacing * 2;                               // Target freq 30s x 2 algos
    const int64_t N = 60;                                                         // Window size
    const int64_t k = N * (N + 1) * T / 2;                                        // Constant == 109800
    const int64_t height = pindexLast->nHeight + 1;                               // Block height

    arith_uint256 sum_target;
    int64_t t = 0, j = 0;
    int64_t solvetime = 0;

    std::vector<const CBlockIndex*> SameAlgoBlocks;
    for (int c = height - 1; SameAlgoBlocks.size() < (unsigned int)(N + 1); c--) {
        const CBlockIndex* block = pindexLast->GetAncestor(c);
        if (block->GetBlockHeader().GetPoWType() == powType) {
            SameAlgoBlocks.push_back(block);
        }

        if (c < 100) { // If there are not enough blocks with this algo, return powLimit until dataset is big enough
            return powLimit.GetCompact();
        }
    }
    // Creates vector with {block1000, block997, block993}, so we start at the back

    // Loop through N most recent blocks. starting with the lowest blockheight
    for (int i = N; i > 0; i--) {
        const CBlockIndex* block = SameAlgoBlocks[i - 1];
        const CBlockIndex* block_Prev = SameAlgoBlocks[i];

        solvetime = block->GetBlockTime() - block_Prev->GetBlockTime();
        // solvetime is always min 1 second, max 360s to avoid huge variances and negative timestamps
        solvetime = std::min(solvetime, 6 * T);
        if (solvetime < 1)
            solvetime = 1;

        j++;
        t += solvetime * j; // Weighted solvetime sum.

        // Target sum divided by a factor, (k N^2).
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

// ========================================================================
// GetNextWorkRequired — main dispatcher
// Pre-dual-algo: DarkGravityWave (always active on Avian from block 0)
// Post-dual-algo: delegated to LWMA via ContextualCheckBlockHeader
// ========================================================================

unsigned int GetNextWorkRequired(const CBlockIndex* pindexLast, const CBlockHeader *pblock, const Consensus::Params& params)
{
    assert(pindexLast != nullptr);

    // Avian always uses DarkGravityWave for pre-dual-algo blocks (nDGWActivationBlock = 0)
    return DarkGravityWave(pindexLast, pblock, params);
}

unsigned int CalculateNextWorkRequired(const CBlockIndex* pindexLast, int64_t nFirstBlockTime, const Consensus::Params& params)
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

// ========================================================================
// PermittedDifficultyTransition
// Avian uses per-block difficulty adjustment (DGW/LWMA), so any transition
// is permitted. Actual difficulty validation is done in ContextualCheckBlockHeader.
// ========================================================================

bool PermittedDifficultyTransition(const Consensus::Params& params, int64_t /*height*/, uint32_t /*old_nbits*/, uint32_t /*new_nbits*/)
{
    // Avian adjusts difficulty every block (DGW before dual-algo, LWMA after).
    // The Bitcoin-style "factor of 4" check at 2016-block intervals does not apply.
    // Always return true — real validation happens in ContextualCheckBlockHeader.
    return true;
}

// ========================================================================
// CheckProofOfWork
// ========================================================================

// Bypasses the actual proof of work check during fuzz testing with a simplified validation checking whether
// the most significant bit of the last byte of the hash is set.
bool CheckProofOfWork(uint256 hash, unsigned int nBits, const Consensus::Params& params)
{
    if (EnableFuzzDeterminism()) return (hash.data()[31] & 0x80) == 0;
    return CheckProofOfWorkImpl(hash, nBits, params);
}

std::optional<arith_uint256> DeriveTarget(unsigned int nBits, const uint256 pow_limit)
{
    bool fNegative;
    bool fOverflow;
    arith_uint256 bnTarget;

    bnTarget.SetCompact(nBits, &fNegative, &fOverflow);

    // Check range
    if (fNegative || bnTarget == 0 || fOverflow || bnTarget > UintToArith256(pow_limit))
        return {};

    return bnTarget;
}

bool CheckProofOfWorkImpl(uint256 hash, unsigned int nBits, const Consensus::Params& params)
{
    auto bnTarget{DeriveTarget(nBits, params.powLimit)};
    if (!bnTarget) return false;

    // Check proof of work matches claimed amount
    if (UintToArith256(hash) > bnTarget)
        return false;

    return true;
}

// Dual-algo aware CheckProofOfWork that uses per-algorithm pow limits
static bool CheckProofOfWorkDualAlgo(uint256 hash, unsigned int nBits, const Consensus::Params& params, POW_TYPE powType)
{
    bool fNegative;
    bool fOverflow;
    arith_uint256 bnTarget;

    bnTarget.SetCompact(nBits, &fNegative, &fOverflow);

    // Check range against per-algorithm limit
    if (fNegative || bnTarget == 0 || fOverflow || bnTarget > UintToArith256(params.powTypeLimits[powType]))
        return false;

    // Check proof of work matches claimed amount
    if (UintToArith256(hash) > bnTarget)
        return false;

    return true;
}

// Block-header-aware CheckProofOfWork that dispatches based on time/algorithm
bool CheckProofOfWork(const CBlockHeader& block, const Consensus::Params& params)
{
    if (EnableFuzzDeterminism()) {
        uint256 hash = block.GetHash();
        return (hash.data()[31] & 0x80) == 0;
    }

    if (block.GetBlockTime() > (int64_t)params.powForkTime) {
        // Post dual-algo: use per-algorithm pow limits
        return CheckProofOfWorkDualAlgo(block.GetHash(), block.nBits, params, block.GetPoWType());
    } else {
        // Pre dual-algo: use global pow limit
        return CheckProofOfWorkImpl(block.GetHash(), block.nBits, params);
    }
}

// Index-aware CheckProofOfWork for block loading — determines the correct pow limit
// from the stored nTime and nVersion fields.
bool CheckProofOfWorkFromIndex(uint256 hash, unsigned int nBits, uint32_t nTime, int32_t nVersion, const Consensus::Params& params)
{
    if (EnableFuzzDeterminism()) return (hash.data()[31] & 0x80) == 0;

    if (nTime > params.powForkTime) {
        // Post dual-algo: use per-algorithm pow limits
        POW_TYPE powType = (POW_TYPE)((nVersion >> 16) & 0xFF);
        return CheckProofOfWorkDualAlgo(hash, nBits, params, powType);
    } else {
        // Pre dual-algo: use global pow limit
        return CheckProofOfWorkImpl(hash, nBits, params);
    }
}
