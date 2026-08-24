// Copyright (c) 2026 The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_ASSETS_CBOR_H
#define BITCOIN_ASSETS_CBOR_H

// Minimal CBOR encoder/decoder for ANS PROFILE records (AIP-0009 §3.3).
//
// Only the subset of RFC 8949 required by the PROFILE record type is
// implemented:
//   • Definite-length maps   (major type 5)
//   • Unsigned integer keys  (major type 0)
//   • Text string values     (major type 3)  — tstr
//   • Byte string values     (major type 2)  — bstr
//   • Length arguments ≤ 2 bytes (additional-info ≤ 25)
//
// Indefinite-length encodings, tagged values, and non-integer keys are
// rejected by the decoder and never produced by the encoder.
//
// Usage (encoder):
//   ANSProfileData p;
//   p.name = "Satoshi"; p.addr = "RHwoNwr7...";
//   std::string cbor = ANS_CBOR::EncodeProfile(p);
//   std::string ansID = "ANS2" + cbor;           // ready for strANSID
//
// Usage (decoder):
//   ANSProfileData p;
//   std::string err;
//   if (!ANS_CBOR::DecodeProfile(raw_cbor_bytes, p, err)) { /* invalid */ }

#include <util/string.h>
#include <assets/ans.h>

#include <string>
#include <cstdint>

namespace ANS_CBOR {

// ---------------------------------------------------------------------------
// Encoder
// ---------------------------------------------------------------------------

namespace detail {

// Write a CBOR head byte + argument (major type × 32 + additional-info).
inline void write_head(std::string& out, uint8_t major, uint64_t val)
{
    const uint8_t base = uint8_t(major << 5);
    if (val <= 23) {
        out += char(base | uint8_t(val));
    } else if (val <= 0xffu) {
        out += char(base | 24u);
        out += char(uint8_t(val));
    } else if (val <= 0xffffu) {
        out += char(base | 25u);
        out += char(uint8_t(val >> 8));
        out += char(uint8_t(val));
    } else {
        // 4-byte argument — not needed for PROFILE payloads ≤ 468 bytes,
        // but included for completeness.
        out += char(base | 26u);
        out += char(uint8_t(val >> 24));
        out += char(uint8_t(val >> 16));
        out += char(uint8_t(val >>  8));
        out += char(uint8_t(val));
    }
}

inline void encode_uint(std::string& out, uint64_t key)           { write_head(out, 0, key); }
inline void encode_tstr(std::string& out, const std::string& s)   { write_head(out, 3, s.size()); out += s; }
inline void encode_bstr(std::string& out, const std::string& s)   { write_head(out, 2, s.size()); out += s; }

} // namespace detail

// Encode an ANSProfileData into raw CBOR bytes.
// Only non-empty fields are included; keys are written in ascending order
// (canonical form per RFC 8949 §4.2.1).
// Returns an empty string if no fields are set (caller should validate before use).
inline std::string EncodeProfile(const ANSProfileData& p)
{
    int n = 0;
    if (!p.addr.empty())   ++n;
    if (!p.name.empty())   ++n;
    if (!p.avatar.empty()) ++n;
    if (!p.url.empty())    ++n;
    if (!p.banner.empty()) ++n;
    if (n == 0) return {};

    std::string out;
    out.reserve(n * 8 + p.addr.size() + p.name.size() + p.avatar.size() + p.url.size() + p.banner.size());

    detail::write_head(out, 5, uint64_t(n)); // map header

    if (!p.addr.empty()) {
        detail::encode_uint(out, 0);
        detail::encode_tstr(out, p.addr);
    }
    if (!p.name.empty()) {
        detail::encode_uint(out, 1);
        detail::encode_tstr(out, p.name);
    }
    if (!p.avatar.empty()) {
        detail::encode_uint(out, 2);
        if (p.avatar_binary) detail::encode_bstr(out, p.avatar);
        else                 detail::encode_tstr(out, p.avatar);
    }
    if (!p.url.empty()) {
        detail::encode_uint(out, 3);
        detail::encode_tstr(out, p.url);
    }
    if (!p.banner.empty()) {
        detail::encode_uint(out, 4);
        if (p.banner_binary) detail::encode_bstr(out, p.banner);
        else                 detail::encode_tstr(out, p.banner);
    }

    return out;
}

// ---------------------------------------------------------------------------
// Decoder
// ---------------------------------------------------------------------------

// Decode raw CBOR bytes into ANSProfileData fields.
// Returns true on success, false on any encoding violation (sets error).
// Unknown integer keys are silently skipped (forward compatibility).
inline bool DecodeProfile(const std::string& payload, ANSProfileData& out, std::string& error)
{
    const uint8_t* d   = reinterpret_cast<const uint8_t*>(payload.data());
    const size_t   len = payload.size();
    size_t pos = 0;

    // Helper: read a CBOR argument from additional-info field.
    auto read_arg = [&](uint8_t info, uint64_t& val) -> bool {
        if (info <= 23) { val = info; return true; }
        if (info == 24) {
            if (pos >= len) { error = "truncated"; return false; }
            val = d[pos++]; return true;
        }
        if (info == 25) {
            if (pos + 1 >= len) { error = "truncated"; return false; }
            val = (uint64_t(d[pos]) << 8) | d[pos + 1];
            pos += 2; return true;
        }
        error = "unsupported CBOR additional info " + util::ToString(info);
        return false;
    };

    // Top-level item must be a definite-length map.
    if (pos >= len) { error = "empty payload"; return false; }
    uint8_t head  = d[pos++];
    uint8_t major = head >> 5;
    uint8_t minfo = head & 0x1fu;
    if (major != 5)  { error = "expected CBOR map (major type 5)"; return false; }
    if (minfo == 31) { error = "indefinite-length encoding not allowed"; return false; }

    uint64_t count;
    if (!read_arg(minfo, count)) return false;
    if (count == 0) { error = "CBOR map must have at least one entry"; return false; }

    for (uint64_t i = 0; i < count; ++i) {
        // Key: unsigned integer (major type 0).
        if (pos >= len) { error = "truncated at key"; return false; }
        uint8_t kh   = d[pos++];
        uint8_t kmaj = kh >> 5;
        uint8_t kinf = kh & 0x1fu;
        if (kmaj != 0)   { error = "non-integer map key"; return false; }
        if (kinf == 31)  { error = "indefinite-length key not allowed"; return false; }
        uint64_t key;
        if (!read_arg(kinf, key)) return false;

        // Value: tstr (major 3) or bstr (major 2).
        if (pos >= len) { error = "truncated at value"; return false; }
        uint8_t vh   = d[pos++];
        uint8_t vmaj = vh >> 5;
        uint8_t vinf = vh & 0x1fu;
        if (vmaj != 2 && vmaj != 3) { error = "unsupported value major type " + util::ToString(vmaj); return false; }
        if (vinf == 31) { error = "indefinite-length string not allowed"; return false; }
        uint64_t vlen;
        if (!read_arg(vinf, vlen)) return false;
        if (pos + vlen > len) { error = "value out of bounds"; return false; }

        std::string val(reinterpret_cast<const char*>(d + pos), vlen);
        pos += vlen;

        switch (key) {
            case 0: out.addr   = val; break;
            case 1: out.name   = val; break;
            case 2: out.avatar = val; out.avatar_binary = (vmaj == 2); break;
            case 3: out.url    = val; break;
            case 4: out.banner = val; out.banner_binary = (vmaj == 2); break;
            default: break; // unknown keys silently skipped
        }
    }

    if (pos != len) { error = "trailing bytes in CBOR payload"; return false; }
    return true;
}

} // namespace ANS_CBOR

#endif // BITCOIN_ASSETS_CBOR_H
