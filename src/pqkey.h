// Copyright (c) 2026 ALENOC <https://github.com/ALENOC> (Ravencoin RIP-25)
// Copyright (c) 2024-present The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

// RIP-25: Post-quantum key types for ML-DSA-44 (FIPS 204).
// Ported from Ravencoin RIP-25 <https://github.com/RavenProject/Ravencoin/pull/1281>
// CPQPubKey wraps the 1312-byte ML-DSA-44 public key.
// CPQKey wraps the 2560-byte ML-DSA-44 secret key with secure storage.

#ifndef BITCOIN_PQKEY_H
#define BITCOIN_PQKEY_H

#include <crypto/mldsa.h>
#include <support/allocators/secure.h>
#include <uint256.h>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

/** An ML-DSA-44 public key (1312 bytes). */
class CPQPubKey
{
public:
    static constexpr size_t SIZE = mldsa::PUBKEY_SIZE;

private:
    std::array<uint8_t, SIZE> m_data{};
    bool m_valid{false};

public:
    CPQPubKey() = default;

    /** Construct from a raw 1312-byte buffer. */
    explicit CPQPubKey(std::span<const uint8_t, SIZE> data)
        : m_valid{true}
    {
        std::copy(data.begin(), data.end(), m_data.begin());
    }

    bool IsValid() const { return m_valid; }

    /** Return the raw public key bytes. */
    std::span<const uint8_t, SIZE> GetData() const
    {
        return std::span<const uint8_t, SIZE>{m_data};
    }

    /**
     * Return the 32-byte witness program for this key.
     * The witness program is SHA256(pubkey), matching the scriptPubKey output:
     *   OP_2 <32-byte-hash>
     */
    uint256 GetWitnessProgram() const;

    /**
     * Verify an ML-DSA-44 signature over msg.
     * @param sig  Must be exactly mldsa::SIG_SIZE bytes.
     * @param msg  Message that was signed.
     * @param ctx  FIPS 204 domain-separation context; must match signing.
     *             Consensus spends pass GetMLDsa44DomainContext(); the default
     *             empty context is for standalone crypto round-trips only.
     */
    bool Verify(std::span<const uint8_t> sig,
                std::span<const uint8_t> msg,
                std::span<const uint8_t> ctx = {}) const;

    bool operator==(const CPQPubKey& other) const
    {
        return m_valid == other.m_valid && m_data == other.m_data;
    }
    bool operator!=(const CPQPubKey& other) const { return !(*this == other); }

    /** Serialization — store/load raw bytes. */
    template <typename Stream>
    void Serialize(Stream& s) const
    {
        s.write(MakeByteSpan(m_data));
    }
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        s.read(MakeWritableByteSpan(m_data));
        m_valid = true;
    }
};

/** An ML-DSA-44 secret key (2560 bytes, secured memory). */
class CPQKey
{
public:
    static constexpr size_t SIZE = mldsa::SECRETKEY_SIZE;

    using KeyData = std::vector<uint8_t, secure_allocator<uint8_t>>;

private:
    KeyData m_keydata;
    CPQPubKey m_pubkey;

public:
    CPQKey() = default;
    ~CPQKey() { Clear(); }

    CPQKey(const CPQKey&) = delete;
    CPQKey& operator=(const CPQKey&) = delete;

    CPQKey(CPQKey&& other) noexcept
        : m_keydata(std::move(other.m_keydata))
        , m_pubkey(std::move(other.m_pubkey))
    {
        other.Clear();
    }
    CPQKey& operator=(CPQKey&& other) noexcept
    {
        if (this != &other) {
            m_keydata = std::move(other.m_keydata);
            m_pubkey  = std::move(other.m_pubkey);
            other.Clear();
        }
        return *this;
    }

    bool IsValid() const { return m_keydata.size() == SIZE; }

    void Clear()
    {
        memory_cleanse(m_keydata.data(), m_keydata.size());
        m_keydata.clear();
        m_pubkey = CPQPubKey{};
    }

    /**
     * Generate a new random ML-DSA-44 keypair.
     * @return true on success.
     */
    bool MakeNewKey();

    /**
     * Generate a deterministic ML-DSA-44 keypair from a 32-byte seed.
     * The same seed always produces the same keypair (when liboqs supports it).
     * @param seed  32-byte deterministic seed.
     * @return true on success.
     */
    bool SetSeed(std::span<const uint8_t, mldsa::SEED_SIZE> seed);

    /**
     * Load a secret key from raw bytes (e.g., from wallet DB).
     * @param data  Must be exactly SIZE bytes.
     * @param pubkey  Corresponding public key.
     * @return true on success.
     */
    bool SetKeyData(std::span<const uint8_t, SIZE> data,
                    const CPQPubKey& pubkey);

    const CPQPubKey& GetPubKey() const { return m_pubkey; }

    /**
     * Sign a message with this key.
     * @param sig_out  Receives the signature; resized to mldsa::SIG_SIZE.
     * @param msg      Message to sign.
     * @param ctx      FIPS 204 domain-separation context; must match verify.
     *                 Consensus spends pass GetMLDsa44DomainContext(); the
     *                 default empty context is for standalone crypto only.
     * @return true on success.
     */
    bool Sign(std::vector<uint8_t>& sig_out,
              std::span<const uint8_t> msg,
              std::span<const uint8_t> ctx = {}) const;

    /** Return a read-only view of the raw secret key bytes. */
    std::span<const uint8_t, SIZE> GetData() const
    {
        return std::span<const uint8_t, SIZE>{m_keydata.data(), SIZE};
    }
};

#endif // BITCOIN_PQKEY_H
