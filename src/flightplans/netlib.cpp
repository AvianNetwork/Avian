#include "netlib.h"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <cstdlib>
#include <iostream>
#include <string>

#include "lua/lua.hpp"

namespace beast = boost::beast; // from <boost/beast.hpp>
namespace http = beast::http;   // from <boost/beast/http.hpp>
namespace net = boost::asio;    // from <boost/asio.hpp>
using tcp = net::ip::tcp;       // from <boost/asio/ip/tcp.hpp>

/** Static RPCs */
const static std::string POLYGON_RPC = "polygon-rpc.com";

// Performs an HTTP POST and returns the response
std::string http_rpc(std::string host, std::string command)
{
    std::string result;
    try {
        // Set port and target
        const char* port = "80";
        const char* target = "/";

        // Set POST body
        std::string body = std::string("{\"jsonrpc\":\"2.0\",\"method\":") 
                            + std::string("\"") + command + std::string("\"") 
                            + std::string(",\"params\":[],\"id\":1}");

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

        boost::beast::flat_buffer buffer;
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

static int polygon_rpc(lua_State* L)
{
    if (lua_isstring(L, 1)) {
        const char* command = lua_tostring(L, 1);
        std::string result = http_rpc(POLYGON_RPC, std::string(command));
        lua_pushstring(L, result.c_str());
    } else {
        lua_pushliteral(L, "Missing or invalid argument.");
        lua_error(L);
    }

    /* return the number of results */
    return 1;
}

void register_netlib(lua_State* L)
{
    static const struct luaL_Reg polygon[] = {
        {"rpc", polygon_rpc},
        {NULL, NULL}};

    lua_newtable(L);

    lua_newtable(L);
    luaL_setfuncs(L, polygon, 0);
    lua_setfield(L, -2, "polygon");

    lua_setglobal(L, "net");
}
