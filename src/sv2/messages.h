// Copyright (c) 2026 The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_SV2_MESSAGES_H
#define BITCOIN_SV2_MESSAGES_H

#include <consensus/consensus.h>
#include <streams.h>
#include <sv2/types.h>
#include <uint256.h>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

//! Message framing and the message set Avian's Template Provider speaks: the
//! Common protocol, Extensions Negotiation (sv2-spec extension 0x0001), the
//! Template Distribution Protocol, and Avian's own multi-PoW extension.
//!
//! Mining Protocol and Job Declaration Protocol messages are deliberately
//! absent. A Template Provider does not speak them, and a channel-scoped
//! message arriving on a TDP connection is a protocol violation.
namespace sv2 {

//! Frame header size in bytes (sv2-spec 3.2).
static constexpr size_t HEADER_SIZE{6};

//! msg_length is a U24, so this is the largest payload any frame can carry.
static constexpr uint32_t MAX_PAYLOAD{0xffffff};

//! The most significant bit of extension_type. When set, the first four
//! payload bytes are a U32 channel_id.
static constexpr uint16_t CHANNEL_MSG_BIT{0x8000};

static constexpr uint16_t EXT_CORE{0x0000};        //!< Messages defined by the core spec.
static constexpr uint16_t EXT_NEGOTIATION{0x0001}; //!< Extensions Negotiation.

//! Avian's multi-PoW extension.
//!
//! 0x4001 lies in the spec's experimental range (0x4000-0x7fff). The spec
//! requires experimental identifiers not to be present in production
//! equipment, so this value MUST be replaced by a registry-allocated one
//! before any mainnet release. It is defined here once and never written
//! inline so that swap stays a one-line change. See doc/sv2.md.
static constexpr uint16_t AVIAN_EXTENSION_ID{0x4001};

//! A Template Provider can only ever serve whole blocks inside a B0_16M
//! transaction_list, so a block-size increase past the U24 ceiling must be a
//! compile error rather than a silently truncated field.
static_assert(MAX_BLOCK_SERIALIZED_SIZE < (1u << 24),
              "Sv2 RequestTransactionData.Success cannot carry a block this large");

//! SetupConnection.protocol (sv2-spec 3.6.1).
enum class Protocol : uint8_t {
    MINING = 0,
    JOB_DECLARATION = 1,
    TEMPLATE_DISTRIBUTION = 2,
};

//! The only protocol version defined; min_version and max_version must
//! bracket it.
static constexpr uint16_t PROTOCOL_VERSION{2};

//! msg_type is scoped by extension_type, so the numbering restarts in each
//! namespace: 0x00 is SetupConnection under EXT_CORE and RequestExtensions
//! under EXT_NEGOTIATION.
enum class CoreMsg : uint8_t {
    SETUP_CONNECTION = 0x00,
    SETUP_CONNECTION_SUCCESS = 0x01,
    SETUP_CONNECTION_ERROR = 0x02,
    CHANNEL_ENDPOINT_CHANGED = 0x03,
    RECONNECT = 0x04,
    COINBASE_OUTPUT_CONSTRAINTS = 0x70,
    NEW_TEMPLATE = 0x71,
    SET_NEW_PREV_HASH = 0x72,
    REQUEST_TRANSACTION_DATA = 0x73,
    REQUEST_TRANSACTION_DATA_SUCCESS = 0x74,
    REQUEST_TRANSACTION_DATA_ERROR = 0x75,
    SUBMIT_SOLUTION = 0x76,
};

enum class NegotiationMsg : uint8_t {
    REQUEST_EXTENSIONS = 0x00,
    REQUEST_EXTENSIONS_SUCCESS = 0x01,
    REQUEST_EXTENSIONS_ERROR = 0x02,
};

enum class AvianMsg : uint8_t {
    SELECT_ALGORITHM = 0x00,
    SELECT_ALGORITHM_SUCCESS = 0x01,
    SELECT_ALGORITHM_ERROR = 0x02,
    TIME_CONSTRAINTS = 0x03,
    //! The Mining Protocol form of TIME_CONSTRAINTS, keyed on channel and job
    //! instead of template. Specified in doc/sv2.md and reserved here so the
    //! number cannot be reused; a pool implements it, a Template Provider has
    //! no occasion to send it.
    JOB_TIME_CONSTRAINTS = 0x04,
};

//! Frame header: extension_type, msg_type, then a U24 payload length.
struct NetHeader {
    uint16_t extension_type{0};
    uint8_t msg_type{0};
    uint32_t msg_length{0};

    NetHeader() = default;
    NetHeader(uint16_t ext, uint8_t type, uint32_t len)
        : extension_type{ext}, msg_type{type}, msg_length{len} {}

    SERIALIZE_METHODS(NetHeader, obj)
    {
        READWRITE(obj.extension_type, obj.msg_type, Using<U24>(obj.msg_length));
    }

    //! True if the payload begins with a U32 channel_id.
    bool IsChannelMsg() const { return (extension_type & CHANNEL_MSG_BIT) != 0; }

    //! extension_type with the channel_msg bit masked off, which is the form
    //! used to look an extension up (sv2-spec 3.4).
    uint16_t Extension() const { return extension_type & ~CHANNEL_MSG_BIT; }
};

// Common protocol.

struct SetupConnection {
    uint8_t protocol{0};
    uint16_t min_version{0};
    uint16_t max_version{0};
    uint32_t flags{0};
    std::string endpoint_host;
    uint16_t endpoint_port{0};
    std::string vendor;
    std::string hardware_version;
    std::string firmware;
    std::string device_id;

    SERIALIZE_METHODS(SetupConnection, obj)
    {
        READWRITE(obj.protocol, obj.min_version, obj.max_version, obj.flags,
                  Using<Str0_255>(obj.endpoint_host), obj.endpoint_port,
                  Using<Str0_255>(obj.vendor), Using<Str0_255>(obj.hardware_version),
                  Using<Str0_255>(obj.firmware), Using<Str0_255>(obj.device_id));
    }
};

struct SetupConnectionSuccess {
    uint16_t used_version{0};
    uint32_t flags{0};

    SERIALIZE_METHODS(SetupConnectionSuccess, obj) { READWRITE(obj.used_version, obj.flags); }
};

struct SetupConnectionError {
    uint32_t flags{0};
    std::string error_code;

    SERIALIZE_METHODS(SetupConnectionError, obj) { READWRITE(obj.flags, Using<Str0_255>(obj.error_code)); }
};

// Extensions Negotiation (extension 0x0001).

struct RequestExtensions {
    uint16_t request_id{0};
    std::vector<uint16_t> requested_extensions;

    SERIALIZE_METHODS(RequestExtensions, obj)
    {
        READWRITE(obj.request_id, Using<Seq0_64K<>>(obj.requested_extensions));
    }
};

struct RequestExtensionsSuccess {
    uint16_t request_id{0};
    std::vector<uint16_t> supported_extensions;

    SERIALIZE_METHODS(RequestExtensionsSuccess, obj)
    {
        READWRITE(obj.request_id, Using<Seq0_64K<>>(obj.supported_extensions));
    }
};

struct RequestExtensionsError {
    uint16_t request_id{0};
    std::vector<uint16_t> unsupported_extensions;
    std::vector<uint16_t> required_extensions;

    SERIALIZE_METHODS(RequestExtensionsError, obj)
    {
        READWRITE(obj.request_id, Using<Seq0_64K<>>(obj.unsupported_extensions),
                  Using<Seq0_64K<>>(obj.required_extensions));
    }
};

// Template Distribution Protocol.

struct CoinbaseOutputConstraints {
    uint32_t coinbase_output_max_additional_size{0};
    uint16_t coinbase_output_max_additional_sigops{0};

    SERIALIZE_METHODS(CoinbaseOutputConstraints, obj)
    {
        READWRITE(obj.coinbase_output_max_additional_size, obj.coinbase_output_max_additional_sigops);
    }
};

struct NewTemplate {
    uint64_t template_id{0};
    bool future_template{false};
    uint32_t version{0};
    uint32_t coinbase_tx_version{0};
    //! Leading scriptSig bytes before the extranonce; begins with the BIP34
    //! height push.
    std::vector<uint8_t> coinbase_prefix;
    uint32_t coinbase_tx_input_sequence{0};
    uint64_t coinbase_tx_value_remaining{0};
    uint32_t coinbase_tx_outputs_count{0};
    //! Serialized outputs with no CompactSize prefix, appended as the last
    //! coinbase outputs. For Avian this carries the founder payment and the
    //! witness commitment.
    std::vector<uint8_t> coinbase_tx_outputs;
    uint32_t coinbase_tx_locktime{0};
    //! Ordered from the deepest.
    std::vector<uint256> merkle_path;

    SERIALIZE_METHODS(NewTemplate, obj)
    {
        READWRITE(obj.template_id, Using<Bool>(obj.future_template), obj.version,
                  obj.coinbase_tx_version, Using<B0_255>(obj.coinbase_prefix),
                  obj.coinbase_tx_input_sequence, obj.coinbase_tx_value_remaining,
                  obj.coinbase_tx_outputs_count, Using<B0_64K>(obj.coinbase_tx_outputs),
                  obj.coinbase_tx_locktime, Using<Seq0_255<>>(obj.merkle_path));
    }
};

struct SetNewPrevHash {
    uint64_t template_id{0};
    uint256 prev_hash;
    uint32_t ntime_start{0};
    uint32_t nbits{0};
    uint256 target;

    SERIALIZE_METHODS(SetNewPrevHash, obj)
    {
        READWRITE(obj.template_id, obj.prev_hash, obj.ntime_start, obj.nbits, obj.target);
    }
};

struct RequestTransactionData {
    uint64_t template_id{0};

    SERIALIZE_METHODS(RequestTransactionData, obj) { READWRITE(obj.template_id); }
};

struct RequestTransactionDataSuccess {
    uint64_t template_id{0};
    std::vector<uint8_t> excess_data;
    //! Full transactions excluding the coinbase, in block order.
    std::vector<std::vector<uint8_t>> transaction_list;

    SERIALIZE_METHODS(RequestTransactionDataSuccess, obj)
    {
        READWRITE(obj.template_id, Using<B0_64K>(obj.excess_data),
                  Using<Seq0_64K<B0_16M>>(obj.transaction_list));
    }
};

struct RequestTransactionDataError {
    uint64_t template_id{0};
    std::string error_code;

    SERIALIZE_METHODS(RequestTransactionDataError, obj)
    {
        READWRITE(obj.template_id, Using<Str0_255>(obj.error_code));
    }
};

struct SubmitSolution {
    uint64_t template_id{0};
    uint32_t version{0};
    uint32_t ntime{0};
    uint32_t nonce{0};
    //! Full serialized coinbase with SegWit marker, flag and witness intact.
    std::vector<uint8_t> coinbase_tx;

    SERIALIZE_METHODS(SubmitSolution, obj)
    {
        READWRITE(obj.template_id, obj.version, obj.ntime, obj.nonce, Using<B0_64K>(obj.coinbase_tx));
    }
};

// Avian multi-PoW extension (AVIAN_EXTENSION_ID).

//! Selects the proof-of-work algorithm for this connection. Sent once, after
//! RequestExtensions.Success and before CoinbaseOutputConstraints; the choice
//! is immutable afterwards because a connection carries one nBits and target
//! stream.
struct AvianSelectAlgorithm {
    uint8_t pow_type{0};

    SERIALIZE_METHODS(AvianSelectAlgorithm, obj) { READWRITE(obj.pow_type); }
};

//! version_rolling_mask states which nVersion bits the client may roll. Avian
//! keeps the algorithm in bits 16-23, so the mask never opens them and is 0 in
//! the first protocol version.
struct AvianSelectAlgorithmSuccess {
    uint8_t pow_type{0};
    uint32_t version_rolling_mask{0};

    SERIALIZE_METHODS(AvianSelectAlgorithmSuccess, obj) { READWRITE(obj.pow_type, obj.version_rolling_mask); }
};

struct AvianSelectAlgorithmError {
    std::string error_code;

    SERIALIZE_METHODS(AvianSelectAlgorithmError, obj) { READWRITE(Using<Str0_255>(obj.error_code)); }
};

//! Absolute ntime bounds for one template. The spec gives SubmitSolution a
//! lower bound only, but X16RT rederives its hash chain every 128 seconds, so
//! a client rolling ntime past that boundary would silently compute the wrong
//! function. Absolute bounds rather than a mask, because an in-place template
//! refresh does not re-send SetNewPrevHash and so cannot re-anchor one.
struct AvianTimeConstraints {
    uint64_t template_id{0};
    uint32_t ntime_min{0};
    uint32_t ntime_max{0};

    SERIALIZE_METHODS(AvianTimeConstraints, obj) { READWRITE(obj.template_id, obj.ntime_min, obj.ntime_max); }
};

//! Serialize a message body to its payload bytes, without a frame header.
template <typename M>
std::vector<uint8_t> EncodePayload(const M& msg)
{
    DataStream ss{};
    ss << msg;
    return std::vector<uint8_t>{UCharCast(ss.data()), UCharCast(ss.data()) + ss.size()};
}

//! Parse a message body from payload bytes, requiring that every byte be
//! consumed. Trailing bytes mean the sender and receiver disagree about the
//! message, which is a protocol error rather than something to ignore.
template <typename M>
M DecodePayload(std::span<const uint8_t> payload)
{
    SpanReader reader{std::as_bytes(payload)};
    M msg;
    reader >> msg;
    if (!reader.empty()) throw std::ios_base::failure("sv2: trailing bytes after message");
    return msg;
}

} // namespace sv2

#endif // BITCOIN_SV2_MESSAGES_H
