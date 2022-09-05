// Copyright (c) 2022 The Avian Core developers
// Copyright (c) 2022 Shafil Alam
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef AVIAN_ANS_H
#define AVIAN_ANS_H

#include <string>

#include "univalue.h"

/* Class for ANS (Avian Name System) ID */
class CAvianNameSystemID {
public:  
    inline const static std::string prefix = "ANS";

    enum Type {
        ADDR = 0x0,
        IP = 0x1
    };

    inline const static Type AllTypes[] = { ADDR, IP };

    CAvianNameSystemID(Type type, std::string rawData);
    CAvianNameSystemID(std::string ansID);

    std::string to_string();
    UniValue to_object();

    Type type() { return m_type; };
    std::string addr() { return m_addr; };
    std::string ip() { return m_ip; };

    static bool IsValidID(std::string ansID);
    static bool CheckTypeData(Type type, std::string typeData);

    static std::pair<std::string, std::string> enum_to_string(Type type) {
    switch(type) {
        case ADDR:
            return std::pair<std::string, std::string>("Avian address", "Enter an Avian address");
        case IP:
            return std::pair<std::string, std::string>("IP [DNS A record]", "Enter IP address");
        default:
            return std::pair<std::string, std::string>("Invalid", "Invalid");
    }
}

private:
    Type m_type;
    std::string m_addr;
    std::string m_ip;
};

#endif
