// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-present The Bitcoin Core developers
// Copyright (c) 2017 The Raven Core developers
// Copyright (c) 2021 The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_POW_H
#define BITCOIN_POW_H

#include <consensus/params.h>
#include <primitives/block.h>

#include <cstdint>
#include <optional>

class CBlockIndex;
class uint256;
class arith_uint256;

/** Helper: check if dual-algorithm PoW is enabled at a given block index */
bool IsDualAlgoEnabled(const CBlockIndex* pindexPrev, const Consensus::Params& params);

/**
 * LWMA difficulty adjustment dispatcher (post dual-algo activation).
 * Routes to LWMA1, LWMA2, or LWMA3 based on chain height and timestamps.
 */
unsigned int GetNextWorkRequiredLWMA(const CBlockIndex* pindexLast, const CBlockHeader* pblock, const Consensus::Params& params, POW_TYPE powType);
unsigned int GetNextWorkRequiredLWMA1(const CBlockIndex* pindexLast, const CBlockHeader* pblock, const Consensus::Params& params, POW_TYPE powType);
unsigned int GetNextWorkRequiredLWMA2(const CBlockIndex* pindexLast, const CBlockHeader* pblock, const Consensus::Params& params, POW_TYPE powType);
unsigned int GetNextWorkRequiredLWMA3(const CBlockIndex* pindexLast, const CBlockHeader* pblock, const Consensus::Params& params, POW_TYPE powType);

/**
 * Convert nBits value to target.
 *
 * @param[in] nBits     compact representation of the target
 * @param[in] pow_limit PoW limit (consensus parameter)
 *
 * @return              the proof-of-work target or nullopt if the nBits value
 *                      is invalid (due to overflow or exceeding pow_limit)
 */
std::optional<arith_uint256> DeriveTarget(unsigned int nBits, const uint256 pow_limit);

/** Pre-dual-algo difficulty adjustment (DarkGravityWave) */
unsigned int GetNextWorkRequired(const CBlockIndex* pindexLast, const CBlockHeader *pblock, const Consensus::Params&);
unsigned int CalculateNextWorkRequired(const CBlockIndex* pindexLast, int64_t nFirstBlockTime, const Consensus::Params&);

/** Check whether a block hash satisfies the proof-of-work requirement specified by nBits */
bool CheckProofOfWork(uint256 hash, unsigned int nBits, const Consensus::Params&);
bool CheckProofOfWorkImpl(uint256 hash, unsigned int nBits, const Consensus::Params&);

/** Check whether a block header satisfies proof-of-work (dual-algo aware, uses per-algorithm pow limits) */
bool CheckProofOfWork(const CBlockHeader& block, const Consensus::Params& params);

/** Check whether a stored block hash satisfies proof-of-work (index-aware, uses per-algorithm limits based on nVersion/nTime) */
bool CheckProofOfWorkFromIndex(uint256 hash, unsigned int nBits, uint32_t nTime, int32_t nVersion, const Consensus::Params& params);

/**
 * Return false if the proof-of-work requirement specified by new_nbits at a
 * given height is not possible, given the proof-of-work on the prior block as
 * specified by old_nbits.
 *
 * Avian uses per-block difficulty adjustment (DGW/LWMA), so this always
 * returns true — actual difficulty validation is done in ContextualCheckBlockHeader.
 */
bool PermittedDifficultyTransition(const Consensus::Params& params, int64_t height, uint32_t old_nbits, uint32_t new_nbits);

#endif // BITCOIN_POW_H
