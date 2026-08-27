// Copyright (c) 2026 ALENOC <https://github.com/ALENOC> (Ravencoin RIP-25)
// Copyright (c) 2024-present The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

// RIP-25: Post-quantum key implementation for Avian 5.x.
// Ported from Ravencoin RIP-25 <https://github.com/RavenProject/Ravencoin/pull/1281>

#include <pqkey.h>

#include <crypto/sha256.h>
#include <support/cleanse.h>

#include <algorithm>
#include <cassert>

// ─── CPQPubKey ────────────────────────────────────────────────────────────────

uint256 CPQPubKey::GetWitnessProgram() const
{
    // witness_program = SHA256(mldsa_pubkey)
    uint256 hash;
    CSHA256().Write(m_data.data(), m_data.size()).Finalize(hash.begin());
    return hash;
}

bool CPQPubKey::Verify(std::span<const uint8_t> sig,
                       std::span<const uint8_t> msg,
                       std::span<const uint8_t> ctx) const
{
    if (!m_valid) return false;
    if (sig.size() != mldsa::SIG_SIZE) return false;
    return mldsa::Verify(
        std::span<const uint8_t, mldsa::SIG_SIZE>(sig.data(), mldsa::SIG_SIZE),
        msg,
        ctx,
        GetData());
}

// ─── CPQKey ───────────────────────────────────────────────────────────────────

bool CPQKey::MakeNewKey()
{
    std::array<uint8_t, CPQPubKey::SIZE> pk_buf;
    m_keydata.resize(SIZE);

    if (!mldsa::KeyGenRandom(
            std::span<uint8_t, CPQPubKey::SIZE>(pk_buf),
            std::span<uint8_t, SIZE>(m_keydata.data(), SIZE))) {
        Clear();
        return false;
    }
    m_pubkey = CPQPubKey(std::span<const uint8_t, CPQPubKey::SIZE>(pk_buf));
    return true;
}

bool CPQKey::SetSeed(std::span<const uint8_t, mldsa::SEED_SIZE> seed)
{
    std::array<uint8_t, CPQPubKey::SIZE> pk_buf;
    m_keydata.resize(SIZE);

    if (!mldsa::KeyGenFromSeed(
            std::span<uint8_t, CPQPubKey::SIZE>(pk_buf),
            std::span<uint8_t, SIZE>(m_keydata.data(), SIZE),
            seed)) {
        Clear();
        return false;
    }
    m_pubkey = CPQPubKey(std::span<const uint8_t, CPQPubKey::SIZE>(pk_buf));
    return true;
}

bool CPQKey::SetKeyData(std::span<const uint8_t, SIZE> data,
                        const CPQPubKey& pubkey)
{
    if (!pubkey.IsValid()) return false;
    m_keydata.assign(data.begin(), data.end());
    m_pubkey = pubkey;
    return true;
}

bool CPQKey::Sign(std::vector<uint8_t>& sig_out,
                  std::span<const uint8_t> msg,
                  std::span<const uint8_t> ctx) const
{
    if (!IsValid()) return false;
    sig_out.resize(mldsa::SIG_SIZE);
    return mldsa::Sign(
        std::span<uint8_t, mldsa::SIG_SIZE>(sig_out.data(), mldsa::SIG_SIZE),
        msg,
        ctx,
        GetData());
}
