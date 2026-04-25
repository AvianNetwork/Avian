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
#include <algorithm>
#include <cstring>
#include <mutex>

namespace mldsa {

#ifdef HAVE_LIBOQS

namespace {

// RAII wrapper for OQS_SIG to ensure cleanup on all exit paths.
struct OqsSig {
    OQS_SIG* sig;
    explicit OqsSig() : sig(OQS_SIG_new(OQS_SIG_alg_ml_dsa_44)) {}
    ~OqsSig() { if (sig) OQS_SIG_free(sig); }
    OqsSig(const OqsSig&) = delete;
    OqsSig& operator=(const OqsSig&) = delete;
    bool ok() const { return sig != nullptr; }
};

// ---- Deterministic RNG state for KeyGenFromSeed ----------------------------
// liboqs 0.12.0 does not expose OQS_SIG_ml_dsa_44_keypair_derand publicly.
// We achieve deterministic key generation by temporarily replacing the global
// liboqs RNG with a SHA256-based counter-mode KDF seeded from the provided
// 32-byte seed.  Access is serialised by s_derand_mutex so that this
// manipulation of global OQS RNG state is thread-safe.

static std::mutex s_derand_mutex;
static const uint8_t* s_derand_seed = nullptr;
static uint32_t s_derand_ctr = 0;

// Plain function pointer (no captures) required by OQS_randombytes_custom_algorithm.
void DerandRNG(uint8_t* out, size_t len)
{
    uint8_t block[32 + sizeof(uint32_t)];
    std::memcpy(block, s_derand_seed, 32);
    while (len > 0) {
        std::memcpy(block + 32, &s_derand_ctr, sizeof(s_derand_ctr));
        uint8_t hash[CSHA256::OUTPUT_SIZE];
        CSHA256().Write(block, sizeof(block)).Finalize(hash);
        size_t chunk = std::min(len, sizeof(hash));
        std::memcpy(out, hash, chunk);
        out += chunk;
        len -= chunk;
        ++s_derand_ctr;
    }
}
// ---------------------------------------------------------------------------

} // namespace

bool KeyGenFromSeed(std::span<uint8_t, PUBKEY_SIZE> pubkey,
                    std::span<uint8_t, SECRETKEY_SIZE> seckey,
                    std::span<const uint8_t, SEED_SIZE> seed)
{
    // Use OQS_randombytes_custom_algorithm to inject a deterministic
    // SHA256 counter-mode KDF so that keypair generation is reproducible
    // from the same seed without requiring _derand or NIST KAT DRBG symbols.
    std::lock_guard<std::mutex> lock(s_derand_mutex);
    s_derand_seed = seed.data();
    s_derand_ctr  = 0;
    OQS_randombytes_custom_algorithm(DerandRNG);
    OQS_STATUS rc = OQS_SIG_ml_dsa_44_keypair(pubkey.data(), seckey.data());
    OQS_randombytes_switch_algorithm(OQS_RAND_alg_system);
    s_derand_seed = nullptr;
    return rc == OQS_SUCCESS;
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
          std::span<const uint8_t, SECRETKEY_SIZE> seckey)
{
    OqsSig ctx;
    if (!ctx.ok()) return false;
    size_t sig_len = SIG_SIZE;
    OQS_STATUS rc = OQS_SIG_sign(ctx.sig,
                                 sig.data(), &sig_len,
                                 msg.data(), msg.size(),
                                 seckey.data());
    return rc == OQS_SUCCESS && sig_len == SIG_SIZE;
}

bool Verify(std::span<const uint8_t, SIG_SIZE> sig,
            std::span<const uint8_t> msg,
            std::span<const uint8_t, PUBKEY_SIZE> pubkey)
{
    OqsSig ctx;
    if (!ctx.ok()) return false;
    OQS_STATUS rc = OQS_SIG_verify(ctx.sig,
                                   msg.data(), msg.size(),
                                   sig.data(), SIG_SIZE,
                                   pubkey.data());
    return rc == OQS_SUCCESS;
}

#else // HAVE_LIBOQS not defined — stub implementations

bool KeyGenFromSeed(std::span<uint8_t, PUBKEY_SIZE>, std::span<uint8_t, SECRETKEY_SIZE>,
                    std::span<const uint8_t, SEED_SIZE>)
{ return false; }

bool KeyGenRandom(std::span<uint8_t, PUBKEY_SIZE>, std::span<uint8_t, SECRETKEY_SIZE>)
{ return false; }

bool Sign(std::span<uint8_t, SIG_SIZE>, std::span<const uint8_t>,
          std::span<const uint8_t, SECRETKEY_SIZE>)
{ return false; }

bool Verify(std::span<const uint8_t, SIG_SIZE>, std::span<const uint8_t>,
            std::span<const uint8_t, PUBKEY_SIZE>)
{ return false; }

#endif // HAVE_LIBOQS

} // namespace mldsa
