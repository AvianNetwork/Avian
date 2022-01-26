// Copyright (c) 2022 The Avian Core developers
// Copyright (c) 2022 Shafil Alam
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/*
Avian Flight Plans (smart contracts) are experimental and prone to bugs.
Please take precautions when using this feature.
*/

#include "flightplans.h"
#include <cstdio>
#include <iostream>

#ifdef __cplusplus
extern "C" {
#include "lua/lua.h"
#include "lua/lualib.h"
#include "lua/lauxlib.h"
}
#endif //__cplusplus

FlightPlanResult AvianFlightPlans::run_f(const char* file, const char* func)
{
    // Result object
    FlightPlanResult result;

    lua_State* L = luaL_newstate();

    // Make standard libraries available in the Lua object
    luaL_openlibs(L);

    int status;

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

        // /* the first argument */
        // lua_pushnumber(L, x);

        // /* the second argument */
        // lua_pushnumber(L, y);

        /* call the function with 0 arguments, return 1 result */
        lua_call(L, 0, 1);

        /* get the result */
        if (lua_isstring(L, -1)) {
            result.result = (char*)lua_tostring(L, -1);
            lua_pop(L, 1);
        } else {
            result.result = "Return value was not a string.";
            result.is_error = true;
            return result;
        }
    } else {
        result.result = "Function not found or not a valid function.";
        result.is_error = true;
        return result;
    }

    lua_close(L);

    return result;
}
