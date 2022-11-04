// Copyright (c) 2022 The Avian Core developers
// Copyright (c) 2022 Shafil Alam
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/**
Avian Name System functions are consensus critical, 
please be safe and run proper tests.
*/

#include "ans.h"

#include <iostream>
#include <string>
#include <sstream>

#include "univalue.h"
#include "util.h"
#include "utilstrencodings.h"
#include "base58.h"
#include "script/standard.h"

#include "boost/asio.hpp"

using namespace boost::asio::ip;

/** Static prefix from ANS IDs */
const std::string CAvianNameSystem::prefix = "ANS";

/** Static domain */
const std::string CAvianNameSystem::domain = ".AVN";

static std::string IPv4ToHex(std::string strIPv4)
{
    boost::system::error_code error;
    auto ip = address_v4::from_string(strIPv4, error);
    if (error) return "0";

    std::stringstream ss;
    ss << std::hex << ip.to_ulong();
    return ss.str();
}

static std::string HexToIPv4(std::string hexIPv4)
{
    if(!IsHexNumber(hexIPv4)) return "0.0.0.0";

    unsigned int hex = std::stoul(hexIPv4, 0, 16);
    auto ip = address_v4(hex);
    return ip.to_string();
}

bool CAvianNameSystem::CheckIPv4(std::string rawipv4, bool isHex) {
    if (isHex) rawipv4 = HexToIPv4(rawipv4);

    boost::system::error_code error;
    address_v4::from_string(rawipv4, error);

    if (error) return false;
    else return true;
}

// TODO: Add error result?
bool CAvianNameSystem::CheckTypeData(Type type, std::string typeData) 
{
    switch (type) {
    case ADDR: {
        CTxDestination destination = DecodeDestination(typeData);
        if (!IsValidDestination(destination)) return false;
    }
    case IPv4: if (!CheckIPv4(typeData, true)) return false;
    default: /** Unknown type */ return false;
    }
    return true;
}

std::string CAvianNameSystem::FormatTypeData(Type type, std::string typeData, std::string& error)
{
    std::string returnStr = typeData;

    // Check and set type data
    switch (type) {
        case ADDR: {
            CTxDestination destination = DecodeDestination(typeData);
            if (!IsValidDestination(destination)) {
                error = (typeData != "") 
                ? std::string("Invalid Avian address: ") + typeData 
                : std::string("Empty Avian address.");
            }
            break;
        }
        case IPv4: {
            if (!CheckIPv4(typeData, false)) {
                error = (typeData != "") 
                ? std::string("Invalid IPv4 address: ") + typeData 
                : std::string("Empty IPv4 addresss.");
            }
            returnStr = IPv4ToHex(typeData);
            break;
        }
    }

    return returnStr;
}

bool CAvianNameSystem::IsValidID(std::string ansID) 
{
    // Check for min length
    if(ansID.length() <= prefix.size() + 1) return false;

    // Check for prefix
    bool hasPrefix = (ansID.substr(0, CAvianNameSystem::prefix.length()) == CAvianNameSystem::prefix) && (ansID.size() <= 64);
    if (!hasPrefix) return false;

    // Must be valid hex number
    std::string hexStr = ansID.substr(prefix.length(), 1);
    if (!IsHexNumber(hexStr)) return false;

    // Check type
    int hexInt = stoi(hexStr, 0, 16);
    Type type = static_cast<Type>(hexInt);
    std::string rawData = ansID.substr(prefix.length() + 1);

    if (!CheckTypeData(type, rawData)) return false;

    return true;
}

CAvianNameSystem::CAvianNameSystem(Type type, std::string rawData) :
    m_addr(""),
    m_ipv4("")
{
    this->m_type = type;

    if (!CheckTypeData(this->m_type, rawData)) return;

    switch(this->m_type) {
        case ADDR: this->m_addr = rawData;
        case IPv4: this->m_ipv4 = HexToIPv4(rawData);
    }
}

CAvianNameSystem::CAvianNameSystem(std::string ansID) :
    m_addr(""),
    m_ipv4("")
{
    // Check if valid ID
    if(!IsValidID(ansID)) return;

    // Get type
    Type type = static_cast<Type>(stoi(ansID.substr(prefix.length(), 1), 0, 16));
    this->m_type = type;

    // Set info based on data
    std::string data = ansID.substr(prefix.length() + 1); // prefix + type

    switch(this->m_type) {
        case ADDR: this->m_addr = data;
        case IPv4: this->m_ipv4 = HexToIPv4(data);
    }
}

std::string CAvianNameSystem::to_string() 
{
    std::string id = "";

    // 1. Add prefix
    id += prefix;

    // 2. Add type
    std::stringstream ss;
    ss << std::hex << this->m_type;
    id += ss.str();

    // 3. Add data
    switch(this->m_type) {
        case ADDR: id += m_addr;
        case IPv4: IPv4ToHex(m_ipv4);
    }
    
    return id;
}

UniValue CAvianNameSystem::to_object()
{
    UniValue ansInfo(UniValue::VOBJ);

    ansInfo.pushKV("ans_id", this->to_string());
    ansInfo.pushKV("type_hex", this->m_type);

    switch(this->m_type) {
        case ADDR: ansInfo.pushKV("ans_addr", this->m_addr);
        case IPv4: ansInfo.pushKV("ans_ip", this->m_ipv4);
    }

    return ansInfo;
}
