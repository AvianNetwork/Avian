// Copyright (c) 2022 The Avian Core developers
// Copyright (c) 2022 Shafil Alam
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_ASSETS_ANS_H
#define BITCOIN_ASSETS_ANS_H

#include <string>
#include <array>
#include <cstdint>

/* Decoded fields from a PROFILE (type 0x2) CBOR record */
struct ANSProfileData {
    std::string addr;           // key 0: payment address (overrides owner-token fallback)
    std::string name;           // key 1: human-readable display name
    std::string avatar;         // key 2: tstr = URL/CID, bstr = inline image bytes
    bool        avatar_binary;  // true if avatar is raw binary (bstr), false if URL/CID (tstr)
    std::string url;            // key 3: website URL
    std::string banner;         // key 4: tstr = URL/CID, bstr = inline image bytes
    bool        banner_binary;  // true if banner is raw binary (bstr), false if URL/CID (tstr)
    ANSProfileData() : avatar_binary(false), banner_binary(false) {}
};

/* Class for ANS (Avian Name System) ID */
class CAvianNameSystemID {
public:
    static const std::string prefix;
    static const std::string domain;

    enum Type {
        ADDR    = 0x0,
        XADDR   = 0x1,  // AIP-0010: external/cross-chain address (not Avian-specific)
        PROFILE = 0x2,  // AIP-0009 §3.3: compact CBOR identity record
        // Types 0x3-0xf reserved for future AIPs
    };

    CAvianNameSystemID(Type type, std::string rawData);
    CAvianNameSystemID(std::string ansID);

    std::string to_string();

    Type type() { return m_type; };
    std::string addr() { return m_addr; };
    const std::string& cbor_payload() const { return m_cbor_payload; };
    const ANSProfileData& profile() const { return m_profile; };

    static bool IsValidID(std::string ansID);

    static bool CheckTypeData(Type type, std::string typeData);
    static std::string FormatTypeData(Type type, std::string typeData, std::string& error);

    static std::pair<std::string, std::string> enum_to_string(Type type) {
        switch(type) {
            case ADDR:
                return std::make_pair("Avian address", "Enter an Avian address");
            case XADDR:
                return std::make_pair("External address", "Enter the address for this chain");
            case PROFILE:
                return std::make_pair("CBOR profile", "Enter hex-encoded CBOR map bytes");
            default:
                return std::make_pair("Invalid", "Invalid");
        }
    }

private:
    Type m_type;
    std::string m_addr;
    std::string m_cbor_payload;  // raw CBOR bytes for PROFILE records (type 0x2)
    ANSProfileData m_profile;    // decoded fields from a PROFILE record
};

// Types available for ROOT .AVN identity assets
constexpr std::array<CAvianNameSystemID::Type, 2> ANSTypes {
    CAvianNameSystemID::ADDR,
    CAvianNameSystemID::PROFILE
};

// Types available for sub-assets of .AVN names (AIP-0010 multi-chain records)
constexpr std::array<CAvianNameSystemID::Type, 1> ANSSubTypes {
    CAvianNameSystemID::XADDR
};

#endif // BITCOIN_ASSETS_ANS_H
