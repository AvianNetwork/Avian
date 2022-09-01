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
    inline static std::string prefix = "ANS";

    enum Type {
        ADDR = 0x0,
        IP = 0x1
    };

    CAvianNameSystemID(Type type, std::string rawData);
    CAvianNameSystemID(std::string ansID);

    std::string to_string();
    UniValue to_object();

    Type type() { return m_type; };
    std::string addr() { return m_addr; };
    std::string ip() { return m_ip; };

    static bool IsValidID(std::string ansID);

private:
    Type m_type;
    std::string m_addr;
    std::string m_ip;
};

/* Class for ANS (Avian Name System) */
class CAvianNameSystem {
public:

};

#endif
