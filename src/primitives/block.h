// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2022 The Bitcoin Core developers
// Copyright (c) 2017 The Raven Core developers
// Copyright (c) 2022 The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_PRIMITIVES_BLOCK_H
#define BITCOIN_PRIMITIVES_BLOCK_H

#include <primitives/transaction.h>
#include <serialize.h>
#include <uint256.h>
#include <util/time.h>

#include <string>

#include <util/fs.h>

// Dual Algo: An impossible pow hash (can't meet any target)
const uint256 HIGH_HASH = uint256{"0fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"};

// Dual Algo: Default value for -powalgo argument
const std::string DEFAULT_POW_TYPE = "x16rt";

// Dual Algo: Pow type names
const std::string POW_TYPE_NAMES[] = {
    "x16rt",
    "minotaurx"};

// Dual Algo: Pow type IDs
enum POW_TYPE {
    POW_TYPE_X16RT,
    POW_TYPE_MINOTAURX,
    //
    NUM_BLOCK_TYPES
};

// Called during initialization to set the PoW algorithm timestamps.
// Must be called before any block hashing occurs (except genesis).
void SetPoWHashParams(uint32_t nX16rtTimestamp, uint32_t nDualAlgoTimestamp);
bool ArePoWHashParamsSet();

// PowCache disk persistence: save/load expensive PoW hash cache to/from disk
bool SavePowCache(const fs::path& cache_path);
bool LoadPowCache(const fs::path& cache_path);

/** Nodes collect new transactions into a block, hash them into a hash tree,
 * and scan through nonce values to make the block's hash satisfy proof-of-work
 * requirements.  When they solve the proof-of-work, they broadcast the block
 * to everyone and the block is added to the block chain.  The first transaction
 * in the block is a special one that creates a new coin owned by the creator
 * of the block.
 */
class CBlockHeader
{
public:
    // header
    int32_t nVersion;
    uint256 hashPrevBlock;
    uint256 hashMerkleRoot;
    uint32_t nTime;
    uint32_t nBits;
    uint32_t nNonce;

    // Cached PoW hash (not serialized, mutable for const access)
    mutable uint256 m_cachedPoWHash;
    mutable bool m_hasPoWHash{false};

    CBlockHeader()
    {
        SetNull();
    }

    SERIALIZE_METHODS(CBlockHeader, obj) { READWRITE(obj.nVersion, obj.hashPrevBlock, obj.hashMerkleRoot, obj.nTime, obj.nBits, obj.nNonce); }

    void SetNull()
    {
        nVersion = 0;
        hashPrevBlock.SetNull();
        hashMerkleRoot.SetNull();
        nTime = 0;
        nBits = 0;
        nNonce = 0;
        m_cachedPoWHash.SetNull();
        m_hasPoWHash = false;
    }

    bool IsNull() const
    {
        return (nBits == 0);
    }

    // Returns the SHA256d hash (used when PoW params not yet set)
    uint256 GetSHA256Hash() const;

    // Computes the PoW hash using provided timestamps (no global state access)
    uint256 ComputePoWHash(uint32_t nX16rtTimestamp, uint32_t nDualAlgoTimestamp) const;

    // Returns the PoW hash (X16R/X16RT/MinotaurX) when PoW params are set,
    // or SHA256d before initialization. Result is cached.
    uint256 GetHash() const;

    // Direct X16R hash using hashPrevBlock for hash selection (genesis blocks)
    uint256 GetX16RHash() const;

    // Dual Algo: MinotaurX hash of arbitrary data
    static uint256 MinotaurxHashArbitrary(const char* data);

    NodeSeconds Time() const
    {
        return NodeSeconds{std::chrono::seconds{nTime}};
    }

    int64_t GetBlockTime() const
    {
        return (int64_t)nTime;
    }

    // Dual Algo: Get pow type from version bits
    POW_TYPE GetPoWType() const
    {
        return (POW_TYPE)((nVersion >> 16) & 0xFF);
    }

    // Dual Algo: Get pow type name
    std::string GetPoWTypeName() const
    {
        POW_TYPE pt = GetPoWType();
        if (pt >= NUM_BLOCK_TYPES)
            return "unrecognised";
        return POW_TYPE_NAMES[pt];
    }
};


class CBlock : public CBlockHeader
{
public:
    // network and disk
    std::vector<CTransactionRef> vtx;

    // founder payment
    mutable CTxOut txoutFounder;

    // Memory-only flags for caching expensive checks
    mutable bool fChecked;                            // CheckBlock()
    mutable bool m_checked_witness_commitment{false}; // CheckWitnessCommitment()
    mutable bool m_checked_merkle_root{false};        // CheckMerkleRoot()

    CBlock()
    {
        SetNull();
    }

    CBlock(const CBlockHeader &header)
    {
        SetNull();
        *(static_cast<CBlockHeader*>(this)) = header;
    }

    SERIALIZE_METHODS(CBlock, obj)
    {
        READWRITE(AsBase<CBlockHeader>(obj), obj.vtx);
    }

    void SetNull()
    {
        CBlockHeader::SetNull();
        vtx.clear();
        fChecked = false;
        m_checked_witness_commitment = false;
        m_checked_merkle_root = false;
        txoutFounder = CTxOut();
    }

    CBlockHeader GetBlockHeader() const
    {
        CBlockHeader block;
        block.nVersion       = nVersion;
        block.hashPrevBlock  = hashPrevBlock;
        block.hashMerkleRoot = hashMerkleRoot;
        block.nTime          = nTime;
        block.nBits          = nBits;
        block.nNonce         = nNonce;
        return block;
    }

    std::string ToString() const;
};

/** Describes a place in the block chain to another node such that if the
 * other node doesn't have the same branch, it can find a recent common trunk.
 * The further back it is, the further before the fork it may be.
 */
struct CBlockLocator
{
    /** Historically CBlockLocator's version field has been written to network
     * streams as the negotiated protocol version and to disk streams as the
     * client version, but the value has never been used.
     *
     * Hard-code to the highest protocol version ever written to a network stream.
     * SerParams can be used if the field requires any meaning in the future,
     **/
    static constexpr int DUMMY_VERSION = 70016;

    std::vector<uint256> vHave;

    CBlockLocator() = default;

    explicit CBlockLocator(std::vector<uint256>&& have) : vHave(std::move(have)) {}

    SERIALIZE_METHODS(CBlockLocator, obj)
    {
        int nVersion = DUMMY_VERSION;
        READWRITE(nVersion);
        READWRITE(obj.vHave);
    }

    void SetNull()
    {
        vHave.clear();
    }

    bool IsNull() const
    {
        return vHave.empty();
    }
};

#endif // BITCOIN_PRIMITIVES_BLOCK_H
