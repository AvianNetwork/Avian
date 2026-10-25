// Copyright (c) 2026 The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_SV2_CONNMAN_H
#define BITCOIN_SV2_CONNMAN_H

#include <key.h>
#include <netaddress.h>
#include <sv2/messages.h>
#include <sv2/noise.h>
#include <sv2/options.h>
#include <sv2/transport.h>
#include <sync.h>
#include <util/sock.h>
#include <util/threadinterrupt.h>

#include <chrono>
#include <cstdint>
#include <list>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace sv2 {

using ClientId = uint64_t;

//! How long the socket loop blocks waiting for IO.
//!
//! Shorter than Sock's own MAX_WAIT_FOR_IO of one second, because a template
//! queued from another thread should reach a miner promptly after a new block;
//! a second of added latency would be real lost work.
static constexpr auto SOCKET_WAIT{std::chrono::milliseconds{50}};

//! How long to keep a rejected connection open so its error reply can be
//! written. A peer that is not reading does not get to hold the slot.
static constexpr auto DRAIN_TIMEOUT{std::chrono::seconds{2}};

//! Hook for the protocol layers above the Common protocol.
//!
//! The connection manager answers SetupConnection itself, since that exchange
//! is what decides whether a session exists at all. Everything after it is
//! Template Distribution Protocol business and is handed over here.
class EventsInterface
{
public:
    virtual ~EventsInterface() = default;

    //! The client completed SetupConnection and may now be sent messages.
    virtual void ClientReady(ClientId id) {}

    virtual void ClientDisconnected(ClientId id) {}

    //! A message the Common protocol does not own. Return false to disconnect.
    //!
    //! The default refuses everything, which is what a Template Provider that
    //! cannot yet serve templates should do: handing a client a session and
    //! then silently ignoring its requests is worse than closing it.
    [[nodiscard]] virtual bool ReceivedMessage(ClientId id, const NetHeader& header,
                                              std::span<const uint8_t> payload)
    {
        return false;
    }
};

//! Accepts and services encrypted Sv2 connections on one thread.
class Connman
{
public:
    Connman(Options opts, CKey static_key, SignatureNoiseMessage cert);
    ~Connman();
    Connman(const Connman&) = delete;
    Connman& operator=(const Connman&) = delete;

    //! Bind the listening sockets and start the socket thread. `events` must
    //! outlive this object, or be null to refuse everything past setup.
    [[nodiscard]] bool Start(EventsInterface* events);

    //! Wake the loop and tell it to stop. Safe to call more than once.
    void Interrupt();

    //! Join the thread and close every socket.
    void Stop();

    //! Queue a message to one client. Thread safe. Returns false if the client
    //! is gone, is not past setup, or the payload does not fit.
    [[nodiscard]] bool Send(ClientId id, const NetHeader& header, std::span<const uint8_t> payload);

    void Disconnect(ClientId id);

    size_t ClientCount() const;

    //! The addresses actually bound, which is what a test needs when it asked
    //! for port 0.
    std::vector<CService> BoundAddresses() const;

private:
    //! Where a connection is in the Common protocol exchange.
    enum class State : uint8_t {
        //! Awaiting SetupConnection. Nothing else may be sent.
        SETUP,
        //! Setup answered; awaiting RequestExtensions. Only reached when the
        //! policy demands negotiation.
        NEGOTIATE,
        //! Extensions agreed and Avian's is among them; awaiting an algorithm
        //! choice. A connection carries one nBits and target stream, so this
        //! has to be settled before any template is built.
        SELECT_ALGO,
        //! May exchange Template Distribution messages.
        READY,
    };

    struct Client {
        ClientId id;
        std::shared_ptr<Sock> sock;
        CService addr;
        Transport transport;
        std::chrono::steady_clock::time_point connected;
        State state{State::SETUP};
        //! A client that omits a required extension gets one chance to ask
        //! again, as the spec allows, before it is dropped.
        bool extensions_retried{false};
        //! Negotiation happens before protocol messages, so it is answered at
        //! most once per connection.
        bool extensions_answered{false};
        //! True once Avian's extension was agreed, which is what entitles the
        //! client to select an algorithm.
        bool avian_extension{false};
        bool algo_selected{false};
        //! The algorithm templates for this connection are built for.
        POW_TYPE pow_type{POW_TYPE_X16RT};
        //! Close immediately, discarding anything still queued.
        bool disconnect{false};
        //! Stop reading and close once the send buffer drains, so a rejection
        //! reply actually reaches the peer. Closing the socket the moment a
        //! reply is queued would leave the client guessing.
        bool drain{false};
        std::chrono::steady_clock::time_point drain_until{};
    };

    void ThreadHandler();
    Sock::EventsPerSock GenerateWaitSockets() EXCLUSIVE_LOCKS_REQUIRED(m_mutex);
    void AcceptNew(const Sock::EventsPerSock& events) EXCLUSIVE_LOCKS_REQUIRED(m_mutex);
    void ServiceClients(const Sock::EventsPerSock& events) EXCLUSIVE_LOCKS_REQUIRED(m_mutex);
    void DropMarked() EXCLUSIVE_LOCKS_REQUIRED(m_mutex);

    //! True if `addr` is inside an -sv2allowip subnet, or is loopback when
    //! none were given.
    bool IsAllowed(const CService& addr) const;

    //! Handle a message the Common protocol owns. Returns false to disconnect.
    [[nodiscard]] bool HandleCommon(Client& client, const ReceivedMsg& msg) EXCLUSIVE_LOCKS_REQUIRED(m_mutex);
    [[nodiscard]] bool HandleSetupConnection(Client& client, const ReceivedMsg& msg) EXCLUSIVE_LOCKS_REQUIRED(m_mutex);
    [[nodiscard]] bool HandleRequestExtensions(Client& client, const ReceivedMsg& msg) EXCLUSIVE_LOCKS_REQUIRED(m_mutex);
    [[nodiscard]] bool HandleAvianSelectAlgorithm(Client& client, const ReceivedMsg& msg) EXCLUSIVE_LOCKS_REQUIRED(m_mutex);

    //! Reject a client with `msg`, flushing the reply before closing.
    template <typename M>
    [[nodiscard]] bool Reject(Client& client, uint16_t ext, uint8_t type, const M& msg, const std::string& why)
        EXCLUSIVE_LOCKS_REQUIRED(m_mutex);

    //! Serialize `msg` into a frame on `client`.
    template <typename M>
    [[nodiscard]] bool SendTo(Client& client, uint16_t ext, uint8_t type, const M& msg);

    const Options m_opts;
    const CKey m_static_key;
    const SignatureNoiseMessage m_cert;

    EventsInterface* m_events{nullptr};
    std::vector<std::shared_ptr<Sock>> m_listen;
    std::thread m_thread;
    CThreadInterrupt m_interrupt;

    mutable Mutex m_mutex;
    std::list<Client> m_clients GUARDED_BY(m_mutex);
    ClientId m_next_id GUARDED_BY(m_mutex){1};
};

//! Process-wide lifecycle, following the torcontrol pattern so that
//! node/context.h does not need to learn about sv2 and no include cycle
//! appears. Returns false and reports via InitError on failure.
[[nodiscard]] bool StartSv2(const ArgsManager& args, ChainType chain);
void InterruptSv2();
void StopSv2();

//! The running instance, or null. For tests and for the Template Provider to
//! attach itself in a later change.
Connman* GetSv2Connman();

} // namespace sv2

#endif // BITCOIN_SV2_CONNMAN_H
