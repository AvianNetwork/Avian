// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <kernel/chainparams.h>

#include <chainparamsseeds.h>
#include <consensus/amount.h>
#include <consensus/merkle.h>
#include <consensus/params.h>
#include <hash.h>
#include <kernel/messagestartchars.h>
#include <logging.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/interpreter.h>
#include <script/script.h>
#include <uint256.h>
#include <util/chaintype.h>
#include <util/strencodings.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <type_traits>

using namespace util::hex_literals;

// Workaround MSVC bug triggering C7595 when calling consteval constructors in
// initializer lists.
// https://developercommunity.visualstudio.com/t/Bogus-C7595-error-on-valid-C20-code/10906093
#if defined(_MSC_VER)
auto consteval_ctor(auto&& input) { return input; }
#else
#define consteval_ctor(input) (input)
#endif

static CBlock CreateGenesisBlock(const char* pszTimestamp, const CScript& genesisOutputScript, uint32_t nTime, uint32_t nNonce, uint32_t nBits, int32_t nVersion, const CAmount& genesisReward)
{
    CMutableTransaction txNew;
    txNew.version = 1;
    txNew.vin.resize(1);
    txNew.vout.resize(1);
    // Avian genesis coinbase includes CScriptNum(0) prefix
    txNew.vin[0].scriptSig = CScript() << CScriptNum(0) << 486604799 << CScriptNum(4) << std::vector<unsigned char>((const unsigned char*)pszTimestamp, (const unsigned char*)pszTimestamp + strlen(pszTimestamp));
    txNew.vout[0].nValue = 2500 * COIN; // Avian genesis block hardcoded value
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
 * Avian genesis block:
 * CBlock(hash=000000cdb10fc0, ver=4, hashPrevBlock=00000000000000, hashMerkleRoot=63d9b6, nTime=1630067829, nBits=1e00ffff, nNonce=8650489, vtx=1)
 *   CTransaction(hash=..., ver=1, vin.size=1, vout.size=1, nLockTime=0)
 *     CTxIn(COutPoint(000000, -1), coinbase ...)
 *     CTxOut(nValue=2500.00000000, scriptPubKey=...)
 *   vMerkleTree: 63d9b6
 */
static CBlock CreateGenesisBlock(uint32_t nTime, uint32_t nNonce, uint32_t nBits, int32_t nVersion, const CAmount& genesisReward)
{
    const char* pszTimestamp = "RavencoinLite is still here";
    // Original hex "01fds01189fe5548..." contains invalid char 's' at position 4.
    // Avian's old ParseHex (BTC 0.16 era) stopped at the first invalid char,
    // producing only bytes [0x01, 0xFD]. Replicate that exact behavior here.
    const CScript genesisOutputScript = CScript() << std::vector<unsigned char>{0x01, 0xFD} << OP_CHECKSIG;
    return CreateGenesisBlock(pszTimestamp, genesisOutputScript, nTime, nNonce, nBits, nVersion, genesisReward);
}

/**
 * Main network on which people trade goods and services.
 */
class CMainParams : public CChainParams {
public:
    CMainParams() {
        m_chain_type = ChainType::MAIN;
        consensus.signet_blocks = false;
        consensus.signet_challenge.clear();
        consensus.nSubsidyHalvingInterval = 2100000; // ~2 yrs at 30s block time
        consensus.nBIP34Enabled = true;
        consensus.nBIP65Enabled = true;
        consensus.nBIP66Enabled = true;
        consensus.nSegwitEnabled = true;
        consensus.nCSVEnabled = true;
        consensus.BIP34Height = 1;
        consensus.BIP34Hash = uint256{};
        consensus.BIP65Height = 1;
        consensus.BIP66Height = 1;
        consensus.CSVHeight = 1;
        consensus.SegwitHeight = 1;
        consensus.MinBIP9WarningHeight = 0;
        consensus.powLimit = uint256{"00000fffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"};
        consensus.nPowTargetTimespan = 2016 * 30; // 1.4 days
        consensus.nPowTargetSpacing = 1 * 30; // 30 seconds
        consensus.fPowAllowMinDifficultyBlocks = false;
        consensus.enforce_BIP94 = false;
        consensus.fPowNoRetargeting = false;

        // BIP9 deployments
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].bit = 28;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nStartTime = 1199145601; // January 1, 2008
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nTimeout = 1230767999; // December 31, 2008
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].min_activation_height = 0;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].threshold = 1815; // 90%
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].period = 2016;

        // Taproot - not deployed on Avian mainnet
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].bit = 2;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].nStartTime = Consensus::BIP9Deployment::NEVER_ACTIVE;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].min_activation_height = 0;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].threshold = 1815; // 90%
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].period = 2016;

        // Avian network upgrades (hardforks)
        consensus.vUpgrades[Consensus::UPGRADE_X16RT_SWITCH].nTimestamp = 1638847406;
        consensus.vUpgrades[Consensus::UPGRADE_DUAL_ALGO].nTimestamp = 1638847407;
        consensus.vUpgrades[Consensus::UPGRADE_AVIAN_ASSETS].nTimestamp = 1666202400;
        // Flight Plans and ANS are deferred (use default max timestamp)

        // Dual algorithm consensus
        consensus.powForkTime = 1638847407;       // Time of PoW hash method change (Dec 06 2021)
        consensus.lwmaAveragingWindow = 45;       // Averaging window size for LWMA diff adjust
        consensus.diffRetargetFix = 275109;       // Block for diff algo fix
        consensus.diffRetargetTake2 = 1639269000; // Third iteration of diff retargetter fix

        // Per-algorithm difficulty limits (used after LWMA3 goes live)
        consensus.powTypeLimits.emplace_back(uint256{"00000fffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"}); // X16RT limit
        consensus.powTypeLimits.emplace_back(uint256{"000fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"}); // MinotaurX limit

        consensus.nMinimumChainWork = uint256{"00000000000000000000000000000000000000000000000029178e309cb56715"}; // Block 1072359
        consensus.defaultAssumeValid = uint256{"00000000005ab90c287e481b1f2911228d26723ac07bcadd65031158ad733316"}; // Block 1072359

        // Founder payment
        consensus.founderAddress = "rPC7kPCNPAVnUvQs4fWEvnFwJ4yfKvArXM";
        consensus.founderStartBlock = 1121000;
        consensus.founderRewardStructures = {{std::numeric_limits<int>::max(), 5}}; // 5% forever

        // Max reorg protection
        consensus.nMaxReorganizationDepth = 60;
        consensus.nMinReorganizationPeers = 4;
        consensus.nMinReorganizationAge = 60 * 60 * 12; // 12 hours
        
        pchMessageStart[0] = 0x52; // R
        pchMessageStart[1] = 0x56; // V
        pchMessageStart[2] = 0x4c; // L
        pchMessageStart[3] = 0x4d; // M
        nDefaultPort = 7895;
        nPruneAfterHeight = 100000;
        m_assumed_blockchain_size = 20;
        m_assumed_chain_state_size = 2;

        genesis = CreateGenesisBlock(1630067829, 8650489, 0x1e00ffff, 4, 10 * COIN);
        // GetHash() returns SHA256d during construction (PoW params not yet set).
        // Set hashGenesisBlock to the known PoW hash (X16R) directly.
        consensus.hashGenesisBlock = uint256{"000000cdb10fc01df7fba251f2168ef7cd7854b571049db4902c315694461dd0"};
        assert(genesis.hashMerkleRoot == uint256{"63d9b6b6b549a2d96eb5ac4eb2ab80761e6d7bffa9ae1a647191e08d6416184d"});

        // Main seeders
        vSeeds.emplace_back("dnsseed.us.avn.network.");
        vSeeds.emplace_back("dnsseed.ap.avn.network.");
        vSeeds.emplace_back("dnsseed.eu.avn.network.");

        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1, 60);
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1, 122);
        base58Prefixes[SECRET_KEY] =     std::vector<unsigned char>(1, 128);
        base58Prefixes[EXT_PUBLIC_KEY] = {0x04, 0x88, 0xB2, 0x1E};
        base58Prefixes[EXT_SECRET_KEY] = {0x04, 0x88, 0xAD, 0xE4};

        bech32_hrp = "avn";

        vFixedSeeds.clear();

        fDefaultConsistencyChecks = false;
        m_is_mockable_chain = false;

        // No assumeutxo data available for Avian yet
        m_assumeutxo_data = {};

        chainTxData = ChainTxData{
            .nTime    = 1663533875, // UNIX timestamp of last known number of transactions
            .tx_count = 1451962,    // total number of transactions between genesis and that timestamp
            .dTxRate  = 0.04,       // estimated number of transactions per second
        };
    }
};

/**
 * Testnet (v6): public test network for Avian.
 */
class CTestNetParams : public CChainParams {
public:
    CTestNetParams() {
        m_chain_type = ChainType::TESTNET;
        consensus.signet_blocks = false;
        consensus.signet_challenge.clear();
        consensus.nSubsidyHalvingInterval = 2100000; // ~4 yrs at 30s block time
        consensus.nBIP34Enabled = true;
        consensus.nBIP65Enabled = true;
        consensus.nBIP66Enabled = true;
        consensus.nSegwitEnabled = true;
        consensus.nCSVEnabled = true;
        consensus.BIP34Height = 1;
        consensus.BIP34Hash = uint256{};
        consensus.BIP65Height = 1;
        consensus.BIP66Height = 1;
        consensus.CSVHeight = 1;
        consensus.SegwitHeight = 1;
        consensus.MinBIP9WarningHeight = 0;
        consensus.powLimit = uint256{"00000fffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"};
        consensus.nPowTargetTimespan = 2016 * 30; // 1.4 days
        consensus.nPowTargetSpacing = 1 * 30; // 30 seconds
        consensus.fPowAllowMinDifficultyBlocks = true;
        consensus.enforce_BIP94 = false;
        consensus.fPowNoRetargeting = false;

        // BIP9 deployments
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].bit = 28;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nStartTime = 0;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].min_activation_height = 0;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].threshold = 1512; // 75%
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].period = 2016;

        // Taproot - not deployed on Avian testnet
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].bit = 2;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].nStartTime = Consensus::BIP9Deployment::NEVER_ACTIVE;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].min_activation_height = 0;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].threshold = 1512; // 75%
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].period = 2016;

        // Avian network upgrades (hardforks)
        consensus.vUpgrades[Consensus::UPGRADE_X16RT_SWITCH].nTimestamp = 1634101200; // Oct 13, 2021
        consensus.vUpgrades[Consensus::UPGRADE_DUAL_ALGO].nTimestamp = 1639005225;
        consensus.vUpgrades[Consensus::UPGRADE_AVIAN_ASSETS].nTimestamp = 1645104453; // Feb 17, 2022
        consensus.vUpgrades[Consensus::UPGRADE_AVIAN_FLIGHT_PLANS].nTimestamp = 1645104453; // Feb 17, 2022
        consensus.vUpgrades[Consensus::UPGRADE_AVIAN_NAME_SYSTEM].nTimestamp = 1645104453; // Feb 17, 2022

        // Dual algorithm consensus
        consensus.powForkTime = 1639005225;       // Time of PoW hash method change
        consensus.lwmaAveragingWindow = 45;       // Averaging window size for LWMA diff adjust
        consensus.diffRetargetFix = 0;            // Block of diff algo change
        consensus.diffRetargetTake2 = 1639269000; // Third iteration of LWMA retarget activation timestamp

        // Per-algorithm difficulty limits
        consensus.powTypeLimits.emplace_back(uint256{"00000fffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"}); // X16RT limit
        consensus.powTypeLimits.emplace_back(uint256{"000fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"}); // MinotaurX limit

        consensus.nMinimumChainWork = uint256{"0000000000000000000000000000000000000000000000000000000000000002"};
        consensus.defaultAssumeValid = uint256{"00016603365e3252687eeb7a309d9d6b903b81239d9bce670286a7a9d26131b9"};

        // Founder payment (testnet)
        consensus.founderAddress = "2MvpouPdDEujBZg5eZnLNv5bCn78EE2bi65"; // P2SH testnet
        consensus.founderStartBlock = 10;
        consensus.founderRewardStructures = {{std::numeric_limits<int>::max(), 5}}; // 5% forever

        // Max reorg protection
        consensus.nMaxReorganizationDepth = 60;
        consensus.nMinReorganizationPeers = 4;
        consensus.nMinReorganizationAge = 60 * 60 * 12; // 12 hours

        pchMessageStart[0] = 0x52; // R
        pchMessageStart[1] = 0x56; // V
        pchMessageStart[2] = 0x4c; // L
        pchMessageStart[3] = 0x54; // T
        nDefaultPort = 18770;
        nPruneAfterHeight = 1000;
        m_assumed_blockchain_size = 2;
        m_assumed_chain_state_size = 1;

        genesis = CreateGenesisBlock(1630065295, 24922064, 0x1e00ffff, 4, 10 * COIN);
        // Set hashGenesisBlock to the known PoW hash (X16R) directly.
        consensus.hashGenesisBlock = uint256{"00000084af22998d2aed78cc29f1fa587f854150ccd2991dfc82241c8f049219"};
        assert(genesis.hashMerkleRoot == uint256{"63d9b6b6b549a2d96eb5ac4eb2ab80761e6d7bffa9ae1a647191e08d6416184d"});

        vFixedSeeds.clear();
        vSeeds.clear();

        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1, 111);
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1, 196);
        base58Prefixes[SECRET_KEY] =     std::vector<unsigned char>(1, 239);
        base58Prefixes[EXT_PUBLIC_KEY] = {0x04, 0x35, 0x87, 0xCF};
        base58Prefixes[EXT_SECRET_KEY] = {0x04, 0x35, 0x83, 0x94};

        bech32_hrp = "tavn";

        fDefaultConsistencyChecks = false;
        m_is_mockable_chain = false;

        m_assumeutxo_data = {};

        chainTxData = ChainTxData{
            .nTime    = 0,
            .tx_count = 0,
            .dTxRate  = 0,
        };
    }
};

/**
 * Testnet4: Not used by Avian. Stub for compatibility with Bitcoin Core infrastructure.
 */
class CTestNet4Params : public CChainParams {
public:
    CTestNet4Params() {
        m_chain_type = ChainType::TESTNET4;
        consensus.signet_blocks = false;
        consensus.signet_challenge.clear();
        consensus.nSubsidyHalvingInterval = 2100000;
        consensus.nBIP34Enabled = true;
        consensus.nBIP65Enabled = true;
        consensus.nBIP66Enabled = true;
        consensus.nSegwitEnabled = true;
        consensus.nCSVEnabled = true;
        consensus.BIP34Height = 1;
        consensus.BIP34Hash = uint256{};
        consensus.BIP65Height = 1;
        consensus.BIP66Height = 1;
        consensus.CSVHeight = 1;
        consensus.SegwitHeight = 1;
        consensus.MinBIP9WarningHeight = 0;
        consensus.powLimit = uint256{"7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"};
        consensus.nPowTargetTimespan = 2016 * 30;
        consensus.nPowTargetSpacing = 1 * 30;
        consensus.fPowAllowMinDifficultyBlocks = true;
        consensus.enforce_BIP94 = false;
        consensus.fPowNoRetargeting = true;

        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].bit = 28;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nStartTime = Consensus::BIP9Deployment::NEVER_ACTIVE;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].min_activation_height = 0;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].threshold = 1512;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].period = 2016;

        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].bit = 2;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].nStartTime = Consensus::BIP9Deployment::NEVER_ACTIVE;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].min_activation_height = 0;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].threshold = 1512;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].period = 2016;

        consensus.nMinimumChainWork = uint256{};
        consensus.defaultAssumeValid = uint256{};

        pchMessageStart[0] = 0x1c;
        pchMessageStart[1] = 0x16;
        pchMessageStart[2] = 0x3f;
        pchMessageStart[3] = 0x28;
        nDefaultPort = 48333;
        nPruneAfterHeight = 1000;
        m_assumed_blockchain_size = 0;
        m_assumed_chain_state_size = 0;

        // Reuse Avian regtest-style genesis for this stub network
        genesis = CreateGenesisBlock(1629951211, 1, 0x207fffff, 2, 2500 * COIN);
        consensus.hashGenesisBlock = uint256{"653634d03d27ed84e8aba5dd47903906ad7be4876a1d3677be0db2891dcf787f"};
        // No hash assertion for stub network

        vFixedSeeds.clear();
        vSeeds.clear();

        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1, 111);
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1, 196);
        base58Prefixes[SECRET_KEY] =     std::vector<unsigned char>(1, 239);
        base58Prefixes[EXT_PUBLIC_KEY] = {0x04, 0x35, 0x87, 0xCF};
        base58Prefixes[EXT_SECRET_KEY] = {0x04, 0x35, 0x83, 0x94};

        bech32_hrp = "tavn4";

        fDefaultConsistencyChecks = false;
        m_is_mockable_chain = false;

        m_assumeutxo_data = {};

        chainTxData = ChainTxData{
            .nTime = 0,
            .tx_count = 0,
            .dTxRate = 0,
        };
    }
};

/**
 * Signet: Not used by Avian. Stub for compatibility with Bitcoin Core infrastructure.
 */
class SigNetParams : public CChainParams {
public:
    explicit SigNetParams(const SigNetOptions& options)
    {
        std::vector<uint8_t> bin;
        vFixedSeeds.clear();
        vSeeds.clear();

        if (!options.challenge) {
            // Default signet challenge (not used by Avian, kept for infrastructure compat)
            bin = "512103ad5e0edad18cb1f0fc0d28a3d4f1f3e445640337489abb10404f2d1e086be430210359ef5021964fe22d6f8e05b2463c9540ce96883fe3b278760f048f5189f2e6c452ae"_hex_v_u8;

            consensus.nMinimumChainWork = uint256{};
            consensus.defaultAssumeValid = uint256{};
            m_assumed_blockchain_size = 0;
            m_assumed_chain_state_size = 0;
            chainTxData = ChainTxData{
                .nTime    = 0,
                .tx_count = 0,
                .dTxRate  = 0,
            };
        } else {
            bin = *options.challenge;
            consensus.nMinimumChainWork = uint256{};
            consensus.defaultAssumeValid = uint256{};
            m_assumed_blockchain_size = 0;
            m_assumed_chain_state_size = 0;
            chainTxData = ChainTxData{
                0,
                0,
                0,
            };
            LogInfo("Signet with challenge %s", HexStr(bin));
        }

        if (options.seeds) {
            vSeeds = *options.seeds;
        }

        m_chain_type = ChainType::SIGNET;
        consensus.signet_blocks = true;
        consensus.signet_challenge.assign(bin.begin(), bin.end());
        consensus.nSubsidyHalvingInterval = 2100000;
        consensus.nBIP34Enabled = true;
        consensus.nBIP65Enabled = true;
        consensus.nBIP66Enabled = true;
        consensus.nSegwitEnabled = true;
        consensus.nCSVEnabled = true;
        consensus.BIP34Height = 1;
        consensus.BIP34Hash = uint256{};
        consensus.BIP65Height = 1;
        consensus.BIP66Height = 1;
        consensus.CSVHeight = 1;
        consensus.SegwitHeight = 1;
        consensus.nPowTargetTimespan = 2016 * 30;
        consensus.nPowTargetSpacing = 1 * 30;
        consensus.fPowAllowMinDifficultyBlocks = true;
        consensus.enforce_BIP94 = false;
        consensus.fPowNoRetargeting = true;
        consensus.MinBIP9WarningHeight = 0;
        consensus.powLimit = uint256{"7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"};

        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].bit = 28;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nStartTime = Consensus::BIP9Deployment::NEVER_ACTIVE;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].min_activation_height = 0;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].threshold = 1512;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].period = 2016;

        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].bit = 2;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].nStartTime = Consensus::BIP9Deployment::NEVER_ACTIVE;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].min_activation_height = 0;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].threshold = 1512;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].period = 2016;

        // message start is defined as the first 4 bytes of the sha256d of the block script
        HashWriter h{};
        h << consensus.signet_challenge;
        uint256 hash = h.GetHash();
        std::copy_n(hash.begin(), 4, pchMessageStart.begin());

        nDefaultPort = 38333;
        nPruneAfterHeight = 1000;

        // Reuse Avian regtest-style genesis for this stub network
        genesis = CreateGenesisBlock(1629951211, 1, 0x207fffff, 2, 2500 * COIN);
        consensus.hashGenesisBlock = uint256{"653634d03d27ed84e8aba5dd47903906ad7be4876a1d3677be0db2891dcf787f"};
        // No hash assertion for stub network
        assert(genesis.hashMerkleRoot == uint256{"63d9b6b6b549a2d96eb5ac4eb2ab80761e6d7bffa9ae1a647191e08d6416184d"});

        m_assumeutxo_data = {};

        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1, 111);
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1, 196);
        base58Prefixes[SECRET_KEY] =     std::vector<unsigned char>(1, 239);
        base58Prefixes[EXT_PUBLIC_KEY] = {0x04, 0x35, 0x87, 0xCF};
        base58Prefixes[EXT_SECRET_KEY] = {0x04, 0x35, 0x83, 0x94};

        bech32_hrp = "tavn";

        fDefaultConsistencyChecks = false;
        m_is_mockable_chain = false;
    }
};

/**
 * Regression test: intended for private networks only. Has minimal difficulty to ensure that
 * blocks can be found instantly.
 */
class CRegTestParams : public CChainParams
{
public:
    explicit CRegTestParams(const RegTestOptions& opts)
    {
        m_chain_type = ChainType::REGTEST;
        consensus.signet_blocks = false;
        consensus.signet_challenge.clear();
        consensus.nSubsidyHalvingInterval = 150;
        consensus.nBIP34Enabled = true;
        consensus.nBIP65Enabled = true;
        consensus.nBIP66Enabled = true;
        consensus.nSegwitEnabled = true;
        consensus.nCSVEnabled = true;
        consensus.BIP34Height = 1; // Always active unless overridden
        consensus.BIP34Hash = uint256();
        consensus.BIP65Height = 1;  // Always active unless overridden
        consensus.BIP66Height = 1;  // Always active unless overridden
        consensus.CSVHeight = 1;    // Always active unless overridden
        consensus.SegwitHeight = 0; // Always active unless overridden
        consensus.MinBIP9WarningHeight = 0;
        consensus.powLimit = uint256{"7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"};
        consensus.nPowTargetTimespan = 2016 * 30; // 1.4 days
        consensus.nPowTargetSpacing = 1 * 30; // 30 seconds
        consensus.fPowAllowMinDifficultyBlocks = true;
        consensus.enforce_BIP94 = opts.enforce_bip94;
        consensus.fPowNoRetargeting = true;

        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].bit = 28;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nStartTime = 0;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].min_activation_height = 0;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].threshold = 108; // 75%
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].period = 144; // Faster than normal for regtest (144 instead of 2016)

        // Taproot - not deployed on Avian regtest
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].bit = 2;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].nStartTime = Consensus::BIP9Deployment::NEVER_ACTIVE;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].min_activation_height = 0;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].threshold = 108; // 75%
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].period = 144;

        // Avian network upgrades — all active at genesis+1 timestamp for regtest
        consensus.vUpgrades[Consensus::UPGRADE_X16RT_SWITCH].nTimestamp = 1629951212;       // genesis+1
        consensus.vUpgrades[Consensus::UPGRADE_DUAL_ALGO].nTimestamp = 1629951212;          // genesis+1
        consensus.vUpgrades[Consensus::UPGRADE_AVIAN_ASSETS].nTimestamp = 1629951212;       // genesis+1
        consensus.vUpgrades[Consensus::UPGRADE_AVIAN_FLIGHT_PLANS].nTimestamp = 1629951212; // genesis+1
        consensus.vUpgrades[Consensus::UPGRADE_AVIAN_NAME_SYSTEM].nTimestamp = 1629951212;  // genesis+1

        // Dual algorithm consensus — all active from start for regtest
        consensus.powForkTime = 1629951212;       // Time of PoW hash method change (genesis+1)
        consensus.lwmaAveragingWindow = 45;       // Averaging window size for LWMA diff adjust
        consensus.diffRetargetFix = 0;            // Block of diff algo change
        consensus.diffRetargetTake2 = 1629951212; // LWMA3 activation timestamp (genesis+1)

        // Per-algorithm difficulty limits (minimal for regtest)
        consensus.powTypeLimits.emplace_back(uint256{"7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"}); // X16RT limit
        consensus.powTypeLimits.emplace_back(uint256{"7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"}); // MinotaurX limit

        consensus.nMinimumChainWork = uint256{};
        consensus.defaultAssumeValid = uint256{};

        // Founder payment (regtest)
        consensus.founderAddress = "2MwTWhsKXQTpMPEGsWZCdYy7UZRECPPapM1"; // P2SH regtest
        consensus.founderStartBlock = 1;
        consensus.founderRewardStructures = {{std::numeric_limits<int>::max(), 5}}; // 5% forever

        // Max reorg protection
        consensus.nMaxReorganizationDepth = 60;
        consensus.nMinReorganizationPeers = 4;
        consensus.nMinReorganizationAge = 60 * 60 * 12; // 12 hours

        pchMessageStart[0] = 0x52; // R
        pchMessageStart[1] = 0x56; // V
        pchMessageStart[2] = 0x4c; // L
        pchMessageStart[3] = 0x45; // E
        nDefaultPort = 18444;
        nPruneAfterHeight = opts.fastprune ? 100 : 1000;
        m_assumed_blockchain_size = 0;
        m_assumed_chain_state_size = 0;

        for (const auto& [dep, height] : opts.activation_heights) {
            switch (dep) {
            case Consensus::BuriedDeployment::DEPLOYMENT_SEGWIT:
                consensus.SegwitHeight = int{height};
                break;
            case Consensus::BuriedDeployment::DEPLOYMENT_HEIGHTINCB:
                consensus.BIP34Height = int{height};
                break;
            case Consensus::BuriedDeployment::DEPLOYMENT_DERSIG:
                consensus.BIP66Height = int{height};
                break;
            case Consensus::BuriedDeployment::DEPLOYMENT_CLTV:
                consensus.BIP65Height = int{height};
                break;
            case Consensus::BuriedDeployment::DEPLOYMENT_CSV:
                consensus.CSVHeight = int{height};
                break;
            }
        }

        for (const auto& [deployment_pos, version_bits_params] : opts.version_bits_parameters) {
            consensus.vDeployments[deployment_pos].nStartTime = version_bits_params.start_time;
            consensus.vDeployments[deployment_pos].nTimeout = version_bits_params.timeout;
            consensus.vDeployments[deployment_pos].min_activation_height = version_bits_params.min_activation_height;
        }

        genesis = CreateGenesisBlock(1629951211, 1, 0x207fffff, 2, 2500 * COIN);
        // Set hashGenesisBlock to the known PoW hash (X16R) directly.
        consensus.hashGenesisBlock = uint256{"653634d03d27ed84e8aba5dd47903906ad7be4876a1d3677be0db2891dcf787f"};
        assert(genesis.hashMerkleRoot == uint256{"63d9b6b6b549a2d96eb5ac4eb2ab80761e6d7bffa9ae1a647191e08d6416184d"});

        vFixedSeeds.clear(); //!< Regtest mode doesn't have any fixed seeds.
        vSeeds.clear();

        fDefaultConsistencyChecks = true;
        m_is_mockable_chain = true;

        // No assumeutxo data available for Avian regtest
        m_assumeutxo_data = {};

        chainTxData = ChainTxData{
            .nTime = 0,
            .tx_count = 0,
            .dTxRate = 0.001, // Set a non-zero rate to make it testable
        };

        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1, 111);
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1, 196);
        base58Prefixes[SECRET_KEY] =     std::vector<unsigned char>(1, 239);
        base58Prefixes[EXT_PUBLIC_KEY] = {0x04, 0x35, 0x87, 0xCF};
        base58Prefixes[EXT_SECRET_KEY] = {0x04, 0x35, 0x83, 0x94};

        bech32_hrp = "avnrt";
    }
};

std::unique_ptr<const CChainParams> CChainParams::SigNet(const SigNetOptions& options)
{
    return std::make_unique<const SigNetParams>(options);
}

std::unique_ptr<const CChainParams> CChainParams::RegTest(const RegTestOptions& options)
{
    return std::make_unique<const CRegTestParams>(options);
}

std::unique_ptr<const CChainParams> CChainParams::Main()
{
    return std::make_unique<const CMainParams>();
}

std::unique_ptr<const CChainParams> CChainParams::TestNet()
{
    return std::make_unique<const CTestNetParams>();
}

std::unique_ptr<const CChainParams> CChainParams::TestNet4()
{
    return std::make_unique<const CTestNet4Params>();
}

std::vector<int> CChainParams::GetAvailableSnapshotHeights() const
{
    std::vector<int> heights;
    heights.reserve(m_assumeutxo_data.size());

    for (const auto& data : m_assumeutxo_data) {
        heights.emplace_back(data.height);
    }
    return heights;
}

std::optional<ChainType> GetNetworkForMagic(const MessageStartChars& message)
{
    const auto mainnet_msg = CChainParams::Main()->MessageStart();
    const auto testnet_msg = CChainParams::TestNet()->MessageStart();
    const auto testnet4_msg = CChainParams::TestNet4()->MessageStart();
    const auto regtest_msg = CChainParams::RegTest({})->MessageStart();
    const auto signet_msg = CChainParams::SigNet({})->MessageStart();

    if (std::ranges::equal(message, mainnet_msg)) {
        return ChainType::MAIN;
    } else if (std::ranges::equal(message, testnet_msg)) {
        return ChainType::TESTNET;
    } else if (std::ranges::equal(message, testnet4_msg)) {
        return ChainType::TESTNET4;
    } else if (std::ranges::equal(message, regtest_msg)) {
        return ChainType::REGTEST;
    } else if (std::ranges::equal(message, signet_msg)) {
        return ChainType::SIGNET;
    }
    return std::nullopt;
}
