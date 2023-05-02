// Copyright (c) 2022 The Avian Core developers
// Copyright (c) 2022 Shafil Alam
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/**
Avian Flight Plans are experimental and prone to bugs.
Please take precautions when using this feature.
*/

#include "flightplans.h"

#include "avianlib.h"
#include "util.h"
#include "fs.h"

#include <cstddef>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include "lua/lua.hpp"

FlightPlanResult CAvianFlightPlans::RunFile(const char* file, const char* func, std::vector<std::string> args)
{
    // Warn user
    LogPrintf("Running flight plan; Avian Flight Plans are experimental and prone to bugs. Please take precautions when using this feature.\n");

    // Result object
    FlightPlanResult result;
    int status;

    lua_State* L = luaL_newstate();

    // Make standard libraries available in the Lua object
    luaopen_base(L);
    luaopen_table(L);
    luaopen_string(L);
    luaopen_math(L);

    // Register Avian lib
    register_avianlib(L);

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

        if (args.size() >= 1) {
            n = args.size();
            // Loop through each argument
            for (int i = 0; i < n; i++) {
                /* push argument */
                lua_pushstring(L, args[i].c_str());
            }
        }

        // Call the function with n arguments, return 1 result
        status = lua_pcall(L, n, 1, 0);

        if (status != LUA_OK) {
            const char* message = lua_tostring(L, -1);
            result.result = message;
            result.is_error = true;
            lua_pop(L, 1);
            return result;
        }

        // Get the result
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

std::vector<std::string> CAvianFlightPlans::GetPlans()
{
    std::vector<std::string> plans;
    fs::path path = GetDataDir(false) / "flightplans";
    for (auto& file : fs::directory_iterator(path)) {
        if (file.path().extension() == ".lua") {
            plans.push_back(file.path().stem().string());
        }
    }
    return plans;
}
