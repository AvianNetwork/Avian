// Copyright (c) 2022 The Avian Core developers
// Copyright (c) 2022 Shafil Alam
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <assets/ans.h>

#include <string>
#include <sstream>
#include <cstring>
#include <cstdint>

#include <util/strencodings.h>
#include <key_io.h>

// IsHexNumber was removed in BTC 30.2. Define a local version.
static bool IsHexNumber(const std::string& str) {
    if (str.empty()) return false;
    size_t start = 0;
    if (str.size() > 2 && str[0] == '0' && str[1] == 'x') start = 2;
    for (size_t i = start; i < str.size(); i++) {
        if (!((str[i] >= '0' && str[i] <= '9') ||
              (str[i] >= 'a' && str[i] <= 'f') ||
              (str[i] >= 'A' && str[i] <= 'F')))
            return false;
    }
    return true;
}

/* Static prefix */
const std::string CAvianNameSystemID::prefix = "ANS";

/* Static domain */
const std::string CAvianNameSystemID::domain = ".AVN";

static std::string IPToHex(std::string strIP)
{
    // Parse IPv4 address manually (replaces boost::asio::ip::address_v4)
    uint32_t parts[4];
    if (sscanf(strIP.c_str(), "%u.%u.%u.%u", &parts[0], &parts[1], &parts[2], &parts[3]) != 4)
        return "0";
    for (int i = 0; i < 4; i++) {
        if (parts[i] > 255) return "0";
    }
    uint32_t ip = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
    std::stringstream ss;
    ss << std::hex << ip;
    return ss.str();
}

static std::string HexToIP(std::string hexIP)
{
    if(!IsHexNumber(hexIP)) return "0.0.0.0";

    unsigned int hex = std::stoul(hexIP, 0, 16);
    uint8_t a = (hex >> 24) & 0xFF;
    uint8_t b = (hex >> 16) & 0xFF;
    uint8_t c = (hex >> 8) & 0xFF;
    uint8_t d = hex & 0xFF;
    return std::to_string(a) + "." + std::to_string(b) + "." + std::to_string(c) + "." + std::to_string(d);
}

bool CAvianNameSystemID::CheckIP(std::string rawip, bool isHex) {
    std::string ip = rawip;
    if (isHex) ip = HexToIP(rawip.c_str());

    uint32_t parts[4];
    if (sscanf(ip.c_str(), "%u.%u.%u.%u", &parts[0], &parts[1], &parts[2], &parts[3]) != 4)
        return false;
    for (int i = 0; i < 4; i++) {
        if (parts[i] > 255) return false;
    }
    return true;
}

// TODO: Add error result?
bool CAvianNameSystemID::CheckTypeData(Type type, std::string typeData) {
    if (type == Type::ADDR) {
        CTxDestination destination = DecodeDestination(typeData);
        if (!IsValidDestination(destination)) return false;
    } else if (type == Type::IP) {
        if (!CheckIP(typeData, true)) return false;
    } else {
        // Unknown type
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
    } else if (type == IP) {
        if (!CheckIP(typeData, false)) {
            error = (typeData != "")
            ? std::string("Invalid IPv4 address: ") + typeData
            : std::string("Empty IPv4 addresss.");
        }
        returnStr = IPToHex(typeData);
    }

    return returnStr;
}

bool CAvianNameSystemID::IsValidID(std::string ansID) {
    // Check for min length
    if(ansID.length() <= prefix.size() + 1) return false;

    // Check for prefix
    bool hasPrefix = (ansID.substr(0, CAvianNameSystemID::prefix.length()) == CAvianNameSystemID::prefix) && (ansID.size() <= 64);
    if (!hasPrefix) return false;

    // Must be valid hex char
    std::string hexStr = ansID.substr(prefix.length(), 1);
    if (!IsHexNumber(hexStr)) return false;

    // Hex value must be less than 0xf
    int hexInt = stoi(hexStr, 0, 16);
    if (hexInt > 0xf) return false;

    // Check type
    Type type = static_cast<Type>(hexInt);
    std::string rawData = ansID.substr(prefix.length() + 1);

    if (!CheckTypeData(type, rawData)) return false;

    return true;
}

CAvianNameSystemID::CAvianNameSystemID(Type type, std::string rawData) :
    m_addr(""),
    m_ip("")
{
    this->m_type = type;

    if (!CheckTypeData(this->m_type, rawData)) return;

    if (this->m_type == Type::ADDR) {
        // Avian address
        this->m_addr = rawData;
    }
    else if (this->m_type == Type::IP) {
        // Raw IP (127.0.0.1)
        this->m_ip = HexToIP(rawData.c_str());
    }
}

CAvianNameSystemID::CAvianNameSystemID(std::string ansID) :
    m_addr(""),
    m_ip("")
{
    // Check if valid ID
    if(!IsValidID(ansID)) return;

    // Get type
    Type type = static_cast<Type>(stoi(ansID.substr(prefix.length(), 1), 0, 16));
    this->m_type = type;

    // Set info based on data
    std::string data = ansID.substr(prefix.length() + 1); // prefix + type

    if (this->m_type == Type::ADDR) {
        this->m_addr = data;
    } else if(this->m_type == Type::IP) {
        this->m_ip = HexToIP(data.c_str());
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
    if (this->m_type == Type::ADDR) {
       id += m_addr;
    } else if (this->m_type == Type::IP) {
        id += IPToHex(m_ip);
    }

    return id;
}
