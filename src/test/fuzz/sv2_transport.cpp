// Copyright (c) 2026 The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <key.h>
#include <pubkey.h>
#include <sv2/messages.h>
#include <sv2/noise.h>
#include <sv2/transport.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <test/fuzz/util.h>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <span>
#include <vector>

//! Framing fuzzing for the Sv2 transport.
//!
//! The transport knows nothing about sockets, which is what lets a fuzzer drive
//! it: the two things a real network varies, how a frame is split across reads
//! and how much of a write a socket accepts, are both parameters here. Those
//! are the cases a loopback test never produces reliably, and they are where
//! the state machine keeps its difficulty, because an AEAD stream cannot
//! re-read a header it has already decrypted.
//!
//! Two modes, chosen per run. Honest framing has an exact oracle: every frame
//! sent must arrive once, in order, byte for byte. Garbage has an absolute one:
//! a fuzzer cannot forge a MAC, so nothing it writes may ever decode.

namespace {

struct ServerIdentity {
    CKey authority;
    CKey static_key;
    XOnlyPubKey authority_pub;
    sv2::SignatureNoiseMessage cert;

    //! One key pair and one certificate for the whole campaign. Generating them
    //! per iteration would spend the fuzzer's time in libsecp256k1 rather than
    //! in the state machine under test.
    ServerIdentity()
    {
        authority.MakeNewKey(/*fCompressedIn=*/true);
        static_key.MakeNewKey(/*fCompressedIn=*/true);
        authority_pub = XOnlyPubKey{authority.GetPubKey()};
        cert.version = 0;
        cert.valid_from = 0;
        cert.not_valid_after = std::numeric_limits<uint32_t>::max();
        const bool signed_ok{cert.Sign(authority, XOnlyPubKey{static_key.GetPubKey()})};
        assert(signed_ok);
    }
};

//! Created once and deliberately never destroyed.
//!
//! A CKey allocates from the locked memory pool, and that pool is a static
//! singleton built on the first such allocation, so it finishes construction
//! after a namespace-scope global does and is therefore destroyed before one.
//! Cleansing key material at that point writes to pages the pool has already
//! unmapped. Measured, as an AddressSanitizer SEGV inside memory_cleanse under
//! __run_exit_handlers. Leaking it avoids the ordering question rather than
//! reasoning about it, and LSan stays quiet because a static pointer is a root.
ServerIdentity& Server()
{
    static ServerIdentity* const server{new ServerIdentity()};
    return *server;
}

//! The clock the certificate is checked against. Fixed, because the handshake
//! here exists to produce a pair of ciphers rather than to test validity
//! windows, which the unit tests cover.
constexpr int64_t NOW{1000};

//! Take up to `count` queued bytes out of a transport and into a buffer this
//! harness owns, so delivery can be fragmented independently of production.
void Drain(sv2::Transport& from, std::vector<std::byte>& wire, size_t count)
{
    const auto out{from.SendBuffer()};
    const size_t take{std::min(count, out.size())};
    if (take == 0) return;
    wire.insert(wire.end(), out.begin(), out.begin() + take);
    from.Sent(take);
}

[[nodiscard]] bool Handshake(sv2::Transport& initiator, sv2::Transport& responder)
{
    std::vector<std::byte> wire;
    Drain(initiator, wire, std::numeric_limits<size_t>::max());
    responder.Received(wire);
    if (!responder.Advance(NOW)) return false;
    wire.clear();
    Drain(responder, wire, std::numeric_limits<size_t>::max());
    initiator.Received(wire);
    return initiator.Advance(NOW);
}

//! A handshaked pair, performed once and copied per run.
//!
//! The handshake takes no input from the fuzzer, so repeating it every run buys
//! nothing and costs almost everything: measured at 62 executions a second with
//! the input truncated to 8 bytes, against 65 with full inputs, which says the
//! framing loop under test was about 5% of the work. Copying a pair whose
//! ciphers are still at nonce zero gives every run the same starting state
//! without four EllSwift operations and a Schnorr verification to get there.
//! The handshake itself wants a target of its own rather than a share of this
//! one.
struct Connected {
    sv2::Transport initiator;
    sv2::Transport responder;
};

const Connected& Pair()
{
    // Leaked for the same reason Server() is, and it holds a copy of that key.
    static const Connected* const pair{[] {
        auto* c{new Connected{sv2::Transport::Initiator(Server().authority_pub),
                              sv2::Transport::Responder(Server().static_key, Server().cert)}};
        const bool handshaked{Handshake(c->initiator, c->responder)};
        assert(handshaked);
        assert(c->initiator.HandshakeComplete());
        assert(c->responder.HandshakeComplete());
        assert(c->initiator.RemoteStaticKey().has_value());
        assert(c->initiator.SendBufferEmpty() && c->responder.SendBufferEmpty());
        assert(c->initiator.RecvBufferSize() == 0 && c->responder.RecvBufferSize() == 0);
        return c;
    }()};
    return *pair;
}

void Initialize()
{
    static ECC_Context ecc_context{};
    // Built here rather than on first use, so no single run carries the cost.
    Pair();
}

std::vector<uint8_t> Filler(size_t len)
{
    std::vector<uint8_t> v(len);
    for (size_t i = 0; i < len; ++i) v[i] = static_cast<uint8_t>((i * 31 + 7) & 0xff);
    return v;
}

//! Payload lengths worth reaching. The chunk boundary is where a payload stops
//! being a single AEAD operation, and MAX_INBOUND_PAYLOAD is where the receiver
//! starts refusing. Mostly small, because a run that spends itself on 256 KB
//! frames explores less of the state machine than one that sends twenty.
size_t ConsumePayloadLen(FuzzedDataProvider& provider)
{
    // Half the draws land in the default arm. Measured: with the large cases a
    // fifth of all draws the target managed 62 executions a second, which is
    // too slow to grow a corpus however interesting the frames are.
    switch (provider.ConsumeIntegralInRange<int>(0, 15)) {
    case 0: return 0;
    case 1: return 1;
    case 2: return sv2::MAX_PLAINTEXT_CHUNK - 1;
    case 3: return sv2::MAX_PLAINTEXT_CHUNK;
    case 4: return sv2::MAX_PLAINTEXT_CHUNK + 1;
    case 5: return 2 * sv2::MAX_PLAINTEXT_CHUNK;
    case 6: return sv2::MAX_INBOUND_PAYLOAD;
    // One over what the receiver accepts. The sender is content to queue it,
    // since Send only bounds against what msg_length can describe, so this is
    // the one honest frame whose refusal is correct.
    case 7: return sv2::MAX_INBOUND_PAYLOAD + 1;
    default: return provider.ConsumeIntegralInRange<size_t>(0, 512);
    }
}

//! What the receiver may still hold after Advance has taken everything it can:
//! the tail of one incomplete frame, nothing more. The socket loop's
//! -sv2maxrecvbuffer bound rests on this.
constexpr size_t MAX_LEFTOVER{sv2::ENCRYPTED_HEADER_SIZE + sv2::CiphertextLen(sv2::MAX_INBOUND_PAYLOAD)};

//! Advance the receiver and match what came out against what went in. Returns
//! false once the receiver has reported a fatal error.
[[nodiscard]] bool Pull(sv2::Transport& receiver, std::deque<sv2::ReceivedMsg>& expected, bool oversize)
{
    if (!receiver.Advance(NOW)) {
        // Honest framing has exactly one legitimate refusal: a payload larger
        // than the receiver accepts. Anything else is a bug here.
        assert(oversize);
        return false;
    }
    while (const auto got = receiver.Next()) {
        assert(!expected.empty());
        const sv2::ReceivedMsg& want{expected.front()};
        assert(got->header.extension_type == want.header.extension_type);
        assert(got->header.msg_type == want.header.msg_type);
        // Send derives msg_length from the payload it was given, so this holds
        // even though the header handed to it said zero.
        assert(got->header.msg_length == want.payload.size());
        assert(got->payload == want.payload);
        expected.pop_front();
    }
    assert(receiver.RecvBufferSize() <= MAX_LEFTOVER);
    return true;
}

} // namespace

FUZZ_TARGET(sv2_transport, .init = Initialize)
{
    FuzzedDataProvider provider{buffer.data(), buffer.size()};

    // Copies of a pair that finished its handshake once, at startup. Nothing in
    // a run touches the global random state, which is what keeps a crash here
    // reproducible from its own corpus entry.
    sv2::Transport initiator{Pair().initiator};
    sv2::Transport responder{Pair().responder};

    if (provider.ConsumeBool()) {
        // Garbage into a completed stream. The oracle is absolute: nothing the
        // fuzzer writes may decode into a message, because that would mean a
        // forged MAC rather than a lucky input. Advance must not crash, and
        // once it has reported failure it must keep reporting failure instead
        // of trying to resynchronize, which an authenticated stream cannot
        // safely do.
        sv2::Transport& victim{provider.ConsumeBool() ? initiator : responder};
        bool failed{false};
        while (provider.remaining_bytes() > 0) {
            const size_t n{provider.ConsumeIntegralInRange<size_t>(1, 4096)};
            const auto chunk{provider.ConsumeBytes<std::byte>(n)};
            if (chunk.empty()) break;
            victim.Received(chunk);
            const bool ok{victim.Advance(NOW)};
            if (failed) assert(!ok);
            if (!ok) failed = true;
            assert(!victim.Next().has_value());
        }
        return;
    }

    // Honest framing, with the fuzzer choosing the sizes, the write sizes and
    // the read sizes. Only one direction per run: the two ciphers are
    // independent, so driving both would halve the depth without covering
    // anything new.
    sv2::Transport& sender{provider.ConsumeBool() ? initiator : responder};
    sv2::Transport& receiver{&sender == &initiator ? responder : initiator};

    std::deque<sv2::ReceivedMsg> expected;
    std::vector<std::byte> wire;
    //! Total plaintext a run may queue, so a corpus entry cannot turn into a
    //! multi-megabyte memcpy exercise. At 256 KB a run affords one frame at the
    //! receiver's limit, which is the point of having that case at all.
    size_t budget{1 << 18};
    bool oversize{false};
    bool failed{false};

    while (provider.remaining_bytes() > 0 && !failed) {
        switch (provider.ConsumeIntegralInRange<int>(0, 2)) {
        case 0: {
            const size_t len{ConsumePayloadLen(provider)};
            if (len > budget) break;
            budget -= len;
            // msg_length is deliberately left at zero: Send is supposed to
            // overwrite it with the real payload length, and Pull checks that
            // the receiver saw the length rather than the lie.
            sv2::NetHeader header{provider.ConsumeIntegral<uint16_t>(),
                                  provider.ConsumeIntegral<uint8_t>(), /*len=*/0};
            std::vector<uint8_t> payload{Filler(len)};
            const bool queued{sender.Send(header, payload)};
            assert(queued);
            header.msg_length = static_cast<uint32_t>(len);
            if (len > sv2::MAX_INBOUND_PAYLOAD) oversize = true;
            expected.push_back(sv2::ReceivedMsg{header, std::move(payload)});
            break;
        }
        case 1: {
            const size_t have{sender.SendBuffer().size()};
            if (have == 0) break;
            // A partial write is the normal case on a busy socket, and it is
            // what exercises the send buffer's compaction.
            Drain(sender, wire, provider.ConsumeIntegralInRange<size_t>(1, have));
            assert(sender.SendBufferEmpty() == (sender.SendBuffer().size() == 0));
            break;
        }
        case 2: {
            if (wire.empty()) break;
            // The fuzzer is free to deliver one byte at a time, which is
            // exactly the case worth testing, but doing it to a 256 KB frame
            // would spend the whole run inside Advance. Hence a floor once a
            // lot is pending: small frames still arrive byte by byte.
            const size_t floor{wire.size() > 4096 ? size_t{1024} : size_t{1}};
            const size_t n{provider.ConsumeIntegralInRange<size_t>(floor, wire.size())};
            receiver.Received(std::span{wire}.first(n));
            wire.erase(wire.begin(), wire.begin() + n);
            failed = !Pull(receiver, expected, oversize);
            break;
        }
        }
    }

    if (failed) return;

    // Flush. Everything still queued has to arrive, which is what turns the
    // per-step checks into a statement about the whole run: no frame was
    // dropped, duplicated or reordered.
    Drain(sender, wire, std::numeric_limits<size_t>::max());
    assert(sender.SendBufferEmpty());
    receiver.Received(wire);
    if (Pull(receiver, expected, oversize)) {
        assert(expected.empty());
        assert(receiver.RecvBufferSize() == 0);
    }
}
