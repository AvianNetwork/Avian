// Copyright (c) 2017-2019 The Raven Core developers
// Copyright (c) 2022 The Avian Core developers

// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef AVN_HASHALGOS_H
#define AVN_HASHALGOS_H

#include "serialize.h"
#include "version.h"

#include "../../crypto/sha256.h"
#include "../../uint256.h"
#include "sph_blake.h"
#include "sph_bmw.h"
#include "sph_cubehash.h"
#include "sph_echo.h"
#include "sph_fugue.h"
#include "sph_groestl.h"
#include "sph_hamsi.h"
#include "sph_haval.h"
#include "sph_jh.h"
#include "sph_keccak.h"
#include "sph_luffa.h"
#include "sph_sha2.h"
#include "sph_shabal.h"
#include "sph_shavite.h"
#include "sph_simd.h"
#include "sph_skein.h"
#include "sph_whirlpool.h"

#include "gost_streebog.h"
#include "lyra2.h"
#include "sph_tiger.h"

#include <vector>

class CBlockHeader;

#ifndef QT_NO_DEBUG
#include <string>
#endif

#ifdef GLOBALDEFINED
#define GLOBAL
#else
#define GLOBAL extern
#endif

inline int GetHashSelection(const uint256 PrevBlockHash, int index)
{
    assert(index >= 0);
    assert(index < 16);

#define START_OF_LAST_16_NIBBLES_OF_HASH 48
    int hashSelection = PrevBlockHash.GetNibble(START_OF_LAST_16_NIBBLES_OF_HASH + index);
    return (hashSelection);
}

// SHA-256
/** A hasher class for Bitcoin's 256-bit hash (double SHA-256). */
class CHash256
{
private:
    CSHA256 sha;

public:
    static const size_t OUTPUT_SIZE = CSHA256::OUTPUT_SIZE;

    void Finalize(unsigned char hash[OUTPUT_SIZE])
    {
        unsigned char buf[CSHA256::OUTPUT_SIZE];
        sha.Finalize(buf);
        sha.Reset().Write(buf, CSHA256::OUTPUT_SIZE).Finalize(hash);
    }

    CHash256& Write(const unsigned char* data, size_t len)
    {
        sha.Write(data, len);
        return *this;
    }

    CHash256& Reset()
    {
        sha.Reset();
        return *this;
    }
};

/** Compute the 256-bit hash of an object. */
template <typename T1>
inline uint256 Hash(const T1 pbegin, const T1 pend)
{
    static const unsigned char pblank[1] = {};
    uint256 result;
    CHash256().Write(pbegin == pend ? pblank : (const unsigned char*)&pbegin[0], (pend - pbegin) * sizeof(pbegin[0])).Finalize((unsigned char*)&result);
    return result;
}

/** A writer stream (for serialization) that computes a 256-bit hash. */
class CHashWriter
{
private:
    CHash256 ctx;

public:
    int nType;
    int nVersion;

    CHashWriter(int nTypeIn, int nVersionIn) : nType(nTypeIn), nVersion(nVersionIn) {}

    int GetType() const { return nType; }
    int GetVersion() const { return nVersion; }

    void write(const char* pch, size_t size)
    {
        ctx.Write((const unsigned char*)pch, size);
    }

    // invalidates the object
    uint256 GetHash()
    {
        uint256 result;
        ctx.Finalize((unsigned char*)&result);
        return result;
    }

    template <typename T>
    CHashWriter& operator<<(const T& obj)
    {
        // Serialize to this stream
        ::Serialize(*this, obj);
        return (*this);
    }
};

/** Compute the 256-bit hash of an object's serialization. */
template <typename T>
uint256 SerializeHash(const T& obj, int nType = SER_GETHASH, int nVersion = PROTOCOL_VERSION)
{
    CHashWriter ss(nType, nVersion);
    ss << obj;
    return ss.GetHash();
}

// x16r
template <typename T1>
inline uint256 HashX16R(const T1 pbegin, const T1 pend, const uint256 PrevBlockHash)
{
    int hashSelection;

    sph_blake512_context ctx_blake;       // 0
    sph_bmw512_context ctx_bmw;           // 1
    sph_groestl512_context ctx_groestl;   // 2
    sph_jh512_context ctx_jh;             // 3
    sph_keccak512_context ctx_keccak;     // 4
    sph_skein512_context ctx_skein;       // 5
    sph_luffa512_context ctx_luffa;       // 6
    sph_cubehash512_context ctx_cubehash; // 7
    sph_shavite512_context ctx_shavite;   // 8
    sph_simd512_context ctx_simd;         // 9
    sph_echo512_context ctx_echo;         // A
    sph_hamsi512_context ctx_hamsi;       // B
    sph_fugue512_context ctx_fugue;       // C
    sph_shabal512_context ctx_shabal;     // D
    sph_whirlpool_context ctx_whirlpool;  // E
    sph_sha512_context ctx_sha512;        // F


    static unsigned char pblank[1];

    uint512 hash[16];

    for (int i = 0; i < 16; i++) {
        const void* toHash;
        int lenToHash;
        if (i == 0) {
            toHash = (pbegin == pend ? pblank : static_cast<const void*>(&pbegin[0]));
            lenToHash = (pend - pbegin) * sizeof(pbegin[0]);
        } else {
            toHash = static_cast<const void*>(&hash[i - 1]);
            lenToHash = 64;
        }

        hashSelection = GetHashSelection(PrevBlockHash, i);

        switch (hashSelection) {
        case 0:
            sph_blake512_init(&ctx_blake);
            sph_blake512(&ctx_blake, toHash, lenToHash);
            sph_blake512_close(&ctx_blake, static_cast<void*>(&hash[i]));
            break;
        case 1:
            sph_bmw512_init(&ctx_bmw);
            sph_bmw512(&ctx_bmw, toHash, lenToHash);
            sph_bmw512_close(&ctx_bmw, static_cast<void*>(&hash[i]));
            break;
        case 2:
            sph_groestl512_init(&ctx_groestl);
            sph_groestl512(&ctx_groestl, toHash, lenToHash);
            sph_groestl512_close(&ctx_groestl, static_cast<void*>(&hash[i]));
            break;
        case 3:
            sph_jh512_init(&ctx_jh);
            sph_jh512(&ctx_jh, toHash, lenToHash);
            sph_jh512_close(&ctx_jh, static_cast<void*>(&hash[i]));
            break;
        case 4:
            sph_keccak512_init(&ctx_keccak);
            sph_keccak512(&ctx_keccak, toHash, lenToHash);
            sph_keccak512_close(&ctx_keccak, static_cast<void*>(&hash[i]));
            break;
        case 5:
            sph_skein512_init(&ctx_skein);
            sph_skein512(&ctx_skein, toHash, lenToHash);
            sph_skein512_close(&ctx_skein, static_cast<void*>(&hash[i]));
            break;
        case 6:
            sph_luffa512_init(&ctx_luffa);
            sph_luffa512(&ctx_luffa, toHash, lenToHash);
            sph_luffa512_close(&ctx_luffa, static_cast<void*>(&hash[i]));
            break;
        case 7:
            sph_cubehash512_init(&ctx_cubehash);
            sph_cubehash512(&ctx_cubehash, toHash, lenToHash);
            sph_cubehash512_close(&ctx_cubehash, static_cast<void*>(&hash[i]));
            break;
        case 8:
            sph_shavite512_init(&ctx_shavite);
            sph_shavite512(&ctx_shavite, toHash, lenToHash);
            sph_shavite512_close(&ctx_shavite, static_cast<void*>(&hash[i]));
            break;
        case 9:
            sph_simd512_init(&ctx_simd);
            sph_simd512(&ctx_simd, toHash, lenToHash);
            sph_simd512_close(&ctx_simd, static_cast<void*>(&hash[i]));
            break;
        case 10:
            sph_echo512_init(&ctx_echo);
            sph_echo512(&ctx_echo, toHash, lenToHash);
            sph_echo512_close(&ctx_echo, static_cast<void*>(&hash[i]));
            break;
        case 11:
            sph_hamsi512_init(&ctx_hamsi);
            sph_hamsi512(&ctx_hamsi, toHash, lenToHash);
            sph_hamsi512_close(&ctx_hamsi, static_cast<void*>(&hash[i]));
            break;
        case 12:
            sph_fugue512_init(&ctx_fugue);
            sph_fugue512(&ctx_fugue, toHash, lenToHash);
            sph_fugue512_close(&ctx_fugue, static_cast<void*>(&hash[i]));
            break;
        case 13:
            sph_shabal512_init(&ctx_shabal);
            sph_shabal512(&ctx_shabal, toHash, lenToHash);
            sph_shabal512_close(&ctx_shabal, static_cast<void*>(&hash[i]));
            break;
        case 14:
            sph_whirlpool_init(&ctx_whirlpool);
            sph_whirlpool(&ctx_whirlpool, toHash, lenToHash);
            sph_whirlpool_close(&ctx_whirlpool, static_cast<void*>(&hash[i]));
            break;
        case 15:
            sph_sha512_init(&ctx_sha512);
            sph_sha512(&ctx_sha512, toHash, lenToHash);
            sph_sha512_close(&ctx_sha512, static_cast<void*>(&hash[i]));
            break;
        }
    }

    return hash[15].trim256();
}

// x16rv2
template <typename T1>
inline uint256 HashX16RV2(const T1 pbegin, const T1 pend, const uint256 PrevBlockHash)
{
    int hashSelection;

    sph_blake512_context ctx_blake;       // 0
    sph_bmw512_context ctx_bmw;           // 1
    sph_groestl512_context ctx_groestl;   // 2
    sph_jh512_context ctx_jh;             // 3
    sph_keccak512_context ctx_keccak;     // 4
    sph_skein512_context ctx_skein;       // 5
    sph_luffa512_context ctx_luffa;       // 6
    sph_cubehash512_context ctx_cubehash; // 7
    sph_shavite512_context ctx_shavite;   // 8
    sph_simd512_context ctx_simd;         // 9
    sph_echo512_context ctx_echo;         // A
    sph_hamsi512_context ctx_hamsi;       // B
    sph_fugue512_context ctx_fugue;       // C
    sph_shabal512_context ctx_shabal;     // D
    sph_whirlpool_context ctx_whirlpool;  // E

    sph_sha512_context ctx_sha512;
    sph_tiger_context ctx_tiger;


    static unsigned char pblank[1];

    uint512 hash[16];

    for (int i = 0; i < 16; i++) {
        const void* toHash;
        int lenToHash;
        if (i == 0) {
            toHash = (pbegin == pend ? pblank : static_cast<const void*>(&pbegin[0]));
            lenToHash = (pend - pbegin) * sizeof(pbegin[0]);
        } else {
            toHash = static_cast<const void*>(&hash[i - 1]);
            lenToHash = 64;
        }

        hashSelection = GetHashSelection(PrevBlockHash, i);

        switch (hashSelection) {
        case 0:
            sph_blake512_init(&ctx_blake);
            sph_blake512(&ctx_blake, toHash, lenToHash);
            sph_blake512_close(&ctx_blake, static_cast<void*>(&hash[i]));
            break;
        case 1:
            sph_bmw512_init(&ctx_bmw);
            sph_bmw512(&ctx_bmw, toHash, lenToHash);
            sph_bmw512_close(&ctx_bmw, static_cast<void*>(&hash[i]));
            break;
        case 2:
            sph_groestl512_init(&ctx_groestl);
            sph_groestl512(&ctx_groestl, toHash, lenToHash);
            sph_groestl512_close(&ctx_groestl, static_cast<void*>(&hash[i]));
            break;
        case 3:
            sph_jh512_init(&ctx_jh);
            sph_jh512(&ctx_jh, toHash, lenToHash);
            sph_jh512_close(&ctx_jh, static_cast<void*>(&hash[i]));
            break;
        case 4:
            sph_tiger_init(&ctx_tiger);
            sph_tiger(&ctx_tiger, toHash, lenToHash);
            sph_tiger_close(&ctx_tiger, static_cast<void*>(&hash[i]));

            sph_keccak512_init(&ctx_keccak);
            sph_keccak512(&ctx_keccak, static_cast<const void*>(&hash[i]), 64);
            sph_keccak512_close(&ctx_keccak, static_cast<void*>(&hash[i]));
            break;
        case 5:
            sph_skein512_init(&ctx_skein);
            sph_skein512(&ctx_skein, toHash, lenToHash);
            sph_skein512_close(&ctx_skein, static_cast<void*>(&hash[i]));
            break;
        case 6:
            sph_tiger_init(&ctx_tiger);
            sph_tiger(&ctx_tiger, toHash, lenToHash);
            sph_tiger_close(&ctx_tiger, static_cast<void*>(&hash[i]));

            sph_luffa512_init(&ctx_luffa);
            sph_luffa512(&ctx_luffa, static_cast<const void*>(&hash[i]), 64);
            sph_luffa512_close(&ctx_luffa, static_cast<void*>(&hash[i]));
            break;
        case 7:
            sph_cubehash512_init(&ctx_cubehash);
            sph_cubehash512(&ctx_cubehash, toHash, lenToHash);
            sph_cubehash512_close(&ctx_cubehash, static_cast<void*>(&hash[i]));
            break;
        case 8:
            sph_shavite512_init(&ctx_shavite);
            sph_shavite512(&ctx_shavite, toHash, lenToHash);
            sph_shavite512_close(&ctx_shavite, static_cast<void*>(&hash[i]));
            break;
        case 9:
            sph_simd512_init(&ctx_simd);
            sph_simd512(&ctx_simd, toHash, lenToHash);
            sph_simd512_close(&ctx_simd, static_cast<void*>(&hash[i]));
            break;
        case 10:
            sph_echo512_init(&ctx_echo);
            sph_echo512(&ctx_echo, toHash, lenToHash);
            sph_echo512_close(&ctx_echo, static_cast<void*>(&hash[i]));
            break;
        case 11:
            sph_hamsi512_init(&ctx_hamsi);
            sph_hamsi512(&ctx_hamsi, toHash, lenToHash);
            sph_hamsi512_close(&ctx_hamsi, static_cast<void*>(&hash[i]));
            break;
        case 12:
            sph_fugue512_init(&ctx_fugue);
            sph_fugue512(&ctx_fugue, toHash, lenToHash);
            sph_fugue512_close(&ctx_fugue, static_cast<void*>(&hash[i]));
            break;
        case 13:
            sph_shabal512_init(&ctx_shabal);
            sph_shabal512(&ctx_shabal, toHash, lenToHash);
            sph_shabal512_close(&ctx_shabal, static_cast<void*>(&hash[i]));
            break;
        case 14:
            sph_whirlpool_init(&ctx_whirlpool);
            sph_whirlpool(&ctx_whirlpool, toHash, lenToHash);
            sph_whirlpool_close(&ctx_whirlpool, static_cast<void*>(&hash[i]));
            break;
        case 15:
            sph_tiger_init(&ctx_tiger);
            sph_tiger(&ctx_tiger, toHash, lenToHash);
            sph_tiger_close(&ctx_tiger, static_cast<void*>(&hash[i]));

            sph_sha512_init(&ctx_sha512);
            sph_sha512(&ctx_sha512, static_cast<const void*>(&hash[i]), 64);
            sph_sha512_close(&ctx_sha512, static_cast<void*>(&hash[i]));
            break;
        }
    }

    return hash[15].trim256();
}

#endif // AVN_HASHALGOS_H