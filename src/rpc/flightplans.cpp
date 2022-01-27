// Copyright (c) 2022 The Avian Core developers
// Copyright (c) 2022 Shafil Alam
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "rpc/flightplans.h"
#include "flightplans/flightplans.h"

#include "chainparams.h"
#include "clientversion.h"
#include "core_io.h"
#include "net.h"
#include "net_processing.h"
#include "netbase.h"
#include "policy/policy.h"
#include "protocol.h"
#include "rpc/server.h"
#include "sync.h"
#include "timedata.h"
#include "ui_interface.h"
#include "util.h"
#include "utilstrencodings.h"
#include "validation.h"
#include "version.h"
#include "warnings.h"

#include <stdint.h>
#include <univalue.h>

#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

UniValue call_function(const JSONRPCRequest& request)
{
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
            HelpExampleCli("call_function", "\"Like Count Contract\" \"getLikes\"") + HelpExampleRpc("call_function", "\"Like Count Contract\" \"getLikes\""));

    LOCK(cs_main);

    std::vector<std::string> args = {};

    for(size_t i = 0; i < request.params.size(); i++) {
        args.push_back(request.params[i].get_str());
    }

    args.erase(args.begin());
    args.erase(args.begin());

    auto flightplans = AvianFlightPlans();

    std::string datadir = boost::filesystem::canonical(GetDataDir(false)).string();

    // TODO: Fix this to use lib instead of relying on marcos.
    
    #if !defined(_WIN32) && (defined(__unix__) || defined(__unix) || (defined(__APPLE__) && defined(__MACH__)))
    std::string path = datadir + "/flightplans/" + request.params[0].get_str() + ".lua";
    #endif

    #ifdef _WIN32
        std::string path = datadir + "\\flightplans\\" + request.params[0].get_str() + ".lua";
    #endif

    FlightPlanResult result = flightplans.run_f(path.c_str(), request.params[1].get_str().c_str(), args);

    boost::filesystem::path file(path);
    if (boost::filesystem::exists(file)) {
        if (result.is_error) {
            throw JSONRPCError(RPC_MISC_ERROR, result.result);
        } else {
            return result.result;
        }
    } else {
        throw JSONRPCError(RPC_MISC_ERROR, "Flightplan does not exist.");
    }
}

static const CRPCCommand commands[] =
    { //  category              name                      actor (function)         argNames
      //  --------------------- ------------------------  -----------------------  ----------
        {"flightplans", "call_function", &call_function, {}}};

void RegisterFlightPlanRPCCommands(CRPCTable& t)
{
    for (unsigned int vcidx = 0; vcidx < ARRAYLEN(commands); vcidx++)
        t.appendCommand(commands[vcidx].name, &commands[vcidx]);
}
