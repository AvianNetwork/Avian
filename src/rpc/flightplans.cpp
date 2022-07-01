// Copyright (c) 2022 The Avian Core developers
// Copyright (c) 2022 Shafil Alam
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "rpc/flightplans.h"
#include "flightplans/flightplans.h"

#include "rpc/server.h"
#include "validation.h"
#include "fs.h"
#include "util.h"

#include <stdint.h>
#include <univalue.h>

#include <string>
#include <vector>

UniValue call_function(const JSONRPCRequest& request)
{
    if (!AreFlightPlansDeployed())
        throw std::runtime_error(
            "Coming soon: Avian flight plan function will be available in a future release.\n");

    if (request.fHelp || request.params.size() < 2)
        throw std::runtime_error(
            "call_function\n"
            "\nCall an Avian flight plan function.\n"
            "\nArguments:\n"
            "1. contract name      (string, required) Lua file.\n"
            "2. function      (string, required) Lua function.\n"
            "3. args      (string, not needed) Lua args.\n"
            "\nResult:\n"
            "1.    (string) Result from called function\n"
            "\nExamples:\n" +
            HelpExampleCli("call_function", "\"social\" \"getLikes\"") + HelpExampleRpc("call_function", "\"social\" \"getLikes\""));

    LOCK(cs_main);

    if (gArgs.IsArgSet("-flightplans")) {
        std::vector<std::string> args = {};
        std::string file = request.params[0].get_str() + ".lua";

        for (size_t i = 0; i < request.params.size(); i++) {
            args.push_back(request.params[i].get_str());
        }

        // Remove first 2 arguments
        args.erase(args.begin());
        args.erase(args.begin());

        auto flightplans = AvianFlightPlans();

        // TODO: Make sure works on Windows and Linux
        fs::path path = GetDataDir(false) / "flightplans" / file.c_str();

        FlightPlanResult result = flightplans.run_file(path.string().c_str(), request.params[1].get_str().c_str(), args);

        if (fs::exists(path)) {
            if (result.is_error) {
                throw JSONRPCError(RPC_MISC_ERROR, result.result);
            } else {
                return result.result;
            }
        } else {
            throw JSONRPCError(RPC_MISC_ERROR, "Flight plan does not exist.");
        }
    } else {
        throw JSONRPCError(RPC_MISC_ERROR, "Flight Plans are experimental and prone to bugs. Please take precautions when using this feature. To enable, launch Avian with the -flightplans flag.");
    }
}

UniValue list_flightplans(const JSONRPCRequest& request)
{
    if (!AreFlightPlansDeployed())
        throw std::runtime_error(
            "Coming soon: Avian flight plan function will be available in a future release.\n");

    if (request.fHelp)
        throw std::runtime_error(
            "list_flightplans\n"
            "\nList avian flight plans.\n"
            "\nResult:\n"
            "[ flight plan name ]     (array) list of avian flight plans\n"
            "\nExamples:\n" +
            HelpExampleCli("list_flightplans", "") + HelpExampleRpc("list_flightplans", ""));

    LOCK(cs_main);

    if (gArgs.IsArgSet("-flightplans")) {
        UniValue plans(UniValue::VARR);
        fs::path path = GetDataDir(false) / "flightplans";
        for (auto& file : fs::directory_iterator(path)) {
            plans.push_back(file.path().string());
        }
        return plans;
    } else {
        throw JSONRPCError(RPC_MISC_ERROR, "Flight Plans are experimental and prone to bugs. Please take precautions when using this feature. To enable, launch Avian with the -flightplans flag.");
    }
}

static const CRPCCommand commands[] =
    { //  category              name                      actor (function)         argNames
      //  --------------------- ------------------------  -----------------------  ----------
        {"flightplans",         "call_function",          &call_function,          {"contract_name", "function", "args"}},
        {"flightplans",         "list_flightplans",       &list_flightplans,       {}}
    };

void RegisterFlightPlanRPCCommands(CRPCTable& t)
{
    for (unsigned int vcidx = 0; vcidx < ARRAYLEN(commands); vcidx++)
        t.appendCommand(commands[vcidx].name, &commands[vcidx]);
}
