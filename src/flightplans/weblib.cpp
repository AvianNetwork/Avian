#include "weblib.h"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <cstdlib>
#include <algorithm>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include "keccak.h"
#include "lua/lua.hpp"

namespace beast = boost::beast; // from <boost/beast.hpp>
namespace http = beast::http;   // from <boost/beast/http.hpp>
namespace net = boost::asio;    // from <boost/asio.hpp>
using tcp = net::ip::tcp;       // from <boost/asio/ip/tcp.hpp>

/** Static RPCs */
const static std::string POLYGON_RPC = "polygon-rpc.com";

// Performs an HTTP POST and returns the response
std::string http_rpc(std::string host, std::string command, std::string args)
{
    std::string result;
    try {
        // Set port and target
        const char* port = "80";
        const char* target = "/";

        // Set POST body
        std::string body = std::string("{\"jsonrpc\":\"2.0\",\"method\":") + std::string("\"") + command + std::string("\"") + std::string(",\"params\":[" + args + "],\"id\":1}");

        // The io_context is required for all I/O
        net::io_context ioc;

        // These objects perform our I/O
        tcp::resolver resolver(ioc);
        beast::tcp_stream stream(ioc);

        // Look up the domain name
        auto const results = resolver.resolve(host.c_str(), port);

        // Make the connection on the IP address we get from a lookup
        stream.connect(results);

        // Set up an HTTP POST request message
        http::request<http::string_body> request;
        request.version(11);
        request.method(http::verb::post);
        request.target(target);
        request.set(http::field::host, host);
        request.set(http::field::content_type, "application/x-www-form-urlencoded");
        request.body() = body.c_str();
        request.prepare_payload();

        http::write(stream, request);

        beast::flat_buffer buffer;
        http::response<boost::beast::http::dynamic_body> res;

        http::read(stream, buffer, res);

        result = beast::buffers_to_string(res.body().data());

        // Gracefully close the socket
        beast::error_code ec;
        stream.socket().shutdown(tcp::socket::shutdown_both, ec);

        // not_connected happens sometimes
        // so don't bother reporting it.
        if (ec && ec != beast::errc::not_connected)
            throw beast::system_error{ec};

        // If we get here then the connection is closed gracefully
    } catch (std::exception const& e) {
        result = "Error: " + std::string(e.what());
    }
    return result;
}

/** Polygon Lua RPC */
static int polygon_rpc(lua_State* L)
{
    if (lua_isstring(L, 1)) {
        if (lua_isstring(L, 2)) {
            const char* command = lua_tostring(L, 1);
            const char* args = lua_tostring(L, 2);
            std::string result = http_rpc(POLYGON_RPC, std::string(command), std::string(args));
            lua_pushstring(L, result.c_str());

        } else {
            lua_pushliteral(L, "Missing args. If no args needed then use: \"\"");
            lua_error(L);
        }
    } else {
        lua_pushliteral(L, "Missing RPC command");
        lua_error(L);
    }

    /* return the number of results */
    return 1;
}

/** ABI Encoder */
std::string abi_function(std::string func, std::vector<std::string> args, std::vector<std::string> types)
{
    Keccak keccak256(Keccak::Keccak256);
    std::stringstream stream;

    // Remove first two elements in vector
    args.erase(args.begin());
    args.erase(args.begin());

    // Get first 8 bytes of Keccak hash from function name
    std::string keccakHash(keccak256(func), 0, 8);
    stream << "0x" << keccakHash;

    if(types.size() < args.size()) return "ABI Error: No type were given for arugment.";

    // Add argument with padded hex
    for (size_t i = 0; i < args.size(); ++i) {
        if (types[i] == "address") {
            std::string arg_item(args[i]);
            std::string arg_item_padded(arg_item, 2, 40);
            stream << std::setfill('0') << std::setw(64) << arg_item_padded;
        } else {
            return "ABI Error: Unknown type: " + types[i];
        }
    }

    // Return ABI result
    return stream.str();
}

/** Helper split function */
std::vector<std::string> split(const std::string& s, char delim)
{
    std::vector<std::string> result;
    std::stringstream ss(s);
    std::string item;

    if(s == "") return {};

    while (getline(ss, item, delim)) {
        result.push_back(item);
    }

    return result;
}

static int lua_abi_function(lua_State* L)
{
    std::vector<std::string> args;

    /* get number of arguments */
    int n = lua_gettop(L);

    if(n <= 2) {
        lua_pushliteral(L, "Invalid amount of arguments.");
        lua_error(L);
    }

    /* loop through each argument */
    for (int i = 1; i <= n; i++) {
        if (lua_isstring(L, i)) {
            std::string arg = std::string(lua_tostring(L, i));
            args.push_back(arg);
        } else {
            lua_pushliteral(L, "Invalid argument");
            lua_error(L);
        }
    }

    std::string result = abi_function(args[0], args, split(args[1], ','));

    /* check if result is in hex */
    if(result[0] != '0' && result[1] != 'x') {
        lua_pushstring(L, result.c_str());
        lua_error(L);
    } else {
        lua_pushstring(L, result.c_str());
    }

    return 1;
}

void register_weblib(lua_State* L)
{
    static const struct luaL_Reg polygon[] = {
        {"rpc", polygon_rpc},
        {NULL, NULL}};

    static const struct luaL_Reg abi[] = {
        {"encode", lua_abi_function},
        {NULL, NULL}};

    lua_newtable(L);

    lua_newtable(L);
    luaL_setfuncs(L, polygon, 0);
    lua_setfield(L, -2, "polygon");

    lua_newtable(L);
    luaL_setfuncs(L, abi, 0);
    lua_setfield(L, -2, "abi");

    lua_setglobal(L, "web3");
}
