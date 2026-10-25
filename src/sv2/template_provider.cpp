// Copyright (c) 2026 The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <sv2/template_provider.h>

#include <chain.h>
#include <common/args.h>
#include <common/init.h>
#include <chainparams.h>
#include <consensus/consensus.h>
#include <logging.h>
#include <node/interface_ui.h>
#include <node/types.h>
#include <policy/policy.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <streams.h>
#include <util/strencodings.h>
#include <util/time.h>
#include <util/translation.h>

#include <algorithm>
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
    LOCK(m_mutex);
    const auto it{m_clients.find(id)};
    if (it == m_clients.end()) return false;

    if (header.Extension() != EXT_CORE) {
        LogPrintLevel(BCLog::SV2, BCLog::Level::Debug,
                      "client=%d sent extension 0x%04x, which carries no template messages\n", id,
                      header.Extension());
        return false;
    }

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

    return SendNewTemplate(id, state);
}

bool TemplateProvider::SendNewTemplate(ClientId id, ClientState& state)
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
    cached.pow_type = state.pow_type;
    cached.version = static_cast<uint32_t>(header.nVersion);
    cached.nbits = header.nBits;
    cached.prev_hash = header.hashPrevBlock;
    cached.ntime = header.nTime;
    cached.coinbase_tx_version = static_cast<uint32_t>(coinbase->version);
    cached.coinbase_tx_input_sequence = coinbase->vin[0].nSequence;
    cached.coinbase_tx_locktime = coinbase->nLockTime;
    cached.required_outputs = SerializeOutputs(required);
    cached.required_outputs_count = static_cast<uint32_t>(required.size());
    cached.value_remaining = static_cast<uint64_t>(coinbase->vout[0].nValue);
    cached.merkle_path = handle->getCoinbaseMerklePath();

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
    // Request-driven, so this template is for the current tip rather than a
    // future one. Tip-change pushes arrive with SetNewPrevHash.
    msg.future_template = false;
    msg.version = cached.version;
    msg.coinbase_tx_version = cached.coinbase_tx_version;
    msg.coinbase_prefix = cached.coinbase_prefix;
    msg.coinbase_tx_input_sequence = cached.coinbase_tx_input_sequence;
    msg.coinbase_tx_value_remaining = cached.value_remaining;
    msg.coinbase_tx_outputs_count = cached.required_outputs_count;
    msg.coinbase_tx_outputs = cached.required_outputs;
    msg.coinbase_tx_locktime = cached.coinbase_tx_locktime;
    msg.merkle_path = cached.merkle_path;

    AvianTimeConstraints times;
    times.template_id = template_id;
    times.ntime_min = cached.ntime_min;
    times.ntime_max = cached.ntime_max;

    cached.handle = std::move(handle);
    state.templates.emplace(template_id, std::move(cached));

    if (!m_connman->Send(id, NetHeader{EXT_CORE, static_cast<uint8_t>(CoreMsg::NEW_TEMPLATE), 0},
                         EncodePayload(msg))) {
        return false;
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
    return true;
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

    if (!g_connman->Start(g_provider.get())) {
        g_connman.reset();
        g_provider.reset();
        return InitError(Untranslated("Cannot start the Stratum v2 Template Provider"));
    }
    return true;
}

void InterruptSv2()
{
    if (g_connman) g_connman->Interrupt();
}

void StopSv2()
{
    // The listener goes first: it holds the clients whose cached templates
    // reference the mining interface.
    if (g_connman) {
        g_connman->Stop();
        g_connman.reset();
    }
    g_provider.reset();
}

} // namespace sv2
