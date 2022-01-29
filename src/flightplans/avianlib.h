// Copyright (c) 2022 The Avian Core developers
// Copyright (c) 2022 Shafil Alam
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef AVIAN_FLIGHTPLANS_AVIANLIB_H
#define AVIAN_FLIGHTPLANS_AVIANLIB_H

#include "lua/lua.hpp"
#include <string>

/* Run RPC commands */
bool RPCParse(std::string& strResult, const std::string& strCommand, const bool fExecute, std::string* const pstrFilteredOut);

/* Lua */
void register_avianlib(lua_State* L);

#endif
