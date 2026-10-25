// Copyright (c) 2026 The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <sv2/connman.h>

#include <common/args.h>
#include <compat/compat.h>
#include <logging.h>
#include <netbase.h>
#include <pubkey.h>
#include <tinyformat.h>
#include <util/thread.h>
#include <util/time.h>

#include <algorithm>
#include <cassert>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace sv2 {

namespace {
//! True if a socket error means the operation can be retried, matching the
//! set net.cpp treats as transient.
bool IsTransientError(int err)
{
    return err == WSAEWOULDBLOCK || err == WSAEMSGSIZE || err == WSAEINTR || err == WSAEINPROGRESS;
}

//! Bind one listening socket, mirroring CConnman::BindListenPort.
std::shared_ptr<Sock> BindOne(const CService& addr, std::string& error)
{
    struct sockaddr_storage storage;
    socklen_t len{sizeof(storage)};
    if (!addr.GetSockAddr(reinterpret_cast<struct sockaddr*>(&storage), &len)) {
        error = strprintf("Bind address family for %s not supported", addr.ToStringAddrPort());
        return nullptr;
    }

    std::unique_ptr<Sock> sock{CreateSock(addr.GetSAFamily(), SOCK_STREAM, IPPROTO_TCP)};
    if (!sock) {
        error = strprintf("Cannot create socket for %s: %s", addr.ToStringAddrPort(),
                          NetworkErrorString(WSAGetLastError()));
        return nullptr;
    }

    const int one{1};
    if (sock->SetSockOpt(SOL_SOCKET, SO_REUSEADDR, (sockopt_arg_type)&one, sizeof(one)) == SOCKET_ERROR) {
        LogPrintf("sv2: cannot set SO_REUSEADDR on %s, continuing anyway\n", addr.ToStringAddrPort());
    }

    if (addr.IsIPv6()) {
#ifdef IPV6_V6ONLY
        if (sock->SetSockOpt(IPPROTO_IPV6, IPV6_V6ONLY, (sockopt_arg_type)&one, sizeof(one)) == SOCKET_ERROR) {
            LogPrintf("sv2: cannot set IPV6_V6ONLY on %s, continuing anyway\n", addr.ToStringAddrPort());
        }
#endif
#ifdef WIN32
        const int prot_level{PROTECTION_LEVEL_UNRESTRICTED};
        if (sock->SetSockOpt(IPPROTO_IPV6, IPV6_PROTECTION_LEVEL, (const char*)&prot_level, sizeof(prot_level)) == SOCKET_ERROR) {
            LogPrintf("sv2: cannot set IPV6_PROTECTION_LEVEL on %s, continuing anyway\n", addr.ToStringAddrPort());
        }
#endif
    }

    if (sock->Bind(reinterpret_cast<struct sockaddr*>(&storage), len) == SOCKET_ERROR) {
        const int err{WSAGetLastError()};
        error = err == WSAEADDRINUSE
                    ? strprintf("Cannot bind to %s: address already in use", addr.ToStringAddrPort())
                    : strprintf("Cannot bind to %s: %s", addr.ToStringAddrPort(), NetworkErrorString(err));
        return nullptr;
    }

    if (sock->Listen(SOMAXCONN) == SOCKET_ERROR) {
        error = strprintf("Cannot listen on %s: %s", addr.ToStringAddrPort(),
                          NetworkErrorString(WSAGetLastError()));
        return nullptr;
    }

    LogPrintf("sv2: listening on %s\n", addr.ToStringAddrPort());
    return std::shared_ptr<Sock>{sock.release()};
}
} // namespace

Connman::Connman(Options opts, CKey static_key, SignatureNoiseMessage cert)
    : m_opts{std::move(opts)}, m_static_key{std::move(static_key)}, m_cert{std::move(cert)}
{
}

Connman::~Connman() { Stop(); }

bool Connman::Start(EventsInterface* events)
{
    m_events = events;
    for (const CService& addr : m_opts.binds) {
        std::string error;
        auto sock{BindOne(addr, error)};
        if (!sock) {
            LogError("sv2: %s\n", error);
            m_listen.clear();
            return false;
        }
        m_listen.push_back(std::move(sock));
    }
    m_thread = std::thread{&util::TraceThread, "sv2", [this] { ThreadHandler(); }};
    return true;
}

void Connman::Interrupt() { m_interrupt(); }

void Connman::Stop()
{
    m_interrupt();
    if (m_thread.joinable()) m_thread.join();
    LOCK(m_mutex);
    m_clients.clear();
    m_listen.clear();
}

size_t Connman::ClientCount() const
{
    LOCK(m_mutex);
    return m_clients.size();
}

std::vector<CService> Connman::BoundAddresses() const
{
    std::vector<CService> out;
    for (const auto& sock : m_listen) {
        struct sockaddr_storage storage;
        socklen_t len{sizeof(storage)};
        if (sock->GetSockName(reinterpret_cast<struct sockaddr*>(&storage), &len) != 0) continue;
        CService addr;
        if (addr.SetSockAddr(reinterpret_cast<const struct sockaddr*>(&storage), len)) out.push_back(addr);
    }
    return out;
}

bool Connman::IsAllowed(const CService& addr) const
{
    if (m_opts.allow.empty()) return addr.IsLocal();
    return std::any_of(m_opts.allow.begin(), m_opts.allow.end(),
                       [&addr](const CSubNet& net) { return net.Match(addr); });
}

bool Connman::Send(ClientId id, const NetHeader& header, std::span<const uint8_t> payload)
{
    LOCK(m_mutex);
    for (auto& client : m_clients) {
        if (client.id != id) continue;
        if (client.state != State::READY || client.disconnect) return false;
        if (!client.transport.Send(header, payload)) return false;
        if (client.transport.SendBuffer().size() > m_opts.max_send_buffer) {
            LogPrintLevel(BCLog::SV2, BCLog::Level::Warning,
                          "client=%d send buffer over %d MiB, disconnecting\n", id,
                          m_opts.max_send_buffer >> 20);
            client.disconnect = true;
            return false;
        }
        return true;
    }
    return false;
}

void Connman::Disconnect(ClientId id)
{
    LOCK(m_mutex);
    for (auto& client : m_clients) {
        if (client.id == id) client.disconnect = true;
    }
}

template <typename M>
bool Connman::SendTo(Client& client, uint16_t ext, uint8_t type, const M& msg)
{
    const auto payload{EncodePayload(msg)};
    return client.transport.Send(NetHeader{ext, type, 0}, payload);
}

template <typename M>
bool Connman::Reject(Client& client, uint16_t ext, uint8_t type, const M& msg, const std::string& why)
{
    LogPrintLevel(BCLog::SV2, BCLog::Level::Debug, "client=%d rejected: %s\n", client.id, why);
    (void)SendTo(client, ext, type, msg);
    // Drain rather than disconnect, so the reply actually gets written. Closing
    // the moment it is queued would leave the client knowing only that it was
    // dropped.
    client.drain = true;
    client.drain_until = std::chrono::steady_clock::now() + DRAIN_TIMEOUT;
    return false;
}

bool Connman::HandleSetupConnection(Client& client, const ReceivedMsg& msg)
{
    SetupConnection setup;
    try {
        setup = DecodePayload<SetupConnection>(msg.payload);
    } catch (const std::ios_base::failure& e) {
        LogPrintLevel(BCLog::SV2, BCLog::Level::Debug,
                      "client=%d sent a malformed SetupConnection: %s\n", client.id, e.what());
        return false;
    }

    if (setup.protocol != static_cast<uint8_t>(Protocol::TEMPLATE_DISTRIBUTION)) {
        return Reject(client, EXT_CORE, static_cast<uint8_t>(CoreMsg::SETUP_CONNECTION_ERROR),
                      SetupConnectionError{0, "unsupported-protocol"}, "unsupported-protocol");
    }
    if (setup.min_version > PROTOCOL_VERSION || setup.max_version < PROTOCOL_VERSION) {
        return Reject(client, EXT_CORE, static_cast<uint8_t>(CoreMsg::SETUP_CONNECTION_ERROR),
                      SetupConnectionError{0, "protocol-version-mismatch"}, "protocol-version-mismatch");
    }
    if (setup.flags != 0) {
        // No flags are defined for template distribution, so echo back the
        // bits we could not honour rather than pretending we did.
        return Reject(client, EXT_CORE, static_cast<uint8_t>(CoreMsg::SETUP_CONNECTION_ERROR),
                      SetupConnectionError{setup.flags, "unsupported-feature-flags"},
                      "unsupported-feature-flags");
    }

    if (!SendTo(client, EXT_CORE, static_cast<uint8_t>(CoreMsg::SETUP_CONNECTION_SUCCESS),
                SetupConnectionSuccess{PROTOCOL_VERSION, 0})) {
        return false;
    }

    LogPrintLevel(BCLog::SV2, BCLog::Level::Debug,
                  "client=%d setup complete (vendor=%s firmware=%s device=%s)\n", client.id,
                  setup.vendor, setup.firmware, setup.device_id);

    // Negotiation only gates the session when the policy demands it.
    if (m_opts.extension_policy == ExtensionPolicy::REQUIRE) {
        client.state = State::NEGOTIATE;
        return true;
    }
    client.state = State::READY;
    m_pending.push_back(Notification{Notification::Kind::READY, client.id, client.pow_type, {}});
    return true;
}

bool Connman::HandleRequestExtensions(Client& client, const ReceivedMsg& msg)
{
    if (client.extensions_answered) {
        LogPrintLevel(BCLog::SV2, BCLog::Level::Debug,
                      "client=%d sent a second RequestExtensions\n", client.id);
        return false;
    }

    RequestExtensions req;
    try {
        req = DecodePayload<RequestExtensions>(msg.payload);
    } catch (const std::ios_base::failure& e) {
        LogPrintLevel(BCLog::SV2, BCLog::Level::Debug,
                      "client=%d sent a malformed RequestExtensions: %s\n", client.id, e.what());
        return false;
    }

    // Extension 0x0001 is the negotiation mechanism itself, so it is supported
    // by definition. OFF withholds everything else, which is how a stock
    // client can be proven to still work.
    std::set<uint16_t> supported{EXT_NEGOTIATION};
    if (m_opts.extension_policy != ExtensionPolicy::OFF) {
        supported.insert(m_opts.supported_extensions.begin(), m_opts.supported_extensions.end());
    }

    const std::set<uint16_t> requested{req.requested_extensions.begin(), req.requested_extensions.end()};
    std::vector<uint16_t> agreed;
    std::vector<uint16_t> unsupported;
    for (const uint16_t ext : requested) {
        (supported.count(ext) > 0 ? agreed : unsupported).push_back(ext);
    }

    std::vector<uint16_t> missing;
    if (m_opts.extension_policy == ExtensionPolicy::REQUIRE) {
        for (const uint16_t ext : m_opts.required_extensions) {
            if (requested.count(ext) == 0) missing.push_back(ext);
        }
    }

    if (!missing.empty()) {
        RequestExtensionsError err;
        err.request_id = req.request_id;
        err.unsupported_extensions = unsupported;
        err.required_extensions = missing;
        if (client.extensions_retried) {
            return Reject(client, EXT_NEGOTIATION,
                          static_cast<uint8_t>(NegotiationMsg::REQUEST_EXTENSIONS_ERROR), err,
                          "a required extension was still missing after a retry");
        }
        // The spec allows one further attempt, so say what is missing and wait
        // rather than closing on the first omission.
        client.extensions_retried = true;
        LogPrintLevel(BCLog::SV2, BCLog::Level::Debug,
                      "client=%d omitted %d required extension(s), awaiting a retry\n", client.id,
                      missing.size());
        return SendTo(client, EXT_NEGOTIATION,
                      static_cast<uint8_t>(NegotiationMsg::REQUEST_EXTENSIONS_ERROR), err);
    }

    if (!requested.empty() && agreed.empty()) {
        // Nothing the client asked for exists here. Not fatal: with nothing
        // required, the spec lets it carry on without extensions.
        RequestExtensionsError err;
        err.request_id = req.request_id;
        err.unsupported_extensions = unsupported;
        if (!SendTo(client, EXT_NEGOTIATION,
                    static_cast<uint8_t>(NegotiationMsg::REQUEST_EXTENSIONS_ERROR), err)) {
            return false;
        }
    } else {
        RequestExtensionsSuccess ok;
        ok.request_id = req.request_id;
        ok.supported_extensions = agreed;
        if (!SendTo(client, EXT_NEGOTIATION,
                    static_cast<uint8_t>(NegotiationMsg::REQUEST_EXTENSIONS_SUCCESS), ok)) {
            return false;
        }
    }

    client.extensions_answered = true;
    client.avian_extension =
        std::find(agreed.begin(), agreed.end(), m_opts.extension_id) != agreed.end();

    if (client.state != State::NEGOTIATE) return true;

    // With Avian's extension agreed, an algorithm must be chosen before any
    // template can be built for this connection.
    if (client.avian_extension) {
        client.state = State::SELECT_ALGO;
        return true;
    }
    client.state = State::READY;
    m_pending.push_back(Notification{Notification::Kind::READY, client.id, client.pow_type, {}});
    return true;
}

bool Connman::HandleAvianSelectAlgorithm(Client& client, const ReceivedMsg& msg)
{
    AvianSelectAlgorithm select;
    try {
        select = DecodePayload<AvianSelectAlgorithm>(msg.payload);
    } catch (const std::ios_base::failure& e) {
        LogPrintLevel(BCLog::SV2, BCLog::Level::Debug,
                      "client=%d sent a malformed AvianSelectAlgorithm: %s\n", client.id, e.what());
        return false;
    }

    // Not a lambda: a lambda body hides the enclosing function's lock context
    // from clang's thread-safety analysis, and Reject needs m_mutex held.
    if (select.pow_type >= NUM_BLOCK_TYPES) {
        AvianSelectAlgorithmError err;
        err.error_code = "unsupported-pow-type";
        return Reject(client, m_opts.extension_id,
                      static_cast<uint8_t>(AvianMsg::SELECT_ALGORITHM_ERROR), err, err.error_code);
    }
    const auto algo{static_cast<POW_TYPE>(select.pow_type)};
    if (std::find(m_opts.pow_algos.begin(), m_opts.pow_algos.end(), algo) == m_opts.pow_algos.end()) {
        AvianSelectAlgorithmError err;
        err.error_code = "pow-type-not-enabled";
        return Reject(client, m_opts.extension_id,
                      static_cast<uint8_t>(AvianMsg::SELECT_ALGORITHM_ERROR), err, err.error_code);
    }

    AvianSelectAlgorithmSuccess ok;
    ok.pow_type = select.pow_type;
    // Zero in this version. Bits 16-23 can never be opened, so a client that
    // obeys this mask cannot change which algorithm its share is judged under.
    ok.version_rolling_mask = m_opts.version_rolling_mask;
    if (!SendTo(client, m_opts.extension_id,
                static_cast<uint8_t>(AvianMsg::SELECT_ALGORITHM_SUCCESS), ok)) {
        return false;
    }

    client.pow_type = algo;
    client.algo_selected = true;
    client.state = State::READY;
    LogPrintLevel(BCLog::SV2, BCLog::Level::Debug,
                  "client=%d selected %s, version rolling mask 0x%08x\n", client.id,
                  POW_TYPE_NAMES[algo], m_opts.version_rolling_mask);
    m_pending.push_back(Notification{Notification::Kind::READY, client.id, client.pow_type, {}});
    return true;
}

bool Connman::HandleCommon(Client& client, const ReceivedMsg& msg)
{
    // A channel-scoped message has no meaning on a Template Distribution
    // connection, so its presence means the peer is speaking the Mining
    // Protocol at us.
    if (msg.header.IsChannelMsg()) {
        LogPrintLevel(BCLog::SV2, BCLog::Level::Debug,
                      "client=%d sent a channel message on a template connection\n", client.id);
        return false;
    }

    const bool is_setup{msg.header.Extension() == EXT_CORE &&
                        msg.header.msg_type == static_cast<uint8_t>(CoreMsg::SETUP_CONNECTION)};
    const bool is_request_extensions{msg.header.Extension() == EXT_NEGOTIATION &&
                                     msg.header.msg_type ==
                                         static_cast<uint8_t>(NegotiationMsg::REQUEST_EXTENSIONS)};
    const bool is_avian_select{msg.header.Extension() == m_opts.extension_id &&
                               msg.header.msg_type ==
                                   static_cast<uint8_t>(AvianMsg::SELECT_ALGORITHM)};

    switch (client.state) {
    case State::SETUP:
        if (!is_setup) {
            // There is no session to reply on yet, so the only option is to
            // close.
            LogPrintLevel(BCLog::SV2, BCLog::Level::Debug,
                          "client=%d sent msg_type=0x%02x before SetupConnection\n", client.id,
                          msg.header.msg_type);
            return false;
        }
        return HandleSetupConnection(client, msg);

    case State::NEGOTIATE:
        if (is_setup) {
            // Renegotiating a live session is not something the protocol has a
            // notion of.
            LogPrintLevel(BCLog::SV2, BCLog::Level::Debug,
                          "client=%d sent a second SetupConnection\n", client.id);
            return false;
        }
        if (!is_request_extensions) {
            // A server cannot announce a required extension unprompted:
            // RequestExtensions.Error is only ever a response. So a client that
            // never negotiates can only be dropped, with no on-wire
            // explanation. That is the documented cost of this policy.
            LogPrintLevel(BCLog::SV2, BCLog::Level::Debug,
                          "client=%d sent msg_type=0x%02x instead of negotiating extensions\n",
                          client.id, msg.header.msg_type);
            return false;
        }
        return HandleRequestExtensions(client, msg);

    case State::SELECT_ALGO:
        if (!is_avian_select) {
            LogPrintLevel(BCLog::SV2, BCLog::Level::Debug,
                          "client=%d sent msg_type=0x%02x before selecting an algorithm\n",
                          client.id, msg.header.msg_type);
            return false;
        }
        return HandleAvianSelectAlgorithm(client, msg);

    case State::READY:
        if (is_setup) {
            LogPrintLevel(BCLog::SV2, BCLog::Level::Debug,
                          "client=%d sent a second SetupConnection\n", client.id);
            return false;
        }
        if (is_request_extensions) return HandleRequestExtensions(client, msg);
        if (is_avian_select) {
            if (!client.avian_extension) {
                // The spec forbids using a feature from an extension the
                // server never confirmed.
                LogPrintLevel(BCLog::SV2, BCLog::Level::Debug,
                              "client=%d used extension 0x%04x without negotiating it\n", client.id,
                              m_opts.extension_id);
                return false;
            }
            // Being READY is not evidence that an algorithm was chosen.
            // Under the optional and off policies a client reaches READY
            // straight from SetupConnection, so its first selection arrives
            // here and has to be served rather than refused as a second one.
            // Without this, -sv2extensionpolicy=optional cannot serve
            // minotaurx to anyone at all.
            if (!client.algo_selected) return HandleAvianSelectAlgorithm(client, msg);

            // A real second selection. The algorithm fixes this connection's
            // nBits and target stream, so it cannot be changed underneath
            // templates already issued.
            AvianSelectAlgorithmError err;
            err.error_code = "algo-already-selected";
            return Reject(client, m_opts.extension_id,
                          static_cast<uint8_t>(AvianMsg::SELECT_ALGORITHM_ERROR), err,
                          "algo-already-selected");
        }
        if (!m_events) {
            LogPrintLevel(BCLog::SV2, BCLog::Level::Debug,
                          "client=%d sent msg_type=0x%02x but no template provider is attached\n",
                          client.id, msg.header.msg_type);
            return false;
        }
        m_pending.push_back(Notification{Notification::Kind::MESSAGE, client.id, client.pow_type, msg});
        return true;
    }
    return false;
}

Sock::EventsPerSock Connman::GenerateWaitSockets()
{
    Sock::EventsPerSock events;
    for (const auto& sock : m_listen) {
        events.emplace(sock, Sock::Events{Sock::RECV});
    }
    for (const auto& client : m_clients) {
        if (client.disconnect) continue;
        Sock::Event req{client.drain ? Sock::Event{0} : Sock::Event{Sock::RECV}};
        if (!client.transport.SendBufferEmpty()) req |= Sock::SEND;
        events.emplace(client.sock, Sock::Events{req});
    }
    return events;
}

void Connman::AcceptNew(const Sock::EventsPerSock& events)
{
    for (const auto& listen : m_listen) {
        const auto it{events.find(listen)};
        if (it == events.end() || !(it->second.occurred & Sock::RECV)) continue;

        struct sockaddr_storage storage;
        socklen_t len{sizeof(storage)};
        auto sock{listen->Accept(reinterpret_cast<struct sockaddr*>(&storage), &len)};
        if (!sock) continue;

        CService addr;
        if (!addr.SetSockAddr(reinterpret_cast<const struct sockaddr*>(&storage), len)) {
            LogPrintLevel(BCLog::SV2, BCLog::Level::Warning, "accepted a socket of unknown family\n");
            continue;
        }

        // Everything below is checked before a single byte is read, let alone
        // decrypted, because the handshake does not authenticate the client.
        if (!IsAllowed(addr)) {
            LogPrintLevel(BCLog::SV2, BCLog::Level::Debug, "refused %s: not in -sv2allowip\n",
                          addr.ToStringAddrPort());
            continue;
        }
        if (m_clients.size() >= m_opts.max_clients) {
            LogPrintLevel(BCLog::SV2, BCLog::Level::Debug, "refused %s: -sv2maxclients reached\n",
                          addr.ToStringAddrPort());
            continue;
        }
        const size_t from_ip{static_cast<size_t>(std::count_if(
            m_clients.begin(), m_clients.end(),
            [&addr](const Client& c) { return static_cast<CNetAddr>(c.addr) == static_cast<CNetAddr>(addr); }))};
        if (from_ip >= m_opts.max_clients_per_ip) {
            LogPrintLevel(BCLog::SV2, BCLog::Level::Debug, "refused %s: -sv2maxclientsperip reached\n",
                          addr.ToStringAddrPort());
            continue;
        }
        if (!sock->SetNonBlocking()) {
            LogPrintLevel(BCLog::SV2, BCLog::Level::Warning, "cannot set %s non-blocking\n",
                          addr.ToStringAddrPort());
            continue;
        }

        const ClientId id{m_next_id++};
        // The remaining members use their default initializers.
        m_clients.push_back(Client{id, std::shared_ptr<Sock>{sock.release()}, addr,
                                   Transport::Responder(m_static_key, m_cert),
                                   std::chrono::steady_clock::now()});
        m_clients.back().pow_type =
            m_opts.pow_algos.empty() ? POW_TYPE_X16RT : m_opts.pow_algos.front();
        LogPrintLevel(BCLog::SV2, BCLog::Level::Debug, "client=%d connected from %s\n", id,
                      addr.ToStringAddrPort());
    }
}

void Connman::ServiceClients(const Sock::EventsPerSock& events)
{
    const auto now{std::chrono::steady_clock::now()};
    const int64_t unix_now{GetTime()};

    for (auto& client : m_clients) {
        if (client.disconnect) continue;

        // A client that connects and then says nothing holds a slot, so bound
        // how long it may do so.
        if (client.state != State::READY && now - client.connected > m_opts.handshake_timeout) {
            LogPrintLevel(BCLog::SV2, BCLog::Level::Debug,
                          "client=%d did not finish setup within -sv2handshaketimeout\n", client.id);
            client.disconnect = true;
            continue;
        }

        const auto it{events.find(client.sock)};
        if (it == events.end()) continue;
        const auto occurred{it->second.occurred};

        if (occurred & Sock::ERR) {
            client.disconnect = true;
            continue;
        }

        if (occurred & Sock::SEND) {
            const auto out{client.transport.SendBuffer()};
            if (!out.empty()) {
                const ssize_t sent{client.sock->Send(out.data(), out.size(), MSG_NOSIGNAL | MSG_DONTWAIT)};
                if (sent > 0) {
                    client.transport.Sent(static_cast<size_t>(sent));
                } else if (sent < 0 && !IsTransientError(WSAGetLastError())) {
                    client.disconnect = true;
                    continue;
                }
            }
        }

        if (client.drain) {
            // Finished once the reply is out, or once the peer has clearly
            // stopped reading and is not coming back for it.
            if (client.transport.SendBufferEmpty() || now > client.drain_until) {
                client.disconnect = true;
            }
            continue;
        }

        if (occurred & Sock::RECV) {
            uint8_t buf[0x10000];
            const ssize_t got{client.sock->Recv(buf, sizeof(buf), MSG_DONTWAIT)};
            if (got == 0) {
                LogPrintLevel(BCLog::SV2, BCLog::Level::Debug, "client=%d disconnected\n", client.id);
                client.disconnect = true;
                continue;
            }
            if (got < 0) {
                if (!IsTransientError(WSAGetLastError())) client.disconnect = true;
                continue;
            }

            client.transport.Received(std::as_bytes(std::span{buf, static_cast<size_t>(got)}));

            // The option existed, was documented as the per-client cap, and
            // was never read by anything. A client that sends without ever
            // completing a frame, or stalls partway through one, is holding
            // memory for nothing.
            if (client.transport.RecvBufferSize() > m_opts.max_recv_buffer) {
                LogPrintLevel(BCLog::SV2, BCLog::Level::Debug,
                              "client=%d buffered %d bytes without completing a message\n",
                              client.id, client.transport.RecvBufferSize());
                client.disconnect = true;
                continue;
            }

            if (!client.transport.Advance(unix_now)) {
                LogPrintLevel(BCLog::SV2, BCLog::Level::Debug,
                              "client=%d transport error, disconnecting\n", client.id);
                client.disconnect = true;
                continue;
            }

            while (auto msg{client.transport.Next()}) {
                if (!HandleCommon(client, *msg)) {
                    if (!client.drain) client.disconnect = true;
                    break;
                }
            }
        }
    }
}

void Connman::DropMarked()
{
    for (auto it = m_clients.begin(); it != m_clients.end();) {
        if (!it->disconnect) {
            ++it;
            continue;
        }
        const ClientId id{it->id};
        const bool was_ready{it->state == State::READY};
        it = m_clients.erase(it);
        if (was_ready) {
            m_pending.push_back(Notification{Notification::Kind::DISCONNECTED, id, POW_TYPE_X16RT, {}});
        }
    }
}

void Connman::ThreadHandler()
{
    while (!m_interrupt) {
        Sock::EventsPerSock events;
        {
            LOCK(m_mutex);
            events = GenerateWaitSockets();
        }

        // WaitMany is a virtual member so that tests can mock it, hence the
        // call through an arbitrary socket in the set. On failure nothing is
        // marked ready, so fall back to sleeping rather than spinning.
        if (events.empty() || !events.begin()->first->WaitMany(SOCKET_WAIT, events)) {
            m_interrupt.sleep_for(SOCKET_WAIT);
        }

        {
            LOCK(m_mutex);
            AcceptNew(events);
            ServiceClients(events);
            DropMarked();
        }
        DispatchNotifications();
    }
}

//! Deliver queued events to the provider.
//!
//! These run on the socket thread, which is the only one this class has. So
//! while a callback runs, nothing is being read, written, accepted or
//! handshaked for any client. Two of the provider's handlers take
//! milliseconds rather than microseconds: a submission ends in a MinotaurX or
//! X16RT hash and then ProcessNewBlock, and a transaction-data request
//! serializes a block that can be eight megabytes.
//!
//! That is bounded rather than solved. Both handlers are metered per client
//! before they do any work, a submission whose proof of work verifies is
//! refunded so honest pools are not charged, and -sv2maxclients bounds how
//! many clients there are. With a pool opening one connection per algorithm,
//! the exposure is small.
//!
//! The structural fix is to hand provider work to a worker thread and leave
//! this one doing transport only. Worth doing before this serves many
//! independent pools; not worth the concurrency it introduces while it
//! serves a handful.
void Connman::DispatchNotifications()
{
    std::vector<Notification> pending;
    WITH_LOCK(m_mutex, pending.swap(m_pending));
    if (!m_events) return;

    for (const Notification& note : pending) {
        switch (note.kind) {
        case Notification::Kind::READY:
            m_events->ClientReady(note.id, note.pow_type);
            break;
        case Notification::Kind::DISCONNECTED:
            m_events->ClientDisconnected(note.id);
            break;
        case Notification::Kind::MESSAGE:
            if (!m_events->ReceivedMessage(note.id, note.msg.header, note.msg.payload)) {
                Disconnect(note.id);
            }
            break;
        }
    }
}

} // namespace sv2
