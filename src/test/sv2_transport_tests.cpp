// Copyright (c) 2026 The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <key.h>
#include <pubkey.h>
#include <sv2/transport.h>
#include <test/util/setup_common.h>

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include <boost/test/unit_test.hpp>

//! Framing tests for the Sv2 transport.
//!
//! The transport deliberately knows nothing about sockets, which is what lets
//! these run without a network. The cases that matter most are the ones a
//! loopback test would never produce: a frame arriving one byte at a time, a
//! payload spanning several MAC-tagged chunks, and several frames coalesced
//! into a single read.
BOOST_FIXTURE_TEST_SUITE(sv2_transport_tests, BasicTestingSetup)

namespace {
struct Peers {
    CKey authority;
    CKey static_key;
    sv2::SignatureNoiseMessage cert;
};

Peers MakePeers()
{
    Peers peers;
    peers.authority.MakeNewKey(/*fCompressedIn=*/true);
    peers.static_key.MakeNewKey(/*fCompressedIn=*/true);
    peers.cert.version = 0;
    peers.cert.valid_from = 1000;
    peers.cert.not_valid_after = 2000;
    BOOST_REQUIRE(peers.cert.Sign(peers.authority, XOnlyPubKey{peers.static_key.GetPubKey()}));
    return peers;
}

//! Move everything one side has queued into the other in a single read.
bool Pump(sv2::Transport& from, sv2::Transport& to, int64_t now = 1500)
{
    const auto out{from.SendBuffer()};
    const std::vector<std::byte> copy{out.begin(), out.end()};
    from.Sent(copy.size());
    to.Received(copy);
    return to.Advance(now);
}

//! Same, but one byte per Received(), as a congested socket would deliver it.
bool PumpByteWise(sv2::Transport& from, sv2::Transport& to, int64_t now = 1500)
{
    const auto out{from.SendBuffer()};
    const std::vector<std::byte> copy{out.begin(), out.end()};
    from.Sent(copy.size());
    for (size_t i = 0; i < copy.size(); ++i) {
        to.Received(std::span{copy}.subspan(i, 1));
        if (!to.Advance(now)) return false;
    }
    return true;
}

std::vector<uint8_t> Pattern(size_t n)
{
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = static_cast<uint8_t>((i * 31 + 7) & 0xff);
    return v;
}
} // namespace

BOOST_AUTO_TEST_CASE(ciphertext_length_boundaries)
{
    // The spec converts a plaintext length to a ciphertext length by adding
    // one MAC per chunk, so the expansion is not constant.
    BOOST_CHECK_EQUAL(sv2::CiphertextLen(0), 0u);
    BOOST_CHECK_EQUAL(sv2::CiphertextLen(1), 1u + sv2::MAC_SIZE);
    BOOST_CHECK_EQUAL(sv2::CiphertextLen(sv2::MAX_PLAINTEXT_CHUNK), sv2::MAX_CIPHERTEXT_CHUNK);
    // One byte past a chunk boundary costs a whole second MAC.
    BOOST_CHECK_EQUAL(sv2::CiphertextLen(sv2::MAX_PLAINTEXT_CHUNK + 1),
                      sv2::MAX_PLAINTEXT_CHUNK + 1 + 2 * sv2::MAC_SIZE);
}

BOOST_AUTO_TEST_CASE(handshake_then_frames_both_ways)
{
    const auto peers{MakePeers()};
    auto initiator{sv2::Transport::Initiator(XOnlyPubKey{peers.authority.GetPubKey()})};
    auto responder{sv2::Transport::Responder(peers.static_key, peers.cert)};

    BOOST_CHECK(!initiator.HandshakeComplete());
    // Nothing may be sent before the transport keys exist.
    BOOST_CHECK(!initiator.Send(sv2::NetHeader{sv2::EXT_CORE, 0, 0}, {}));

    BOOST_REQUIRE(Pump(initiator, responder));
    BOOST_CHECK(responder.HandshakeComplete());
    BOOST_REQUIRE(Pump(responder, initiator));
    BOOST_CHECK(initiator.HandshakeComplete());

    const auto payload{Pattern(100)};
    BOOST_REQUIRE(initiator.Send(sv2::NetHeader{sv2::EXT_CORE, 0x70, 0}, payload));
    BOOST_REQUIRE(Pump(initiator, responder));

    auto msg{responder.Next()};
    BOOST_REQUIRE(msg.has_value());
    BOOST_CHECK_EQUAL(msg->header.extension_type, sv2::EXT_CORE);
    BOOST_CHECK_EQUAL(msg->header.msg_type, 0x70);
    // msg_length is set from the payload, so a caller cannot misdeclare it.
    BOOST_CHECK_EQUAL(msg->header.msg_length, payload.size());
    BOOST_CHECK(msg->payload == payload);
    BOOST_CHECK(!responder.Next().has_value());

    const auto reply{Pattern(42)};
    BOOST_REQUIRE(responder.Send(sv2::NetHeader{sv2::EXT_CORE, 0x71, 0}, reply));
    BOOST_REQUIRE(Pump(responder, initiator));
    auto back{initiator.Next()};
    BOOST_REQUIRE(back.has_value());
    BOOST_CHECK(back->payload == reply);
}

BOOST_AUTO_TEST_CASE(coalesced_frames_arrive_in_order)
{
    const auto peers{MakePeers()};
    auto initiator{sv2::Transport::Initiator(XOnlyPubKey{peers.authority.GetPubKey()})};
    auto responder{sv2::Transport::Responder(peers.static_key, peers.cert)};
    BOOST_REQUIRE(Pump(initiator, responder));
    BOOST_REQUIRE(Pump(responder, initiator));

    const auto a{Pattern(10)};
    const auto b{Pattern(20)};
    BOOST_REQUIRE(initiator.Send(sv2::NetHeader{sv2::EXT_CORE, 0x72, 0}, a));
    BOOST_REQUIRE(initiator.Send(sv2::NetHeader{sv2::EXT_CORE, 0x73, 0}, b));
    BOOST_REQUIRE(Pump(initiator, responder));

    auto first{responder.Next()};
    auto second{responder.Next()};
    BOOST_REQUIRE(first.has_value());
    BOOST_REQUIRE(second.has_value());
    // Order matters: each frame consumes a nonce, so they cannot be reordered.
    BOOST_CHECK_EQUAL(first->header.msg_type, 0x72);
    BOOST_CHECK(first->payload == a);
    BOOST_CHECK_EQUAL(second->header.msg_type, 0x73);
    BOOST_CHECK(second->payload == b);
}

BOOST_AUTO_TEST_CASE(multi_chunk_payloads)
{
    const auto peers{MakePeers()};
    auto initiator{sv2::Transport::Initiator(XOnlyPubKey{peers.authority.GetPubKey()})};
    auto responder{sv2::Transport::Responder(peers.static_key, peers.cert)};
    BOOST_REQUIRE(Pump(initiator, responder));
    BOOST_REQUIRE(Pump(responder, initiator));

    // Exactly one chunk, one byte over, and well into a third.
    for (const size_t len : {sv2::MAX_PLAINTEXT_CHUNK,
                             sv2::MAX_PLAINTEXT_CHUNK + 1,
                             2 * sv2::MAX_PLAINTEXT_CHUNK + 100}) {
        const auto payload{Pattern(len)};
        BOOST_REQUIRE(initiator.Send(sv2::NetHeader{sv2::EXT_CORE, 0x74, 0}, payload));
        BOOST_REQUIRE(Pump(initiator, responder));
        auto msg{responder.Next()};
        BOOST_REQUIRE(msg.has_value());
        BOOST_CHECK_EQUAL(msg->payload.size(), len);
        BOOST_CHECK(msg->payload == payload);
    }
}

BOOST_AUTO_TEST_CASE(an_oversized_payload_is_refused_before_it_is_buffered)
{
    // msg_length is a U24, so the format allows 16 MiB. Nothing a client
    // sends is near that: the largest is SubmitSolution, whose coinbase is a
    // B0_64K. Honouring the format's ceiling meant a client could make the
    // node buffer 16 MiB per connection before the message was dispatched and
    // refused, which is the kind of limit that is only tested by someone
    // attacking it.
    BOOST_CHECK(sv2::MAX_INBOUND_PAYLOAD < sv2::MAX_PAYLOAD);

    const auto peers{MakePeers()};
    auto initiator{sv2::Transport::Initiator(XOnlyPubKey{peers.authority.GetPubKey()})};
    auto responder{sv2::Transport::Responder(peers.static_key, peers.cert)};
    BOOST_REQUIRE(Pump(initiator, responder));
    BOOST_REQUIRE(Pump(responder, initiator));

    // One byte inside the cap still works, so the limit is not simply
    // refusing everything large.
    const auto fits{Pattern(sv2::MAX_INBOUND_PAYLOAD)};
    BOOST_REQUIRE(initiator.Send(sv2::NetHeader{sv2::EXT_CORE, 0x74, 0}, fits));
    BOOST_REQUIRE(Pump(initiator, responder));
    auto msg{responder.Next()};
    BOOST_REQUIRE(msg.has_value());
    BOOST_CHECK_EQUAL(msg->payload.size(), sv2::MAX_INBOUND_PAYLOAD);

    // One byte over is fatal, and fatal on the header rather than after the
    // body has been read, which is the whole point.
    const auto over{Pattern(sv2::MAX_INBOUND_PAYLOAD + 1)};
    BOOST_REQUIRE(initiator.Send(sv2::NetHeader{sv2::EXT_CORE, 0x74, 0}, over));
    BOOST_CHECK(!Pump(initiator, responder));
}

BOOST_AUTO_TEST_CASE(byte_at_a_time_delivery)
{
    // This is the case a loopback test never produces, and it exercises
    // resuming a frame whose header was decrypted before its body arrived.
    // The header must not be decrypted twice, because that would consume a
    // second nonce and desynchronize the stream.
    const auto peers{MakePeers()};
    auto initiator{sv2::Transport::Initiator(XOnlyPubKey{peers.authority.GetPubKey()})};
    auto responder{sv2::Transport::Responder(peers.static_key, peers.cert)};

    BOOST_REQUIRE(PumpByteWise(initiator, responder));
    BOOST_CHECK(responder.HandshakeComplete());
    BOOST_REQUIRE(PumpByteWise(responder, initiator));
    BOOST_CHECK(initiator.HandshakeComplete());

    const auto payload{Pattern(70000)};
    BOOST_REQUIRE(initiator.Send(sv2::NetHeader{sv2::EXT_CORE, 0x75, 0}, payload));
    BOOST_REQUIRE(PumpByteWise(initiator, responder));

    auto msg{responder.Next()};
    BOOST_REQUIRE(msg.has_value());
    BOOST_CHECK(msg->payload == payload);
}

BOOST_AUTO_TEST_CASE(tampered_header_is_fatal_and_sticky)
{
    const auto peers{MakePeers()};
    auto initiator{sv2::Transport::Initiator(XOnlyPubKey{peers.authority.GetPubKey()})};
    auto responder{sv2::Transport::Responder(peers.static_key, peers.cert)};
    BOOST_REQUIRE(Pump(initiator, responder));
    BOOST_REQUIRE(Pump(responder, initiator));

    const auto payload{Pattern(50)};
    BOOST_REQUIRE(initiator.Send(sv2::NetHeader{sv2::EXT_CORE, 0x76, 0}, payload));
    const auto out{initiator.SendBuffer()};
    std::vector<std::byte> copy{out.begin(), out.end()};
    initiator.Sent(copy.size());
    copy[3] ^= std::byte{0x01};

    responder.Received(copy);
    BOOST_CHECK(!responder.Advance(1500));
    // An authenticated stream has no safe way to skip a bad frame, so the
    // failure must latch rather than let a careless caller resynchronize.
    BOOST_CHECK(!responder.Advance(1500));
}

BOOST_AUTO_TEST_CASE(tampered_body_is_fatal)
{
    const auto peers{MakePeers()};
    auto initiator{sv2::Transport::Initiator(XOnlyPubKey{peers.authority.GetPubKey()})};
    auto responder{sv2::Transport::Responder(peers.static_key, peers.cert)};
    BOOST_REQUIRE(Pump(initiator, responder));
    BOOST_REQUIRE(Pump(responder, initiator));

    const auto payload{Pattern(50)};
    BOOST_REQUIRE(initiator.Send(sv2::NetHeader{sv2::EXT_CORE, 0x76, 0}, payload));
    const auto out{initiator.SendBuffer()};
    std::vector<std::byte> copy{out.begin(), out.end()};
    initiator.Sent(copy.size());
    copy[sv2::ENCRYPTED_HEADER_SIZE + 2] ^= std::byte{0x01};

    responder.Received(copy);
    BOOST_CHECK(!responder.Advance(1500));
}

BOOST_AUTO_TEST_CASE(expired_certificate_fails_handshake)
{
    const auto peers{MakePeers()};
    auto initiator{sv2::Transport::Initiator(XOnlyPubKey{peers.authority.GetPubKey()})};
    auto responder{sv2::Transport::Responder(peers.static_key, peers.cert)};

    // The responder has no reason to object; the initiator is the side that
    // checks the certificate.
    BOOST_REQUIRE(Pump(initiator, responder, /*now=*/5000));
    BOOST_CHECK(!Pump(responder, initiator, /*now=*/5000));
}

BOOST_AUTO_TEST_SUITE_END()
