// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2016 The Bitcoin Core developers
// Copyright (c) 2017 The Raven Core developers
// Copyright (c) 2022 The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef AVIAN_CONSENSUS_PARAMS_H
#define AVIAN_CONSENSUS_PARAMS_H

#include "uint256.h"
#include "founder_payment.h"

#include <cstdint>
#include <map>
#include <string>

namespace Consensus {

/**
 * BIP9 deployments
 */
enum DeploymentPos
{
    DEPLOYMENT_TESTDUMMY,
    // DEPLOYMENT_CSV, // Deployment of BIP68, BIP112, and BIP113.
    // DEPLOYMENT_SEGWIT, // Deployment of BIP141, BIP143, and BIP147.
    // NOTE: Also add new deployments to VersionBitsDeploymentInfo in versionbits.cpp,
    MAX_VERSION_BITS_DEPLOYMENTS
};

/**
 * Avian network upgrades using timestamps
 */
enum UpgradeIndex {
    UPGRADE_X16RT_SWITCH,
    UPGRADE_CROW_DUAL_ALGO,
    UPGRADE_AVIAN_ASSETS,
    UPGRADE_AVIAN_FLIGHT_PLANS,
    UPGRADE_AVIAN_NAME_SYSTEM,
    MAX_NETWORK_UPGRADES
};

/**
 * Struct for each individual consensus rule change using BIP9.
 */
struct BIP9Deployment {
    /** Bit position to select the particular bit in nVersion. */
    int bit;
    /** Start MedianTime for version bits miner confirmation. Can be a date in the past */
    int64_t nStartTime;
    /** Timeout/expiry MedianTime for the deployment attempt. */
    int64_t nTimeout;
};

/**
 * Struct for each network upgrade using timestamp.
 */
struct NetworkUpgrade {
    uint32_t nTimestamp;
};

/**
 * Parameters that influence chain consensus.
 */
struct ConsensusParams {
    uint256 hashGenesisBlock;
    int nSubsidyHalvingInterval;
    /** Block height and hash at which BIP34 becomes active */
    bool nBIP34Enabled;
    bool nBIP65Enabled;
    bool nBIP66Enabled;
    // uint256 BIP34Hash;
    /** Block height at which BIP65 becomes active */
    // int BIP65Height;
    /** Block height at which BIP66 becomes active */
    // int BIP66Height;
    /**
     * Minimum blocks including miner confirmation of the total of 2016 blocks in a retargeting period,
     * (nPowTargetTimespan / nPowTargetSpacing) which is also used for BIP9 deployments.
     * Examples: 1916 for 95%, 1512 for testchains.
     */
    uint32_t nRuleChangeActivationThreshold;
    uint32_t nMinerConfirmationWindow;

    /** BIP9 deployments */
    BIP9Deployment vDeployments[MAX_VERSION_BITS_DEPLOYMENTS];

    /** Avian network upgrades */
    NetworkUpgrade vUpgrades[MAX_NETWORK_UPGRADES];

    /** Proof of work parameters */
    uint256 powLimit;
    bool fPowAllowMinDifficultyBlocks;
    bool fPowNoRetargeting;
    int64_t nPowTargetSpacing;
    int64_t nPowTargetTimespan;
    int64_t DifficultyAdjustmentInterval() const { return nPowTargetTimespan / nPowTargetSpacing; }
    uint256 nMinimumChainWork;
    uint256 defaultAssumeValid;
    bool nSegwitEnabled;
    bool nCSVEnabled;
    uint32_t nX16rtTimestamp;

    // Crow consensus fields
    uint32_t powForkTime;
    int64_t diffRetargetFix;
    int64_t diffRetargetTake2;
    int64_t lwmaAveragingWindow;        // Averaging window size for LWMA diff adjust
    std::vector<uint256> powTypeLimits; // Limits for each pow type (with future-proofing space; can't pick up NUM_BLOCK_TYPES here)

    // AVN Founder Payment
    FounderPayment nFounderPayment;

};
} // namespace Consensus

#endif // AVIAN_CONSENSUS_PARAMS_H
