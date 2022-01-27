// Copyright (c) 2022 The Avian Core developers
// Copyright (c) 2022 Shafil Alam
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/*
Avian Flight Plans (smart contracts) are experimental and prone to bugs.
Please take precautions when using this feature.
*/

#include "flightplans.h"

#include "amount.h"
#include "base58.h"
#include "chain.h"
#include "consensus/validation.h"
#include "core_io.h"
#include "httpserver.h"
#include "net.h"
#include "policy/feerate.h"
#include "policy/fees.h"
#include "policy/policy.h"
#include "policy/rbf.h"
#include "rpc/mining.h"
#include "rpc/safemode.h"
#include "rpc/server.h"
#include "script/sign.h"
#include "timedata.h"
#include "util.h"
#include "utilmoneystr.h"
#include "utiltime.h"
#include "validation.h"
#include "wallet/coincontrol.h"
#include "wallet/feebumper.h"
#include "wallet/wallet.h"
#include "wallet/walletdb.h"

#include <cstddef>
#include <cstdio>
#include <iostream>
#include <string>

#include "lua/lua.hpp"

/* Avian Lua Lib */
static int balance(lua_State* L)
{
    CWallet* const pwallet = vpwallets[0];

    lua_pushnumber(L, pwallet->GetBalance());

    /* return the number of results */
    return 1;
}

FlightPlanResult AvianFlightPlans::run_f(const char* file, const char* func, std::vector<std::string> args)
{
    // Result object
    FlightPlanResult result;
    int status;

    lua_State* L = luaL_newstate();

    // Make standard libraries available in the Lua object
    luaL_openlibs(L);

    // Register Avian libs
    lua_register(L, "avnBalance", balance);

    // Load the program
    status = luaL_dofile(L, file);

    if (status != LUA_OK) {
        const char* message = lua_tostring(L, -1);
        result.result = message;
        result.is_error = true;
        lua_pop(L, 1);
        return result;
    }

    // The function name
    lua_getglobal(L, func);

    if (lua_isfunction(L, -1)) {
        int n = 0;

        if(args.size() >= 1) {
            n = args.size();
            /* loop through each argument */
            for (int i = 0; i < n; i++) {
                /* push argument */
                lua_pushstring(L, args[i].c_str());
            }
        }

        /* call the function with n arguments, return 1 result */
        status = lua_pcall(L, n, 1, 0);

        if (status != LUA_OK) {
            const char* message = lua_tostring(L, -1);
            result.result = message;
            result.is_error = true;
            lua_pop(L, 1);
            return result;
        }

        /* get the result */
        if (lua_isstring(L, -1)) {
            result.result = (char*)lua_tostring(L, -1);
            lua_pop(L, 1);
        } else {
            result.result = "Return value was null.";
            result.is_error = true;
            return result;
        }
    } else {
        result.result = "Function not found or invalid.";
        result.is_error = true;
        return result;
    }

    lua_close(L);

    return result;
}
