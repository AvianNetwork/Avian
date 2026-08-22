// Copyright (c) 2026 ALENOC <https://github.com/ALENOC> (Ravencoin RIP-25)
// Copyright (c) 2024-present The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

// RIP-25: ML-DSA-44 (FIPS 204) post-quantum signature scheme wrapper.
// Ported from Ravencoin RIP-25 <https://github.com/RavenProject/Ravencoin/pull/1281>
// https://github.com/open-quantum-safe/liboqs

#ifndef BITCOIN_CRYPTO_MLDSA_H
#define BITCOIN_CRYPTO_MLDSA_H

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace mldsa {

//! Key and signature sizes for ML-DSA-44 (FIPS 204 parameter set I).
static constexpr size_t PUBKEY_SIZE    = 1312;
static constexpr size_t SECRETKEY_SIZE = 2560;
static constexpr size_t SIG_SIZE       = 2420;
static constexpr size_t SEED_SIZE      = 32;   // Deterministic keygen seed

/**
 * Deterministically derive an ML-DSA-44 keypair from a 32-byte seed.
 * The seed is domain-separated and expanded to the FIPS 204 keygen seed xi
 * (xi = SHA256("AVN/RIP-25/ML-DSA-44/keygen/v1" || seed)), then run through
 * ML-DSA.KeyGen_internal; see doc/rip-25.md. The mapping is stable and pinned
 * by a known-answer vector, so the same seed always yields the same keypair.
 * @param[out] pubkey  Output buffer, must be PUBKEY_SIZE bytes.
 * @param[out] seckey  Output buffer, must be SECRETKEY_SIZE bytes.
 * @param[in]  seed    32-byte deterministic seed.
 * @return true on success.
 */
bool KeyGenFromSeed(std::span<uint8_t, PUBKEY_SIZE> pubkey,
                    std::span<uint8_t, SECRETKEY_SIZE> seckey,
                    std::span<const uint8_t, SEED_SIZE> seed);

/**
 * Generate a random ML-DSA-44 keypair.
 * @param[out] pubkey  Output buffer, must be PUBKEY_SIZE bytes.
 * @param[out] seckey  Output buffer, must be SECRETKEY_SIZE bytes.
 * @return true on success.
 */
bool KeyGenRandom(std::span<uint8_t, PUBKEY_SIZE> pubkey,
                  std::span<uint8_t, SECRETKEY_SIZE> seckey);

/**
 * Sign a message with an ML-DSA-44 secret key.
 * @param[out] sig     Signature buffer, must be SIG_SIZE bytes.
 * @param[in]  msg     Message bytes.
 * @param[in]  seckey  Secret key, must be SECRETKEY_SIZE bytes.
 * @return true on success.
 */
bool Sign(std::span<uint8_t, SIG_SIZE> sig,
          std::span<const uint8_t> msg,
          std::span<const uint8_t, SECRETKEY_SIZE> seckey);

/**
 * Verify an ML-DSA-44 signature.
 * @param[in] sig     Signature, must be SIG_SIZE bytes.
 * @param[in] msg     Message bytes.
 * @param[in] pubkey  Public key, must be PUBKEY_SIZE bytes.
 * @return true if signature is valid.
 */
bool Verify(std::span<const uint8_t, SIG_SIZE> sig,
            std::span<const uint8_t> msg,
            std::span<const uint8_t, PUBKEY_SIZE> pubkey);

} // namespace mldsa

#endif // BITCOIN_CRYPTO_MLDSA_H
