// Copyright (c) 2022 The Avian Core developers
// Copyright (c) 2022 Shafil Alam
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "avianlib.h"

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
#include "rpc/client.h"
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

#include <string>

#include "lua/lua.hpp"

/* Parse RPC commands */
bool RPCParse(std::string& strResult, const std::string& strCommand, const bool fExecute, std::string* const pstrFilteredOut)
{
    std::vector<std::vector<std::string>> stack;
    stack.push_back(std::vector<std::string>());

    enum CmdParseState {
        STATE_EATING_SPACES,
        STATE_EATING_SPACES_IN_ARG,
        STATE_EATING_SPACES_IN_BRACKETS,
        STATE_ARGUMENT,
        STATE_SINGLEQUOTED,
        STATE_DOUBLEQUOTED,
        STATE_ESCAPE_OUTER,
        STATE_ESCAPE_DOUBLEQUOTED,
        STATE_COMMAND_EXECUTED,
        STATE_COMMAND_EXECUTED_INNER
    } state = STATE_EATING_SPACES;
    std::string curarg;
    UniValue lastResult;
    unsigned nDepthInsideSensitive = 0;
    size_t filter_begin_pos = 0, chpos;
    std::vector<std::pair<size_t, size_t>> filter_ranges;

    auto add_to_current_stack = [&](const std::string& strArg) {
        if (stack.back().empty() && (!nDepthInsideSensitive)) {
            nDepthInsideSensitive = 1;
            filter_begin_pos = chpos;
        }
        // Make sure stack is not empty before adding something
        if (stack.empty()) {
            stack.push_back(std::vector<std::string>());
        }
        stack.back().push_back(strArg);
    };

    auto close_out_params = [&]() {
        if (nDepthInsideSensitive) {
            if (!--nDepthInsideSensitive) {
                assert(filter_begin_pos);
                filter_ranges.push_back(std::make_pair(filter_begin_pos, chpos));
                filter_begin_pos = 0;
            }
        }
        stack.pop_back();
    };

    std::string strCommandTerminated = strCommand;
    if (strCommandTerminated.back() != '\n')
        strCommandTerminated += "\n";
    for (chpos = 0; chpos < strCommandTerminated.size(); ++chpos) {
        char ch = strCommandTerminated[chpos];
        switch (state) {
        case STATE_COMMAND_EXECUTED_INNER:
        case STATE_COMMAND_EXECUTED: {
            bool breakParsing = true;
            switch (ch) {
            case '[':
                curarg.clear();
                state = STATE_COMMAND_EXECUTED_INNER;
                break;
            default:
                if (state == STATE_COMMAND_EXECUTED_INNER) {
                    if (ch != ']') {
                        // append char to the current argument (which is also used for the query command)
                        curarg += ch;
                        break;
                    }
                    if (curarg.size() && fExecute) {
                        // if we have a value query, query arrays with index and objects with a string key
                        UniValue subelement;
                        if (lastResult.isArray()) {
                            for (char argch : curarg)
                                if (!std::isdigit(argch))
                                    throw std::runtime_error("Invalid result query");
                            subelement = lastResult[atoi(curarg.c_str())];
                        } else if (lastResult.isObject())
                            subelement = find_value(lastResult, curarg);
                        else
                            throw std::runtime_error("Invalid result query"); // no array or object: abort
                        lastResult = subelement;
                    }

                    state = STATE_COMMAND_EXECUTED;
                    break;
                }
                // don't break parsing when the char is required for the next argument
                breakParsing = false;

                // pop the stack and return the result to the current command arguments
                close_out_params();

                // don't stringify the json in case of a string to avoid doublequotes
                if (lastResult.isStr())
                    curarg = lastResult.get_str();
                else
                    curarg = lastResult.write(2);

                // if we have a non empty result, use it as stack argument otherwise as general result
                if (curarg.size()) {
                    if (stack.size())
                        add_to_current_stack(curarg);
                    else
                        strResult = curarg;
                }
                curarg.clear();
                // assume eating space state
                state = STATE_EATING_SPACES;
            }
            if (breakParsing)
                break;
        }
        case STATE_ARGUMENT: // In or after argument
        case STATE_EATING_SPACES_IN_ARG:
        case STATE_EATING_SPACES_IN_BRACKETS:
        case STATE_EATING_SPACES: // Handle runs of whitespace
            switch (ch) {
            case '"':
                state = STATE_DOUBLEQUOTED;
                break;
            case '\'':
                state = STATE_SINGLEQUOTED;
                break;
            case '\\':
                state = STATE_ESCAPE_OUTER;
                break;
            case '(':
            case ')':
            case '\n':
                if (state == STATE_EATING_SPACES_IN_ARG)
                    throw std::runtime_error("Invalid Syntax");
                if (state == STATE_ARGUMENT) {
                    if (ch == '(' && stack.size() && stack.back().size() > 0) {
                        if (nDepthInsideSensitive) {
                            ++nDepthInsideSensitive;
                        }
                        stack.push_back(std::vector<std::string>());
                    }

                    // don't allow commands after executed commands on baselevel
                    if (!stack.size())
                        throw std::runtime_error("Invalid Syntax");

                    add_to_current_stack(curarg);
                    curarg.clear();
                    state = STATE_EATING_SPACES_IN_BRACKETS;
                }
                if ((ch == ')' || ch == '\n') && stack.size() > 0) {
                    if (fExecute) {
                        // Convert argument list to JSON objects in method-dependent way,
                        // and pass it along with the method name to the dispatcher.
                        JSONRPCRequest req;
                        req.params = RPCConvertValues(stack.back()[0], std::vector<std::string>(stack.back().begin() + 1, stack.back().end()));
                        req.strMethod = stack.back()[0];
                        lastResult = tableRPC.execute(req);
                    }

                    state = STATE_COMMAND_EXECUTED;
                    curarg.clear();
                }
                break;
            case ' ':
            case ',':
            case '\t':
                if (state == STATE_EATING_SPACES_IN_ARG && curarg.empty() && ch == ',')
                    throw std::runtime_error("Invalid Syntax");

                else if (state == STATE_ARGUMENT) // Space ends argument
                {
                    add_to_current_stack(curarg);
                    curarg.clear();
                }
                if ((state == STATE_EATING_SPACES_IN_BRACKETS || state == STATE_ARGUMENT) && ch == ',') {
                    state = STATE_EATING_SPACES_IN_ARG;
                    break;
                }
                state = STATE_EATING_SPACES;
                break;
            default:
                curarg += ch;
                state = STATE_ARGUMENT;
            }
            break;
        case STATE_SINGLEQUOTED: // Single-quoted string
            switch (ch) {
            case '\'':
                state = STATE_ARGUMENT;
                break;
            default:
                curarg += ch;
            }
            break;
        case STATE_DOUBLEQUOTED: // Double-quoted string
            switch (ch) {
            case '"':
                state = STATE_ARGUMENT;
                break;
            case '\\':
                state = STATE_ESCAPE_DOUBLEQUOTED;
                break;
            default:
                curarg += ch;
            }
            break;
        case STATE_ESCAPE_OUTER: // '\' outside quotes
            curarg += ch;
            state = STATE_ARGUMENT;
            break;
        case STATE_ESCAPE_DOUBLEQUOTED:                  // '\' in double-quoted text
            if (ch != '"' && ch != '\\') curarg += '\\'; // keep '\' for everything but the quote and '\' itself
            curarg += ch;
            state = STATE_DOUBLEQUOTED;
            break;
        }
    }
    if (pstrFilteredOut) {
        if (STATE_COMMAND_EXECUTED == state) {
            assert(!stack.empty());
            close_out_params();
        }
        *pstrFilteredOut = strCommand;
        for (auto i = filter_ranges.rbegin(); i != filter_ranges.rend(); ++i) {
            pstrFilteredOut->replace(i->first, i->second - i->first, "(…)");
        }
    }
    switch (state) // final state
    {
    case STATE_COMMAND_EXECUTED:
        if (lastResult.isStr())
            strResult = lastResult.get_str();
        else
            strResult = lastResult.write(2);
    case STATE_ARGUMENT:
    case STATE_EATING_SPACES:
        return true;
    default: // ERROR to end in one of the other states
        return false;
    }
}

/* Call RPC method with multiple arguments */
int RPCCall(lua_State* L, std::string command)
{
    std::string args;

    /* get number of arguments */
    int n = lua_gettop(L);

    /* loop through each argument */
    for (int i = 1; i <= n; i++) {
        if (lua_isstring(L, i)) {
            std::string arg = std::string(lua_tostring(L, i));
            args = args + " " + arg;
        } else {
            lua_pushliteral(L, "Invalid argument");
            lua_error(L);
        }
    }

    std::string result;
    if (RPCParse(result, command + args, true, nullptr)) {
        lua_pushstring(L, result.c_str());
    } else {
        lua_pushliteral(L, "RPC Parse error: unbalanced ' or \"");
        lua_error(L);
    }

    return 1;
}

/* -- Avian Lua Lib -- */

/* Main */

// Call RPC method
static int rpc_call(lua_State* L)
{
    if (lua_isstring(L, 1)) {
        const char* rpc_command = lua_tostring(L, 1);
        std::string result;
        if (RPCParse(result, std::string(rpc_command), true, nullptr)) {
            lua_pushstring(L, result.c_str());
        } else {
            lua_pushliteral(L, "RPC Parse error: unbalanced ' or \"");
            lua_error(L);
        }
    } else {
        lua_pushliteral(L, "Missing or invalid argument.");
        lua_error(L);
    }

    /* return the number of results */
    return 1;
}

/* Assets */

// getassetdata "asset_name"
static int getassetdata(lua_State* L)
{
    return RPCCall(L, "getassetdata");
}

// getcacheinfo
static int getcacheinfo(lua_State* L)
{
    return RPCCall(L, "getcacheinfo");
}

// getsnapshot
static int getsnapshot(lua_State* L)
{
    return RPCCall(L, "getsnapshot");
}

// issue
static int issue(lua_State* L)
{
    return RPCCall(L, "issue");
}

// issueunique
static int issueunique(lua_State* L)
{
    return RPCCall(L, "issueunique");
}

// listaddressesbyasset
static int listaddressesbyasset(lua_State* L)
{
    return RPCCall(L, "listaddressesbyasset");
}

// listassetbalancesbyaddress
static int getaslistassetbalancesbyaddresssetdata(lua_State* L)
{
    return RPCCall(L, "listassetbalancesbyaddress");
}

// listassets
static int listassets(lua_State* L)
{
    return RPCCall(L, "listassets");
}

// listmyassets
static int listmyassets(lua_State* L)
{
    return RPCCall(L, "listmyassets");
}

// purgesnapshot
static int purgesnapshot(lua_State* L)
{
    return RPCCall(L, "purgesnapshot");
}

// reissue
static int reissue(lua_State* L)
{
    return RPCCall(L, "reissue");
}

// transfer
static int transfer(lua_State* L)
{
    return RPCCall(L, "transfer");
}

// transferfromaddress
static int transferfromaddress(lua_State* L)
{
    return RPCCall(L, "transferfromaddress");
}

// transferfromaddresses
static int transferfromaddresses(lua_State* L)
{
    return RPCCall(L, "transferfromaddresses");
}

/* Blockchain */

// decodeblock "blockhex"
static int decodeblock(lua_State* L)
{
    return RPCCall(L, "decodeblock");
}

// getbestblockhash
static int getbestblockhash(lua_State* L)
{
    return RPCCall(L, "getbestblockhash");
}

// getblock "blockhash"
static int getblock(lua_State* L)
{
    return RPCCall(L, "getblock");
}

// getblockchaininfo
static int getblockchaininfo(lua_State* L)
{
    return RPCCall(L, "getblockchaininfo");
}

// getblockcount
static int getblockcount(lua_State* L)
{
    return RPCCall(L, "getblockcount");
}

// getblockhash height
static int getblockhash(lua_State* L)
{
    return RPCCall(L, "getblockhash");
}

// getblockhashes timestamp
static int getblockhashes(lua_State* L)
{
    return RPCCall(L, "getblockhashes");
}

// getblockheader "hash"
static int getblockheader(lua_State* L)
{
    return RPCCall(L, "getblockheader");
}

// getchaintips
static int getchaintips(lua_State* L)
{
    return RPCCall(L, "getchaintips");
}

// getchaintxstats
static int getchaintxstats(lua_State* L)
{
    return RPCCall(L, "getchaintxstats");
}

// getdifficulty
static int getdifficulty(lua_State* L)
{
    return RPCCall(L, "getdifficulty");
}

// getmempoolancestors txid
static int getmempoolancestors(lua_State* L)
{
    return RPCCall(L, "getmempoolancestors");
}

// getmempoolentry txid
static int getmempoolentry(lua_State* L)
{
    return RPCCall(L, "getmempoolentry");
}

// getmempoolinfo
static int getmempoolinfo(lua_State* L)
{
    return RPCCall(L, "getmempoolinfo");
}

// getrawmempool
static int getrawmempool(lua_State* L)
{
    return RPCCall(L, "getrawmempool");
}

// getspentinfo
static int getspentinfo(lua_State* L)
{
    return RPCCall(L, "getspentinfo");
}

// gettxoutsetinfo
static int gettxoutsetinfo(lua_State* L)
{
    return RPCCall(L, "gettxoutsetinfo");
}

// gettxout
static int gettxout(lua_State* L)
{
    return RPCCall(L, "gettxout");
}

/* Address Index */

// getaddressbalance
static int getaddressbalance(lua_State* L)
{
    return RPCCall(L, "getaddressbalance");
}

// getaddressdeltas
static int getaddressdeltas(lua_State* L)
{
    return RPCCall(L, "getaddressdeltas");
}

// getaddressmempool
static int getaddressmempool(lua_State* L)
{
    return RPCCall(L, "getaddressmempool");
}

// getaddresstxids
static int getaddresstxids(lua_State* L)
{
    return RPCCall(L, "getaddresstxids");
}

// getaddressutxos
static int gettxout(lua_State* L)
{
    return RPCCall(L, "getaddressutxos");
}

/* Messages */

// clearmessages
static int clearmessages(lua_State* L)
{
    return RPCCall(L, "clearmessages");
}

// sendmessage
static int sendmessage(lua_State* L)
{
    return RPCCall(L, "sendmessage");
}

// subscribetochannel
static int subscribetochannel(lua_State* L)
{
    return RPCCall(L, "subscribetochannel");
}

// unsubscribefromchannel
static int unsubscribefromchannel(lua_State* L)
{
    return RPCCall(L, "unsubscribefromchannel");
}

// viewallmessagechannels
static int viewallmessagechannels(lua_State* L)
{
    return RPCCall(L, "viewallmessagechannels");
}

// viewallmessages
static int viewallmessages(lua_State* L)
{
    return RPCCall(L, "viewallmessages");
}

void register_avianlib(lua_State* L)
{
    static const struct luaL_Reg avian_main[] = {
        {"rpc_call", rpc_call},
        {NULL, NULL}};

    static const struct luaL_Reg avian_assets[] = {
        {"getassetdata", getassetdata},
        {"getcacheinfo", getcacheinfo},
        {"getsnapshot", getsnapshot},
        {"issue", issue},
        {"issueunique", issueunique},
        {"listaddressesbyasset", listaddressesbyasset},
        {"listassetbalancesbyaddress", listassetbalancesbyaddress},
        {"listassets", listassets},
        {"listmyassets", listmyassets},
        {"purgesnapshot", purgesnapshot},
        {"reissue", reissue},
        {"transfer", transfer},
        {"transferfromaddress", transferfromaddress},
        {"transferfromaddresses", transferfromaddresses},
        {NULL, NULL}};

    static const struct luaL_Reg avian_blockchain[] = {
        {"decodeblock", decodeblock},
        {"getbestblockhash", getbestblockhash},
        {"getblock", getblock},
        {"getblockchaininfo", getblockchaininfo},
        {"getblockcount", getblockcount},
        {"getblockhash", getblockhash},
        {"getblockhashes", getblockhashes},
        {"getblockheader", getblockheader},
        {"getchaintips", getchaintips},
        {"getchaintxstats", getchaintxstats},
        {"getdifficulty", getdifficulty},
        {"getmempoolancestors", getmempoolancestors},
        {"getmempoolentry", getmempoolentry},
        {"getmempoolinfo", getmempoolinfo},
        {"getrawmempool", getrawmempool},
        {"getspentinfo", getspentinfo},
        {"gettxoutsetinfo", gettxoutsetinfo},
        {"gettxout", gettxout},
        {NULL, NULL}};
    
    static const struct luaL_Reg avian_addressindex[] = {
        {"getaddressbalance", getaddressbalance},
        {"getaddressdeltas", getaddressdeltas},
        {"getaddressmempool", getaddressmempool},
        {"getaddresstxids", getaddresstxids},
        {"getaddressutxos", getaddressutxos},
        {NULL, NULL}};

    static const struct luaL_Reg avian_messages[] = {
        {"clearmessages", clearmessages},
        {"sendmessage", sendmessage},
        {"subscribetochannel", subscribetochannel},
        {"unsubscribefromchannel", unsubscribefromchannel},
        {"viewallmessagechannels", viewallmessagechannels},
        {"viewallmessages", viewallmessages},
        {NULL, NULL}};

    lua_newtable(L); // create the avian table

    lua_newtable(L);
    luaL_setfuncs(L, avian_main, 0);
    lua_setfield(L, -2, "main");

    lua_newtable(L);
    luaL_setfuncs(L, avian_assets, 0);
    lua_setfield(L, -2, "assets");

    lua_newtable(L);
    luaL_setfuncs(L, avian_blockchain, 0);
    lua_setfield(L, -2, "blockchain");

    lua_newtable(L);
    luaL_setfuncs(L, avian_addressindex, 0);
    lua_setfield(L, -2, "addressIndex");

    lua_newtable(L);
    luaL_setfuncs(L, avian_messages, 0);
    lua_setfield(L, -2, "messages");

    lua_setglobal(L, "avian"); // assign the avian table to global `avian`
}
