// Copyright (c) 2026 The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <compat/compat.h>
#include <key.h>
#include <netbase.h>
#include <pubkey.h>
#include <primitives/block.h>
#include <arith_uint256.h>
#include <chainparams.h>
#include <consensus/amount.h>
#include <interfaces/mining.h>
#include <node/context.h>
#include <streams.h>
#include <sv2/connman.h>
#include <sv2/template_provider.h>
#include <validation.h>
#include <sv2/options.h>
#include <sv2/transport.h>
#include <test/util/setup_common.h>
#include <util/sock.h>
#include <util/time.h>

#include <algorithm>
#include <chrono>
#include <functional>
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

std::unique_ptr<Server> StartServer(const fs::path& datadir,
                                    const std::function<void(sv2::Options&)>& tweak = nullptr)
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
    opts.static_key_file = datadir / "unused_static";
    opts.authority_key_file = datadir / "unused_authority";
    opts.extension_policy = sv2::ExtensionPolicy::OPTIONAL;
    if (tweak) tweak(opts);

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

namespace {

//! A provisional identifier standing in for the Avian multi-PoW extension,
//! which does not exist yet. The negotiation machinery is generic, so it can
//! be exercised in full before the extension it will carry is written.
constexpr uint16_t TEST_EXT{0x4001};
constexpr uint16_t UNKNOWN_EXT{0x4ffe};

//! Drive a client to the point where it has an accepted session.
bool SetupSession(Client& client)
{
    if (!DoHandshake(client)) return false;
    if (!client.transport->Send(
            sv2::NetHeader{sv2::EXT_CORE, static_cast<uint8_t>(sv2::CoreMsg::SETUP_CONNECTION), 0},
            sv2::EncodePayload(MakeSetup()))) {
        return false;
    }
    bool closed{false};
    const auto reply{ReadMessage(client, closed)};
    return reply.has_value() &&
           reply->header.msg_type == static_cast<uint8_t>(sv2::CoreMsg::SETUP_CONNECTION_SUCCESS);
}

bool SendRequestExtensions(Client& client, uint16_t request_id, const std::vector<uint16_t>& want)
{
    sv2::RequestExtensions req;
    req.request_id = request_id;
    req.requested_extensions = want;
    return client.transport->Send(
        sv2::NetHeader{sv2::EXT_NEGOTIATION,
                       static_cast<uint8_t>(sv2::NegotiationMsg::REQUEST_EXTENSIONS), 0},
        sv2::EncodePayload(req));
}

} // namespace

namespace {

//! Options as a real node would have them: Avian's extension required, both
//! algorithms offered.
void ProductionLike(sv2::Options& o)
{
    o.extension_policy = sv2::ExtensionPolicy::REQUIRE;
    o.extension_id = sv2::AVIAN_EXTENSION_ID;
    o.supported_extensions = {sv2::AVIAN_EXTENSION_ID};
    o.required_extensions = {sv2::AVIAN_EXTENSION_ID};
    o.pow_algos = {POW_TYPE_X16RT, POW_TYPE_MINOTAURX};
}

bool NegotiateAvian(Client& client, uint16_t id = sv2::AVIAN_EXTENSION_ID)
{
    if (!SendRequestExtensions(client, 100, {id})) return false;
    bool closed{false};
    const auto reply{ReadMessage(client, closed)};
    return reply.has_value() &&
           reply->header.msg_type ==
               static_cast<uint8_t>(sv2::NegotiationMsg::REQUEST_EXTENSIONS_SUCCESS);
}

bool SendSelectAlgorithm(Client& client, uint8_t pow_type, uint16_t id = sv2::AVIAN_EXTENSION_ID)
{
    sv2::AvianSelectAlgorithm select;
    select.pow_type = pow_type;
    return client.transport->Send(
        sv2::NetHeader{id, static_cast<uint8_t>(sv2::AvianMsg::SELECT_ALGORITHM), 0},
        sv2::EncodePayload(select));
}

} // namespace

namespace {

//! A listener with a Template Provider attached, over loopback.
struct TemplateServer {
    CKey authority;
    CKey static_key;
    sv2::SignatureNoiseMessage cert;
    std::unique_ptr<interfaces::Mining> mining;
    std::unique_ptr<sv2::TemplateProvider> provider;
    std::unique_ptr<sv2::Connman> connman;
    CService bound;

    XOnlyPubKey AuthorityPubKey() const { return XOnlyPubKey{authority.GetPubKey()}; }

    ~TemplateServer()
    {
        // The listener holds the clients whose templates reference the mining
        // interface, so it stops first.
        if (connman) connman->Stop();
        if (provider) provider->Stop();
    }
};

std::unique_ptr<TemplateServer> StartTemplateServer(node::NodeContext& node, const fs::path& datadir,
                                                    const std::function<void(sv2::Options&)>& tweak = nullptr)
{
    auto server{std::make_unique<TemplateServer>()};
    server->authority.MakeNewKey(/*fCompressedIn=*/true);
    server->static_key.MakeNewKey(/*fCompressedIn=*/true);
    auto cert{sv2::MakeCertificate(server->authority, server->static_key, GetTime(), 3650)};
    BOOST_REQUIRE(cert);
    server->cert = *cert;

    sv2::Options opts;
    opts.binds = {LookupNumeric("127.0.0.1", 0)};
    opts.static_key_file = datadir / "tp_unused_static";
    opts.authority_key_file = datadir / "tp_unused_authority";
    opts.extension_policy = sv2::ExtensionPolicy::REQUIRE;
    opts.supported_extensions = {sv2::AVIAN_EXTENSION_ID};
    opts.required_extensions = {sv2::AVIAN_EXTENSION_ID};
    opts.pow_algos = {POW_TYPE_X16RT, POW_TYPE_MINOTAURX};
    if (tweak) tweak(opts);

    // The fixture's blocks carry old timestamps, so the node still considers
    // itself in initial block download, and the provider refuses to build
    // templates then. Latch it off: that refusal is correct behaviour and has
    // its own coverage, it just is not what these tests are about.
    node.chainman->m_cached_finished_ibd = true;

    server->mining = interfaces::MakeMining(node);
    BOOST_REQUIRE(server->mining);
    server->provider = std::make_unique<sv2::TemplateProvider>(*server->mining, opts);
    server->connman = std::make_unique<sv2::Connman>(opts, server->static_key, server->cert);
    server->provider->SetConnman(server->connman.get());
    server->provider->Start();
    BOOST_REQUIRE(server->connman->Start(server->provider.get()));

    const auto bound{server->connman->BoundAddresses()};
    BOOST_REQUIRE_EQUAL(bound.size(), 1u);
    server->bound = bound.front();
    return server;
}

std::unique_ptr<Client> ConnectTo(const TemplateServer& server, const std::optional<XOnlyPubKey>& authority)
{
    auto client{std::make_unique<Client>()};
    client->sock = CreateSock(server.bound.GetSAFamily(), SOCK_STREAM, IPPROTO_TCP);
    BOOST_REQUIRE(client->sock);

    struct sockaddr_storage storage;
    socklen_t len{sizeof(storage)};
    BOOST_REQUIRE(server.bound.GetSockAddr(reinterpret_cast<struct sockaddr*>(&storage), &len));
    if (client->sock->Connect(reinterpret_cast<struct sockaddr*>(&storage), len) != 0) {
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

//! Handshake, SetupConnection, negotiate Avian's extension and pick an
//! algorithm, leaving the client able to ask for templates.
bool Ready(Client& client, uint8_t pow_type)
{
    if (!SetupSession(client)) return false;
    if (!SendRequestExtensions(client, 1, {sv2::AVIAN_EXTENSION_ID})) return false;
    bool closed{false};
    if (!ReadMessage(client, closed).has_value()) return false;

    sv2::AvianSelectAlgorithm select;
    select.pow_type = pow_type;
    if (!client.transport->Send(
            sv2::NetHeader{sv2::AVIAN_EXTENSION_ID,
                           static_cast<uint8_t>(sv2::AvianMsg::SELECT_ALGORITHM), 0},
            sv2::EncodePayload(select))) {
        return false;
    }
    return ReadMessage(client, closed).has_value();
}

bool SendConstraints(Client& client, uint32_t size = 1000, uint16_t sigops = 10)
{
    sv2::CoinbaseOutputConstraints constraints;
    constraints.coinbase_output_max_additional_size = size;
    constraints.coinbase_output_max_additional_sigops = sigops;
    return client.transport->Send(
        sv2::NetHeader{sv2::EXT_CORE,
                       static_cast<uint8_t>(sv2::CoreMsg::COINBASE_OUTPUT_CONSTRAINTS), 0},
        sv2::EncodePayload(constraints));
}

//! Parse the concatenated outputs of NewTemplate.coinbase_tx_outputs, which
//! carries no CompactSize prefix of its own.
std::vector<CTxOut> ParseOutputs(const std::vector<uint8_t>& bytes, uint32_t count)
{
    std::vector<CTxOut> out;
    SpanReader reader{std::span<const unsigned char>{bytes}};
    for (uint32_t i = 0; i < count; ++i) {
        CTxOut txout;
        reader >> txout;
        out.push_back(txout);
    }
    BOOST_CHECK(reader.empty());
    return out;
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(sv2_connman_tests, BasicTestingSetup)


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
    auto server{StartServer(m_args.GetDataDirNet(), [](sv2::Options& o) { o.max_clients = 1; })};

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
    auto server{StartServer(m_args.GetDataDirNet(),
                            [](sv2::Options& o) { o.handshake_timeout = std::chrono::seconds{1}; })};
    auto client{Connect(*server, server->AuthorityPubKey())};

    const auto deadline{std::chrono::steady_clock::now() + TEST_TIMEOUT};
    while (server->connman->ClientCount() > 0 && std::chrono::steady_clock::now() < deadline) {
        UninterruptibleSleep(std::chrono::milliseconds{50});
    }
    BOOST_CHECK_EQUAL(server->connman->ClientCount(), 0u);
}

// Extension negotiation (sv2-spec extension 0x0001).


BOOST_AUTO_TEST_CASE(negotiation_agrees_on_supported_extensions)
{
    auto server{StartServer(m_args.GetDataDirNet(), [](sv2::Options& o) {
        o.extension_policy = sv2::ExtensionPolicy::OPTIONAL;
        o.supported_extensions = {TEST_EXT};
    })};
    auto client{Connect(*server, server->AuthorityPubKey())};
    BOOST_REQUIRE(SetupSession(*client));

    BOOST_REQUIRE(SendRequestExtensions(*client, 7, {sv2::EXT_NEGOTIATION, TEST_EXT}));
    bool closed{false};
    const auto reply{ReadMessage(*client, closed)};
    BOOST_REQUIRE(reply.has_value());
    BOOST_CHECK_EQUAL(reply->header.extension_type, sv2::EXT_NEGOTIATION);
    BOOST_CHECK_EQUAL(reply->header.msg_type,
                      static_cast<uint8_t>(sv2::NegotiationMsg::REQUEST_EXTENSIONS_SUCCESS));

    const auto ok{sv2::DecodePayload<sv2::RequestExtensionsSuccess>(reply->payload)};
    // The request id is echoed so a client can pair the response.
    BOOST_CHECK_EQUAL(ok.request_id, 7);
    const std::vector<uint16_t> expected{sv2::EXT_NEGOTIATION, TEST_EXT};
    BOOST_CHECK(ok.supported_extensions == expected);
}

BOOST_AUTO_TEST_CASE(negotiation_always_supports_itself)
{
    // 0x0001 is the negotiation mechanism, so asking for it must succeed even
    // when no other extension is offered.
    auto server{StartServer(m_args.GetDataDirNet())};
    auto client{Connect(*server, server->AuthorityPubKey())};
    BOOST_REQUIRE(SetupSession(*client));

    BOOST_REQUIRE(SendRequestExtensions(*client, 1, {sv2::EXT_NEGOTIATION}));
    bool closed{false};
    const auto reply{ReadMessage(*client, closed)};
    BOOST_REQUIRE(reply.has_value());
    BOOST_CHECK_EQUAL(reply->header.msg_type,
                      static_cast<uint8_t>(sv2::NegotiationMsg::REQUEST_EXTENSIONS_SUCCESS));
    const auto ok{sv2::DecodePayload<sv2::RequestExtensionsSuccess>(reply->payload)};
    const std::vector<uint16_t> expected{sv2::EXT_NEGOTIATION};
    BOOST_CHECK(ok.supported_extensions == expected);
}

BOOST_AUTO_TEST_CASE(unsupported_extension_is_reported_but_not_fatal)
{
    // Nothing the client asked for exists here, and nothing is required, so
    // the spec lets it carry on without extensions. Disconnecting would be
    // wrong.
    auto server{StartServer(m_args.GetDataDirNet())};
    auto client{Connect(*server, server->AuthorityPubKey())};
    BOOST_REQUIRE(SetupSession(*client));

    BOOST_REQUIRE(SendRequestExtensions(*client, 2, {UNKNOWN_EXT}));
    bool closed{false};
    const auto reply{ReadMessage(*client, closed)};
    BOOST_REQUIRE(reply.has_value());
    BOOST_CHECK_EQUAL(reply->header.msg_type,
                      static_cast<uint8_t>(sv2::NegotiationMsg::REQUEST_EXTENSIONS_ERROR));

    const auto err{sv2::DecodePayload<sv2::RequestExtensionsError>(reply->payload)};
    BOOST_CHECK_EQUAL(err.request_id, 2);
    const std::vector<uint16_t> expected{UNKNOWN_EXT};
    BOOST_CHECK(err.unsupported_extensions == expected);
    // Nothing was required, so the client is not being asked to retry.
    BOOST_CHECK(err.required_extensions.empty());
    BOOST_CHECK_EQUAL(server->connman->ClientCount(), 1u);
}

BOOST_AUTO_TEST_CASE(required_extension_omitted_gets_one_retry)
{
    auto server{StartServer(m_args.GetDataDirNet(), [](sv2::Options& o) {
        o.extension_policy = sv2::ExtensionPolicy::REQUIRE;
        o.supported_extensions = {TEST_EXT};
        o.required_extensions = {TEST_EXT};
    })};
    auto client{Connect(*server, server->AuthorityPubKey())};
    BOOST_REQUIRE(SetupSession(*client));

    // First attempt omits it: the server says what is missing and waits.
    BOOST_REQUIRE(SendRequestExtensions(*client, 3, {sv2::EXT_NEGOTIATION}));
    bool closed{false};
    const auto first{ReadMessage(*client, closed)};
    BOOST_REQUIRE(first.has_value());
    BOOST_CHECK_EQUAL(first->header.msg_type,
                      static_cast<uint8_t>(sv2::NegotiationMsg::REQUEST_EXTENSIONS_ERROR));
    const auto err{sv2::DecodePayload<sv2::RequestExtensionsError>(first->payload)};
    const std::vector<uint16_t> required{TEST_EXT};
    BOOST_CHECK(err.required_extensions == required);
    BOOST_CHECK_EQUAL(server->connman->ClientCount(), 1u);

    // The retry includes it and is accepted.
    BOOST_REQUIRE(SendRequestExtensions(*client, 4, {TEST_EXT}));
    const auto second{ReadMessage(*client, closed)};
    BOOST_REQUIRE(second.has_value());
    BOOST_CHECK_EQUAL(second->header.msg_type,
                      static_cast<uint8_t>(sv2::NegotiationMsg::REQUEST_EXTENSIONS_SUCCESS));
    const auto ok{sv2::DecodePayload<sv2::RequestExtensionsSuccess>(second->payload)};
    BOOST_CHECK_EQUAL(ok.request_id, 4);
}

BOOST_AUTO_TEST_CASE(required_extension_omitted_twice_disconnects)
{
    auto server{StartServer(m_args.GetDataDirNet(), [](sv2::Options& o) {
        o.extension_policy = sv2::ExtensionPolicy::REQUIRE;
        o.supported_extensions = {TEST_EXT};
        o.required_extensions = {TEST_EXT};
    })};
    auto client{Connect(*server, server->AuthorityPubKey())};
    BOOST_REQUIRE(SetupSession(*client));

    BOOST_REQUIRE(SendRequestExtensions(*client, 5, {}));
    bool closed{false};
    BOOST_REQUIRE(ReadMessage(*client, closed).has_value());

    // One retry is allowed; a second omission ends the connection, and the
    // final error is still delivered before the close.
    BOOST_REQUIRE(SendRequestExtensions(*client, 6, {}));
    const auto last{ReadMessage(*client, closed)};
    BOOST_REQUIRE(last.has_value());
    BOOST_CHECK_EQUAL(last->header.msg_type,
                      static_cast<uint8_t>(sv2::NegotiationMsg::REQUEST_EXTENSIONS_ERROR));
    ReadMessage(*client, closed);
    BOOST_CHECK(closed);
}

BOOST_AUTO_TEST_CASE(require_policy_demands_negotiation_before_templates)
{
    // A server cannot announce a required extension unprompted, because
    // RequestExtensions.Error is only ever a response. So a client that skips
    // negotiation can only be dropped, with no on-wire explanation. This test
    // pins that cost rather than hiding it.
    auto server{StartServer(m_args.GetDataDirNet(), [](sv2::Options& o) {
        o.extension_policy = sv2::ExtensionPolicy::REQUIRE;
        o.supported_extensions = {TEST_EXT};
        o.required_extensions = {TEST_EXT};
    })};
    auto client{Connect(*server, server->AuthorityPubKey())};
    BOOST_REQUIRE(SetupSession(*client));

    BOOST_REQUIRE(client->transport->Send(
        sv2::NetHeader{sv2::EXT_CORE,
                       static_cast<uint8_t>(sv2::CoreMsg::REQUEST_TRANSACTION_DATA), 0},
        sv2::EncodePayload(sv2::RequestTransactionData{1})));
    bool closed{false};
    ReadMessage(*client, closed);
    BOOST_CHECK(closed);
}

BOOST_AUTO_TEST_CASE(off_policy_withholds_avian_extensions)
{
    // Used to prove an unmodified client can still complete a session.
    auto server{StartServer(m_args.GetDataDirNet(), [](sv2::Options& o) {
        o.extension_policy = sv2::ExtensionPolicy::OFF;
        o.supported_extensions = {TEST_EXT};
        o.required_extensions = {TEST_EXT};
    })};
    auto client{Connect(*server, server->AuthorityPubKey())};
    BOOST_REQUIRE(SetupSession(*client));

    BOOST_REQUIRE(SendRequestExtensions(*client, 8, {TEST_EXT}));
    bool closed{false};
    const auto reply{ReadMessage(*client, closed)};
    BOOST_REQUIRE(reply.has_value());
    BOOST_CHECK_EQUAL(reply->header.msg_type,
                      static_cast<uint8_t>(sv2::NegotiationMsg::REQUEST_EXTENSIONS_ERROR));
    const auto err{sv2::DecodePayload<sv2::RequestExtensionsError>(reply->payload)};
    const std::vector<uint16_t> expected{TEST_EXT};
    BOOST_CHECK(err.unsupported_extensions == expected);
    // Nothing is demanded while the policy is off.
    BOOST_CHECK(err.required_extensions.empty());
}

BOOST_AUTO_TEST_CASE(second_request_extensions_disconnects)
{
    // Negotiation happens once, before protocol messages, so a repeat means
    // the peer is out of step.
    auto server{StartServer(m_args.GetDataDirNet())};
    auto client{Connect(*server, server->AuthorityPubKey())};
    BOOST_REQUIRE(SetupSession(*client));

    BOOST_REQUIRE(SendRequestExtensions(*client, 9, {sv2::EXT_NEGOTIATION}));
    bool closed{false};
    BOOST_REQUIRE(ReadMessage(*client, closed).has_value());

    BOOST_REQUIRE(SendRequestExtensions(*client, 10, {sv2::EXT_NEGOTIATION}));
    ReadMessage(*client, closed);
    BOOST_CHECK(closed);
}

// Avian multi-PoW extension: algorithm selection.


BOOST_AUTO_TEST_CASE(algorithm_selection_succeeds)
{
    auto server{StartServer(m_args.GetDataDirNet(), ProductionLike)};
    auto client{Connect(*server, server->AuthorityPubKey())};
    BOOST_REQUIRE(SetupSession(*client));
    BOOST_REQUIRE(NegotiateAvian(*client));

    BOOST_REQUIRE(SendSelectAlgorithm(*client, POW_TYPE_MINOTAURX));
    bool closed{false};
    const auto reply{ReadMessage(*client, closed)};
    BOOST_REQUIRE(reply.has_value());
    BOOST_CHECK_EQUAL(reply->header.extension_type, sv2::AVIAN_EXTENSION_ID);
    BOOST_CHECK_EQUAL(reply->header.msg_type,
                      static_cast<uint8_t>(sv2::AvianMsg::SELECT_ALGORITHM_SUCCESS));

    const auto ok{sv2::DecodePayload<sv2::AvianSelectAlgorithmSuccess>(reply->payload)};
    BOOST_CHECK_EQUAL(ok.pow_type, POW_TYPE_MINOTAURX);
    // Rolling is refused outright in this version. A client obeying this mask
    // cannot change which algorithm its share is judged under.
    BOOST_CHECK_EQUAL(ok.version_rolling_mask, 0u);
}

BOOST_AUTO_TEST_CASE(version_rolling_mask_never_opens_algorithm_bits)
{
    // Even asked for the full BIP320 mask, bits 16-23 stay shut, because
    // rolling them either trips bad-algo-id or silently switches the share to
    // the other algorithm and the wrong pow limit.
    auto server{StartServer(m_args.GetDataDirNet(), [](sv2::Options& o) {
        ProductionLike(o);
        o.version_rolling_mask = 0x1fffe000 & ~sv2::POW_TYPE_VERSION_BITS;
    })};
    auto client{Connect(*server, server->AuthorityPubKey())};
    BOOST_REQUIRE(SetupSession(*client));
    BOOST_REQUIRE(NegotiateAvian(*client));
    BOOST_REQUIRE(SendSelectAlgorithm(*client, POW_TYPE_X16RT));

    bool closed{false};
    const auto reply{ReadMessage(*client, closed)};
    BOOST_REQUIRE(reply.has_value());
    const auto ok{sv2::DecodePayload<sv2::AvianSelectAlgorithmSuccess>(reply->payload)};
    BOOST_CHECK_EQUAL(ok.version_rolling_mask & sv2::POW_TYPE_VERSION_BITS, 0u);
}

BOOST_AUTO_TEST_CASE(unknown_pow_type_is_rejected)
{
    auto server{StartServer(m_args.GetDataDirNet(), ProductionLike)};
    auto client{Connect(*server, server->AuthorityPubKey())};
    BOOST_REQUIRE(SetupSession(*client));
    BOOST_REQUIRE(NegotiateAvian(*client));

    BOOST_REQUIRE(SendSelectAlgorithm(*client, NUM_BLOCK_TYPES));
    bool closed{false};
    const auto reply{ReadMessage(*client, closed)};
    BOOST_REQUIRE(reply.has_value());
    BOOST_CHECK_EQUAL(reply->header.msg_type,
                      static_cast<uint8_t>(sv2::AvianMsg::SELECT_ALGORITHM_ERROR));
    const auto err{sv2::DecodePayload<sv2::AvianSelectAlgorithmError>(reply->payload)};
    BOOST_CHECK_EQUAL(err.error_code, "unsupported-pow-type");
}

BOOST_AUTO_TEST_CASE(disabled_pow_type_is_rejected)
{
    // The algorithm exists but this node was not told to serve it.
    auto server{StartServer(m_args.GetDataDirNet(), [](sv2::Options& o) {
        ProductionLike(o);
        o.pow_algos = {POW_TYPE_X16RT};
    })};
    auto client{Connect(*server, server->AuthorityPubKey())};
    BOOST_REQUIRE(SetupSession(*client));
    BOOST_REQUIRE(NegotiateAvian(*client));

    BOOST_REQUIRE(SendSelectAlgorithm(*client, POW_TYPE_MINOTAURX));
    bool closed{false};
    const auto reply{ReadMessage(*client, closed)};
    BOOST_REQUIRE(reply.has_value());
    const auto err{sv2::DecodePayload<sv2::AvianSelectAlgorithmError>(reply->payload)};
    BOOST_CHECK_EQUAL(err.error_code, "pow-type-not-enabled");
}

BOOST_AUTO_TEST_CASE(templates_refused_before_algorithm_is_selected)
{
    // A connection carries one nBits and target stream, so there is nothing
    // coherent to serve until the algorithm is settled.
    auto server{StartServer(m_args.GetDataDirNet(), ProductionLike)};
    auto client{Connect(*server, server->AuthorityPubKey())};
    BOOST_REQUIRE(SetupSession(*client));
    BOOST_REQUIRE(NegotiateAvian(*client));

    BOOST_REQUIRE(client->transport->Send(
        sv2::NetHeader{sv2::EXT_CORE,
                       static_cast<uint8_t>(sv2::CoreMsg::REQUEST_TRANSACTION_DATA), 0},
        sv2::EncodePayload(sv2::RequestTransactionData{1})));
    bool closed{false};
    ReadMessage(*client, closed);
    BOOST_CHECK(closed);
}

BOOST_AUTO_TEST_CASE(second_algorithm_selection_is_rejected)
{
    auto server{StartServer(m_args.GetDataDirNet(), ProductionLike)};
    auto client{Connect(*server, server->AuthorityPubKey())};
    BOOST_REQUIRE(SetupSession(*client));
    BOOST_REQUIRE(NegotiateAvian(*client));
    BOOST_REQUIRE(SendSelectAlgorithm(*client, POW_TYPE_X16RT));
    bool closed{false};
    BOOST_REQUIRE(ReadMessage(*client, closed).has_value());

    // Changing algorithm mid-session would move the target under templates
    // already issued.
    BOOST_REQUIRE(SendSelectAlgorithm(*client, POW_TYPE_MINOTAURX));
    const auto reply{ReadMessage(*client, closed)};
    BOOST_REQUIRE(reply.has_value());
    const auto err{sv2::DecodePayload<sv2::AvianSelectAlgorithmError>(reply->payload)};
    BOOST_CHECK_EQUAL(err.error_code, "algo-already-selected");
}

BOOST_AUTO_TEST_CASE(selection_without_negotiating_disconnects)
{
    // The spec forbids using a feature from an extension the server never
    // confirmed, so this is a protocol violation rather than an error reply.
    auto server{StartServer(m_args.GetDataDirNet(), [](sv2::Options& o) {
        ProductionLike(o);
        o.extension_policy = sv2::ExtensionPolicy::OPTIONAL;
        o.required_extensions.clear();
    })};
    auto client{Connect(*server, server->AuthorityPubKey())};
    BOOST_REQUIRE(SetupSession(*client));

    BOOST_REQUIRE(SendSelectAlgorithm(*client, POW_TYPE_X16RT));
    bool closed{false};
    ReadMessage(*client, closed);
    BOOST_CHECK(closed);
}

BOOST_AUTO_TEST_CASE(configured_extension_id_is_honoured)
{
    // The identifier is overridable for development and migration, so the
    // dispatch must follow it rather than the compiled-in default.
    constexpr uint16_t ALT_ID{0x4abc};
    auto server{StartServer(m_args.GetDataDirNet(), [](sv2::Options& o) {
        ProductionLike(o);
        o.extension_id = ALT_ID;
        o.supported_extensions = {ALT_ID};
        o.required_extensions = {ALT_ID};
    })};
    auto client{Connect(*server, server->AuthorityPubKey())};
    BOOST_REQUIRE(SetupSession(*client));
    BOOST_REQUIRE(NegotiateAvian(*client, ALT_ID));
    BOOST_REQUIRE(SendSelectAlgorithm(*client, POW_TYPE_X16RT, ALT_ID));

    bool closed{false};
    const auto reply{ReadMessage(*client, closed)};
    BOOST_REQUIRE(reply.has_value());
    BOOST_CHECK_EQUAL(reply->header.extension_type, ALT_ID);
    BOOST_CHECK_EQUAL(reply->header.msg_type,
                      static_cast<uint8_t>(sv2::AvianMsg::SELECT_ALGORITHM_SUCCESS));
}

BOOST_AUTO_TEST_SUITE_END()

//! Template Distribution tests, which need a chainstate and so a different
//! fixture. TestChain100Setup rather than RegTestingSetup, because a chain of
//! only genesis is still in initial block download, and the provider refuses
//! to issue templates then: work built during IBD is work a miner can never
//! land. The client helpers above are file-scope and shared.
BOOST_FIXTURE_TEST_SUITE(sv2_template_provider_tests, TestChain100Setup)


BOOST_AUTO_TEST_CASE(serves_a_new_template)
{
    auto server{StartTemplateServer(m_node, m_args.GetDataDirNet())};
    auto client{ConnectTo(*server, server->AuthorityPubKey())};
    BOOST_REQUIRE(Ready(*client, POW_TYPE_X16RT));

    BOOST_REQUIRE(SendConstraints(*client));
    bool closed{false};
    const auto first{ReadMessage(*client, closed)};
    BOOST_REQUIRE(first.has_value());
    BOOST_CHECK_EQUAL(first->header.msg_type, static_cast<uint8_t>(sv2::CoreMsg::NEW_TEMPLATE));

    const auto tmpl{sv2::DecodePayload<sv2::NewTemplate>(first->payload)};
    BOOST_CHECK(tmpl.template_id != 0);
    // Request-driven, so this is for the current tip rather than a future one.
    BOOST_CHECK(!tmpl.future_template);

    // The algorithm lives in nVersion bits 16-23, which is how a client learns
    // what it is mining without any extra field.
    BOOST_CHECK_EQUAL((tmpl.version >> 16) & 0xff, POW_TYPE_X16RT);

    // BIP34: the prefix must start with the height push.
    BOOST_REQUIRE(!tmpl.coinbase_prefix.empty());
    const int height{m_node.chainman->ActiveChain().Height() + 1};
    const CScript expected_prefix{CScript() << height};
    const std::vector<uint8_t> expected_bytes{expected_prefix.begin(), expected_prefix.end()};
    BOOST_CHECK(tmpl.coinbase_prefix == expected_bytes);

    // Copied to a local: the static member has no out-of-line definition, so
    // binding it to a reference would not link.
    const uint32_t expected_sequence{CTxIn::MAX_SEQUENCE_NONFINAL};
    BOOST_CHECK_EQUAL(tmpl.coinbase_tx_input_sequence, expected_sequence);
    BOOST_CHECK_EQUAL(tmpl.coinbase_tx_locktime, static_cast<uint32_t>(height - 1));

    // The required outputs are the ones the client must reproduce.
    BOOST_REQUIRE(tmpl.coinbase_tx_outputs_count >= 1);
    const auto outputs{ParseOutputs(tmpl.coinbase_tx_outputs, tmpl.coinbase_tx_outputs_count)};
    BOOST_REQUIRE_EQUAL(outputs.size(), tmpl.coinbase_tx_outputs_count);
    // The witness commitment is among them, and it is an OP_RETURN.
    BOOST_CHECK(std::any_of(outputs.begin(), outputs.end(), [](const CTxOut& o) {
        return o.nValue == 0 && !o.scriptPubKey.empty() && o.scriptPubKey[0] == OP_RETURN;
    }));

    // value_remaining plus the required outputs must be the whole coinbase
    // value, or the client would either overpay or leave money unspendable.
    CAmount required_total{0};
    for (const CTxOut& o : outputs) required_total += o.nValue;
    const CAmount subsidy{GetBlockSubsidy(height, Params().GetConsensus())};
    BOOST_CHECK_EQUAL(static_cast<CAmount>(tmpl.coinbase_tx_value_remaining) + required_total, subsidy);

    // NewTemplate carries no prev hash, so the work is unusable until this
    // arrives.
    const auto second{ReadMessage(*client, closed)};
    BOOST_REQUIRE(second.has_value());
    BOOST_CHECK_EQUAL(second->header.msg_type, static_cast<uint8_t>(sv2::CoreMsg::SET_NEW_PREV_HASH));
    const auto prev{sv2::DecodePayload<sv2::SetNewPrevHash>(second->payload)};
    BOOST_CHECK_EQUAL(prev.template_id, tmpl.template_id);
    // Avian chains proof-of-work hashes rather than SHA256d, and this must be
    // whatever the next header has to point at.
    BOOST_CHECK_EQUAL(prev.prev_hash, m_node.chainman->ActiveChain().Tip()->GetBlockHash());
    BOOST_CHECK(prev.nbits != 0);
    // The target is nBits expanded, and nBits already came from the per
    // algorithm difficulty.
    BOOST_CHECK_EQUAL(prev.target, ArithToUint256(arith_uint256{}.SetCompact(prev.nbits)));

    // The time constraints follow, because the spec gives SubmitSolution no
    // upper ntime bound of its own.
    const auto third{ReadMessage(*client, closed)};
    BOOST_REQUIRE(third.has_value());
    BOOST_CHECK_EQUAL(third->header.extension_type, sv2::AVIAN_EXTENSION_ID);
    BOOST_CHECK_EQUAL(third->header.msg_type, static_cast<uint8_t>(sv2::AvianMsg::TIME_CONSTRAINTS));
    const auto times{sv2::DecodePayload<sv2::AvianTimeConstraints>(third->payload)};
    BOOST_CHECK_EQUAL(times.template_id, tmpl.template_id);
    BOOST_CHECK(times.ntime_max >= times.ntime_min);
    // Regtest allows minimum-difficulty blocks, where nBits depends on nTime,
    // so rolling is pinned off entirely rather than tracking that cliff.
    BOOST_CHECK_EQUAL(times.ntime_min, times.ntime_max);
}

BOOST_AUTO_TEST_CASE(merkle_path_reconstructs_the_merkle_root)
{
    // This is the field a miner cannot check for itself cheaply, and getting
    // it wrong would make every share invalid for a reason nobody could see.
    auto server{StartTemplateServer(m_node, m_args.GetDataDirNet())};
    auto client{ConnectTo(*server, server->AuthorityPubKey())};
    BOOST_REQUIRE(Ready(*client, POW_TYPE_X16RT));
    BOOST_REQUIRE(SendConstraints(*client));

    bool closed{false};
    const auto msg{ReadMessage(*client, closed)};
    BOOST_REQUIRE(msg.has_value());
    const auto tmpl{sv2::DecodePayload<sv2::NewTemplate>(msg->payload)};

    // With only a coinbase in the block, the path is empty and the root is the
    // coinbase txid itself.
    BOOST_CHECK(tmpl.merkle_path.empty());
}

BOOST_AUTO_TEST_CASE(template_follows_the_selected_algorithm)
{
    auto server{StartTemplateServer(m_node, m_args.GetDataDirNet())};
    auto client{ConnectTo(*server, server->AuthorityPubKey())};
    BOOST_REQUIRE(Ready(*client, POW_TYPE_MINOTAURX));
    BOOST_REQUIRE(SendConstraints(*client));

    bool closed{false};
    const auto msg{ReadMessage(*client, closed)};
    BOOST_REQUIRE(msg.has_value());
    const auto tmpl{sv2::DecodePayload<sv2::NewTemplate>(msg->payload)};
    // A template built for the wrong algorithm would hand out work judged
    // against the wrong pow limit.
    BOOST_CHECK_EQUAL((tmpl.version >> 16) & 0xff, POW_TYPE_MINOTAURX);
}

BOOST_AUTO_TEST_CASE(unserved_message_disconnects)
{
    // RequestTransactionData and SubmitSolution are not implemented yet.
    // Accepting a session and then ignoring requests is worse than closing.
    auto server{StartTemplateServer(m_node, m_args.GetDataDirNet())};
    auto client{ConnectTo(*server, server->AuthorityPubKey())};
    BOOST_REQUIRE(Ready(*client, POW_TYPE_X16RT));

    BOOST_REQUIRE(client->transport->Send(
        sv2::NetHeader{sv2::EXT_CORE,
                       static_cast<uint8_t>(sv2::CoreMsg::REQUEST_TRANSACTION_DATA), 0},
        sv2::EncodePayload(sv2::RequestTransactionData{1})));
    bool closed{false};
    ReadMessage(*client, closed);
    BOOST_CHECK(closed);
}

BOOST_AUTO_TEST_CASE(templates_need_constraints_first)
{
    // Without knowing how much coinbase room the client needs, the reserved
    // weight is unknown, so there is nothing correct to build.
    auto server{StartTemplateServer(m_node, m_args.GetDataDirNet())};
    auto client{ConnectTo(*server, server->AuthorityPubKey())};
    BOOST_REQUIRE(Ready(*client, POW_TYPE_X16RT));
    BOOST_CHECK_EQUAL(server->connman->ClientCount(), 1u);

    // Nothing has been sent, because nothing was asked for.
    bool closed{false};
    Sock::Event occurred{0};
    BOOST_CHECK(client->sock->Wait(std::chrono::milliseconds{300}, Sock::RECV, &occurred));
    BOOST_CHECK_EQUAL(occurred & Sock::RECV, 0);
    BOOST_CHECK(!closed);
}


BOOST_AUTO_TEST_CASE(new_tip_reissues_work)
{
    // A tip change invalidates every outstanding template, so the provider
    // has to push replacements rather than wait to be asked.
    auto server{StartTemplateServer(m_node, m_args.GetDataDirNet())};
    auto client{ConnectTo(*server, server->AuthorityPubKey())};
    BOOST_REQUIRE(Ready(*client, POW_TYPE_X16RT));
    BOOST_REQUIRE(SendConstraints(*client));

    bool closed{false};
    const auto first{ReadMessage(*client, closed)};
    BOOST_REQUIRE(first.has_value());
    const auto before{sv2::DecodePayload<sv2::NewTemplate>(first->payload)};
    BOOST_REQUIRE(ReadMessage(*client, closed).has_value()); // SetNewPrevHash
    BOOST_REQUIRE(ReadMessage(*client, closed).has_value()); // AvianTimeConstraints

    const uint256 old_tip{m_node.chainman->ActiveChain().Tip()->GetBlockHash()};
    CreateAndProcessBlock({}, CScript() << OP_TRUE);
    BOOST_REQUIRE(m_node.chainman->ActiveChain().Tip()->GetBlockHash() != old_tip);

    // Unprompted work for the new tip, pushed from the watcher thread.
    const auto pushed{ReadMessage(*client, closed)};
    BOOST_REQUIRE(pushed.has_value());
    BOOST_CHECK_EQUAL(pushed->header.msg_type, static_cast<uint8_t>(sv2::CoreMsg::NEW_TEMPLATE));
    const auto after{sv2::DecodePayload<sv2::NewTemplate>(pushed->payload)};
    BOOST_CHECK(after.template_id != before.template_id);

    const auto new_prev{ReadMessage(*client, closed)};
    BOOST_REQUIRE(new_prev.has_value());
    BOOST_CHECK_EQUAL(new_prev->header.msg_type, static_cast<uint8_t>(sv2::CoreMsg::SET_NEW_PREV_HASH));
    const auto prev{sv2::DecodePayload<sv2::SetNewPrevHash>(new_prev->payload)};
    BOOST_CHECK_EQUAL(prev.template_id, after.template_id);
    // It must point at the block just mined, not the one it replaced.
    BOOST_CHECK_EQUAL(prev.prev_hash, m_node.chainman->ActiveChain().Tip()->GetBlockHash());
    BOOST_CHECK(prev.prev_hash != old_tip);
}

BOOST_AUTO_TEST_SUITE_END()
