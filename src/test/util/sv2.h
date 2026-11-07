// Copyright (c) 2026 The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_TEST_UTIL_SV2_H
#define BITCOIN_TEST_UTIL_SV2_H

#include <compat/compat.h>
#include <netaddress.h>
#include <netbase.h>
#include <pubkey.h>
#include <sv2/messages.h>
#include <sv2/transport.h>
#include <util/sock.h>
#include <util/time.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

//! A minimal Sv2 client over a real socket, shared by the listener and
//! Template Provider tests.
//!
//! Nothing here asserts: helpers return a value and the caller decides what is
//! fatal. That keeps the helpers usable from both suites and keeps the failure
//! reported at the line that cared.
namespace sv2test {

static constexpr auto TIMEOUT{std::chrono::seconds{10}};

//! Deadline for work the node pushes of its own accord.
//!
//! Longer than TIMEOUT because it waits on something different. A reply to a
//! request is produced on the socket thread and arrives promptly or not at
//! all, whereas pushed work comes from the watcher thread, which polls for a
//! tip change in one-second slices and then rebuilds templates under cs_main.
//! On a machine running a whole test suite in parallel that chain has taken
//! longer than TIMEOUT, which is a loaded machine rather than a defect.
static constexpr auto PUSH_TIMEOUT{std::chrono::seconds{60}};

struct Client {
    std::unique_ptr<Sock> sock;
    std::optional<sv2::Transport> transport;

    //! Push whatever the transport has queued. A would-block is not a failure.
    bool Flush()
    {
        const auto out{transport->SendBuffer()};
        if (out.empty()) return true;
        const ssize_t sent{sock->Send(out.data(), out.size(), MSG_NOSIGNAL)};
        if (sent > 0) {
            transport->Sent(static_cast<size_t>(sent));
            return true;
        }
        return sent < 0 && (WSAGetLastError() == WSAEWOULDBLOCK || WSAGetLastError() == WSAEINTR);
    }

    //! Read until `done` is satisfied, the peer closes, or the deadline passes.
    //! `closed` distinguishes a close from a timeout.
    bool PumpUntil(const std::function<bool()>& done, bool& closed,
                   std::chrono::seconds timeout = TIMEOUT)
    {
        closed = false;
        const auto deadline{std::chrono::steady_clock::now() + timeout};
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

//! Connect to `addr` and start an Sv2 handshake as the initiator. Returns null
//! if the socket could not be connected.
inline std::unique_ptr<Client> Connect(const CService& addr, const std::optional<XOnlyPubKey>& authority)
{
    auto client{std::make_unique<Client>()};
    client->sock = CreateSock(addr.GetSAFamily(), SOCK_STREAM, IPPROTO_TCP);
    if (!client->sock) return nullptr;

    struct sockaddr_storage storage;
    socklen_t len{sizeof(storage)};
    if (!addr.GetSockAddr(reinterpret_cast<struct sockaddr*>(&storage), &len)) return nullptr;

    if (client->sock->Connect(reinterpret_cast<struct sockaddr*>(&storage), len) != 0) {
        // CreateSock hands back a non-blocking socket, so a connect in
        // progress is the normal case rather than a failure.
        const int err{WSAGetLastError()};
        if (err != WSAEINPROGRESS && err != WSAEWOULDBLOCK) return nullptr;
        Sock::Event occurred{0};
        if (!client->sock->Wait(std::chrono::milliseconds{TIMEOUT}, Sock::SEND, &occurred)) return nullptr;
        if (!(occurred & Sock::SEND)) return nullptr;
        std::string errmsg;
        if (!client->sock->IsConnected(errmsg)) return nullptr;
    }

    client->transport.emplace(sv2::Transport::Initiator(authority));
    return client;
}

//! Wait for one message to arrive.
inline std::optional<sv2::ReceivedMsg> ReadMessage(Client& client, bool& closed,
                                                   std::chrono::seconds timeout = TIMEOUT)
{
    std::optional<sv2::ReceivedMsg> out;
    client.PumpUntil([&] {
        out = client.transport->Next();
        return out.has_value();
    }, closed, timeout);
    return out;
}

inline bool DoHandshake(Client& client)
{
    bool closed{false};
    return client.PumpUntil([&] { return client.transport->HandshakeComplete(); }, closed);
}

inline sv2::SetupConnection MakeSetup(
    uint8_t protocol = static_cast<uint8_t>(sv2::Protocol::TEMPLATE_DISTRIBUTION), uint32_t flags = 0)
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

//! Handshake and SetupConnection, leaving the client with a session.
inline bool SetupSession(Client& client)
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

inline bool SendRequestExtensions(Client& client, uint16_t request_id, const std::vector<uint16_t>& want)
{
    sv2::RequestExtensions req;
    req.request_id = request_id;
    req.requested_extensions = want;
    return client.transport->Send(
        sv2::NetHeader{sv2::EXT_NEGOTIATION,
                       static_cast<uint8_t>(sv2::NegotiationMsg::REQUEST_EXTENSIONS), 0},
        sv2::EncodePayload(req));
}

inline bool SendSelectAlgorithm(Client& client, uint8_t pow_type,
                                uint16_t id = sv2::AVIAN_EXTENSION_ID)
{
    sv2::AvianSelectAlgorithm select;
    select.pow_type = pow_type;
    return client.transport->Send(
        sv2::NetHeader{id, static_cast<uint8_t>(sv2::AvianMsg::SELECT_ALGORITHM), 0},
        sv2::EncodePayload(select));
}

} // namespace sv2test

#endif // BITCOIN_TEST_UTIL_SV2_H
