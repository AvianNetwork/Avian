// Copyright (c) 2022 The Avian Core developers
// Copyright (c) 2022 Shafil Alam
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "ans.h"

#include <string>
#include <sstream>

#include "string.h"

#include "univalue.h"
#include "util.h"
#include "utilstrencodings.h"
#include "base58.h"
#include "script/standard.h"

// TODO: Clean this up.
unsigned int ip_to_hex(const char* strip)
{
   unsigned int ip = 0;
   char* str = strdup(strip);
   char delimiters[] = ".";
   char* token = strtok(str, delimiters);
   int tokencount = 0;
   while(token != NULL)
   {
      if(tokencount < 4)
      {
         int n = atoi(token);
         ip = ip | n << 8 * (3 - tokencount);
      }
      tokencount++;
      token = strtok(NULL, delimiters);
   }
   free(str);

   if(tokencount != 4)
      ip = 0;

   return ip;
}

static std::string hex_to_ip(const char *input)
{
    char *output = (char*)malloc(sizeof(char) * 16);
    unsigned int a, b, c, d;

    if (sscanf(input, "%2x%2x%2x%2x", &a, &b, &c, &d) != 4)
        return output;
    sprintf(output, "%u.%u.%u.%u", a, b, c, d);
    return std::string(output);
}

CAvianNameSystemID::CAvianNameSystemID(Type type, std::string rawData) :
    m_addr(""),
    m_ip("")
{
    this->m_type = type;

    if (this->m_type == Type::ADDR) this->m_addr = rawData;
    else if (this->m_type == Type::IP) this->m_ip = rawData;
}

CAvianNameSystemID::CAvianNameSystemID(std::string ansID) :
    m_addr(""),
    m_ip("")
{
    Type type = static_cast<Type>(stoi(ansID.substr(prefix.length(), 1), 0, 16));
    this->m_type = type;

    std::string data = ansID.substr(prefix.length() + 1); // prefix + type

    if (this->m_type == Type::ADDR) {
        this->m_addr = data; // TODO: Check address!
    } else if(this->m_type == Type::IP) {
        this->m_ip = hex_to_ip(data.c_str()); // TODO: Check IP!
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
        std::stringstream ss;
        ss << std::hex << ip_to_hex(m_ip.c_str());
        id += ss.str();
    }

    return id;
}

UniValue CAvianNameSystemID::to_object()
{
    UniValue ansInfo(UniValue::VOBJ);

    ansInfo.pushKV("ans_id", this->to_string());
    ansInfo.pushKV("type_hex", this->type());

    if (this->type() == CAvianNameSystemID::ADDR) {
        ansInfo.pushKV("ans_addr", this->addr());
    } else if (this->type() == CAvianNameSystemID::IP) {
        ansInfo.pushKV("ans_ip", this->ip());
    }

    return ansInfo;
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

    if (type == Type::ADDR) {
        CTxDestination destination = DecodeDestination(rawData);
        if (!IsValidDestination(destination)) return false;
    } else if (type == Type::IP) {
        // check ip
    } else {
        // Unknown type
        return false;
    }

    return true;
}
