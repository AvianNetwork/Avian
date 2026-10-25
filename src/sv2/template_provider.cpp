// Copyright (c) 2026 The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <sv2/template_provider.h>

#include <arith_uint256.h>
#include <chain.h>
#include <common/args.h>
#include <common/init.h>
#include <chainparams.h>
#include <consensus/consensus.h>
#include <consensus/tx_verify.h>
#include <logging.h>
#include <node/interface_ui.h>
#include <node/types.h>
#include <policy/policy.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <streams.h>
#include <util/strencodings.h>
#include <util/thread.h>
#include <util/time.h>
#include <util/translation.h>

#include <algorithm>
#include <vector>
#include <cassert>

namespace sv2 {

namespace {

//! The leading scriptSig bytes before the extranonce, which the spec requires
//! to begin with the BIP34 height push.
//!
//! The assembler builds the scriptSig as `CScript() << nHeight << OP_0`, where
//! the trailing OP_0 is the extranonce placeholder and is not part of the
//! prefix. Taking the first push rather than trimming a known suffix means
//! this keeps working if that placeholder ever changes.
bool CoinbasePrefix(const CScript& script_sig, std::vector<uint8_t>& out)
{
    CScript::const_iterator it{script_sig.begin()};
    opcodetype op;
    std::vector<unsigned char> push;
    if (!script_sig.GetOp(it, op, push)) return false;
    const auto len{static_cast<size_t>(it - script_sig.begin())};
    out.assign(script_sig.begin(), script_sig.begin() + len);
    return true;
}

//! Serialize the coinbase outputs the client must keep, with no CompactSize
//! prefix, which is how NewTemplate.coinbase_tx_outputs is defined.
std::vector<uint8_t> SerializeOutputs(const std::vector<CTxOut>& outputs)
{
    DataStream ss{};
    for (const CTxOut& out : outputs) {
        ss << out;
    }
    return std::vector<uint8_t>{UCharCast(ss.data()), UCharCast(ss.data()) + ss.size()};
}

} // namespace

TemplateProvider::TemplateProvider(interfaces::Mining& mining, Options opts)
    : m_mining{mining}, m_opts{std::move(opts)}
{
}

TemplateProvider::~TemplateProvider() = default;

void TemplateProvider::ClientReady(ClientId id, POW_TYPE pow_type)
{
    LOCK(m_mutex);
    ClientState& state{m_clients[id]};
    // Reachable twice for one client: under the optional policy it becomes
    // ready at SetupConnection and again when it selects an algorithm. Any
    // templates cached in between are for the wrong algorithm, so they go.
    if (state.pow_type != pow_type) {
        state.templates.clear();
        state.announced_prev_hash.reset();
    }
    state.pow_type = pow_type;
    LogPrintLevel(BCLog::SV2, BCLog::Level::Debug, "client=%d ready for %s templates\n", id,
                  POW_TYPE_NAMES[pow_type]);
}

void TemplateProvider::ClientDisconnected(ClientId id)
{
    LOCK(m_mutex);
    m_clients.erase(id);
}

bool TemplateProvider::ReceivedMessage(ClientId id, const NetHeader& header,
                                       std::span<const uint8_t> payload)
{
    if (header.Extension() != EXT_CORE) {
        LogPrintLevel(BCLog::SV2, BCLog::Level::Debug,
                      "client=%d sent extension 0x%04x, which carries no template messages\n", id,
                      header.Extension());
        return false;
    }

    // Both of these are handled without the provider lock held, because both
    // end in work measured in milliseconds rather than microseconds: a
    // proof-of-work hash, and serializing a block that can be eight
    // megabytes. Each finds its own client and takes the lock in pieces.
    if (static_cast<CoreMsg>(header.msg_type) == CoreMsg::SUBMIT_SOLUTION) {
        return HandleSubmitSolution(id, payload);
    }
    if (static_cast<CoreMsg>(header.msg_type) == CoreMsg::REQUEST_TRANSACTION_DATA) {
        return HandleRequestTransactionData(id, payload);
    }

    LOCK(m_mutex);
    const auto it{m_clients.find(id)};
    if (it == m_clients.end()) return false;

    switch (static_cast<CoreMsg>(header.msg_type)) {
    case CoreMsg::COINBASE_OUTPUT_CONSTRAINTS:
        return HandleCoinbaseOutputConstraints(id, it->second, payload);
    default:
        LogPrintLevel(BCLog::SV2, BCLog::Level::Debug,
                      "client=%d sent msg_type=0x%02x, which is not yet served\n", id,
                      header.msg_type);
        return false;
    }
}

bool TemplateProvider::HandleCoinbaseOutputConstraints(ClientId id, ClientState& state,
                                                       std::span<const uint8_t> payload)
{
    // Metered before anything is parsed, like the other handlers. This was
    // the only one that was free, and it is not cheap: every one of them
    // rebuilds this client's template under cs_main. A single full socket
    // read holds about fifteen hundred of these six-byte messages.
    const auto arrived{std::chrono::steady_clock::now()};
    while (!state.constraints.empty() && arrived - state.constraints.front() > CONSTRAINTS_WINDOW) {
        state.constraints.pop_front();
    }
    if (state.constraints.size() >= MAX_CONSTRAINTS) {
        LogPrintLevel(BCLog::SV2, BCLog::Level::Debug,
                      "client=%d sent more than %d CoinbaseOutputConstraints in %ds\n", id,
                      MAX_CONSTRAINTS, CONSTRAINTS_WINDOW.count());
        return false;
    }
    state.constraints.push_back(arrived);

    CoinbaseOutputConstraints constraints;
    try {
        constraints = DecodePayload<CoinbaseOutputConstraints>(payload);
    } catch (const std::ios_base::failure& e) {
        LogPrintLevel(BCLog::SV2, BCLog::Level::Debug,
                      "client=%d sent a malformed CoinbaseOutputConstraints: %s\n", id, e.what());
        return false;
    }

    // Clamp rather than refuse: a client asking for more room than we will
    // give is usually optimistic, not hostile.
    state.coinbase_max_additional_size =
        std::min(constraints.coinbase_output_max_additional_size, MAX_COINBASE_ADDITIONAL_SIZE);
    state.coinbase_max_additional_sigops =
        std::min<uint16_t>(constraints.coinbase_output_max_additional_sigops, 400);
    state.constraints_received = true;

    LogPrintLevel(BCLog::SV2, BCLog::Level::Debug,
                  "client=%d reserved %d coinbase bytes and %d sigops\n", id,
                  state.coinbase_max_additional_size, state.coinbase_max_additional_sigops);

    return SendNewTemplate(id, state, /*future=*/false);
}

bool TemplateProvider::SendNewTemplate(ClientId id, ClientState& state, bool future)
{
    if (!m_connman || !state.constraints_received) return false;

    if (m_mining.isInitialBlockDownload()) {
        // Templates built during IBD are work a miner can never land, so
        // issuing one would be worse than issuing nothing.
        LogPrintLevel(BCLog::SV2, BCLog::Level::Debug,
                      "client=%d asked for a template during initial block download\n", id);
        return true;
    }

    node::BlockCreateOptions create;
    create.use_mempool = true;
    create.pow_type = state.pow_type;
    // The client builds its own coinbase, so the assembler's is a placeholder
    // that is never put on the wire.
    create.coinbase_output_script = CScript() << OP_TRUE;
    create.coinbase_output_max_additional_sigops = state.coinbase_max_additional_sigops;
    create.block_reserved_weight =
        std::max<size_t>(MINIMUM_BLOCK_RESERVED_WEIGHT,
                         DEFAULT_BLOCK_RESERVED_WEIGHT + WITNESS_SCALE_FACTOR * state.coinbase_max_additional_size);

    auto handle{m_mining.createNewBlock(create)};
    if (!handle) {
        LogPrintLevel(BCLog::SV2, BCLog::Level::Warning,
                      "client=%d could not be given a template; the node is shutting down\n", id);
        return false;
    }

    const CBlockHeader header{handle->getBlockHeader()};
    const CTransactionRef coinbase{handle->getCoinbaseTx()};
    if (!coinbase || coinbase->vin.size() != 1 || coinbase->vout.empty()) {
        LogPrintLevel(BCLog::SV2, BCLog::Level::Error, "the assembler produced an unusable coinbase\n");
        return false;
    }

    // The algorithm lives in nVersion bits 16-23, so a template built for the
    // wrong one would quietly hand out work against the wrong pow limit.
    if (header.GetPoWType() != state.pow_type) {
        LogPrintLevel(BCLog::SV2, BCLog::Level::Error,
                      "the assembler built a %s template when %s was requested\n",
                      header.GetPoWTypeName(), POW_TYPE_NAMES[state.pow_type]);
        return false;
    }

    CachedTemplate cached;
    if (!CoinbasePrefix(coinbase->vin[0].scriptSig, cached.coinbase_prefix)) {
        LogPrintLevel(BCLog::SV2, BCLog::Level::Error, "the coinbase scriptSig has no height push\n");
        return false;
    }

    // Everything after vout[0] is ours to require: Avian's founder payment and
    // the witness commitment. vout[0] is the assembler's placeholder, and its
    // value is already net of the founder payment, so it is exactly what the
    // client has left to spend.
    const std::vector<CTxOut> required{coinbase->vout.begin() + 1, coinbase->vout.end()};

    // Fail closed if the commitment is not where the interface says it is:
    // serving a template whose commitment the client cannot reproduce would
    // produce blocks that only fail at connect time.
    const auto commitment{handle->getCoinbaseCommitment()};
    const int commit_index{handle->getWitnessCommitmentIndex()};
    if (!commitment.empty()) {
        if (commit_index <= 0 || static_cast<size_t>(commit_index) >= coinbase->vout.size()) {
            LogPrintLevel(BCLog::SV2, BCLog::Level::Error,
                          "the witness commitment is at index %d, outside the coinbase outputs\n",
                          commit_index);
            return false;
        }
        const CScript& script{coinbase->vout[commit_index].scriptPubKey};
        if (std::vector<unsigned char>{script.begin(), script.end()} != commitment) {
            LogPrintLevel(BCLog::SV2, BCLog::Level::Error,
                          "the coinbase commitment output does not match getCoinbaseCommitment\n");
            return false;
        }
    }

    const uint64_t template_id{m_next_template_id++};
    state.highest_template_id = template_id;
    cached.pow_type = state.pow_type;
    cached.version = static_cast<uint32_t>(header.nVersion);
    cached.nbits = header.nBits;
    cached.prev_hash = header.hashPrevBlock;
    cached.ntime = header.nTime;
    cached.coinbase_tx_version = static_cast<uint32_t>(coinbase->version);
    cached.coinbase_tx_input_sequence = coinbase->vin[0].nSequence;
    cached.coinbase_tx_locktime = coinbase->nLockTime;
    cached.required_outputs = SerializeOutputs(required);
    cached.required_txouts = required;
    cached.required_outputs_count = static_cast<uint32_t>(required.size());
    // What the client may send is the template's own coinbase plus what it
    // reserved. The weight ceiling is the reservation the assembler honoured,
    // which gives the block limits transitively.
    const size_t own_size{static_cast<size_t>(::GetSerializeSize(TX_WITH_WITNESS(*coinbase)))};
    cached.max_coinbase_size = own_size + state.coinbase_max_additional_size;
    cached.max_coinbase_weight = create.block_reserved_weight;
    cached.max_coinbase_sigops =
        GetLegacySigOpCount(*coinbase) + state.coinbase_max_additional_sigops;
    cached.value_remaining = static_cast<uint64_t>(coinbase->vout[0].nValue);
    cached.merkle_path = handle->getCoinbaseMerklePath();
    // The published specification says excess data "is usually the SegWit
    // commitment", so this is conformant today. sv2-spec#241 proposes
    // requiring the field empty instead, SRI's own template provider already
    // sends it empty, and the question is outstanding with the maintainers.
    // Revisit before anything depends on Avian's production interoperability:
    // a pool has the commitment already, verbatim, among the required
    // outputs, so emptying this loses nothing.
    cached.excess_data = commitment;

    const auto& consensus{Params().GetConsensus()};
    cached.ntime_min = header.nTime;
    cached.ntime_max = header.nTime;
    if (!consensus.fPowAllowMinDifficultyBlocks) {
        // Three independent ceilings, lowest wins.
        //
        // X16RT rederives its hash chain from nTime & TIME_MASK, so rolling
        // past that boundary silently computes a different function. MinotaurX
        // has no such epoch.
        // Computed in int64_t throughout: MAX_FUTURE_BLOCK_TIME_DUAL_ALGO is
        // signed and wider, so mixing it with uint32_t timestamps would both
        // narrow and, near the epoch, wrap.
        int64_t ceiling{static_cast<int64_t>(header.nTime) + m_opts.ntime_forward};
        if (state.pow_type == POW_TYPE_X16RT) {
            ceiling = std::min(ceiling, static_cast<int64_t>(header.nTime & TIME_MASK) + 127);
        }
        const int64_t future_limit{GetTime() + MAX_FUTURE_BLOCK_TIME_DUAL_ALGO - m_opts.ntime_margin};
        const int64_t bound{std::max(static_cast<int64_t>(header.nTime), std::min(ceiling, future_limit))};
        cached.ntime_max = static_cast<uint32_t>(bound);
    }
    // On chains that allow minimum-difficulty blocks, nBits itself depends on
    // nTime, so a rolled timestamp can invalidate the difficulty we issued.
    // Pinning ntime there is simpler than tracking that cliff, and costs a
    // test chain nothing.

    NewTemplate msg;
    msg.template_id = template_id;
    msg.future_template = future;
    msg.version = cached.version;
    msg.coinbase_tx_version = cached.coinbase_tx_version;
    msg.coinbase_prefix = cached.coinbase_prefix;
    msg.coinbase_tx_input_sequence = cached.coinbase_tx_input_sequence;
    msg.coinbase_tx_value_remaining = cached.value_remaining;
    msg.coinbase_tx_outputs_count = cached.required_outputs_count;
    msg.coinbase_tx_outputs = cached.required_outputs;
    msg.coinbase_tx_locktime = cached.coinbase_tx_locktime;
    msg.merkle_path = cached.merkle_path;

    SetNewPrevHash prev;
    prev.template_id = template_id;
    prev.prev_hash = cached.prev_hash;
    prev.ntime_start = cached.ntime_min;
    prev.nbits = cached.nbits;
    // nBits already came from the per-algorithm LWMA, so expanding it yields
    // the target for this connection's algorithm without further work.
    prev.target = ArithToUint256(arith_uint256{}.SetCompact(cached.nbits));

    AvianTimeConstraints times;
    times.template_id = template_id;
    times.ntime_min = cached.ntime_min;
    times.ntime_max = cached.ntime_max;

    const bool announce_prev_hash{state.announced_prev_hash != prev.prev_hash};
    state.announced_prev_hash = prev.prev_hash;

    cached.handle = std::move(handle);
    state.templates.emplace(template_id, std::move(cached));

    if (!m_connman->Send(id, NetHeader{EXT_CORE, static_cast<uint8_t>(CoreMsg::NEW_TEMPLATE), 0},
                         EncodePayload(msg))) {
        return false;
    }
    // NewTemplate carries no prev hash of its own, so without this the client
    // has no header to hash. Sent when the prev hash changes and on a client's
    // first template, not on every refresh: the specification has it follow
    // the validation of a new best block, and a future_template of false
    // already says the template belongs to the SetNewPrevHash before it. A
    // client that took its arrival to mean the tip had moved would otherwise
    // discard its in-flight work on every refresh.
    if (announce_prev_hash) {
        if (!m_connman->Send(id,
                             NetHeader{EXT_CORE, static_cast<uint8_t>(CoreMsg::SET_NEW_PREV_HASH), 0},
                             EncodePayload(prev))) {
            return false;
        }
    }
    // The spec gives SubmitSolution no upper ntime bound, so the client is
    // told one here or it has no way to know when its work expires.
    if (!m_connman->Send(id,
                         NetHeader{m_opts.extension_id,
                                   static_cast<uint8_t>(AvianMsg::TIME_CONSTRAINTS), 0},
                         EncodePayload(times))) {
        return false;
    }

    LogPrintLevel(BCLog::SV2, BCLog::Level::Debug,
                  "client=%d sent template %d for %s, ntime %d..%d\n", id, template_id,
                  POW_TYPE_NAMES[state.pow_type], times.ntime_min, times.ntime_max);

    PruneTemplates(state);
    return true;
}

void TemplateProvider::PruneTemplates(ClientState& state)
{
    if (state.templates.empty()) return;

    // The newest template is the one just issued, and its prev hash is the tip
    // every live template has to share. A template for an abandoned tip cannot
    // produce a block that connects, so it is dropped rather than counted.
    const uint256 tip{std::prev(state.templates.end())->second.prev_hash};
    for (auto it{state.templates.begin()}; it != state.templates.end();) {
        it = it->second.prev_hash == tip ? std::next(it) : state.templates.erase(it);
    }

    // Trim oldest first. std::map orders by the ids we hand out, which only
    // increase, so begin() is always the oldest.
    while (state.templates.size() > m_opts.max_templates) {
        state.templates.erase(state.templates.begin());
    }

    // Keeping a superseded template costs almost nothing except this: the
    // serialized transaction list is a real copy of up to a whole block, where
    // everything else is shared pointers into the mempool. Release it. A
    // client has no reason to ask for transaction data on work it has already
    // been told to replace, and RequestTransactionData rebuilds it from the
    // handle if one ever does.
    for (auto it{state.templates.begin()}; it != std::prev(state.templates.end()); ++it) {
        it->second.transaction_list.reset();
    }
}


void TemplateProvider::Start()
{
    m_thread = std::thread{&util::TraceThread, "sv2-tmpl", [this] { ThreadWatcher(); }};
}

void TemplateProvider::Interrupt() { m_interrupt(); }

void TemplateProvider::Stop()
{
    m_interrupt();
    if (m_thread.joinable()) m_thread.join();
    LOCK(m_mutex);
    m_clients.clear();
}

bool TemplateProvider::HandleRequestTransactionData(ClientId id, std::span<const uint8_t> payload)
{
    RequestTransactionData request;
    try {
        request = DecodePayload<RequestTransactionData>(payload);
    } catch (const std::ios_base::failure& e) {
        LogPrintLevel(BCLog::SV2, BCLog::Level::Debug,
                      "client=%d sent a malformed RequestTransactionData: %s\n", id, e.what());
        return false;
    }

    const auto sendError{[&](const std::string& code) {
        RequestTransactionDataError err;
        err.template_id = request.template_id;
        err.error_code = code;
        LogPrintLevel(BCLog::SV2, BCLog::Level::Debug,
                      "client=%d asked for transaction data for template %d: %s\n", id,
                      request.template_id, code);
        return m_connman->Send(
            id, NetHeader{EXT_CORE, static_cast<uint8_t>(CoreMsg::REQUEST_TRANSACTION_DATA_ERROR), 0},
            EncodePayload(err));
    }};

    std::shared_ptr<interfaces::BlockTemplate> handle;
    std::shared_ptr<const std::vector<std::vector<uint8_t>>> transactions;
    std::vector<uint8_t> excess_data;

    {
        LOCK(m_mutex);
        const auto client_it{m_clients.find(id)};
        if (client_it == m_clients.end()) return false;
        ClientState& state{client_it->second};

        // Meter before doing any work: a handful of request bytes otherwise
        // buys a whole block serialized, encrypted and buffered.
        const auto now{std::chrono::steady_clock::now()};
        while (!state.txdata_requests.empty() && now - state.txdata_requests.front() > TXDATA_WINDOW) {
            state.txdata_requests.pop_front();
        }
        if (state.txdata_requests.size() >= MAX_TXDATA_REQUESTS) {
            LogPrintLevel(BCLog::SV2, BCLog::Level::Debug,
                          "client=%d asked for transaction data more than %d times in %ds\n", id,
                          MAX_TXDATA_REQUESTS, TXDATA_WINDOW.count());
            return false;
        }
        state.txdata_requests.push_back(now);

        const auto it{state.templates.find(request.template_id)};
        if (it == state.templates.end()) {
            // Retention is bounded, so an id we once issued is stale rather
            // than unknown. The distinction tells a client whether to wait
            // for new work or stop asking.
            return sendError(request.template_id != 0 && request.template_id <= state.highest_template_id
                                 ? "stale-template"
                                 : "template-id-not-found");
        }

        // Both of these are pointer copies. The list is null when nobody has
        // asked for this template's transactions yet.
        handle = it->second.handle;
        transactions = it->second.transaction_list;
        excess_data = it->second.excess_data;
    }

    if (!transactions) {
        // Unlocked. getBlock copies a block that can be eight megabytes and
        // then every transaction is serialized again, which is not work to do
        // while every other client waits.
        auto built{std::make_shared<std::vector<std::vector<uint8_t>>>()};
        const CBlock block{handle->getBlock()};
        built->reserve(block.vtx.size() > 0 ? block.vtx.size() - 1 : 0);
        for (size_t i = 1; i < block.vtx.size(); ++i) {
            DataStream ss{};
            ss << TX_WITH_WITNESS(*block.vtx[i]);
            built->emplace_back(UCharCast(ss.data()), UCharCast(ss.data()) + ss.size());
        }
        transactions = std::move(built);

        // Install it for the next asker, if the template is still here and
        // nobody beat us to it. Losing that race costs one wasted
        // construction, which is cheaper than holding the lock to prevent it.
        LOCK(m_mutex);
        const auto client_it{m_clients.find(id)};
        if (client_it != m_clients.end()) {
            const auto it{client_it->second.templates.find(request.template_id)};
            if (it != client_it->second.templates.end() && !it->second.transaction_list) {
                it->second.transaction_list = transactions;
            }
        }
    }

    RequestTransactionDataSuccess success;
    success.template_id = request.template_id;
    success.excess_data = std::move(excess_data);
    success.transaction_list = *transactions;

    if (!m_connman->Send(
            id, NetHeader{EXT_CORE, static_cast<uint8_t>(CoreMsg::REQUEST_TRANSACTION_DATA_SUCCESS), 0},
            EncodePayload(success))) {
        return false;
    }

    LogPrintLevel(BCLog::SV2, BCLog::Level::Debug,
                  "client=%d sent %d transactions for template %d\n", id,
                  transactions->size(), request.template_id);
    return true;
}

void TemplateProvider::RefundSolution(ClientId id)
{
    LOCK(m_mutex);
    const auto it{m_clients.find(id)};
    if (it == m_clients.end()) return;
    // One client's submissions are handled in order on the socket thread, so
    // the newest entry is the one this submission added.
    if (!it->second.solutions.empty()) it->second.solutions.pop_back();
}

bool TemplateProvider::RecordRefusal(ClientId id, uint64_t template_id, SolutionError error)
{
    LOCK(m_mutex);
    const auto it{m_clients.find(id)};
    if (it == m_clients.end()) return false;
    ClientState& state{it->second};

    // There is no SubmitSolution.Error in the Template Distribution Protocol,
    // so a refusal can only be logged and counted. Inventing a reply message
    // would break a stock client's dispatch.
    ++state.rejects;
    LogPrintLevel(BCLog::SV2, BCLog::Level::Debug,
                  "client=%d solution for template %d refused: %s (%d of %d)\n", id, template_id,
                  SolutionErrorString(error), state.rejects, m_opts.max_rejects);
    if (state.rejects >= m_opts.max_rejects) {
        LogPrintLevel(BCLog::SV2, BCLog::Level::Warning,
                      "client=%d refused too many solutions; it is most likely rolling a field "
                      "it was told not to, and will never find a valid block\n", id);
        return false;
    }
    return true;
}

bool TemplateProvider::HandleSubmitSolution(ClientId id, std::span<const uint8_t> payload)
{
    SubmitSolution solution;
    SolutionContext ctx;
    std::shared_ptr<interfaces::BlockTemplate> handle;
    POW_TYPE pow_type{POW_TYPE_X16RT};

    // Everything needing the provider's state happens here, and nothing
    // expensive does. The hash below costs orders of magnitude more than all
    // of it, and holding this lock across that would stall every other client
    // and block the watcher thread from touching any of them.
    {
        LOCK(m_mutex);
        const auto it{m_clients.find(id)};
        if (it == m_clients.end()) return false;
        ClientState& state{it->second};

        // Metered before anything is parsed. This is the primary control:
        // every check after it is cheap, but the hash at the end is not, and
        // a few request bytes must not be able to buy one.
        const auto arrived{std::chrono::steady_clock::now()};
        while (!state.solutions.empty() && arrived - state.solutions.front() > SOLUTION_WINDOW) {
            state.solutions.pop_front();
        }
        if (state.solutions.size() >= MAX_SOLUTIONS) {
            LogPrintLevel(BCLog::SV2, BCLog::Level::Debug,
                          "client=%d submitted more than %d solutions in %ds\n", id, MAX_SOLUTIONS,
                          SOLUTION_WINDOW.count());
            return false;
        }
        state.solutions.push_back(arrived);

        try {
            solution = DecodePayload<SubmitSolution>(payload);
        } catch (const std::ios_base::failure& e) {
            LogPrintLevel(BCLog::SV2, BCLog::Level::Debug,
                          "client=%d sent a malformed SubmitSolution: %s\n", id, e.what());
            return false;
        }

        const auto cached_it{state.templates.find(solution.template_id)};
        if (cached_it == state.templates.end()) {
            ++state.rejects;
            LogPrintLevel(BCLog::SV2, BCLog::Level::Debug,
                          "client=%d solution for template %d refused: %s (%d of %d)\n", id,
                          solution.template_id, SolutionErrorString(SolutionError::UNKNOWN_TEMPLATE),
                          state.rejects, m_opts.max_rejects);
            return state.rejects < m_opts.max_rejects;
        }
        const CachedTemplate& cached{cached_it->second};

        ctx.pow_type = cached.pow_type;
        ctx.version = cached.version;
        ctx.version_rolling_mask = m_opts.version_rolling_mask;
        ctx.nbits = cached.nbits;
        ctx.prev_hash = cached.prev_hash;
        ctx.ntime = cached.ntime;
        ctx.ntime_min = cached.ntime_min;
        ctx.ntime_max = cached.ntime_max;
        ctx.coinbase_tx_version = cached.coinbase_tx_version;
        ctx.coinbase_tx_input_sequence = cached.coinbase_tx_input_sequence;
        ctx.coinbase_tx_locktime = cached.coinbase_tx_locktime;
        ctx.coinbase_prefix = cached.coinbase_prefix;
        ctx.required_outputs = cached.required_txouts;
        ctx.value_remaining = static_cast<int64_t>(cached.value_remaining);
        ctx.max_coinbase_size = cached.max_coinbase_size;
        ctx.max_coinbase_weight = cached.max_coinbase_weight;
        ctx.max_coinbase_sigops = cached.max_coinbase_sigops;
        ctx.merkle_path = cached.merkle_path;

        // A copy of the pointer, not of the block. The template can be pruned
        // out from under us while the hash runs; the handle survives it.
        handle = cached.handle;
        pow_type = cached.pow_type;
    }

    // ComputePoWHash, deliberately not GetHash: GetHash inserts into a
    // disk-persisted cache that evicts by clearing itself entirely, so an
    // attacker able to reach it could wipe a legitimately warm cache.
    const auto& consensus{Params().GetConsensus()};
    const PoWHasher hasher{[&consensus](const CBlockHeader& header) {
        // The same two values chainparams.cpp arms SetPoWHashParams with, so
        // this path and GetHash() can never disagree about which algorithm a
        // header uses. consensus.nX16rtTimestamp looks like the right field
        // and is never assigned by any chain, so it is always zero.
        return header.ComputePoWHash(
            consensus.vUpgrades[Consensus::UPGRADE_X16RT_SWITCH].nTimestamp,
            consensus.vUpgrades[Consensus::UPGRADE_DUAL_ALGO].nTimestamp);
    }};

    const auto check{CheckSolution(ctx, solution, GetTime(), consensus, hasher)};
    if (check.error != SolutionError::NONE) {
        return RecordRefusal(id, solution.template_id, check.error);
    }

    // The proof of work verified, so this submission is not what the rate
    // limit is for and its token goes back. Done before the block is
    // submitted rather than after, because a block that loses a race is still
    // real work and the client should not be charged for it.
    RefundSolution(id);

    // min_pow_checked is justified now, and only now: the proof of work was
    // verified above against this algorithm's own limit. Unlocked too, since
    // this ends in ProcessNewBlock.
    if (!handle->submitSolution(solution.version, solution.ntime, solution.nonce, check.coinbase)) {
        // The block was rejected by validation rather than by us, which means
        // our checks and consensus disagree. Worth saying loudly.
        LogPrintLevel(BCLog::SV2, BCLog::Level::Warning,
                      "client=%d submitted a block that passed every Stratum v2 check and was "
                      "still refused by validation; template %d\n", id, solution.template_id);
        return RecordRefusal(id, solution.template_id, SolutionError::CONSENSUS_REJECTED);
    }

    LOCK(m_mutex);
    const auto it{m_clients.find(id)};
    if (it != m_clients.end()) it->second.rejects = 0;
    LogPrintf("sv2: client=%d found a block for template %d (%s)\n", id, solution.template_id,
              POW_TYPE_NAMES[pow_type]);
    return true;
}


void TemplateProvider::ThreadWatcher()
{
    uint256 last_tip;
    if (const auto tip{m_mining.getTip()}) last_tip = tip->hash;

    auto next_refresh{std::chrono::steady_clock::now() + m_opts.refresh_interval};

    while (!m_interrupt) {
        // waitTipChanged only returns early for a new tip or a node shutdown,
        // neither of which this thread's own interrupt can cause. Poll in short
        // slices instead, so stopping does not have to wait out a long timeout.
        const auto tip{m_mining.waitTipChanged(last_tip, MillisecondsDouble{std::chrono::seconds{1}})};
        if (m_interrupt) break;
        if (!tip) break; // The node is shutting down.

        if (tip->hash != last_tip) {
            last_tip = tip->hash;
            RefreshClients(/*tip_changed=*/true);
            next_refresh = std::chrono::steady_clock::now() + m_opts.refresh_interval;
            continue;
        }

        // No new tip. Refresh anyway on the interval, which is what picks up
        // new fees and keeps X16RT work inside its 128-second epoch.
        if (std::chrono::steady_clock::now() >= next_refresh) {
            RefreshClients(/*tip_changed=*/false);
            next_refresh = std::chrono::steady_clock::now() + m_opts.refresh_interval;
        }
    }
}

void TemplateProvider::RefreshClients(bool tip_changed)
{
    // Collect the ids first so the rebuild, which takes cs_main, does not
    // happen while iterating a container another thread can change.
    std::vector<ClientId> ids;
    WITH_LOCK(m_mutex, {
        for (const auto& [id, state] : m_clients) {
            if (state.constraints_received) ids.push_back(id);
        }
    });
    if (ids.empty()) return;

    if (tip_changed) {
        LogPrintLevel(BCLog::SV2, BCLog::Level::Debug,
                      "new tip, reissuing work to %d client(s)\n", ids.size());
    }

    for (const ClientId id : ids) {
        bool keep{true};
        WITH_LOCK(m_mutex, {
            const auto it{m_clients.find(id)};
            if (it != m_clients.end()) {
                // A tip change invalidates every outstanding template, so
                // nothing is left that a solution could still be valid for.
                if (tip_changed) it->second.templates.clear();
                keep = SendNewTemplate(id, it->second, /*future=*/false);
            }
        });
        if (!keep && m_connman) m_connman->Disconnect(id);
    }
}

// Process-wide lifecycle.

namespace {
std::unique_ptr<TemplateProvider> g_provider;
std::unique_ptr<Connman> g_connman;
} // namespace

Connman* GetSv2Connman() { return g_connman.get(); }

bool StartSv2(const ArgsManager& args, ChainType chain, interfaces::Mining& mining)
{
    assert(!g_connman);

    // Only the in-process mining interface has been exercised. The Cap'n Proto
    // schema does now carry pow_type, so a process-separated provider is no
    // longer obviously broken, but nothing has tested one: the provider holds
    // BlockTemplate handles across the boundary and getBlock copies a whole
    // block. Refuse until that is tried, rather than discover it in
    // production. context() is non-null only for the in-process case.
    if (mining.context() == nullptr) {
        return InitError(Untranslated(
            "-sv2 requires an in-process mining interface; the multiprocess one is untested"));
    }

    auto opts{ReadOptions(args, chain)};
    if (!opts) return InitError(util::ErrorString(opts));

    auto static_key{LoadOrCreateKey(opts->static_key_file)};
    if (!static_key) return InitError(util::ErrorString(static_key));

    auto authority_key{LoadOrCreateKey(opts->authority_key_file)};
    if (!authority_key) return InitError(util::ErrorString(authority_key));

    auto cert{MakeCertificate(*authority_key, *static_key, GetTime(), opts->cert_validity_days)};
    if (!cert) return InitError(util::ErrorString(cert));

    // Operators have to hand this to clients, which verify the server
    // certificate against it, so it belongs in the log at startup.
    const XOnlyPubKey authority_pub{authority_key->GetPubKey()};
    LogPrintf("sv2: authority public key %s\n", HexStr(authority_pub));
    if (opts->extension_id != AVIAN_EXTENSION_ID) {
        LogWarning("sv2: using extension id 0x%04x instead of the default 0x%04x", opts->extension_id,
                   AVIAN_EXTENSION_ID);
    }

    g_provider = std::make_unique<TemplateProvider>(mining, *opts);
    g_connman = std::make_unique<Connman>(*opts, *static_key, *cert);
    g_provider->SetConnman(g_connman.get());
    g_provider->Start();

    if (!g_connman->Start(g_provider.get())) {
        g_connman.reset();
        g_provider->Stop();
        g_provider.reset();
        return InitError(Untranslated("Cannot start the Stratum v2 Template Provider"));
    }
    return true;
}

void InterruptSv2()
{
    if (g_connman) g_connman->Interrupt();
    // Releases a template thread parked in waitTipChanged, which would
    // otherwise sit there until its timeout expires.
    if (g_provider) g_provider->Interrupt();
}

void StopSv2()
{
    // Stop everything before destroying anything. The provider's watcher
    // thread holds a raw Connman* and calls Send() on it, so destroying the
    // listener while that thread is still running is a use-after-free. The
    // listener is stopped first so no new work arrives, but it outlives the
    // thread that might still be talking to it.
    if (g_connman) g_connman->Stop();
    if (g_provider) g_provider->Stop();

    // Now nothing is running. The provider goes first because it is the one
    // holding the pointer.
    g_provider.reset();
    g_connman.reset();
}

} // namespace sv2
