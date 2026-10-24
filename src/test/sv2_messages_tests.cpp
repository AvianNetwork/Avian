// Copyright (c) 2026 The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <streams.h>
#include <sv2/messages.h>
#include <sv2/types.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <util/strencodings.h>

#include <ios>
#include <string>
#include <vector>

#include <boost/test/unit_test.hpp>

//! These tests pin the Sv2 wire format byte for byte, not just round-trip
//! stability. A codec that round-trips against itself but disagrees with the
//! spec would leave the Template Provider silently unable to talk to anything
//! else, so the expected hex below is the actual contract.
BOOST_FIXTURE_TEST_SUITE(sv2_messages_tests, BasicTestingSetup)

namespace {
template <typename M>
std::string WireHex(const M& msg)
{
    return HexStr(sv2::EncodePayload(msg));
}

//! Serialize an arbitrary value through a formatter, for exercising the
//! primitive types directly rather than via a message that happens to use them.
template <typename Formatter, typename T>
std::string FormatHex(const T& value)
{
    DataStream ss{};
    ss << Using<Formatter>(value);
    return HexStr(ss);
}

template <typename Formatter, typename T>
void FormatRead(const std::string& hex, T& out)
{
    const auto bytes{ParseHex(hex)};
    SpanReader reader{std::span<const unsigned char>{bytes}};
    reader >> Using<Formatter>(out);
}
} // namespace

BOOST_AUTO_TEST_CASE(primitive_integers)
{
    // Multibyte integers are little-endian throughout (sv2-spec 3.1).
    BOOST_CHECK_EQUAL(FormatHex<sv2::U24>(uint32_t{0}), "000000");
    BOOST_CHECK_EQUAL(FormatHex<sv2::U24>(uint32_t{1}), "010000");
    BOOST_CHECK_EQUAL(FormatHex<sv2::U24>(uint32_t{0x010203}), "030201");
    BOOST_CHECK_EQUAL(FormatHex<sv2::U24>(uint32_t{0xffffff}), "ffffff");

    // A U24 cannot carry more than three bytes worth.
    BOOST_CHECK_THROW(FormatHex<sv2::U24>(uint32_t{0x01000000}), std::ios_base::failure);

    uint32_t v{0};
    FormatRead<sv2::U24>("030201", v);
    BOOST_CHECK_EQUAL(v, 0x010203u);
}

BOOST_AUTO_TEST_CASE(primitive_bool)
{
    BOOST_CHECK_EQUAL(FormatHex<sv2::Bool>(false), "00");
    BOOST_CHECK_EQUAL(FormatHex<sv2::Bool>(true), "01");

    // Seven high bits are padding and only the least significant bit counts,
    // so 0x02 is false rather than a parse error (sv2-spec 3.1).
    bool b{true};
    FormatRead<sv2::Bool>("02", b);
    BOOST_CHECK(!b);

    b = false;
    FormatRead<sv2::Bool>("ff", b);
    BOOST_CHECK(b);
}

BOOST_AUTO_TEST_CASE(primitive_blobs)
{
    using Bytes = std::vector<uint8_t>;

    // Length prefix widths: one byte for B0_255, two for B0_64K, three for B0_16M.
    BOOST_CHECK_EQUAL(FormatHex<sv2::B0_255>(Bytes{}), "00");
    BOOST_CHECK_EQUAL(FormatHex<sv2::B0_255>(Bytes{0xaa}), "01aa");
    BOOST_CHECK_EQUAL(FormatHex<sv2::B0_64K>(Bytes{}), "0000");
    BOOST_CHECK_EQUAL(FormatHex<sv2::B0_64K>(Bytes{0xaa, 0xbb}), "0200aabb");
    BOOST_CHECK_EQUAL(FormatHex<sv2::B0_16M>(Bytes{}), "000000");
    BOOST_CHECK_EQUAL(FormatHex<sv2::B0_16M>(Bytes{0xaa}), "010000aa");

    BOOST_CHECK_EQUAL(FormatHex<sv2::Str0_255>(std::string{}), "00");
    BOOST_CHECK_EQUAL(FormatHex<sv2::Str0_255>(std::string{"ab"}), "026162");

    // At the limit it encodes; one past it is an error rather than a silent
    // truncation of the length prefix.
    BOOST_CHECK_NO_THROW(FormatHex<sv2::B0_255>(Bytes(255, 0)));
    BOOST_CHECK_THROW(FormatHex<sv2::B0_255>(Bytes(256, 0)), std::ios_base::failure);
    BOOST_CHECK_NO_THROW(FormatHex<sv2::B0_32>(Bytes(32, 0)));
    BOOST_CHECK_THROW(FormatHex<sv2::B0_32>(Bytes(33, 0)), std::ios_base::failure);

    // B0_32 shares the one-byte prefix of B0_255, so a declared length can
    // exceed the type's own maximum. That must be rejected on read.
    Bytes out;
    BOOST_CHECK_THROW(FormatRead<sv2::B0_32>("21" + std::string(66, '0'), out), std::ios_base::failure);

    // A truncated payload is an error, not a short read.
    BOOST_CHECK_THROW(FormatRead<sv2::B0_255>("05aabb", out), std::ios_base::failure);
}

BOOST_AUTO_TEST_CASE(blob_exceeding_alloc_chunk)
{
    // Reads are batched so a three-byte length prefix cannot buy a large
    // allocation for free. Cross the batch boundary to prove the loop
    // reassembles the value correctly rather than dropping or duplicating a
    // chunk.
    const size_t len{sv2::MAX_ALLOC_CHUNK + 100};
    std::vector<uint8_t> big(len);
    for (size_t i = 0; i < len; ++i) big[i] = static_cast<uint8_t>(i & 0xff);

    DataStream ss{};
    ss << Using<sv2::B0_16M>(big);

    std::vector<uint8_t> out;
    ss >> Using<sv2::B0_16M>(out);
    BOOST_CHECK_EQUAL(out.size(), len);
    BOOST_CHECK(out == big);
}

BOOST_AUTO_TEST_CASE(primitive_sequences)
{
    std::vector<uint16_t> seq{0x0001, 0x4001};
    // Two-byte count, then little-endian U16 elements.
    BOOST_CHECK_EQUAL(FormatHex<sv2::Seq0_64K<>>(seq), "0200" "0100" "0140");
    BOOST_CHECK_EQUAL(FormatHex<sv2::Seq0_64K<>>(std::vector<uint16_t>{}), "0000");

    // A one-byte count for SEQ0_255, and U256 elements are 32 raw bytes.
    std::vector<uint256> path{uint256::ONE};
    BOOST_CHECK_EQUAL(FormatHex<sv2::Seq0_255<>>(path),
                      "01" "0100000000000000000000000000000000000000000000000000000000000000");

    BOOST_CHECK_THROW(FormatHex<sv2::Seq0_255<>>(std::vector<uint256>(256)), std::ios_base::failure);
}

BOOST_AUTO_TEST_CASE(net_header)
{
    // extension_type U16, msg_type U8, msg_length U24, all little-endian.
    const sv2::NetHeader hdr{sv2::EXT_NEGOTIATION, static_cast<uint8_t>(sv2::NegotiationMsg::REQUEST_EXTENSIONS), 4};
    BOOST_CHECK_EQUAL(WireHex(hdr), "0100" "00" "040000");
    BOOST_CHECK_EQUAL(sv2::EncodePayload(hdr).size(), sv2::HEADER_SIZE);

    const auto decoded{sv2::DecodePayload<sv2::NetHeader>(sv2::EncodePayload(hdr))};
    BOOST_CHECK_EQUAL(decoded.extension_type, sv2::EXT_NEGOTIATION);
    BOOST_CHECK_EQUAL(decoded.msg_length, 4u);
    BOOST_CHECK(!decoded.IsChannelMsg());

    // The channel_msg bit is the top bit of extension_type and is masked off
    // when matching an extension, so 0x8001 and 0x0001 are the same extension.
    const sv2::NetHeader channel{sv2::CHANNEL_MSG_BIT | sv2::EXT_NEGOTIATION, 0, 0};
    BOOST_CHECK(channel.IsChannelMsg());
    BOOST_CHECK_EQUAL(channel.Extension(), sv2::EXT_NEGOTIATION);
    BOOST_CHECK_EQUAL(WireHex(channel), "0180" "00" "000000");
}

BOOST_AUTO_TEST_CASE(common_protocol_messages)
{
    sv2::SetupConnection setup;
    setup.protocol = static_cast<uint8_t>(sv2::Protocol::TEMPLATE_DISTRIBUTION);
    setup.min_version = sv2::PROTOCOL_VERSION;
    setup.max_version = sv2::PROTOCOL_VERSION;
    setup.flags = 0;
    setup.endpoint_host = "h";
    setup.endpoint_port = 8442;
    setup.vendor = "av";
    BOOST_CHECK_EQUAL(WireHex(setup),
                      "02"       // protocol = template distribution
                      "0200"     // min_version
                      "0200"     // max_version
                      "00000000" // flags
                      "0168"     // endpoint_host "h"
                      "fa20"     // endpoint_port 8442
                      "026176"   // vendor "av"
                      "00"       // hardware_version
                      "00"       // firmware
                      "00");     // device_id

    const auto round{sv2::DecodePayload<sv2::SetupConnection>(sv2::EncodePayload(setup))};
    BOOST_CHECK_EQUAL(round.protocol, setup.protocol);
    BOOST_CHECK_EQUAL(round.endpoint_port, 8442);
    BOOST_CHECK_EQUAL(round.vendor, "av");
    BOOST_CHECK_EQUAL(round.device_id, "");

    const sv2::SetupConnectionSuccess success{sv2::PROTOCOL_VERSION, 0};
    BOOST_CHECK_EQUAL(WireHex(success), "0200" "00000000");

    sv2::SetupConnectionError err;
    err.flags = 0;
    err.error_code = "unsupported-protocol";
    BOOST_CHECK_EQUAL(sv2::DecodePayload<sv2::SetupConnectionError>(sv2::EncodePayload(err)).error_code,
                      "unsupported-protocol");
}

BOOST_AUTO_TEST_CASE(extension_negotiation_messages)
{
    sv2::RequestExtensions req;
    req.request_id = 7;
    req.requested_extensions = {sv2::EXT_NEGOTIATION, sv2::AVIAN_EXTENSION_ID};
    BOOST_CHECK_EQUAL(WireHex(req), "0700" "0200" "0100" "0140");

    const auto round{sv2::DecodePayload<sv2::RequestExtensions>(sv2::EncodePayload(req))};
    BOOST_CHECK_EQUAL(round.request_id, 7);
    BOOST_CHECK(round.requested_extensions == req.requested_extensions);

    sv2::RequestExtensionsError err;
    err.request_id = 7;
    err.unsupported_extensions = {0x4002};
    err.required_extensions = {sv2::AVIAN_EXTENSION_ID};
    const auto err_round{sv2::DecodePayload<sv2::RequestExtensionsError>(sv2::EncodePayload(err))};
    BOOST_CHECK(err_round.required_extensions == std::vector<uint16_t>{sv2::AVIAN_EXTENSION_ID});
}

BOOST_AUTO_TEST_CASE(template_distribution_messages)
{
    const sv2::CoinbaseOutputConstraints constraints{0x01020304, 0x0506};
    BOOST_CHECK_EQUAL(WireHex(constraints), "04030201" "0605");

    sv2::NewTemplate tmpl;
    tmpl.template_id = 1;
    tmpl.future_template = true;
    tmpl.version = 0x20010000; // MinotaurX selector in bits 16-23.
    tmpl.coinbase_tx_version = 2;
    tmpl.coinbase_prefix = {0x03, 0x11, 0x22, 0x33};
    tmpl.coinbase_tx_input_sequence = 0xfffffffe;
    tmpl.coinbase_tx_value_remaining = 95 * 100000000ULL;
    tmpl.coinbase_tx_outputs_count = 2;
    tmpl.coinbase_tx_outputs = {0xde, 0xad};
    tmpl.coinbase_tx_locktime = 0;
    tmpl.merkle_path = {uint256::ONE, uint256::ZERO};

    const auto round{sv2::DecodePayload<sv2::NewTemplate>(sv2::EncodePayload(tmpl))};
    BOOST_CHECK_EQUAL(round.template_id, 1u);
    BOOST_CHECK(round.future_template);
    BOOST_CHECK_EQUAL(round.version, tmpl.version);
    BOOST_CHECK(round.coinbase_prefix == tmpl.coinbase_prefix);
    BOOST_CHECK_EQUAL(round.coinbase_tx_value_remaining, tmpl.coinbase_tx_value_remaining);
    BOOST_CHECK(round.coinbase_tx_outputs == tmpl.coinbase_tx_outputs);
    BOOST_CHECK(round.merkle_path == tmpl.merkle_path);

    sv2::SetNewPrevHash prev;
    prev.template_id = 1;
    prev.prev_hash = uint256::ONE;
    prev.ntime_start = 0x11223344;
    prev.nbits = 0x1d00ffff;
    prev.target = uint256::ONE;
    const auto prev_round{sv2::DecodePayload<sv2::SetNewPrevHash>(sv2::EncodePayload(prev))};
    BOOST_CHECK_EQUAL(prev_round.nbits, 0x1d00ffffu);
    BOOST_CHECK_EQUAL(prev_round.prev_hash, uint256::ONE);
    // U256 is 32 raw little-endian bytes, which is already uint256's layout.
    BOOST_CHECK_EQUAL(sv2::EncodePayload(prev).size(), 8u + 32u + 4u + 4u + 32u);

    sv2::RequestTransactionDataSuccess txdata;
    txdata.template_id = 1;
    txdata.excess_data = {0x01};
    txdata.transaction_list = {{0xaa, 0xbb}, {}};
    const auto tx_round{sv2::DecodePayload<sv2::RequestTransactionDataSuccess>(sv2::EncodePayload(txdata))};
    BOOST_CHECK_EQUAL(tx_round.transaction_list.size(), 2u);
    const std::vector<uint8_t> expected_tx{0xaa, 0xbb};
    BOOST_CHECK(tx_round.transaction_list[0] == expected_tx);
    BOOST_CHECK(tx_round.transaction_list[1].empty());

    sv2::SubmitSolution sol;
    sol.template_id = 1;
    sol.version = 0x20010000;
    sol.ntime = 0x11223344;
    sol.nonce = 0x55667788;
    sol.coinbase_tx = {0x01, 0x02};
    BOOST_CHECK_EQUAL(WireHex(sol),
                      "0100000000000000" // template_id
                      "00000120"         // version
                      "44332211"         // ntime
                      "88776655"         // nonce
                      "0200" "0102");    // coinbase_tx
}

BOOST_AUTO_TEST_CASE(avian_extension_messages)
{
    const sv2::AvianSelectAlgorithm select{1};
    BOOST_CHECK_EQUAL(WireHex(select), "01");

    // The mask never opens nVersion bits 16-23, and is 0 in this version.
    const sv2::AvianSelectAlgorithmSuccess ok{1, 0};
    BOOST_CHECK_EQUAL(WireHex(ok), "01" "00000000");

    sv2::AvianSelectAlgorithmError err;
    err.error_code = "pow-type-not-enabled";
    BOOST_CHECK_EQUAL(sv2::DecodePayload<sv2::AvianSelectAlgorithmError>(sv2::EncodePayload(err)).error_code,
                      "pow-type-not-enabled");

    const sv2::AvianTimeConstraints times{1, 0x11223344, 0x11223355};
    BOOST_CHECK_EQUAL(WireHex(times), "0100000000000000" "44332211" "55332211");
}

BOOST_AUTO_TEST_CASE(trailing_bytes_rejected)
{
    // A payload longer than the message it claims to be means the peers
    // disagree about the message, so it is an error rather than something to
    // ignore.
    auto payload{sv2::EncodePayload(sv2::RequestTransactionData{1})};
    payload.push_back(0x00);
    BOOST_CHECK_THROW(sv2::DecodePayload<sv2::RequestTransactionData>(payload), std::ios_base::failure);

    // And a truncated payload is equally an error.
    auto truncated{sv2::EncodePayload(sv2::RequestTransactionData{1})};
    truncated.pop_back();
    BOOST_CHECK_THROW(sv2::DecodePayload<sv2::RequestTransactionData>(truncated), std::ios_base::failure);
}

BOOST_AUTO_TEST_SUITE_END()
