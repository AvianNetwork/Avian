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
#include <cstring>

namespace mldsa {

#ifdef HAVE_LIBOQS

static_assert(PUBKEY_SIZE == 1312 && SECRETKEY_SIZE == 2560 && SEED_SIZE == 32,
              "RIP-25 keygen assumes ML-DSA-44 (FIPS 204 parameter set I) sizes");

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

// ---- Seeded FIPS-204 key generation ---------------------------------------
// liboqs 0.12.0 exposes no OQS_SIG_ml_dsa_44_keypair_derand, and the vendored
// pqcrystals reference implementation has no public keypair_internal entry
// point.  The previous approach hijacked the process-global liboqs RNG with a
// SHA256 counter-mode KDF: it worked only because keypair() happens to draw
// exactly 32 bytes in a single randombytes() call, it needed a mutex around a
// global, and any change to that call order in a future liboqs would have
// silently produced different (unrecoverable) keys.
//
// Instead we reproduce pqcrystals crypto_sign_keypair (sign.c) verbatim,
// replacing only its randombytes() draw with a seed we control, by calling the
// reference implementation's own exported lattice primitives.  The output is
// therefore byte-for-byte FIPS-204 ML-DSA.KeyGen_internal(xi) with no global
// state, no mutex, and no dependence on liboqs's RNG call order.  A change in
// liboqs's internal ABI would break the build (link/type error) or be caught
// by the RIP-25 known-answer vector in pqkey_tests.cpp, never silently alter a
// derived key.
//
// The symbol names below are exactly those exported by liboqs.a
// (verified with nm: pqcrystals_ml_dsa_44_ref_*).

extern "C" {
    // Reference-implementation data types (params.h / poly.h / polyvec.h).
    // ML-DSA-44: N=256, K=4, L=4.  Layouts copied verbatim; the static_assert
    // above plus the KAT guard against any drift.
    typedef struct { int32_t coeffs[256]; } ref_poly;
    typedef struct { ref_poly vec[4]; }     ref_polyvecl;
    typedef struct { ref_poly vec[4]; }     ref_polyveck;

    void pqcrystals_ml_dsa_44_ref_polyvec_matrix_expand(ref_polyvecl mat[4], const uint8_t rho[32]);
    void pqcrystals_ml_dsa_44_ref_polyvecl_uniform_eta(ref_polyvecl* v, const uint8_t seed[64], uint16_t nonce);
    void pqcrystals_ml_dsa_44_ref_polyveck_uniform_eta(ref_polyveck* v, const uint8_t seed[64], uint16_t nonce);
    void pqcrystals_ml_dsa_44_ref_polyvecl_ntt(ref_polyvecl* v);
    void pqcrystals_ml_dsa_44_ref_polyvec_matrix_pointwise_montgomery(ref_polyveck* t, const ref_polyvecl mat[4], const ref_polyvecl* v);
    void pqcrystals_ml_dsa_44_ref_polyveck_reduce(ref_polyveck* v);
    void pqcrystals_ml_dsa_44_ref_polyveck_invntt_tomont(ref_polyveck* v);
    void pqcrystals_ml_dsa_44_ref_polyveck_add(ref_polyveck* w, const ref_polyveck* u, const ref_polyveck* v);
    void pqcrystals_ml_dsa_44_ref_polyveck_caddq(ref_polyveck* v);
    void pqcrystals_ml_dsa_44_ref_polyveck_power2round(ref_polyveck* v1, ref_polyveck* v0, const ref_polyveck* v);
    void pqcrystals_ml_dsa_44_ref_pack_pk(uint8_t pk[1312], const uint8_t rho[32], const ref_polyveck* t1);
    void pqcrystals_ml_dsa_44_ref_pack_sk(uint8_t sk[2560], const uint8_t rho[32], const uint8_t tr[64],
                                          const uint8_t key[32], const ref_polyveck* t0,
                                          const ref_polyvecl* s1, const ref_polyveck* s2);

    // Top-level liboqs SHAKE256 (FIPS 202 XOF).  Exported by liboqs.a but not
    // present in the installed public headers, so declared here.
    void OQS_SHA3_shake256(uint8_t* output, size_t outlen, const uint8_t* input, size_t inlen);
}

} // namespace

bool KeyGenFromSeed(std::span<uint8_t, PUBKEY_SIZE> pubkey,
                    std::span<uint8_t, SECRETKEY_SIZE> seckey,
                    std::span<const uint8_t, SEED_SIZE> seed)
{
    // RIP-25 key derivation (normative; see doc/rip-25.md):
    //   xi        = SHA256( DST || seed )                    32-byte ML-DSA seed
    //   (pk, sk)  = ML-DSA.KeyGen_internal(xi)               FIPS 204 Alg. 6
    // The versioned domain-separation tag makes the derivation an explicit,
    // written algorithm rather than a side effect of liboqs internals, and
    // lets any future change to it be unambiguous (…/v2).
    static constexpr char DST[] = "AVN/RIP-25/ML-DSA-44/keygen/v1";
    uint8_t xi[SEED_SIZE];
    CSHA256()
        .Write(reinterpret_cast<const uint8_t*>(DST), sizeof(DST) - 1)
        .Write(seed.data(), seed.size())
        .Finalize(xi);

    // Expand xi into (rho, rhoprime, key) exactly as pqcrystals
    // crypto_sign_keypair: SHAKE256( xi || K || L ) -> 128 bytes.
    uint8_t exp_in[SEED_SIZE + 2];
    std::memcpy(exp_in, xi, SEED_SIZE);
    exp_in[SEED_SIZE + 0] = 4;  // K
    exp_in[SEED_SIZE + 1] = 4;  // L
    uint8_t exp_out[2 * SEED_SIZE + 64];  // rho(32) || rhoprime(64) || key(32)
    OQS_SHA3_shake256(exp_out, sizeof(exp_out), exp_in, sizeof(exp_in));
    const uint8_t* rho      = exp_out;
    const uint8_t* rhoprime = exp_out + SEED_SIZE;
    const uint8_t* key      = exp_out + SEED_SIZE + 64;

    ref_polyvecl mat[4];
    ref_polyvecl s1, s1hat;
    ref_polyveck s2, t1, t0;

    // Expand matrix A from rho.
    pqcrystals_ml_dsa_44_ref_polyvec_matrix_expand(mat, rho);

    // Sample short vectors s1, s2 from rhoprime (nonces 0..L-1 for s1, L.. for s2).
    pqcrystals_ml_dsa_44_ref_polyvecl_uniform_eta(&s1, rhoprime, 0);
    pqcrystals_ml_dsa_44_ref_polyveck_uniform_eta(&s2, rhoprime, 4);  // nonce = L

    // t = A*s1 + s2.
    s1hat = s1;
    pqcrystals_ml_dsa_44_ref_polyvecl_ntt(&s1hat);
    pqcrystals_ml_dsa_44_ref_polyvec_matrix_pointwise_montgomery(&t1, mat, &s1hat);
    pqcrystals_ml_dsa_44_ref_polyveck_reduce(&t1);
    pqcrystals_ml_dsa_44_ref_polyveck_invntt_tomont(&t1);
    pqcrystals_ml_dsa_44_ref_polyveck_add(&t1, &t1, &s2);

    // Split t into (t1, t0) and pack the public key.
    pqcrystals_ml_dsa_44_ref_polyveck_caddq(&t1);
    pqcrystals_ml_dsa_44_ref_polyveck_power2round(&t1, &t0, &t1);
    pqcrystals_ml_dsa_44_ref_pack_pk(pubkey.data(), rho, &t1);

    // tr = SHAKE256(pk); pack the secret key.
    uint8_t tr[64];
    OQS_SHA3_shake256(tr, sizeof(tr), pubkey.data(), PUBKEY_SIZE);
    pqcrystals_ml_dsa_44_ref_pack_sk(seckey.data(), rho, tr, key, &t0, &s1, &s2);

    // Wipe secret intermediates.
    memory_cleanse(xi, sizeof(xi));
    memory_cleanse(exp_in, sizeof(exp_in));
    memory_cleanse(exp_out, sizeof(exp_out));
    memory_cleanse(&s1, sizeof(s1));
    memory_cleanse(&s1hat, sizeof(s1hat));
    memory_cleanse(&s2, sizeof(s2));
    memory_cleanse(&t0, sizeof(t0));
    memory_cleanse(tr, sizeof(tr));
    return true;
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
