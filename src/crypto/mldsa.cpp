// Copyright (c) 2026 ALENOC <https://github.com/ALENOC> (Ravencoin RIP-25)
// Copyright (c) 2024-present The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

// RIP-25: ML-DSA-44 (FIPS 204) liboqs wrapper implementation.
// Ported from Ravencoin RIP-25 <https://github.com/RavenProject/Ravencoin/pull/1281>

#include <crypto/mldsa.h>

#ifdef HAVE_LIBOQS
#include <oqs/oqs.h>
#endif

#include <crypto/sha256.h>
#include <support/cleanse.h>

namespace mldsa {

#ifdef HAVE_LIBOQS

static_assert(PUBKEY_SIZE == 1312 && SECRETKEY_SIZE == 2560 && SIG_SIZE == 2420 && SEED_SIZE == 32,
              "RIP-25 assumes ML-DSA-44 (FIPS 204 parameter set I) sizes");

namespace {

// RAII wrapper for OQS_SIG (used only by KeyGenRandom).
struct OqsSig {
    OQS_SIG* sig;
    explicit OqsSig() : sig(OQS_SIG_new(OQS_SIG_alg_ml_dsa_44)) {}
    ~OqsSig() { if (sig) OQS_SIG_free(sig); }
    OqsSig(const OqsSig&) = delete;
    OqsSig& operator=(const OqsSig&) = delete;
    bool ok() const { return sig != nullptr; }
};

// FIPS-204 KeyGen_internal from liboqs's mldsa-native backend (liboqs >= 0.16).
// liboqs exposes no public seeded keypair, so deterministic key generation calls
// this one internal symbol: a standardized, deterministic seed -> (pk, sk)
// contract over plain byte buffers, with no struct-ABI coupling. The portable
// "_C_" reference is compiled on every platform (present in both dist and
// non-dist builds) and produces byte-identical FIPS-204 output regardless of
// CPU; the RIP-25 known-answer vector in pqkey_tests.cpp pins that output, so any
// drift is caught at test time rather than silently changing derived keys.
// (Sign/Verify below use the public OQS ctx-string API and need no internal symbol.)
extern "C" {
    int PQCP_MLDSA_NATIVE_MLDSA44_C_keypair_internal(uint8_t* pk, uint8_t* sk, const uint8_t* seed);
}

} // namespace

bool KeyGenFromSeed(std::span<uint8_t, PUBKEY_SIZE> pubkey,
                    std::span<uint8_t, SECRETKEY_SIZE> seckey,
                    std::span<const uint8_t, SEED_SIZE> seed)
{
    // RIP-25 key derivation (normative; see doc/rip-25.md):
    //   xi        = SHA256( DST || seed )              32-byte ML-DSA seed
    //   (pk, sk)  = ML-DSA.KeyGen_internal(xi)         FIPS 204 Algorithm 6
    // The versioned domain-separation tag makes the derivation an explicit,
    // written algorithm; a future change is unambiguous (…/v2).
    static constexpr char DST[] = "AVN/ML-DSA-44/keygen/v1";
    uint8_t xi[SEED_SIZE];
    CSHA256()
        .Write(reinterpret_cast<const uint8_t*>(DST), sizeof(DST) - 1)
        .Write(seed.data(), seed.size())
        .Finalize(xi);

    const int rc = PQCP_MLDSA_NATIVE_MLDSA44_C_keypair_internal(pubkey.data(), seckey.data(), xi);
    memory_cleanse(xi, sizeof(xi));
    return rc == 0;
}

bool KeyGenRandom(std::span<uint8_t, PUBKEY_SIZE> pubkey,
                  std::span<uint8_t, SECRETKEY_SIZE> seckey)
{
    OqsSig ctx;
    if (!ctx.ok()) return false;
    OQS_STATUS rc = OQS_SIG_keypair(ctx.sig, pubkey.data(), seckey.data());
    return rc == OQS_SUCCESS;
}

bool Sign(std::span<uint8_t, SIG_SIZE> sig,
          std::span<const uint8_t> msg,
          std::span<const uint8_t> ctx,
          std::span<const uint8_t, SECRETKEY_SIZE> seckey)
{
    if (ctx.size() > 255) return false;  // FIPS 204 context-string limit
    size_t sig_len = 0;
    OQS_STATUS rc = OQS_SIG_ml_dsa_44_sign_with_ctx_str(
        sig.data(), &sig_len,
        msg.data(), msg.size(),
        ctx.data(), ctx.size(),
        seckey.data());
    return rc == OQS_SUCCESS && sig_len == SIG_SIZE;
}

bool Verify(std::span<const uint8_t, SIG_SIZE> sig,
            std::span<const uint8_t> msg,
            std::span<const uint8_t> ctx,
            std::span<const uint8_t, PUBKEY_SIZE> pubkey)
{
    if (ctx.size() > 255) return false;
    OQS_STATUS rc = OQS_SIG_ml_dsa_44_verify_with_ctx_str(
        msg.data(), msg.size(),
        sig.data(), SIG_SIZE,
        ctx.data(), ctx.size(),
        pubkey.data());
    return rc == OQS_SUCCESS;
}

bool IsAvailable() { return true; }

#else // HAVE_LIBOQS not defined — stub implementations

bool KeyGenFromSeed(std::span<uint8_t, PUBKEY_SIZE>, std::span<uint8_t, SECRETKEY_SIZE>,
                    std::span<const uint8_t, SEED_SIZE>)
{ return false; }

bool KeyGenRandom(std::span<uint8_t, PUBKEY_SIZE>, std::span<uint8_t, SECRETKEY_SIZE>)
{ return false; }

bool Sign(std::span<uint8_t, SIG_SIZE>, std::span<const uint8_t>,
          std::span<const uint8_t>, std::span<const uint8_t, SECRETKEY_SIZE>)
{ return false; }

bool Verify(std::span<const uint8_t, SIG_SIZE>, std::span<const uint8_t>,
            std::span<const uint8_t>, std::span<const uint8_t, PUBKEY_SIZE>)
{ return false; }

bool IsAvailable() { return false; }

#endif // HAVE_LIBOQS

} // namespace mldsa
