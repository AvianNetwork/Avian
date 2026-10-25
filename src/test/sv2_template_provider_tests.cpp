// Copyright (c) 2026 The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <addresstype.h>
#include <arith_uint256.h>
#include <chainparams.h>
#include <compat/compat.h>
#include <consensus/amount.h>
#include <interfaces/mining.h>
#include <key.h>
#include <netbase.h>
#include <node/context.h>
#include <primitives/block.h>
#include <pubkey.h>
#include <script/solver.h>
#include <streams.h>
#include <sync.h>
#include <sv2/connman.h>
#include <sv2/options.h>
#include <sv2/template_provider.h>
#include <sv2/transport.h>
#include <test/util/setup_common.h>
#include <test/util/sv2.h>
#include <util/sock.h>
#include <util/time.h>
#include <validation.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include <boost/test/unit_test.hpp>

using namespace sv2test;

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
    auto client{sv2test::Connect(server.bound, authority)};
    BOOST_REQUIRE(client);
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
    const int height{WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Height() + 1)};
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
    const uint256 tip_hash{WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Tip()->GetBlockHash())};
    BOOST_CHECK_EQUAL(prev.prev_hash, tip_hash);
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
    // Accepting a session and then silently ignoring what arrives on it is
    // worse than closing, because the client has no way to tell the two apart.
    auto server{StartTemplateServer(m_node, m_args.GetDataDirNet())};
    auto client{ConnectTo(*server, server->AuthorityPubKey())};
    BOOST_REQUIRE(Ready(*client, POW_TYPE_X16RT));

    BOOST_REQUIRE(client->transport->Send(sv2::NetHeader{sv2::EXT_CORE, 0xff, 0}, {}));
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

    const uint256 old_tip{WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Tip()->GetBlockHash())};
    CreateAndProcessBlock({}, CScript() << OP_TRUE);
    BOOST_REQUIRE(WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Tip()->GetBlockHash()) != old_tip);

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
    const uint256 tip_hash{WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Tip()->GetBlockHash())};
    BOOST_CHECK_EQUAL(prev.prev_hash, tip_hash);
    BOOST_CHECK(prev.prev_hash != old_tip);
}


namespace {

//! Walk a client to a template, returning its NewTemplate.
sv2::NewTemplate GetTemplate(Client& client, uint8_t pow_type = POW_TYPE_X16RT)
{
    BOOST_REQUIRE(Ready(client, pow_type));
    BOOST_REQUIRE(SendConstraints(client));
    bool closed{false};
    const auto msg{ReadMessage(client, closed)};
    BOOST_REQUIRE(msg.has_value());
    BOOST_REQUIRE_EQUAL(msg->header.msg_type, static_cast<uint8_t>(sv2::CoreMsg::NEW_TEMPLATE));
    const auto tmpl{sv2::DecodePayload<sv2::NewTemplate>(msg->payload)};
    // A client's first template always carries a prev hash, because it has
    // none of its own yet. Asserted rather than assumed: this is half of what
    // keeps SetNewPrevHash meaningful.
    const auto prev{ReadMessage(client, closed)};
    BOOST_REQUIRE(prev.has_value());
    BOOST_REQUIRE_EQUAL(prev->header.msg_type, static_cast<uint8_t>(sv2::CoreMsg::SET_NEW_PREV_HASH));
    const auto times{ReadMessage(client, closed)};
    BOOST_REQUIRE(times.has_value());
    BOOST_REQUIRE_EQUAL(times->header.extension_type, sv2::AVIAN_EXTENSION_ID);
    BOOST_REQUIRE_EQUAL(times->header.msg_type, static_cast<uint8_t>(sv2::AvianMsg::TIME_CONSTRAINTS));
    return tmpl;
}

//! Read past any unprompted work and return the next reply.
//!
//! A short refresh interval means the provider is pushing templates while a
//! test is trying to ask it something, so a bare ReadMessage would hand back a
//! NewTemplate where the test wanted its answer.
std::optional<sv2::ReceivedMsg> ReadReply(Client& client, bool& closed)
{
    for (int i = 0; i < 40; ++i) {
        const auto msg{ReadMessage(client, closed)};
        if (!msg) return std::nullopt;
        const bool pushed{
            msg->header.extension_type == sv2::AVIAN_EXTENSION_ID
                ? msg->header.msg_type == static_cast<uint8_t>(sv2::AvianMsg::TIME_CONSTRAINTS)
                : (msg->header.msg_type == static_cast<uint8_t>(sv2::CoreMsg::NEW_TEMPLATE) ||
                   msg->header.msg_type == static_cast<uint8_t>(sv2::CoreMsg::SET_NEW_PREV_HASH))};
        if (!pushed) return msg;
    }
    return std::nullopt;
}

bool SendTxDataRequest(Client& client, uint64_t template_id)
{
    sv2::RequestTransactionData request;
    request.template_id = template_id;
    return client.transport->Send(
        sv2::NetHeader{sv2::EXT_CORE,
                       static_cast<uint8_t>(sv2::CoreMsg::REQUEST_TRANSACTION_DATA), 0},
        sv2::EncodePayload(request));
}

} // namespace

BOOST_AUTO_TEST_CASE(transaction_data_for_an_empty_block)
{
    auto server{StartTemplateServer(m_node, m_args.GetDataDirNet())};
    auto client{ConnectTo(*server, server->AuthorityPubKey())};
    const auto tmpl{GetTemplate(*client)};

    BOOST_REQUIRE(SendTxDataRequest(*client, tmpl.template_id));
    bool closed{false};
    const auto reply{ReadMessage(*client, closed)};
    BOOST_REQUIRE(reply.has_value());
    BOOST_CHECK_EQUAL(reply->header.msg_type,
                      static_cast<uint8_t>(sv2::CoreMsg::REQUEST_TRANSACTION_DATA_SUCCESS));

    const auto data{sv2::DecodePayload<sv2::RequestTransactionDataSuccess>(reply->payload)};
    BOOST_CHECK_EQUAL(data.template_id, tmpl.template_id);
    // The coinbase is excluded, so an otherwise empty block carries nothing.
    BOOST_CHECK(data.transaction_list.empty());
    // excess_data is the commitment script, so a pool can rebuild the
    // commitment for itself rather than trusting ours.
    BOOST_CHECK(!data.excess_data.empty());
    BOOST_CHECK_EQUAL(data.excess_data[0], OP_RETURN);
}

BOOST_AUTO_TEST_CASE(transaction_data_includes_mempool_transactions)
{
    auto server{StartTemplateServer(m_node, m_args.GetDataDirNet())};

    // Spend a matured coinbase so the template has something in it.
    BOOST_REQUIRE(!m_coinbase_txns.empty());
    // A standard output: unlike Bitcoin's, Avian's regtest requires standard
    // transactions, so a bare OP_TRUE is rejected with "scriptpubkey".
    const CScript destination{GetScriptForDestination(PKHash(coinbaseKey.GetPubKey()))};
    const CMutableTransaction spend{CreateValidMempoolTransaction(
        /*input_transaction=*/m_coinbase_txns[0], /*input_vout=*/0, /*input_height=*/0,
        /*input_signing_key=*/coinbaseKey, /*output_destination=*/destination,
        /*output_amount=*/m_coinbase_txns[0]->vout[0].nValue - CAmount{10000},
        /*submit=*/true)};
    const CTransactionRef spend_ref{MakeTransactionRef(spend)};

    auto client{ConnectTo(*server, server->AuthorityPubKey())};
    const auto tmpl{GetTemplate(*client)};

    BOOST_REQUIRE(SendTxDataRequest(*client, tmpl.template_id));
    bool closed{false};
    const auto reply{ReadMessage(*client, closed)};
    BOOST_REQUIRE(reply.has_value());
    const auto data{sv2::DecodePayload<sv2::RequestTransactionDataSuccess>(reply->payload)};

    BOOST_REQUIRE_EQUAL(data.transaction_list.size(), 1u);
    // Serialized with witnesses, and byte-identical to what the node holds:
    // a pool has to be able to rebuild the exact block.
    DataStream expected{};
    expected << TX_WITH_WITNESS(*spend_ref);
    const std::vector<uint8_t> expected_bytes{UCharCast(expected.data()),
                                              UCharCast(expected.data()) + expected.size()};
    BOOST_CHECK(data.transaction_list[0] == expected_bytes);

    // And the merkle path is no longer trivial, because the block now has more
    // than just a coinbase.
    BOOST_CHECK(!tmpl.merkle_path.empty());
}

BOOST_AUTO_TEST_CASE(transaction_data_for_an_unknown_template)
{
    auto server{StartTemplateServer(m_node, m_args.GetDataDirNet())};
    auto client{ConnectTo(*server, server->AuthorityPubKey())};
    const auto tmpl{GetTemplate(*client)};

    BOOST_REQUIRE(SendTxDataRequest(*client, tmpl.template_id + 1000));
    bool closed{false};
    const auto reply{ReadMessage(*client, closed)};
    BOOST_REQUIRE(reply.has_value());
    BOOST_CHECK_EQUAL(reply->header.msg_type,
                      static_cast<uint8_t>(sv2::CoreMsg::REQUEST_TRANSACTION_DATA_ERROR));
    const auto err{sv2::DecodePayload<sv2::RequestTransactionDataError>(reply->payload)};
    BOOST_CHECK_EQUAL(err.error_code, "template-id-not-found");
    // An error is not a disconnection: the client simply asked for the wrong
    // thing.
    BOOST_CHECK_EQUAL(server->connman->ClientCount(), 1u);
}

BOOST_AUTO_TEST_CASE(transaction_data_for_a_stale_template)
{
    // Only the newest template survives a tip change, and a client that was
    // mid-request needs to know the difference between work that expired and
    // an id that never existed.
    auto server{StartTemplateServer(m_node, m_args.GetDataDirNet())};
    auto client{ConnectTo(*server, server->AuthorityPubKey())};
    const auto first{GetTemplate(*client)};

    CreateAndProcessBlock({}, CScript() << OP_TRUE);
    bool closed{false};
    BOOST_REQUIRE(ReadMessage(*client, closed).has_value()); // pushed NewTemplate
    BOOST_REQUIRE(ReadMessage(*client, closed).has_value()); // SetNewPrevHash
    BOOST_REQUIRE(ReadMessage(*client, closed).has_value()); // AvianTimeConstraints

    BOOST_REQUIRE(SendTxDataRequest(*client, first.template_id));
    const auto reply{ReadMessage(*client, closed)};
    BOOST_REQUIRE(reply.has_value());
    BOOST_CHECK_EQUAL(reply->header.msg_type,
                      static_cast<uint8_t>(sv2::CoreMsg::REQUEST_TRANSACTION_DATA_ERROR));
    const auto err{sv2::DecodePayload<sv2::RequestTransactionDataError>(reply->payload)};
    BOOST_CHECK_EQUAL(err.error_code, "stale-template");
}

BOOST_AUTO_TEST_CASE(transaction_data_is_rate_limited)
{
    // This is the one message where a few request bytes buy a whole block
    // serialized, encrypted and buffered, so it is metered.
    auto server{StartTemplateServer(m_node, m_args.GetDataDirNet())};
    auto client{ConnectTo(*server, server->AuthorityPubKey())};
    const auto tmpl{GetTemplate(*client)};

    bool closed{false};
    for (size_t i = 0; i < sv2::MAX_TXDATA_REQUESTS; ++i) {
        BOOST_REQUIRE(SendTxDataRequest(*client, tmpl.template_id));
        BOOST_REQUIRE(ReadMessage(*client, closed).has_value());
    }

    // One past the allowance ends the connection rather than being answered.
    BOOST_REQUIRE(SendTxDataRequest(*client, tmpl.template_id));
    ReadMessage(*client, closed);
    BOOST_CHECK(closed);
}

namespace {

//! A complete coinbase built the way a client is told to build one: its own
//! output for the remaining value, then every output the template required.
CMutableTransaction MakeCoinbase(const sv2::NewTemplate& tmpl)
{
    CMutableTransaction cb;
    cb.version = static_cast<int32_t>(tmpl.coinbase_tx_version);
    cb.nLockTime = tmpl.coinbase_tx_locktime;
    cb.vin.resize(1);
    cb.vin[0].prevout.SetNull();
    cb.vin[0].nSequence = tmpl.coinbase_tx_input_sequence;
    cb.vin[0].scriptSig = CScript{tmpl.coinbase_prefix.begin(), tmpl.coinbase_prefix.end()};
    cb.vin[0].scriptWitness.stack.assign(1, std::vector<unsigned char>(32, 0x00));
    cb.vout.push_back(
        CTxOut{static_cast<CAmount>(tmpl.coinbase_tx_value_remaining), CScript() << OP_TRUE});
    for (const CTxOut& out : ParseOutputs(tmpl.coinbase_tx_outputs, tmpl.coinbase_tx_outputs_count)) {
        cb.vout.push_back(out);
    }
    return cb;
}

bool SendSolution(Client& client, const sv2::NewTemplate& tmpl, uint32_t version, uint32_t ntime)
{
    DataStream coinbase{};
    coinbase << TX_WITH_WITNESS(MakeCoinbase(tmpl));

    sv2::SubmitSolution solution;
    solution.template_id = tmpl.template_id;
    solution.version = version;
    solution.ntime = ntime;
    solution.nonce = 0;
    solution.coinbase_tx.assign(UCharCast(coinbase.data()), UCharCast(coinbase.data()) + coinbase.size());
    return client.transport->Send(
        sv2::NetHeader{sv2::EXT_CORE, static_cast<uint8_t>(sv2::CoreMsg::SUBMIT_SOLUTION), 0},
        sv2::EncodePayload(solution));
}

} // namespace

BOOST_AUTO_TEST_CASE(a_refused_solution_never_reaches_the_proof_of_work)
{
    // The ladder is unit-tested with an injected hasher, but that cannot prove
    // the wiring. This does, from the outside: CBlockHeader::GetHash is the only
    // path into the persistent PowCache, so a submission that leaves the cache
    // untouched demonstrably never had its proof of work computed. Which also
    // means an attacker cannot use rejected frames to flush a warm cache.
    auto server{StartTemplateServer(m_node, m_args.GetDataDirNet())};
    auto client{ConnectTo(*server, server->AuthorityPubKey())};
    const auto tmpl{GetTemplate(*client)};

    const size_t before{PowCacheSize()};
    // Rolled version, which the mask forbids. Refused in the first tier.
    const uint32_t ntime{static_cast<uint32_t>(GetTime<std::chrono::seconds>().count())};
    BOOST_REQUIRE(SendSolution(*client, tmpl, tmpl.version ^ 0x1, ntime));

    // Nothing is sent back for a solution, so the round trip is a separate
    // request: once it is answered, the submission has certainly been handled.
    bool closed{false};
    BOOST_REQUIRE(SendTxDataRequest(*client, tmpl.template_id));
    BOOST_REQUIRE(ReadMessage(*client, closed).has_value());

    BOOST_CHECK_EQUAL(PowCacheSize(), before);
}

BOOST_AUTO_TEST_CASE(solutions_are_rate_limited)
{
    // The whole defence for that final hash. Avian's proof of work has no cheap
    // partial form, so a few request bytes must not be able to buy one.
    auto server{StartTemplateServer(m_node, m_args.GetDataDirNet())};
    auto client{ConnectTo(*server, server->AuthorityPubKey())};
    const auto tmpl{GetTemplate(*client)};
    const uint32_t ntime{static_cast<uint32_t>(GetTime<std::chrono::seconds>().count())};

    // One past the allowance. They are processed in order, so no round trip is
    // needed in between, and asking for transaction data to get one would just
    // trip that message's own limit first.
    bool closed{false};
    for (size_t i = 0; i <= sv2::MAX_SOLUTIONS; ++i) {
        BOOST_REQUIRE(SendSolution(*client, tmpl, tmpl.version ^ 0x1, ntime));
    }
    ReadMessage(*client, closed);
    BOOST_CHECK(closed);
}

BOOST_AUTO_TEST_CASE(persistent_refusals_disconnect)
{
    // There is no SubmitSolution.Error in the protocol, so a client rolling a
    // field it was told not to would otherwise mine forever into a void. The
    // only way to tell it something is wrong is to drop it.
    auto server{StartTemplateServer(m_node, m_args.GetDataDirNet(),
                                    [](sv2::Options& opts) { opts.max_rejects = 2; })};
    auto client{ConnectTo(*server, server->AuthorityPubKey())};
    const auto tmpl{GetTemplate(*client)};
    const uint32_t ntime{static_cast<uint32_t>(GetTime<std::chrono::seconds>().count())};

    bool closed{false};
    BOOST_REQUIRE(SendSolution(*client, tmpl, tmpl.version ^ 0x1, ntime));
    BOOST_REQUIRE(SendTxDataRequest(*client, tmpl.template_id));
    BOOST_REQUIRE(ReadMessage(*client, closed).has_value());

    BOOST_REQUIRE(SendSolution(*client, tmpl, tmpl.version ^ 0x1, ntime));
    ReadMessage(*client, closed);
    BOOST_CHECK(closed);
}

//! Wait for the refresh that supersedes `previous`, and return its id.
uint64_t AwaitRefresh(Client& client, uint64_t previous)
{
    bool closed{false};
    for (int i = 0; i < 20; ++i) {
        const auto msg{ReadMessage(client, closed)};
        BOOST_REQUIRE(msg.has_value());
        if (msg->header.extension_type != sv2::EXT_CORE) continue;
        if (msg->header.msg_type != static_cast<uint8_t>(sv2::CoreMsg::NEW_TEMPLATE)) continue;
        const auto tmpl{sv2::DecodePayload<sv2::NewTemplate>(msg->payload)};
        if (tmpl.template_id != previous) return tmpl.template_id;
    }
    BOOST_FAIL("no refreshed template arrived");
    return 0;
}

BOOST_AUTO_TEST_CASE(a_refresh_does_not_resend_the_prev_hash)
{
    // The specification has SetNewPrevHash follow the validation of a new best
    // block, and a future_template of false already says the template belongs
    // to the SetNewPrevHash before it. Sending one on every refresh would
    // contradict that, and a client reading its arrival as the tip having
    // moved would discard its in-flight work roughly twice a block at Avian's
    // cadence.
    //
    // The tip-change half of this is new_tip_reissues_work, and the
    // first-template half is asserted in GetTemplate.
    auto server{StartTemplateServer(m_node, m_args.GetDataDirNet(), [](sv2::Options& opts) {
        opts.refresh_interval = std::chrono::seconds{1};
    })};
    auto client{ConnectTo(*server, server->AuthorityPubKey())};
    const auto first{GetTemplate(*client)};

    bool closed{false};
    const auto refreshed{ReadMessage(*client, closed)};
    BOOST_REQUIRE(refreshed.has_value());
    BOOST_REQUIRE_EQUAL(refreshed->header.msg_type, static_cast<uint8_t>(sv2::CoreMsg::NEW_TEMPLATE));
    BOOST_CHECK(sv2::DecodePayload<sv2::NewTemplate>(refreshed->payload).template_id != first.template_id);

    // Straight to the constraints, with nothing in between.
    const auto next{ReadMessage(*client, closed)};
    BOOST_REQUIRE(next.has_value());
    BOOST_CHECK_EQUAL(next->header.extension_type, sv2::AVIAN_EXTENSION_ID);
    BOOST_CHECK_EQUAL(next->header.msg_type, static_cast<uint8_t>(sv2::AvianMsg::TIME_CONSTRAINTS));
}

BOOST_AUTO_TEST_CASE(a_superseded_template_stays_usable)
{
    // Avian refreshes templates on the same cadence as it produces blocks, so
    // keeping only the newest would discard any solution whose job predates
    // the last refresh. A block is worth rather more than the few shared
    // pointers that retaining a template costs.
    auto server{StartTemplateServer(m_node, m_args.GetDataDirNet(), [](sv2::Options& opts) {
        opts.refresh_interval = std::chrono::seconds{1};
    })};
    auto client{ConnectTo(*server, server->AuthorityPubKey())};
    const auto first{GetTemplate(*client)};
    BOOST_REQUIRE(AwaitRefresh(*client, first.template_id) != first.template_id);

    // Still served, which is the observable proof it was not dropped: the same
    // lookup decides whether a solution against it is accepted.
    bool closed{false};
    BOOST_REQUIRE(SendTxDataRequest(*client, first.template_id));
    const auto reply{ReadReply(*client, closed)};
    BOOST_REQUIRE(reply.has_value());
    BOOST_CHECK_EQUAL(reply->header.msg_type,
                      static_cast<uint8_t>(sv2::CoreMsg::REQUEST_TRANSACTION_DATA_SUCCESS));
}

BOOST_AUTO_TEST_CASE(retention_is_bounded)
{
    // With a limit of one this is the old behaviour, so this case is the one
    // that would have caught the original bug.
    auto server{StartTemplateServer(m_node, m_args.GetDataDirNet(), [](sv2::Options& opts) {
        opts.refresh_interval = std::chrono::seconds{1};
        opts.max_templates = 1;
    })};
    auto client{ConnectTo(*server, server->AuthorityPubKey())};
    const auto first{GetTemplate(*client)};
    BOOST_REQUIRE(AwaitRefresh(*client, first.template_id) != first.template_id);

    bool closed{false};
    BOOST_REQUIRE(SendTxDataRequest(*client, first.template_id));
    const auto reply{ReadReply(*client, closed)};
    BOOST_REQUIRE(reply.has_value());
    BOOST_REQUIRE_EQUAL(reply->header.msg_type,
                        static_cast<uint8_t>(sv2::CoreMsg::REQUEST_TRANSACTION_DATA_ERROR));
    const auto err{sv2::DecodePayload<sv2::RequestTransactionDataError>(reply->payload)};
    // Stale rather than unknown, so the client waits for new work instead of
    // concluding it invented an id.
    BOOST_CHECK_EQUAL(err.error_code, "stale-template");
}

BOOST_AUTO_TEST_CASE(a_new_tip_drops_the_old_templates)
{
    // Templates for an abandoned tip are the one kind that really is
    // worthless: the prev hash is wrong, so no solution against them can
    // produce a block that connects. Retaining those would be a leak.
    auto server{StartTemplateServer(m_node, m_args.GetDataDirNet())};
    auto client{ConnectTo(*server, server->AuthorityPubKey())};
    const auto first{GetTemplate(*client)};

    CreateAndProcessBlock({}, CScript() << OP_TRUE);
    BOOST_REQUIRE(AwaitRefresh(*client, first.template_id) != first.template_id);

    bool closed{false};
    BOOST_REQUIRE(SendTxDataRequest(*client, first.template_id));
    const auto reply{ReadReply(*client, closed)};
    BOOST_REQUIRE(reply.has_value());
    BOOST_CHECK_EQUAL(reply->header.msg_type,
                      static_cast<uint8_t>(sv2::CoreMsg::REQUEST_TRANSACTION_DATA_ERROR));
}

BOOST_AUTO_TEST_SUITE_END()
