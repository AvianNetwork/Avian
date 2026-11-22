// Copyright (c) 2026 The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <sv2/messages.h>
#include <sv2/types.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <test/fuzz/util.h>
#include <uint256.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <ios>
#include <span>
#include <string>
#include <vector>

//! Codec fuzzing for the Sv2 message set.
//!
//! Two targets, because the two directions reach different code. Feeding bytes
//! to the decoder finds its own faults, but random bytes practically never
//! produce a long length-prefixed field: a 16 MB blob needs three specific
//! prefix bytes followed by 16 MB of anything. Building a message and
//! serializing it reaches those fields at their boundaries, but can never
//! produce a malformed encoding. Neither direction alone covers the codec.

namespace {

//! Decode, then re-encode what was decoded.
//!
//! sv2::DecodePayload insists every byte be consumed, so a message that decodes
//! must re-serialize to the same length, and for all but one message to the
//! same bytes.
//!
//! NewTemplate is that one. BOOL carries seven padding high bits that the spec
//! tells a receiver to ignore, so 0x02 decodes to false and comes back as 0x00.
//! `byte_stable` records which case this is rather than weakening the check for
//! every other message.
//!
//! Returns whether the input decoded at all, which the structured target below
//! needs and this one does not.
template <typename M>
bool DecodeAndReencode(std::span<const uint8_t> bytes, bool byte_stable)
{
    M msg;
    try {
        msg = sv2::DecodePayload<M>(bytes);
    } catch (const std::ios_base::failure&) {
        return false;
    }

    // Not inside a try: a decode enforces every field's cap, so re-serializing
    // what it produced cannot trip the serializer's own length guard. If it
    // ever does, the two disagree about a cap, and the exception escaping this
    // harness is the point.
    const std::vector<uint8_t> again{sv2::EncodePayload(msg)};
    assert(again.size() == bytes.size());
    if (byte_stable) assert(std::equal(again.begin(), again.end(), bytes.begin()));

    // Whatever normalization the decoder applied, it applied once: what came
    // back out must parse, and parse to the same thing.
    bool reparsed_ok{true};
    M reparsed;
    try {
        reparsed = sv2::DecodePayload<M>(again);
    } catch (const std::ios_base::failure&) {
        reparsed_ok = false;
    }
    assert(reparsed_ok);
    assert(sv2::EncodePayload(reparsed) == again);
    return true;
}

//! The frame header's own invariants, which the generic round trip cannot see.
void HeaderChecks(std::span<const uint8_t> bytes)
{
    sv2::NetHeader header;
    try {
        header = sv2::DecodePayload<sv2::NetHeader>(bytes);
    } catch (const std::ios_base::failure&) {
        return;
    }
    // msg_length is a U24 and nothing may widen it, since that length is what
    // bounds every allocation downstream.
    assert(header.msg_length <= sv2::MAX_PAYLOAD);
    // The channel_msg bit is a flag, not part of the identifier an extension is
    // looked up by (sv2-spec 3.4).
    assert((header.Extension() & sv2::CHANNEL_MSG_BIT) == 0);
    assert(header.IsChannelMsg() == ((header.extension_type & sv2::CHANNEL_MSG_BIT) != 0));
    if (!header.IsChannelMsg()) assert(header.Extension() == header.extension_type);
    assert(DecodeAndReencode<sv2::NetHeader>(bytes, /*byte_stable=*/true));
}

//! A length the fuzzer may deliberately push past `cap`.
//!
//! The serializer refusing a value too long for its prefix is what stops a
//! field being silently truncated on the wire, so that guard is tested rather
//! than avoided. Lengths cluster on the cap because that is where it lives.
//! One in four rather than half the time: a 65535-element field costs far more
//! to build than it buys once it has been built a few thousand times, and the
//! boundary is reached often enough either way.
size_t ConsumeLength(FuzzedDataProvider& provider, size_t cap, size_t overrun)
{
    if (provider.ConsumeIntegralInRange<int>(0, 3) == 0) {
        const size_t low{cap > 2 ? cap - 2 : 0};
        return provider.ConsumeIntegralInRange<size_t>(low, cap + overrun);
    }
    return provider.ConsumeIntegralInRange<size_t>(0, 8);
}

//! Content is irrelevant to framing, so it is generated rather than consumed:
//! the fuzzer's bytes buy more when spent on lengths and message choice.
std::vector<uint8_t> Filler(size_t len)
{
    std::vector<uint8_t> v(len);
    for (size_t i = 0; i < len; ++i) v[i] = static_cast<uint8_t>((i * 31 + 7) & 0xff);
    return v;
}

std::string FillerStr(size_t len)
{
    std::string s(len, 'a');
    for (size_t i = 0; i < len; ++i) s[i] = static_cast<char>('a' + (i % 26));
    return s;
}

//! Serialize a message the fuzzer built, and require the serializer to refuse
//! it exactly when one of its fields does not fit its length prefix.
template <typename M>
void EncodeThenDecode(const M& msg, bool expect_refusal)
{
    std::vector<uint8_t> bytes;
    try {
        bytes = sv2::EncodePayload(msg);
    } catch (const std::ios_base::failure&) {
        assert(expect_refusal);
        return;
    }
    assert(!expect_refusal);
    // byte_stable even for NewTemplate: a bool serializes as 0x00 or 0x01, so
    // the padding bits that make a decoded BOOL unstable cannot occur here.
    assert(DecodeAndReencode<M>(bytes, /*byte_stable=*/true));
}

} // namespace

FUZZ_TARGET(sv2_messages)
{
    FuzzedDataProvider provider{buffer.data(), buffer.size()};
    const int which{provider.ConsumeIntegralInRange<int>(0, 17)};
    const std::vector<uint8_t> payload{provider.ConsumeRemainingBytes<uint8_t>()};

    switch (which) {
    case 0: HeaderChecks(payload); break;
    case 1: DecodeAndReencode<sv2::SetupConnection>(payload, true); break;
    case 2: DecodeAndReencode<sv2::SetupConnectionSuccess>(payload, true); break;
    case 3: DecodeAndReencode<sv2::SetupConnectionError>(payload, true); break;
    case 4: DecodeAndReencode<sv2::RequestExtensions>(payload, true); break;
    case 5: DecodeAndReencode<sv2::RequestExtensionsSuccess>(payload, true); break;
    case 6: DecodeAndReencode<sv2::RequestExtensionsError>(payload, true); break;
    case 7: DecodeAndReencode<sv2::CoinbaseOutputConstraints>(payload, true); break;
    // The BOOL exception; see DecodeAndReencode.
    case 8: DecodeAndReencode<sv2::NewTemplate>(payload, false); break;
    case 9: DecodeAndReencode<sv2::SetNewPrevHash>(payload, true); break;
    case 10: DecodeAndReencode<sv2::RequestTransactionData>(payload, true); break;
    case 11: DecodeAndReencode<sv2::RequestTransactionDataSuccess>(payload, true); break;
    case 12: DecodeAndReencode<sv2::RequestTransactionDataError>(payload, true); break;
    case 13: DecodeAndReencode<sv2::SubmitSolution>(payload, true); break;
    case 14: DecodeAndReencode<sv2::AvianSelectAlgorithm>(payload, true); break;
    case 15: DecodeAndReencode<sv2::AvianSelectAlgorithmSuccess>(payload, true); break;
    case 16: DecodeAndReencode<sv2::AvianSelectAlgorithmError>(payload, true); break;
    case 17: DecodeAndReencode<sv2::AvianTimeConstraints>(payload, true); break;
    }
}

FUZZ_TARGET(sv2_messages_roundtrip)
{
    FuzzedDataProvider provider{buffer.data(), buffer.size()};

    switch (provider.ConsumeIntegralInRange<int>(0, 4)) {
    case 0: {
        // Five STR0_255 fields in one message, which is where an off-by-one in
        // the one-byte prefix would show.
        sv2::SetupConnection msg;
        msg.protocol = provider.ConsumeIntegral<uint8_t>();
        msg.min_version = provider.ConsumeIntegral<uint16_t>();
        msg.max_version = provider.ConsumeIntegral<uint16_t>();
        msg.flags = provider.ConsumeIntegral<uint32_t>();
        msg.endpoint_port = provider.ConsumeIntegral<uint16_t>();
        const size_t lens[5]{
            ConsumeLength(provider, 0xff, 2), ConsumeLength(provider, 0xff, 2),
            ConsumeLength(provider, 0xff, 2), ConsumeLength(provider, 0xff, 2),
            ConsumeLength(provider, 0xff, 2)};
        msg.endpoint_host = FillerStr(lens[0]);
        msg.vendor = FillerStr(lens[1]);
        msg.hardware_version = FillerStr(lens[2]);
        msg.firmware = FillerStr(lens[3]);
        msg.device_id = FillerStr(lens[4]);
        const bool refuse{std::any_of(std::begin(lens), std::end(lens),
                                      [](size_t n) { return n > 0xff; })};
        EncodeThenDecode(msg, refuse);
        break;
    }
    case 1: {
        // Every formatter the protocol has, in one message: BOOL, B0_255,
        // B0_64K, and a SEQ0_255 of 32-byte elements.
        sv2::NewTemplate msg;
        msg.template_id = provider.ConsumeIntegral<uint64_t>();
        msg.future_template = provider.ConsumeBool();
        msg.version = provider.ConsumeIntegral<uint32_t>();
        msg.coinbase_tx_version = provider.ConsumeIntegral<uint32_t>();
        msg.coinbase_tx_input_sequence = provider.ConsumeIntegral<uint32_t>();
        msg.coinbase_tx_value_remaining = provider.ConsumeIntegral<uint64_t>();
        msg.coinbase_tx_outputs_count = provider.ConsumeIntegral<uint32_t>();
        msg.coinbase_tx_locktime = provider.ConsumeIntegral<uint32_t>();
        const size_t prefix_len{ConsumeLength(provider, 0xff, 2)};
        const size_t outputs_len{ConsumeLength(provider, 0xffff, 2)};
        const size_t path_len{ConsumeLength(provider, 0xff, 2)};
        msg.coinbase_prefix = Filler(prefix_len);
        msg.coinbase_tx_outputs = Filler(outputs_len);
        msg.merkle_path.assign(path_len, uint256{});
        for (size_t i = 0; i < path_len; ++i) {
            msg.merkle_path[i] = uint256{static_cast<uint8_t>(i & 0xff)};
        }
        EncodeThenDecode(msg, prefix_len > 0xff || outputs_len > 0xffff || path_len > 0xff);
        break;
    }
    case 2: {
        // A SEQ0_64K of two-byte elements, so the count prefix is reached at its
        // own maximum rather than a blob prefix's.
        sv2::RequestExtensions msg;
        msg.request_id = provider.ConsumeIntegral<uint16_t>();
        const size_t count{ConsumeLength(provider, 0xffff, 2)};
        msg.requested_extensions.assign(count, 0);
        for (size_t i = 0; i < count; ++i) {
            msg.requested_extensions[i] = static_cast<uint16_t>(i);
        }
        EncodeThenDecode(msg, count > 0xffff);
        break;
    }
    case 3: {
        // SEQ0_64K<B0_16M>: a nested length prefix, and the only place a whole
        // block travels. The elements stay small because the outer count is what
        // is interesting here, and 65535 multi-megabyte elements would spend the
        // run in memset.
        sv2::RequestTransactionDataSuccess msg;
        msg.template_id = provider.ConsumeIntegral<uint64_t>();
        const size_t excess_len{ConsumeLength(provider, 0xffff, 2)};
        msg.excess_data = Filler(excess_len);
        const size_t count{ConsumeLength(provider, 0xffff, 2)};
        msg.transaction_list.assign(count, std::vector<uint8_t>{});
        // Only the first few carry bytes. Filling all 65535 would be 65535
        // allocations under the sanitizers, which buys nothing the first few
        // have not already covered: the nesting is what is under test, not the
        // content.
        for (size_t i = 0; i < std::min<size_t>(count, 8); ++i) {
            msg.transaction_list[i] = Filler(i % 5);
        }
        EncodeThenDecode(msg, excess_len > 0xffff || count > 0xffff);
        break;
    }
    case 4: {
        // The one client-to-server message carrying a blob, and the one whose
        // length the solution checker then bounds again.
        sv2::SubmitSolution msg;
        msg.template_id = provider.ConsumeIntegral<uint64_t>();
        msg.version = provider.ConsumeIntegral<uint32_t>();
        msg.ntime = provider.ConsumeIntegral<uint32_t>();
        msg.nonce = provider.ConsumeIntegral<uint32_t>();
        const size_t len{ConsumeLength(provider, 0xffff, 2)};
        msg.coinbase_tx = Filler(len);
        EncodeThenDecode(msg, len > 0xffff);
        break;
    }
    }
}
