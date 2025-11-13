#pragma once
#include <stdint.h>
#include <string>
#include <vector>

// Basic BIP32 key origin info used by PSBT fields
struct KeyOriginInfo {
    uint32_t fingerprint = 0;   // parent fingerprint
    std::vector<uint32_t> path; // derivation path

    bool operator==(const KeyOriginInfo& other) const
    {
        return fingerprint == other.fingerprint && path == other.path;
    }
};

inline std::string KeyOriginString(const KeyOriginInfo& info)
{
    std::string s;
    char buf[9];
    snprintf(buf, sizeof(buf), "%08x", info.fingerprint);
    s = buf;
    for (auto i : info.path) {
        s += "/";
        s += std::to_string(i);
        s += (i & 0x80000000U) ? "'" : "";
    }
    return s;
}
