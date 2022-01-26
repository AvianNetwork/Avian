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
#include <string>

#include <univalue.h>

UniValue call_function(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() < 2)
        throw std::runtime_error(
            "call_function\n"
            "\nCall an Avian flight plan function.\n"
            "\nArguments:\n"
            "1. contract name      (string, required) Lua file.\n"
            "2. function      (string, required) Lua function.\n"
            "\nResult:\n"
            "1.    (string) Result from called function\n"
            "\nExamples:\n" +
            HelpExampleCli("call_function", "\"Like Count Contract\" \"getLikes\"") + HelpExampleRpc("call_function", "\"Like Count Contract\" \"getLikes\""));

    LOCK(cs_main);

    auto flightplans = AvianFlightPlans();

    std::string datadir = boost::filesystem::canonical(GetDataDir(false)).string();

    #ifdef __linux__ 
        std::string path = datadir + "/flightplans/" + request.params[0].get_str() + ".lua";
    #elif _WIN32
        std::string path = datadir + "\\flightplans\\" + request.params[0].get_str() + ".lua";
    #else
    #endif

    FlightPlanResult result = flightplans.run_f(path.c_str(), request.params[1].get_str().c_str());

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
