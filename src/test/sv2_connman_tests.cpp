// Copyright (c) 2026 The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <compat/compat.h>
#include <key.h>
#include <netbase.h>
#include <pubkey.h>
#include <sv2/connman.h>
#include <sv2/options.h>
#include <sv2/transport.h>
#include <test/util/setup_common.h>
#include <util/sock.h>
#include <util/time.h>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <boost/test/unit_test.hpp>

//! End-to-end tests for the Sv2 listener over real loopback sockets.
//!
//! The transport and handshake are already covered without a network. What
//! these add is the part only a live socket exercises: the accept path, the
//! Common protocol state machine, and the fact that it all works from another
//! thread.
BOOST_FIXTURE_TEST_SUITE(sv2_connman_tests, BasicTestingSetup)

namespace {

constexpr auto TEST_TIMEOUT{std::chrono::seconds{10}};

struct Server {
    CKey authority;
    CKey static_key;
    sv2::SignatureNoiseMessage cert;
    std::unique_ptr<sv2::Connman> connman;
    CService bound;

    XOnlyPubKey AuthorityPubKey() const { return XOnlyPubKey{authority.GetPubKey()}; }

    ~Server()
    {
        if (connman) connman->Stop();
    }
};

std::unique_ptr<Server> StartServer(const fs::path& datadir, size_t max_clients = 16,
                                    std::chrono::seconds handshake_timeout = std::chrono::seconds{10})
{
    auto server{std::make_unique<Server>()};
    server->authority.MakeNewKey(/*fCompressedIn=*/true);
    server->static_key.MakeNewKey(/*fCompressedIn=*/true);

    auto cert{sv2::MakeCertificate(server->authority, server->static_key, GetTime(), 3650)};
    BOOST_REQUIRE(cert);
    server->cert = *cert;

    sv2::Options opts;
    // Port 0 lets the OS choose, so concurrent test runs cannot collide.
    opts.binds = {LookupNumeric("127.0.0.1", 0)};
    opts.max_clients = max_clients;
    opts.handshake_timeout = handshake_timeout;
    opts.static_key_file = datadir / "unused_static";
    opts.authority_key_file = datadir / "unused_authority";

    server->connman = std::make_unique<sv2::Connman>(opts, server->static_key, server->cert);
    BOOST_REQUIRE(server->connman->Start(/*events=*/nullptr));

    const auto bound{server->connman->BoundAddresses()};
    BOOST_REQUIRE_EQUAL(bound.size(), 1u);
    server->bound = bound.front();
    return server;
}

//! A client speaking Sv2 over a real socket.
struct Client {
    std::unique_ptr<Sock> sock;
    std::optional<sv2::Transport> transport;

    //! Push everything the transport has queued.
    bool Flush()
    {
        const auto out{transport->SendBuffer()};
        if (out.empty()) return true;
        const ssize_t sent{sock->Send(out.data(), out.size(), MSG_NOSIGNAL)};
        if (sent > 0) {
            transport->Sent(static_cast<size_t>(sent));
            return true;
        }
        // A would-block here just means try again on the next pass.
        return sent < 0 && (WSAGetLastError() == WSAEWOULDBLOCK || WSAGetLastError() == WSAEINTR);
    }

    //! Read until `done` is satisfied, the peer closes, or the deadline passes.
    //! Returns false on close or timeout, which the caller distinguishes with
    //! `closed`.
    bool PumpUntil(const std::function<bool()>& done, bool& closed)
    {
        closed = false;
        const auto deadline{std::chrono::steady_clock::now() + TEST_TIMEOUT};
        while (std::chrono::steady_clock::now() < deadline) {
            if (done()) return true;
            if (!Flush()) return false;

            Sock::Event occurred{0};
            if (!sock->Wait(std::chrono::milliseconds{100}, Sock::RECV, &occurred)) return false;
            if (!(occurred & Sock::RECV)) continue;

            uint8_t buf[4096];
            const ssize_t got{sock->Recv(buf, sizeof(buf), 0)};
            if (got == 0) {
                closed = true;
                return false;
            }
            if (got < 0) {
                if (WSAGetLastError() == WSAEWOULDBLOCK || WSAGetLastError() == WSAEINTR) continue;
                return false;
            }
            transport->Received(std::as_bytes(std::span{buf, static_cast<size_t>(got)}));
            if (!transport->Advance(GetTime())) return false;
        }
        return false;
    }
};

std::unique_ptr<Client> Connect(const Server& server, const std::optional<XOnlyPubKey>& authority)
{
    auto client{std::make_unique<Client>()};
    client->sock = CreateSock(server.bound.GetSAFamily(), SOCK_STREAM, IPPROTO_TCP);
    BOOST_REQUIRE(client->sock);

    struct sockaddr_storage storage;
    socklen_t len{sizeof(storage)};
    BOOST_REQUIRE(server.bound.GetSockAddr(reinterpret_cast<struct sockaddr*>(&storage), &len));
    if (client->sock->Connect(reinterpret_cast<struct sockaddr*>(&storage), len) != 0) {
        // CreateSock hands back a non-blocking socket, so a connect in
        // progress is the normal case rather than a failure.
        const int err{WSAGetLastError()};
        BOOST_REQUIRE(err == WSAEINPROGRESS || err == WSAEWOULDBLOCK);
        Sock::Event occurred{0};
        BOOST_REQUIRE(client->sock->Wait(std::chrono::milliseconds{TEST_TIMEOUT}, Sock::SEND, &occurred));
        BOOST_REQUIRE(occurred & Sock::SEND);
        std::string errmsg;
        BOOST_REQUIRE(client->sock->IsConnected(errmsg));
    }

    client->transport.emplace(sv2::Transport::Initiator(authority));
    return client;
}

sv2::SetupConnection MakeSetup(uint8_t protocol = static_cast<uint8_t>(sv2::Protocol::TEMPLATE_DISTRIBUTION),
                               uint32_t flags = 0)
{
    sv2::SetupConnection setup;
    setup.protocol = protocol;
    setup.min_version = sv2::PROTOCOL_VERSION;
    setup.max_version = sv2::PROTOCOL_VERSION;
    setup.flags = flags;
    setup.endpoint_host = "127.0.0.1";
    setup.endpoint_port = 0;
    setup.vendor = "avian-test";
    return setup;
}

//! Complete the Noise handshake, leaving the client ready to send messages.
bool DoHandshake(Client& client)
{
    bool closed{false};
    return client.PumpUntil([&] { return client.transport->HandshakeComplete(); }, closed);
}

//! Wait for one message to arrive.
std::optional<sv2::ReceivedMsg> ReadMessage(Client& client, bool& closed)
{
    std::optional<sv2::ReceivedMsg> out;
    client.PumpUntil([&] {
        out = client.transport->Next();
        return out.has_value();
    }, closed);
    return out;
}

} // namespace

BOOST_AUTO_TEST_CASE(handshake_and_setup_connection)
{
    auto server{StartServer(m_args.GetDataDirNet())};
    auto client{Connect(*server, server->AuthorityPubKey())};

    BOOST_REQUIRE(DoHandshake(*client));
    // The client verified the server certificate against the authority key, so
    // it now knows which static key it is talking to.
    BOOST_REQUIRE(client->transport->RemoteStaticKey().has_value());
    BOOST_CHECK(*client->transport->RemoteStaticKey() == XOnlyPubKey{server->static_key.GetPubKey()});

    BOOST_REQUIRE(client->transport->Send(
        sv2::NetHeader{sv2::EXT_CORE, static_cast<uint8_t>(sv2::CoreMsg::SETUP_CONNECTION), 0},
        sv2::EncodePayload(MakeSetup())));

    bool closed{false};
    const auto reply{ReadMessage(*client, closed)};
    BOOST_REQUIRE(reply.has_value());
    BOOST_CHECK_EQUAL(reply->header.msg_type, static_cast<uint8_t>(sv2::CoreMsg::SETUP_CONNECTION_SUCCESS));

    const auto success{sv2::DecodePayload<sv2::SetupConnectionSuccess>(reply->payload)};
    BOOST_CHECK_EQUAL(success.used_version, sv2::PROTOCOL_VERSION);
    BOOST_CHECK_EQUAL(success.flags, 0u);
    BOOST_CHECK_EQUAL(server->connman->ClientCount(), 1u);
}

BOOST_AUTO_TEST_CASE(trust_on_first_use_client)
{
    // A client with no configured authority key still completes the handshake;
    // it simply does not verify the certificate.
    auto server{StartServer(m_args.GetDataDirNet())};
    auto client{Connect(*server, std::nullopt)};
    BOOST_CHECK(DoHandshake(*client));
}

BOOST_AUTO_TEST_CASE(rejects_wrong_protocol)
{
    auto server{StartServer(m_args.GetDataDirNet())};
    auto client{Connect(*server, server->AuthorityPubKey())};
    BOOST_REQUIRE(DoHandshake(*client));

    // The Mining Protocol is not what a Template Provider speaks.
    BOOST_REQUIRE(client->transport->Send(
        sv2::NetHeader{sv2::EXT_CORE, static_cast<uint8_t>(sv2::CoreMsg::SETUP_CONNECTION), 0},
        sv2::EncodePayload(MakeSetup(static_cast<uint8_t>(sv2::Protocol::MINING)))));

    bool closed{false};
    const auto reply{ReadMessage(*client, closed)};
    BOOST_REQUIRE(reply.has_value());
    BOOST_CHECK_EQUAL(reply->header.msg_type, static_cast<uint8_t>(sv2::CoreMsg::SETUP_CONNECTION_ERROR));
    const auto err{sv2::DecodePayload<sv2::SetupConnectionError>(reply->payload)};
    BOOST_CHECK_EQUAL(err.error_code, "unsupported-protocol");
}

BOOST_AUTO_TEST_CASE(rejects_unsupported_flags)
{
    auto server{StartServer(m_args.GetDataDirNet())};
    auto client{Connect(*server, server->AuthorityPubKey())};
    BOOST_REQUIRE(DoHandshake(*client));

    // No flags are defined for template distribution, so the bits we could not
    // honour must be echoed back rather than silently ignored.
    BOOST_REQUIRE(client->transport->Send(
        sv2::NetHeader{sv2::EXT_CORE, static_cast<uint8_t>(sv2::CoreMsg::SETUP_CONNECTION), 0},
        sv2::EncodePayload(MakeSetup(static_cast<uint8_t>(sv2::Protocol::TEMPLATE_DISTRIBUTION), 0x5))));

    bool closed{false};
    const auto reply{ReadMessage(*client, closed)};
    BOOST_REQUIRE(reply.has_value());
    const auto err{sv2::DecodePayload<sv2::SetupConnectionError>(reply->payload)};
    BOOST_CHECK_EQUAL(err.error_code, "unsupported-feature-flags");
    BOOST_CHECK_EQUAL(err.flags, 0x5u);
}

BOOST_AUTO_TEST_CASE(rejects_version_mismatch)
{
    auto server{StartServer(m_args.GetDataDirNet())};
    auto client{Connect(*server, server->AuthorityPubKey())};
    BOOST_REQUIRE(DoHandshake(*client));

    auto setup{MakeSetup()};
    setup.min_version = 3;
    setup.max_version = 4;
    BOOST_REQUIRE(client->transport->Send(
        sv2::NetHeader{sv2::EXT_CORE, static_cast<uint8_t>(sv2::CoreMsg::SETUP_CONNECTION), 0},
        sv2::EncodePayload(setup)));

    bool closed{false};
    const auto reply{ReadMessage(*client, closed)};
    BOOST_REQUIRE(reply.has_value());
    const auto err{sv2::DecodePayload<sv2::SetupConnectionError>(reply->payload)};
    BOOST_CHECK_EQUAL(err.error_code, "protocol-version-mismatch");
}

BOOST_AUTO_TEST_CASE(message_before_setup_disconnects)
{
    auto server{StartServer(m_args.GetDataDirNet())};
    auto client{Connect(*server, server->AuthorityPubKey())};
    BOOST_REQUIRE(DoHandshake(*client));

    // There is no session yet, so there is nothing to answer on: the only
    // correct response is to close.
    BOOST_REQUIRE(client->transport->Send(
        sv2::NetHeader{sv2::EXT_CORE, static_cast<uint8_t>(sv2::CoreMsg::REQUEST_TRANSACTION_DATA), 0},
        sv2::EncodePayload(sv2::RequestTransactionData{1})));

    bool closed{false};
    ReadMessage(*client, closed);
    BOOST_CHECK(closed);
}

BOOST_AUTO_TEST_CASE(template_message_refused_without_provider)
{
    // No EventsInterface is attached, so the node cannot serve templates.
    // Accepting the session and then ignoring requests would be worse than
    // closing, so the client is dropped.
    auto server{StartServer(m_args.GetDataDirNet())};
    auto client{Connect(*server, server->AuthorityPubKey())};
    BOOST_REQUIRE(DoHandshake(*client));

    BOOST_REQUIRE(client->transport->Send(
        sv2::NetHeader{sv2::EXT_CORE, static_cast<uint8_t>(sv2::CoreMsg::SETUP_CONNECTION), 0},
        sv2::EncodePayload(MakeSetup())));
    bool closed{false};
    BOOST_REQUIRE(ReadMessage(*client, closed).has_value());

    BOOST_REQUIRE(client->transport->Send(
        sv2::NetHeader{sv2::EXT_CORE, static_cast<uint8_t>(sv2::CoreMsg::REQUEST_TRANSACTION_DATA), 0},
        sv2::EncodePayload(sv2::RequestTransactionData{1})));
    ReadMessage(*client, closed);
    BOOST_CHECK(closed);
}

BOOST_AUTO_TEST_CASE(max_clients_enforced)
{
    auto server{StartServer(m_args.GetDataDirNet(), /*max_clients=*/1)};

    auto first{Connect(*server, server->AuthorityPubKey())};
    BOOST_REQUIRE(DoHandshake(*first));
    BOOST_CHECK_EQUAL(server->connman->ClientCount(), 1u);

    // The second connection is refused at accept, before any crypto, so the
    // handshake never completes.
    auto second{Connect(*server, server->AuthorityPubKey())};
    bool closed{false};
    BOOST_CHECK(!second->PumpUntil([&] { return second->transport->HandshakeComplete(); }, closed));
    BOOST_CHECK_EQUAL(server->connman->ClientCount(), 1u);
}

BOOST_AUTO_TEST_CASE(handshake_timeout_drops_idle_client)
{
    // A client that connects and says nothing holds a slot, so it must be
    // reaped rather than kept forever.
    auto server{StartServer(m_args.GetDataDirNet(), /*max_clients=*/16,
                            /*handshake_timeout=*/std::chrono::seconds{1})};
    auto client{Connect(*server, server->AuthorityPubKey())};

    const auto deadline{std::chrono::steady_clock::now() + TEST_TIMEOUT};
    while (server->connman->ClientCount() > 0 && std::chrono::steady_clock::now() < deadline) {
        UninterruptibleSleep(std::chrono::milliseconds{50});
    }
    BOOST_CHECK_EQUAL(server->connman->ClientCount(), 0u);
}

BOOST_AUTO_TEST_SUITE_END()
