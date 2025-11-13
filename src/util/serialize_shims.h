#pragma once
#include <pubkey.h>
#include <serialize.h>
#include <span.h>

// Minimal stand-in for Bitcoin v24 helper
struct CompactSizeWriter {
    uint64_t m_type;
    explicit CompactSizeWriter(uint64_t t) : m_type(t) {}
};

// Generic helper: write compactsize + raw bytes
template <typename Stream>
inline void SerializeToVector(Stream& s, const CompactSizeWriter& type, Span<const unsigned char> data)
{
    WriteCompactSize(s, type.m_type);
    s.write((char*)data.data(), data.size());
}

// Overload for CPubKey (avoids needing Span CTAD from CPubKey)
template <typename Stream>
inline void SerializeToVector(Stream& s, const CompactSizeWriter& type, const CPubKey& pk)
{
    WriteCompactSize(s, type.m_type);
    s.write((char*)pk.begin(), pk.size());
}
