// Copyright (c) 2026 The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_SV2_TEMPLATE_PROVIDER_H
#define BITCOIN_SV2_TEMPLATE_PROVIDER_H

#include <interfaces/mining.h>
#include <primitives/block.h>
#include <sv2/connman.h>
#include <sv2/messages.h>
#include <sv2/options.h>
#include <sv2/solution_check.h>
#include <sync.h>
#include <util/threadinterrupt.h>
#include <uint256.h>

#include <cstdint>
#include <chrono>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <thread>

namespace sv2 {

//! Largest extra coinbase space a client may ask to reserve. A request is
//! clamped rather than refused, because a client asking for too much is
//! usually optimistic rather than hostile.
static constexpr uint32_t MAX_COINBASE_ADDITIONAL_SIZE{8000};

//! Transaction data is the one message where a few request bytes produce a
//! whole block of encrypted output, so it is metered rather than merely
//! bounded.
static constexpr size_t MAX_TXDATA_REQUESTS{2};
static constexpr auto TXDATA_WINDOW{std::chrono::seconds{10}};

//! Solutions are metered too, and harder. Avian's proof of work has no cheap
//! partial form, so the single hash a submission earns is thousands of times
//! more expensive than the checks before it.
static constexpr size_t MAX_SOLUTIONS{4};
static constexpr auto SOLUTION_WINDOW{std::chrono::seconds{10}};

//! Everything derived from a block template at the moment it is sent, so that
//! answering a later request or validating a solution costs no block work.
struct CachedTemplate {
    std::unique_ptr<interfaces::BlockTemplate> handle;

    POW_TYPE pow_type{POW_TYPE_X16RT};
    //! nVersion as issued, algorithm bits included. A solution must match it.
    uint32_t version{0};
    uint32_t nbits{0};
    uint256 prev_hash;
    uint32_t ntime{0};
    //! Absolute bounds a solution's ntime must fall inside.
    uint32_t ntime_min{0};
    uint32_t ntime_max{0};

    //! Coinbase fields the client must reproduce exactly.
    uint32_t coinbase_tx_version{0};
    uint32_t coinbase_tx_input_sequence{0};
    uint32_t coinbase_tx_locktime{0};
    std::vector<uint8_t> coinbase_prefix;
    //! The outputs the client must keep: Avian's founder payment and the
    //! witness commitment. Kept in both forms: serialized for the wire, and
    //! parsed for comparing against what a solution actually carries.
    std::vector<uint8_t> required_outputs;
    std::vector<CTxOut> required_txouts;
    uint32_t required_outputs_count{0};
    //! Ceilings for the finished coinbase.
    size_t max_coinbase_size{0};
    size_t max_coinbase_weight{0};
    unsigned int max_coinbase_sigops{0};
    //! Value the client may distribute among its own outputs.
    uint64_t value_remaining{0};

    std::vector<uint256> merkle_path;

    //! The coinbase commitment script, so a pool validating the work can
    //! rebuild the commitment for itself.
    std::vector<uint8_t> excess_data;

    //! The non-coinbase transactions, serialized with witnesses. Built on
    //! first request rather than up front, because getBlock() copies a block
    //! that can be eight megabytes.
    std::vector<std::vector<uint8_t>> transaction_list;
    bool transaction_list_built{false};
};

//! Serves block templates over the Template Distribution Protocol.
//!
//! The connection manager owns the Common protocol and extension negotiation;
//! everything from CoinbaseOutputConstraints onwards arrives here.
class TemplateProvider final : public EventsInterface
{
public:
    TemplateProvider(interfaces::Mining& mining, Options opts);
    ~TemplateProvider() override;

    //! Must be called before clients arrive. Not owned.
    void SetConnman(Connman* connman) { m_connman = connman; }

    //! Start the thread that watches for a new tip and refreshes templates.
    void Start();
    void Interrupt();
    void Stop() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    void ClientReady(ClientId id, POW_TYPE pow_type) override EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    void ClientDisconnected(ClientId id) override EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    [[nodiscard]] bool ReceivedMessage(ClientId id, const NetHeader& header,
                                       std::span<const uint8_t> payload) override
        EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

private:
    struct ClientState {
        POW_TYPE pow_type{POW_TYPE_X16RT};
        //! A template cannot be built until the client says how much coinbase
        //! room it needs.
        bool constraints_received{false};
        uint32_t coinbase_max_additional_size{0};
        uint16_t coinbase_max_additional_sigops{0};
        std::map<uint64_t, CachedTemplate> templates;
        //! The largest id issued to this client, so a request for a template
        //! that has since been dropped can be told apart from one that never
        //! existed.
        uint64_t highest_template_id{0};
        //! The prev hash this client has already been told about, so
        //! SetNewPrevHash is sent when it changes and not on every refresh.
        //! Absent until the first template, which always needs one.
        std::optional<uint256> announced_prev_hash;
        //! When recent transaction-data requests arrived.
        std::deque<std::chrono::steady_clock::time_point> txdata_requests;
        //! When recent solutions arrived. A Template Provider submission means
        //! "I found a block", not "here is a share", so a tight allowance
        //! costs an honest client nothing and is the first line of defence
        //! against buying proof-of-work computations cheaply.
        std::deque<std::chrono::steady_clock::time_point> solutions;
        //! Consecutive refusals, so a client that cannot get it right stops
        //! being given the chance.
        size_t rejects{0};
    };

    [[nodiscard]] bool HandleCoinbaseOutputConstraints(ClientId id, ClientState& state,
                                                       std::span<const uint8_t> payload)
        EXCLUSIVE_LOCKS_REQUIRED(m_mutex);

    //! Build a template for `state` and send NewTemplate followed by the
    //! Avian time constraints. Returns false to disconnect the client.
    [[nodiscard]] bool SendNewTemplate(ClientId id, ClientState& state, bool future)
        EXCLUSIVE_LOCKS_REQUIRED(m_mutex);

    //! Drop templates a solution could no longer use, and release the memory
    //! the rest no longer need. Called after each template is issued.
    void PruneTemplates(ClientState& state) EXCLUSIVE_LOCKS_REQUIRED(m_mutex);

    [[nodiscard]] bool HandleRequestTransactionData(ClientId id, ClientState& state,
                                                     std::span<const uint8_t> payload)
        EXCLUSIVE_LOCKS_REQUIRED(m_mutex);

    [[nodiscard]] bool HandleSubmitSolution(ClientId id, ClientState& state,
                                            std::span<const uint8_t> payload)
        EXCLUSIVE_LOCKS_REQUIRED(m_mutex);

    //! Rebuild and reissue work for every client that has asked for it.
    void RefreshClients(bool tip_changed) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    void ThreadWatcher() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    interfaces::Mining& m_mining;
    const Options m_opts;
    Connman* m_connman{nullptr};

    mutable Mutex m_mutex;
    std::map<ClientId, ClientState> m_clients GUARDED_BY(m_mutex);
    uint64_t m_next_template_id GUARDED_BY(m_mutex){1};

    std::thread m_thread;
    CThreadInterrupt m_interrupt;
};

//! Process-wide lifecycle, following the torcontrol pattern of free functions
//! over a file-static owner, so node/context.h does not need to learn about
//! sv2 and no include cycle appears. Reports failure via InitError.
[[nodiscard]] bool StartSv2(const ArgsManager& args, ChainType chain, interfaces::Mining& mining);
void InterruptSv2();
void StopSv2();

//! The running listener, or null. For tests.
Connman* GetSv2Connman();

} // namespace sv2

#endif // BITCOIN_SV2_TEMPLATE_PROVIDER_H
