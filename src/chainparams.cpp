// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2016 The Bitcoin Core developers
// Copyright (c) 2017 The Raven Core developers
// Copyright (c) 2022 The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "chainparams.h"
#include "consensus/merkle.h"

#include "tinyformat.h"
#include "util.h"
#include "utilstrencodings.h"
#include "arith_uint256.h"

#include <assert.h>
#include "chainparamsseeds.h"

//TODO: Take these out
extern double algoHashTotal[16];
extern int algoHashHits[16];


static CBlock CreateGenesisBlock(const char* pszTimestamp, const CScript& genesisOutputScript, uint32_t nTime, uint32_t nNonce, uint32_t nBits, int32_t nVersion, const CAmount& genesisReward)
{
    CMutableTransaction txNew;
    txNew.nVersion = 1;
    txNew.vin.resize(1);
    txNew.vout.resize(1);
    txNew.vin[0].scriptSig = CScript() << CScriptNum(0) << 486604799 << CScriptNum(4) << std::vector<unsigned char>((const unsigned char*)pszTimestamp, (const unsigned char*)pszTimestamp + strlen(pszTimestamp));
    txNew.vout[0].nValue = 2500 * COIN;
    txNew.vout[0].scriptPubKey = genesisOutputScript;

    CBlock genesis;
    genesis.nTime    = nTime;
    genesis.nBits    = nBits;
    genesis.nNonce   = nNonce;
    genesis.nVersion = nVersion;
    genesis.vtx.push_back(MakeTransactionRef(std::move(txNew)));
    genesis.hashPrevBlock.SetNull();
    genesis.hashMerkleRoot = BlockMerkleRoot(genesis);
    return genesis;
}

/**
 * Build the genesis block. Note that the output of its generation
 * transaction cannot be spent since it did not originally exist in the
 * database.
 *
 * CBlock(hash=00000e31101831561d16bd0b1c5abebcbed5e4a9064761c25e947c011c0e0818, ver=1, hashPrevBlock=00000000000000, hashMerkleRoot=229b338a4bee984d74432c131d548e0a66dbaada0b8434ee45af778380aa0d78, nTime=1629951211, nBits=504365040, nNonce=3176022, vtx=1)
 *   CTransaction(hash=4a5e1e, ver=1, vin.size=1, vout.size=1, nLockTime=0)
 *     CTxIn(COutPoint(000000, -1), coinbase 04ffff001d0104455468652054696d65732030332f4a616e2f32303039204368616e63656c6c6f72206f6e206272696e6b206f66207365636f6e64206261696c6f757420666f722062616e6b73)
 *     CTxOut(nValue=2500, scriptPubKey=04678afdb0fe5548271967f1a67130b7105cd6a828e03909a67962e0ea1f61deb649f6bc3f4cef38c4f35504e51ec112de5c384df7ba0b8d578a4c702b6bf11d5f)
 *   vMerkleTree: 4a5e1e
 */

static CBlock CreateGenesisBlock(uint32_t nTime, uint32_t nNonce, uint32_t nBits, int32_t nVersion, const CAmount& genesisReward)
{
    const char* pszTimestamp = "RavencoinLite is still here";
    const CScript genesisOutputScript = CScript() << ParseHex("01fds01189fe5548271967f1a67130b7105cd6a828e03909a67962e0ea1f61deb649f6bc3f4cef38c4f35504e51ec112de5c384df7ba0b8d578a4c702b6bf09087") << OP_CHECKSIG;
    return CreateGenesisBlock(pszTimestamp, genesisOutputScript, nTime, nNonce, nBits, nVersion, genesisReward);
}

void CChainParams::UpdateVersionBitsParameters(Consensus::DeploymentPos d, int64_t nStartTime, int64_t nTimeout)
{
    consensus.vDeployments[d].nStartTime = nStartTime;
    consensus.vDeployments[d].nTimeout = nTimeout;
}

void CChainParams::TurnOffSegwit() {
	consensus.nSegwitEnabled = false;
}

void CChainParams::TurnOffCSV() {
	consensus.nCSVEnabled = false;
}

void CChainParams::TurnOffBIP34() {
	consensus.nBIP34Enabled = false;
}

void CChainParams::TurnOffBIP65() {
	consensus.nBIP65Enabled = false;
}

void CChainParams::TurnOffBIP66() {
	consensus.nBIP66Enabled = false;
}

bool CChainParams::BIP34() {
	return consensus.nBIP34Enabled;
}

bool CChainParams::BIP65() {
	return consensus.nBIP34Enabled;
}

bool CChainParams::BIP66() {
	return consensus.nBIP34Enabled;
}

bool CChainParams::CSVEnabled() const{
	return consensus.nCSVEnabled;
}


/**
 * Main network
 */
/**
 * What makes a good checkpoint block?
 * + Is surrounded by blocks with reasonable timestamps
 *   (no blocks before with a timestamp after, none after with
 *    timestamp before)
 * + Contains no strange transactions
 */

class CMainParams : public CChainParams {
public:
    CMainParams() {
        strNetworkID = "main";
        consensus.nSubsidyHalvingInterval = 2100000;  //~ 4 yrs at 1 min block time
        consensus.nBIP34Enabled = true;
        consensus.nBIP65Enabled = true; // 000000000000000004c2b624ed5d7756c508d90fd0da2c7c679febfa6c4735f0
        consensus.nBIP66Enabled = true;
        consensus.nSegwitEnabled = true;
        consensus.nCSVEnabled = true;
        consensus.powLimit = uint256S("00000fffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
        consensus.nPowTargetTimespan = 2016 * 30; // 1.4 days
        consensus.nPowTargetSpacing = 1 * 30;
		consensus.fPowAllowMinDifficultyBlocks = false;
        consensus.fPowNoRetargeting = false;
        consensus.nRuleChangeActivationThreshold = 1814; // Approx 90% of 2016
        consensus.nMinerConfirmationWindow = 2016; // nPowTargetTimespan / nPowTargetSpacing
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].bit = 28;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nStartTime = 1199145601; // January 1, 2008
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nTimeout = 1230767999; // December 31, 2008

        // Crow Algo consensus
        consensus.powForkTime = 1638847407;                 // Time of PoW hash method change (Dec 06 2021)
        consensus.lwmaAveragingWindow = 45;                 // Averaging window size for LWMA diff adjust
        consensus.diffRetargetFix = 275109;                 // Block for diff algo fix
        consensus.diffRetargetTake2 = 1639269000;           // Third iteration of diff retargetter fix

	    // until "LWMA3" retarget algo, only "00000fff..." was used. these will apply once LWMA3 goes live
        consensus.powTypeLimits.emplace_back(uint256S("00000fffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"));   // x16rt limit
        consensus.powTypeLimits.emplace_back(uint256S("000fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"));   // Crow limit

        // x16rt switch
        consensus.nX16rtTimestamp = 1638847406;

        // Avian Assets, Messaging, Restricted
        consensus.nAssetActivationTime = 1666202400; // TODO
        consensus.nMessagingActivationTime = 1666202400; // TODO
        consensus.nRestrictedActivationTime = 1666202400; // TODO

        // Avian Flight Plans
        consensus.nFlightPlansActivationTime = 999999999999ULL; // TODO

        // Avian Name System (ANS)
        consensus.nAvianNameSystemTime = 999999999999ULL; // TODO

        // The best chain should have at least this much work.
        consensus.nMinimumChainWork = uint256S("0x00000000000000000000000000000000000000000000000029178e309cb56715"); // Block 1072359
        // By default assume that the signatures in ancestors of this block are valid.
        consensus.defaultAssumeValid = uint256S("0x00000000005ab90c287e481b1f2911228d26723ac07bcadd65031158ad733316"); // Block 1072359

        // The best chain should have at least this much work.

        //TODO: This needs to be changed when we re-start the chain
        //consensus.nMinimumChainWork = uint256S("0x000000000000000000000000000000000000000000000000000000000c000c00");

        //TODO - Set this to genesis block
        // By default assume that the signatures in ancestors of this block are valid.
        //consensus.defaultAssumeValid = uint256S("0x0000000000000000003b9ce759c2a087d52abc4266f8f4ebd6d768b89defa50a"); //477890

        /**
         * The message start string is designed to be unlikely to occur in normal data.
         * The characters are rarely used upper ASCII, not valid as UTF-8, and produce
         * a large 32-bit integer with any alignment.
         */
        pchMessageStart[0] = 0x52; // R
        pchMessageStart[1] = 0x56; // V
        pchMessageStart[2] = 0x4c; // L
        pchMessageStart[3] = 0x4d; // M
        nDefaultPort = 7895;
        nPruneAfterHeight = 100000;

        genesis = CreateGenesisBlock(1630067829, 8650489, 0x1e00ffff, 4, 10 * COIN);

        consensus.hashGenesisBlock = genesis.GetX16RHash();
        assert(consensus.hashGenesisBlock == uint256S("0x000000cdb10fc01df7fba251f2168ef7cd7854b571049db4902c315694461dd0"));
        assert(genesis.hashMerkleRoot == uint256S("0x63d9b6b6b549a2d96eb5ac4eb2ab80761e6d7bffa9ae1a647191e08d6416184d"));

        // Main seeders
        vSeeds.emplace_back("dnsseed.us.avn.network", true);
        vSeeds.emplace_back("dnsseed.ap.avn.network", true);
        vSeeds.emplace_back("dnsseed.eu.avn.network", true);

        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1,60);
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1,122);
        base58Prefixes[SECRET_KEY] =     std::vector<unsigned char>(1,128);
        base58Prefixes[EXT_PUBLIC_KEY] = {0x04, 0x88, 0xB2, 0x1E};
        base58Prefixes[EXT_SECRET_KEY] = {0x04, 0x88, 0xAD, 0xE4};

        // Avian BIP44 cointype in mainnet is '921'
        nExtCoinType = 921;

        vFixedSeeds = std::vector<SeedSpec6>(pnSeed6_main, pnSeed6_main + ARRAYLEN(pnSeed6_main));

        // Founder reward
        vector<FounderRewardStructure> rewardStructures = {
            {"rPC7kPCNPAVnUvQs4fWEvnFwJ4yfKvArXM",1121001,1641000, 5}, // 5% legacy founder/dev fee until block 1641000
            {"rKkJVJKgSPfS7oYmLZWoAHLyRzmFMuxiSU",1641000,INT_MAX, 5}  // 5% founder/dev fee forever
        };
        consensus.nFounderPayment = FounderPayment(rewardStructures); 

        fDefaultConsistencyChecks = false;
        fRequireStandard = true;
        fMineBlocksOnDemand = false;
        fMiningRequiresPeers = true;

        checkpointData = (CCheckpointData) {
            {
                { 0, uint256S("0x000000cdb10fc01df7fba251f2168ef7cd7854b571049db4902c315694461dd0")},
                { 275972, uint256S("0x0000004ac340f01da45c151990567a90a3c65010511ba7a05f3439a83c878efb")},
                { 508245, uint256S("0x00000000006cd2496fb78aedbd6524c8b1993589097fb848740e37eeab651682")},
                { 818787, uint256S("0x0000000247de51f4188fc43316cc5e1f8711cff6210b242d234004aae39163d5")},
                { 939610, uint256S("0x00000003cb151bde7f7c91b0dd145fbd8a0d6267873980662819fcddc3c74e24")},
                { 940202, uint256S("0x00000000ed69247f7ef177a14e44de41d9c1ba689cb930946ff773ebfe23f64c")},
                { 952399, uint256S("0x0000000000a11f354eacb65fee963df9818ee8884d8dd926da33921691ec9969")},
                { 1072359, uint256S("0x00000000005ab90c287e481b1f2911228d26723ac07bcadd65031158ad733316")}
            }
        };

        chainTxData = ChainTxData{
            // Update as we know more about the contents of the Avian chain
            // Stats as of 00000001ce6a1f7137c09b2279fdadc7c23249cef47f666e9df4f6c16187598d block 1072359
            1663533875, // * UNIX timestamp of last known number of transactions
            1451962,    // * total number of transactions between genesis and that timestamp
                        //   (the tx=... number in the SetBestChain debug.log lines)
            0.04        // * estimated number of transactions per second after that timestamp
        };

        /** AVN Start **/
        // Burn Amounts
        nIssueAssetBurnAmount = 500 * COIN;
        nReissueAssetBurnAmount = 100 * COIN;
        nIssueSubAssetBurnAmount = 100 * COIN;
        nIssueUniqueAssetBurnAmount = 5 * COIN;
        nIssueMsgChannelAssetBurnAmount = 100 * COIN;
        nIssueQualifierAssetBurnAmount = 1000 * COIN;
        nIssueSubQualifierAssetBurnAmount = 100 * COIN;
        nIssueRestrictedAssetBurnAmount = 1500 * COIN;
        nAddNullQualifierTagBurnAmount = .1 * COIN;

        // Burn Addresses
        strIssueAssetBurnAddress = "RXissueAssetXXXXXXXXXXXXXXXXXhhZGt";
        strReissueAssetBurnAddress = "RXReissueAssetXXXXXXXXXXXXXXVEFAWu";
        strIssueSubAssetBurnAddress = "RXissueSubAssetXXXXXXXXXXXXXWcwhwL";
        strIssueUniqueAssetBurnAddress = "RXissueUniqueAssetXXXXXXXXXXWEAe58";
        strIssueMsgChannelAssetBurnAddress = "RXissueMsgChanneLAssetXXXXXXSjHvAY";
        strIssueQualifierAssetBurnAddress = "RXissueQuaLifierXXXXXXXXXXXXUgEDbC";
        strIssueSubQualifierAssetBurnAddress = "RXissueSubQuaLifierXXXXXXXXXVTzvv5";
        strIssueRestrictedAssetBurnAddress = "RXissueRestrictedXXXXXXXXXXXXzJZ1q";
        strAddNullQualifierTagBurnAddress = "RXaddTagBurnXXXXXXXXXXXXXXXXZQm5ya";

        // Global Burn Address
        strGlobalBurnAddress = "RXBurnXXXXXXXXXXXXXXXXXXXXXXWUo9FV";

        // DGW Activation
        nDGWActivationBlock = 0;

        nMaxReorganizationDepth = 60; // 60 at 1 minute block timespan is +/- 60 minutes.
        nMinReorganizationPeers = 4;
        nMinReorganizationAge = 60 * 60 * 12; // 12 hours

        // TODO: Assets, Messaging, Restricted
        nAssetActivationHeight = 9999999999; // Asset activated block height
        /** AVN End **/
    }
};

/**
 * Testnet (v6)
 */
class CTestNetParams : public CChainParams {
public:
    CTestNetParams() {
        strNetworkID = "test";
        consensus.nSubsidyHalvingInterval = 2100000;  //~ 4 yrs at 1 min block time
        consensus.nBIP34Enabled = true;
        consensus.nBIP65Enabled = true; // 000000000000000004c2b624ed5d7756c508d90fd0da2c7c679febfa6c4735f0
        consensus.nBIP66Enabled = true;
        consensus.nSegwitEnabled = true;
        consensus.nCSVEnabled = true;

        consensus.powLimit = uint256S("00000fffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
        consensus.nPowTargetTimespan = 2016 * 30; // 1.4 days
        consensus.nPowTargetSpacing = 1 * 30;
        consensus.fPowAllowMinDifficultyBlocks = true;
        consensus.fPowNoRetargeting = false;
        consensus.nRuleChangeActivationThreshold = 1310; // Approx 65% for testchains
        consensus.nMinerConfirmationWindow = 2016; // nPowTargetTimespan / nPowTargetSpacing
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].bit = 28;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nStartTime = 0;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nTimeout = 999999999999ULL;

        // Crow Algo consensus
        consensus.powForkTime = 1639005225;                 // Time of PoW hash method change
        consensus.lwmaAveragingWindow = 45;                 // Averaging window size for LWMA diff adjust
        consensus.diffRetargetFix = 0;                      // Block of diff algo change
        consensus.diffRetargetTake2 = 1639269000;           // Third iteration of LWMA retarget activation timestamp

        consensus.powTypeLimits.emplace_back(uint256S("00000fffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"));   // x16rt limit
        consensus.powTypeLimits.emplace_back(uint256S("000fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"));   // Crow limit

        // testnet x16rt switch
        consensus.nX16rtTimestamp = 1634101200; // Oct 13, 2021 

        // Avian Assets, Messaging, Restricted
        consensus.nAssetActivationTime = 1645104453; // Feb 17, 2022
        consensus.nMessagingActivationTime = 1645104453; // Feb 17, 2022
        consensus.nRestrictedActivationTime = 1645104453; // Feb 17, 2022

        // Avian Flight Plans
        consensus.nFlightPlansActivationTime = 1645104453; // Feb 17, 2022

        // Avian Name System (ANS)
        consensus.nAvianNameSystemTime = 1645104453; // Feb 17, 2022

        // The best chain should have at least this much work.
        consensus.nMinimumChainWork = uint256S("0x0000000000000000000000000000000000000000000000000000000000000002");

        // By default assume that the signatures in ancestors of this block are valid.
        consensus.defaultAssumeValid = uint256S("0x00016603365e3252687eeb7a309d9d6b903b81239d9bce670286a7a9d26131b9");


        pchMessageStart[0] = 0x52; // R
        pchMessageStart[1] = 0x56; // V
        pchMessageStart[2] = 0x4c; // L
        pchMessageStart[3] = 0x54; // T
        nDefaultPort = 18770;
        nPruneAfterHeight = 1000;

        genesis = CreateGenesisBlock(1630065295, 24922064, 0x1e00ffff, 4, 10 * COIN);

        consensus.hashGenesisBlock = genesis.GetX16RHash();
        assert(consensus.hashGenesisBlock == uint256S("0x00000084af22998d2aed78cc29f1fa587f854150ccd2991dfc82241c8f049219"));
        assert(genesis.hashMerkleRoot == uint256S("0x63d9b6b6b549a2d96eb5ac4eb2ab80761e6d7bffa9ae1a647191e08d6416184d"));

        vFixedSeeds.clear();
        vSeeds.clear();

        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1,111);
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1,196);
        base58Prefixes[SECRET_KEY] =     std::vector<unsigned char>(1,239);
        base58Prefixes[EXT_PUBLIC_KEY] = {0x04, 0x35, 0x87, 0xCF};
        base58Prefixes[EXT_SECRET_KEY] = {0x04, 0x35, 0x83, 0x94};

        // Avian BIP44 cointype in testnet
        nExtCoinType = 1;

        vFixedSeeds = std::vector<SeedSpec6>(pnSeed6_test, pnSeed6_test + ARRAYLEN(pnSeed6_test));

        // Founder reward
        vector<FounderRewardStructure> rewardStructures = {
            {"n1BurnXXXXXXXXXXXXXXXXXXXXXXU1qejP",11,30, 5}, // 5% legacy founder/dev fee until block 50
            {"2MvpouPdDEujBZg5eZnLNv5bCn78EE2bi65",30,100, 5}, // 5% legacy founder/dev fee until block 50
            {"2MzJPqGahWsi42LAw2fxz5gjssMhMinTpWG",100,INT_MAX, 5} // 5% founder/dev fee forever
        };
        consensus.nFounderPayment = FounderPayment(rewardStructures); 

        fDefaultConsistencyChecks = false;
        fRequireStandard = false;
        fMineBlocksOnDemand = false;
        fMiningRequiresPeers = true;

        checkpointData = (CCheckpointData) {
            {
                    { 0, uint256S("0x63d9b6b6b549a2d96eb5ac4eb2ab80761e6d7bffa9ae1a647191e08d6416184d")}
            }
        };

        chainTxData = ChainTxData{
            0,
            0,
            0
        };

        /** AVN Start **/
        // Burn Amounts
        nIssueAssetBurnAmount = 500 * COIN;
        nReissueAssetBurnAmount = 100 * COIN;
        nIssueSubAssetBurnAmount = 100 * COIN;
        nIssueUniqueAssetBurnAmount = 5 * COIN;
        nIssueMsgChannelAssetBurnAmount = 100 * COIN;
        nIssueQualifierAssetBurnAmount = 1000 * COIN;
        nIssueSubQualifierAssetBurnAmount = 100 * COIN;
        nIssueRestrictedAssetBurnAmount = 1500 * COIN;
        nAddNullQualifierTagBurnAmount = .1 * COIN;

        // Burn Addresses
        strIssueAssetBurnAddress = "n1issueAssetXXXXXXXXXXXXXXXXWdnemQ";
        strReissueAssetBurnAddress = "n1ReissueAssetXXXXXXXXXXXXXXWG9NLd";
        strIssueSubAssetBurnAddress = "n1issueSubAssetXXXXXXXXXXXXXbNiH6v";
        strIssueUniqueAssetBurnAddress = "n1issueUniqueAssetXXXXXXXXXXS4695i";
        strIssueMsgChannelAssetBurnAddress = "n1issueMsgChanneLAssetXXXXXXT2PBdD";
        strIssueQualifierAssetBurnAddress = "n1issueQuaLifierXXXXXXXXXXXXUysLTj";
        strIssueSubQualifierAssetBurnAddress = "n1issueSubQuaLifierXXXXXXXXXYffPLh";
        strIssueRestrictedAssetBurnAddress = "n1issueRestrictedXXXXXXXXXXXXZVT9V";
        strAddNullQualifierTagBurnAddress = "n1addTagBurnXXXXXXXXXXXXXXXXX5oLMH";

        // Global Burn Address
        strGlobalBurnAddress = "n1BurnXXXXXXXXXXXXXXXXXXXXXXU1qejP";

        // DGW Activation
        nDGWActivationBlock = 0;

        nMaxReorganizationDepth = 60; // 60 at 1 minute block timespan is +/- 60 minutes.
        nMinReorganizationPeers = 4;
        nMinReorganizationAge = 60 * 60 * 12; // 12 hours

        nAssetActivationHeight = 1; // Asset activated block height
        /** AVN End **/
    }
};

/**
 * Regression test
 */
class CRegTestParams : public CChainParams {
public:
    CRegTestParams() {
        strNetworkID = "regtest";
        consensus.nBIP34Enabled = true;
        consensus.nBIP65Enabled = true; // 000000000000000004c2b624ed5d7756c508d90fd0da2c7c679febfa6c4735f0
        consensus.nBIP66Enabled = true;
        consensus.nSegwitEnabled = true;
        consensus.nCSVEnabled = true;
        consensus.nSubsidyHalvingInterval = 150;
        consensus.powLimit = uint256S("7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
        consensus.nPowTargetTimespan = 2016 * 30; // 1.4 days
        consensus.nPowTargetSpacing = 1 * 30;
        consensus.fPowAllowMinDifficultyBlocks = true;
        consensus.fPowNoRetargeting = true;
        consensus.nRuleChangeActivationThreshold = 108; // 75% for testchains
        consensus.nMinerConfirmationWindow = 144; // Faster than normal for regtest (144 instead of 2016)
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].bit = 28;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nStartTime = 0;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nTimeout = 999999999999ULL;

        // Crow Algo consensus
        consensus.powForkTime = 1629951212;        // Time of PoW hash method change
        consensus.lwmaAveragingWindow = 45;        // Averaging window size for LWMA diff adjust
        consensus.diffRetargetFix = 0;             // Block of diff algo change
        consensus.diffRetargetTake2 = 1629951212;  // Third iteration of LWMA retarget activation timestamp

        consensus.powTypeLimits.emplace_back(uint256S("7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"));   // x16rt limit
        consensus.powTypeLimits.emplace_back(uint256S("7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"));   // Crow limit

        // regtest x16rt switch (genesis +1)
        consensus.nX16rtTimestamp = 1629951212;

        // Avian Assets, Messaging, Restricted
        consensus.nAssetActivationTime = 1629951212; // (genesis +1)
        consensus.nMessagingActivationTime = 1629951212; // (genesis +1)
        consensus.nRestrictedActivationTime = 1629951212; // (genesis +1)

        // Avian Flight Plans
        consensus.nFlightPlansActivationTime = 1629951212; // (genesis +1)

        // Avian Name System (ANS)
        consensus.nAvianNameSystemTime = 1629951212; // (genesis +1)

        // The best chain should have at least this much work.
        consensus.nMinimumChainWork = uint256S("0x00");

        // By default assume that the signatures in ancestors of this block are valid.
        consensus.defaultAssumeValid = uint256S("0x00");

        pchMessageStart[0] = 0x52; // R
        pchMessageStart[1] = 0x56; // V
        pchMessageStart[2] = 0x4c; // L
        pchMessageStart[3] = 0x45; // E
        nDefaultPort = 18444;
        nPruneAfterHeight = 1000;

// This is used inorder to mine the genesis block. Once found, we can use the nonce and block hash found to create a valid genesis block
        /////////////////////////////////////////////////////////////////

/**
        arith_uint256 test;
        bool fNegative;
        bool fOverflow;
        test.SetCompact(0x207fffff, &fNegative, &fOverflow);
        std::cout << "Test threshold: " << test.GetHex() << "\n\n";

        int genesisNonce = 0;
        uint256 TempHashHolding = uint256S("0x0000000000000000000000000000000000000000000000000000000000000000");
        uint256 BestBlockHash = uint256S("0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
        for (int i=0;i<40000000;i++) {
            genesis = CreateGenesisBlock(1629951211, i, 0x207fffff, 2, 2500 * COIN);
            //genesis.hashPrevBlock = TempHashHolding;
            consensus.hashGenesisBlock = genesis.GetHash();

            arith_uint256 BestBlockHashArith = UintToArith256(BestBlockHash);
            if (UintToArith256(consensus.hashGenesisBlock) < BestBlockHashArith) {
                BestBlockHash = consensus.hashGenesisBlock;
                std::cout << BestBlockHash.GetHex() << " Nonce: " << i << "\n";
                std::cout << "   PrevBlockHash: " << genesis.hashPrevBlock.GetHex() << "\n";
            }

            TempHashHolding = consensus.hashGenesisBlock;

            if (BestBlockHashArith < test) {
                genesisNonce = i - 1;
                break;
            }
            //std::cout << consensus.hashGenesisBlock.GetHex() << "\n";
        }
        std::cout << "\n";
        std::cout << "\n";
        std::cout << "\n";

        std::cout << "hashGenesisBlock to 0x" << BestBlockHash.GetHex() << std::endl;
        std::cout << "Genesis Nonce to " << genesisNonce << std::endl;
        std::cout << "Genesis Merkle " << genesis.hashMerkleRoot.GetHex() << std::endl;

        std::cout << "\n";
        std::cout << "\n";
        int totalHits = 0;
        double totalTime = 0.0;

        for(int x = 0; x < 16; x++) {
            totalHits += algoHashHits[x];
            totalTime += algoHashTotal[x];
            std::cout << "hash algo " << x << " hits " << algoHashHits[x] << " total " << algoHashTotal[x] << " avg " << algoHashTotal[x]/algoHashHits[x] << std::endl;
        }

        std::cout << "Totals: hash algo " <<  " hits " << totalHits << " total " << totalTime << " avg " << totalTime/totalHits << std::endl;

        genesis.hashPrevBlock = TempHashHolding;

        return;
*/
//        /////////////////////////////////////////////////////////////////


        genesis = CreateGenesisBlock(1629951211, 1, 0x207fffff, 2, 2500 * COIN);

        consensus.hashGenesisBlock = genesis.GetHash();
        assert(consensus.hashGenesisBlock == uint256S("0x653634d03d27ed84e8aba5dd47903906ad7be4876a1d3677be0db2891dcf787f"));
        assert(genesis.hashMerkleRoot == uint256S("63d9b6b6b549a2d96eb5ac4eb2ab80761e6d7bffa9ae1a647191e08d6416184d"));

        // Founder reward
        vector<FounderRewardStructure> rewardStructures = {
            {"2MzJPqGahWsi42LAw2fxz5gjssMhMinTpWG",1,INT_MAX, 5} // 5% founder/dev fee forever
        };
        consensus.nFounderPayment = FounderPayment(rewardStructures);

        vFixedSeeds.clear(); //!< Regtest mode doesn't have any fixed seeds.
        vSeeds.clear();      //!< Regtest mode doesn't have any DNS seeds.

        fDefaultConsistencyChecks = true;
        fRequireStandard = false;
        fMineBlocksOnDemand = true;
        fMiningRequiresPeers = false;

        checkpointData = (CCheckpointData) {
            {
            }
        };

        chainTxData = ChainTxData{
            0,
            0,
            0
        };

        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1,111);
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1,196);
        base58Prefixes[SECRET_KEY] =     std::vector<unsigned char>(1,239);
        base58Prefixes[EXT_PUBLIC_KEY] = {0x04, 0x35, 0x87, 0xCF};
        base58Prefixes[EXT_SECRET_KEY] = {0x04, 0x35, 0x83, 0x94};

        // Avian BIP44 cointype in testnet
        nExtCoinType = 1;

        /** AVN Start **/
        // Burn Amounts
        nIssueAssetBurnAmount = 500 * COIN;
        nReissueAssetBurnAmount = 100 * COIN;
        nIssueSubAssetBurnAmount = 100 * COIN;
        nIssueUniqueAssetBurnAmount = 5 * COIN;
        nIssueMsgChannelAssetBurnAmount = 100 * COIN;
        nIssueQualifierAssetBurnAmount = 1000 * COIN;
        nIssueSubQualifierAssetBurnAmount = 100 * COIN;
        nIssueRestrictedAssetBurnAmount = 1500 * COIN;
        nAddNullQualifierTagBurnAmount = .1 * COIN;

        // Burn Addresses
        strIssueAssetBurnAddress = "n1issueAssetXXXXXXXXXXXXXXXXWdnemQ";
        strReissueAssetBurnAddress = "n1ReissueAssetXXXXXXXXXXXXXXWG9NLd";
        strIssueSubAssetBurnAddress = "n1issueSubAssetXXXXXXXXXXXXXbNiH6v";
        strIssueUniqueAssetBurnAddress = "n1issueUniqueAssetXXXXXXXXXXS4695i";
        strIssueMsgChannelAssetBurnAddress = "n1issueMsgChanneLAssetXXXXXXT2PBdD";
        strIssueQualifierAssetBurnAddress = "n1issueQuaLifierXXXXXXXXXXXXUysLTj";
        strIssueSubQualifierAssetBurnAddress = "n1issueSubQuaLifierXXXXXXXXXYffPLh";
        strIssueRestrictedAssetBurnAddress = "n1issueRestrictedXXXXXXXXXXXXZVT9V";
        strAddNullQualifierTagBurnAddress = "n1addTagBurnXXXXXXXXXXXXXXXXX5oLMH";

        // Global Burn Address
        strGlobalBurnAddress = "n1BurnXXXXXXXXXXXXXXXXXXXXXXU1qejP";

        // DGW Activation
        nDGWActivationBlock = 0;

        nMaxReorganizationDepth = 60; // 60 at 1 minute block timespan is +/- 60 minutes.
        nMinReorganizationPeers = 4;
        nMinReorganizationAge = 60 * 60 * 12; // 12 hours

        nAssetActivationHeight = 1; // Asset activated block height
        /** AVN End **/
    }
};

static std::unique_ptr<CChainParams> globalChainParams;

const CChainParams &Params() {
    assert(globalChainParams);
    return *globalChainParams;
}

std::unique_ptr<CChainParams> CreateChainParams(const std::string& chain)
{
    if (chain == CBaseChainParams::MAIN)
        return std::unique_ptr<CChainParams>(new CMainParams());
    else if (chain == CBaseChainParams::TESTNET)
        return std::unique_ptr<CChainParams>(new CTestNetParams());
    else if (chain == CBaseChainParams::REGTEST)
        return std::unique_ptr<CChainParams>(new CRegTestParams());
    throw std::runtime_error(strprintf("%s: Unknown chain %s.", __func__, chain));
}

void SelectParams(const std::string& network, bool fForceBlockNetwork)
{
    SelectBaseParams(network);
    if (fForceBlockNetwork) {
        bNetwork.SetNetwork(network);
    }
    globalChainParams = CreateChainParams(network);
}

void UpdateVersionBitsParameters(Consensus::DeploymentPos d, int64_t nStartTime, int64_t nTimeout)
{
    globalChainParams->UpdateVersionBitsParameters(d, nStartTime, nTimeout);
}

void TurnOffSegwit(){
	globalChainParams->TurnOffSegwit();
}

void TurnOffCSV() {
	globalChainParams->TurnOffCSV();
}

void TurnOffBIP34() {
	globalChainParams->TurnOffBIP34();
}

void TurnOffBIP65() {
	globalChainParams->TurnOffBIP65();
}

void TurnOffBIP66() {
	globalChainParams->TurnOffBIP66();
}
