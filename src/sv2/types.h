// Copyright (c) 2026 The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_SV2_TYPES_H
#define BITCOIN_SV2_TYPES_H

#include <serialize.h>
#include <span.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <ios>

//! Primitive data types of the Stratum V2 protocol (sv2-spec 3.1).
//!
//! Sv2 serializes multibyte integers little-endian, exactly as Bitcoin does, so
//! the plain READWRITE path is already correct for U8/U16/U32/U64 and for the
//! fixed-width byte types. Only three things need their own formatter: the
//! 24-bit width, which has no native type; the length-prefixed blobs, whose
//! prefix is a fixed-width integer rather than a CompactSize; and BOOL, of
//! which only the least significant bit carries meaning.
namespace sv2 {

//! U24: 24-bit unsigned, little-endian.
using U24 = CustomUintFormatter<3>;

//! Fixed-width byte types. std::array<uint8_t, N> already serializes as N raw
//! bytes, so these need no formatter.
using PubKey = std::array<uint8_t, 32>;    //!< PUBKEY: BIP340 x-only public key.
using Signature = std::array<uint8_t, 64>; //!< SIGNATURE: BIP340 signature.
using EllSwiftKey = std::array<uint8_t, 64>; //!< ELLSWIFT_PUBKEY: BIP324 encoding.
using Mac = std::array<uint8_t, 16>;       //!< MAC: AEAD authentication tag.

//! Largest block we allocate before requiring the peer to actually supply
//! bytes. Without this a three-byte length prefix would buy a 16 MB
//! allocation, so an attacker must send X MB to make us hold X + 1 MB.
static constexpr size_t MAX_ALLOC_CHUNK{1 << 20};

//! BOOL is one byte with seven padding high bits; receivers consider only the
//! least significant bit, so 0x02 is false rather than a parse error.
struct Bool {
    template <typename Stream>
    void Ser(Stream& s, bool v) { ser_writedata8(s, v ? 1 : 0); }

    template <typename Stream>
    void Unser(Stream& s, bool& v) { v = (ser_readdata8(s) & 1) != 0; }
};

//! Length-prefixed byte arrays and strings: an unsigned little-endian length of
//! PrefixBytes bytes, then that many bytes.
//!
//! MaxLen is separate from PrefixBytes because the two are not always implied
//! by each other: B0_32 carries a one-byte prefix but caps at 32.
template <int PrefixBytes, size_t MaxLen>
struct BlobFormatter {
    static_assert(PrefixBytes >= 1 && PrefixBytes <= 3, "Sv2 length prefixes are 1 to 3 bytes");
    static_assert(MaxLen <= (PrefixBytes == 1 ? 0xffU : (PrefixBytes == 2 ? 0xffffU : 0xffffffU)),
                  "MaxLen does not fit in PrefixBytes");

    template <typename Stream, typename V>
    void Ser(Stream& s, const V& v)
    {
        if (v.size() > MaxLen) throw std::ios_base::failure("sv2: value longer than its length prefix allows");
        CustomUintFormatter<PrefixBytes>{}.Ser(s, static_cast<uint64_t>(v.size()));
        if (!v.empty()) s.write(MakeByteSpan(v));
    }

    template <typename Stream, typename V>
    void Unser(Stream& s, V& v)
    {
        uint64_t len{0};
        CustomUintFormatter<PrefixBytes>{}.Unser(s, len);
        if (len > MaxLen) throw std::ios_base::failure("sv2: value longer than its length prefix allows");
        v.clear();
        while (v.size() < len) {
            const size_t have{v.size()};
            const size_t want{std::min<size_t>(len - have, MAX_ALLOC_CHUNK)};
            v.resize(have + want);
            s.read(MakeWritableByteSpan(std::span{v}.subspan(have, want)));
        }
    }
};

using Str0_255 = BlobFormatter<1, 0xff>;    //!< STR0_255
using B0_32 = BlobFormatter<1, 32>;         //!< B0_32
using B0_255 = BlobFormatter<1, 0xff>;      //!< B0_255
using B0_64K = BlobFormatter<2, 0xffff>;    //!< B0_64K
using B0_16M = BlobFormatter<3, 0xffffff>;  //!< B0_16M

//! Sequences: an unsigned little-endian element count of PrefixBytes bytes,
//! then that many elements written with ElemFormatter.
//!
//! The count is bounded by the prefix width, so unlike the blob case there is
//! no need to batch allocation: at most 65535 elements can be claimed.
template <int PrefixBytes, class ElemFormatter = DefaultFormatter>
struct SeqFormatter {
    static_assert(PrefixBytes == 1 || PrefixBytes == 2, "Sv2 sequence counts are 1 or 2 bytes");

    template <typename Stream, typename V>
    void Ser(Stream& s, const V& v)
    {
        constexpr uint64_t max_count{PrefixBytes == 1 ? 0xffU : 0xffffU};
        if (v.size() > max_count) throw std::ios_base::failure("sv2: more elements than the count prefix allows");
        CustomUintFormatter<PrefixBytes>{}.Ser(s, static_cast<uint64_t>(v.size()));
        ElemFormatter formatter;
        for (const auto& elem : v) formatter.Ser(s, elem);
    }

    template <typename Stream, typename V>
    void Unser(Stream& s, V& v)
    {
        uint64_t count{0};
        CustomUintFormatter<PrefixBytes>{}.Unser(s, count);
        ElemFormatter formatter;
        v.clear();
        v.reserve(count);
        while (v.size() < count) {
            v.emplace_back();
            formatter.Unser(s, v.back());
        }
    }
};

template <class ElemFormatter = DefaultFormatter> using Seq0_255 = SeqFormatter<1, ElemFormatter>;
template <class ElemFormatter = DefaultFormatter> using Seq0_64K = SeqFormatter<2, ElemFormatter>;

} // namespace sv2

#endif // BITCOIN_SV2_TYPES_H
