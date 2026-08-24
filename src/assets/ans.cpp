// Copyright (c) 2022 The Avian Core developers
// Copyright (c) 2022 Shafil Alam
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <assets/ans.h>
#include <assets/cbor.h>

#include <string>
#include <sstream>
#include <cstdint>

#include <key_io.h>
#include <util/strencodings.h>

/* Static prefix */
const std::string CAvianNameSystemID::prefix = "ANS";

/* Static domain */
const std::string CAvianNameSystemID::domain = ".AVN";

// TODO: Add error result?
bool CAvianNameSystemID::CheckTypeData(Type type, std::string typeData) {
    if (type == Type::ADDR) {
        CTxDestination destination = DecodeDestination(typeData);
        if (!IsValidDestination(destination)) return false;
    } else if (type == Type::XADDR) {
        // AIP-0010: external address — any non-empty string, max 468 bytes
        if (typeData.empty() || typeData.size() > 468) return false;
    } else if (type == Type::PROFILE) {
        // typeData is raw CBOR bytes; max 468 bytes (active after DEPLOYMENT_ANS_V2).
        if (typeData.size() > 468) return false;
        ANSProfileData profile;
        std::string err;
        if (!ANS_CBOR::DecodeProfile(typeData, profile, err)) return false;
    } else {
        // Unknown/unsupported type
        return false;
    }
    return true;
}

std::string CAvianNameSystemID::FormatTypeData(Type type, std::string typeData, std::string& error)
{
    std::string returnStr = typeData;

    // Check and set type data
    if (type == ADDR) {
        CTxDestination destination = DecodeDestination(typeData);
        if (!IsValidDestination(destination)) {
            error = (typeData != "")
            ? std::string("Invalid Avian address: ") + typeData
            : std::string("Empty Avian address.");
        }
    } else if (type == XADDR) {
        // AIP-0010: external address — only validate non-empty
        if (typeData.empty())
            error = "Empty external address.";
    } else if (type == PROFILE) {
        ANSProfileData profile;
        if (!ANS_CBOR::DecodeProfile(typeData, profile, error)) {
            if (error.empty()) error = "Invalid CBOR payload for PROFILE record.";
        }
    }

    return returnStr;
}

bool CAvianNameSystemID::IsValidID(std::string ansID) {
    // Check for min length
    if(ansID.length() <= prefix.size() + 1) return false;

    // Check for prefix and maximum size (940 bytes: "ANS" + 1 type nibble + 936 hex chars for 468-byte CBOR)
    bool hasPrefix = (ansID.substr(0, CAvianNameSystemID::prefix.length()) == CAvianNameSystemID::prefix) && (ansID.size() <= 940);
    if (!hasPrefix) return false;

    // Must be valid hex char (locale-independent ASCII check; std::isxdigit is locale-dependent)
    std::string hexStr = ansID.substr(prefix.length(), 1);
    const char hc = hexStr[0];
    if (!((hc >= '0' && hc <= '9') || (hc >= 'a' && hc <= 'f') || (hc >= 'A' && hc <= 'F'))) return false;

    // Hex value must be less than 0xf
    int hexInt = HexDigit(hexStr[0]);
    if (hexInt > 0xf) return false;

    // Check type
    Type type = static_cast<Type>(hexInt);
    std::string rawData = ansID.substr(prefix.length() + 1);

    // For PROFILE the ANS ID carries hex-encoded CBOR; decode to raw bytes before validation.
    std::string checkData = rawData;
    if (type == Type::PROFILE) {
        if (!IsHex(rawData)) return false;
        auto bytes = ParseHex(rawData);
        checkData = std::string(bytes.begin(), bytes.end());
    }

    if (!CheckTypeData(type, checkData)) return false;

    return true;
}

CAvianNameSystemID::CAvianNameSystemID(Type type, std::string rawData) :
    m_addr("")
{
    this->m_type = type;

    if (!CheckTypeData(this->m_type, rawData)) return;

    if (this->m_type == Type::ADDR || this->m_type == Type::XADDR) {
        this->m_addr = rawData;
    } else if (this->m_type == Type::PROFILE) {
        this->m_cbor_payload = rawData;
        std::string err;
        ANS_CBOR::DecodeProfile(rawData, this->m_profile, err);
    }
}

CAvianNameSystemID::CAvianNameSystemID(std::string ansID) :
    m_addr("")
{
    // Check if valid ID
    if(!IsValidID(ansID)) return;

    // Get type
    Type type = static_cast<Type>(HexDigit(ansID[prefix.length()]));
    this->m_type = type;

    // Set info based on data
    std::string data = ansID.substr(prefix.length() + 1); // prefix + type nibble

    if (this->m_type == Type::ADDR || this->m_type == Type::XADDR) {
        this->m_addr = data;
    } else if (this->m_type == Type::PROFILE) {
        // data is hex CBOR; decode to raw bytes for internal storage and parsing.
        auto rawBytes = ParseHex(data);
        this->m_cbor_payload = std::string(rawBytes.begin(), rawBytes.end());
        std::string err;
        ANS_CBOR::DecodeProfile(this->m_cbor_payload, this->m_profile, err);
    }
}

std::string CAvianNameSystemID::to_string() {
    std::string id = "";

    // 1. Add prefix
    id += prefix;

    // 2. Add type
    std::stringstream ss;
    ss << std::hex << this->m_type;
    id += ss.str();

    // 3. Add data
    if (this->m_type == Type::ADDR || this->m_type == Type::XADDR) {
        id += m_addr;
    } else if (this->m_type == Type::PROFILE) {
        id += HexStr(m_cbor_payload); // hex-encode raw CBOR bytes per AIP-0009
    }

    return id;
}
