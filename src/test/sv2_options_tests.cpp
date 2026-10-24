// Copyright (c) 2026 The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <common/args.h>
#include <fstream>
#include <key.h>
#include <pubkey.h>
#include <netbase.h>
#include <sv2/options.h>
#include <test/util/setup_common.h>
#include <util/chaintype.h>
#include <util/fs.h>
#include <util/result.h>
#include <util/time.h>

#include <set>
#include <string>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(sv2_options_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(default_ports_are_distinct_and_unused)
{
    // Every chain needs its own port, and none may collide with a port the
    // node already uses. There are three such sets per chain and not two: the
    // P2P port, the RPC port, and the Tor onion service target, which init
    // derives as the P2P port plus one. Leaving that last one out is what let
    // the regtest default collide with 18445 and stop the node listening.
    std::set<uint16_t> taken{7896, 18771, 28771, 38332, 18443}; // RPC
    for (const uint16_t p2p : {7895, 18770, 28770, 38333, 18444}) {
        taken.insert(p2p);
        taken.insert(static_cast<uint16_t>(p2p + 1)); // onion service target
    }
    std::set<uint16_t> seen;
    for (const ChainType chain : {ChainType::MAIN, ChainType::TESTNET, ChainType::TESTNET4,
                                  ChainType::SIGNET, ChainType::REGTEST}) {
        const uint16_t port{sv2::DefaultPort(chain)};
        BOOST_CHECK_MESSAGE(taken.count(port) == 0, "sv2 default port collides with an existing port");
        BOOST_CHECK_MESSAGE(seen.insert(port).second, "two chains share an sv2 default port");
    }
    BOOST_CHECK_EQUAL(seen.size(), 5u);
}

BOOST_AUTO_TEST_CASE(defaults_bind_to_loopback)
{
    // The Noise handshake authenticates the server to the client and not the
    // reverse, so a default of anything but loopback would quietly expose an
    // unauthenticated port.
    const auto opts{sv2::ReadOptions(m_args, ChainType::REGTEST)};
    BOOST_REQUIRE(opts);
    BOOST_REQUIRE_EQUAL(opts->binds.size(), 1u);
    BOOST_CHECK(opts->binds.front().IsLocal());
    BOOST_CHECK_EQUAL(opts->binds.front().GetPort(), sv2::DefaultPort(ChainType::REGTEST));
    // No allow list means loopback only, rather than everything.
    BOOST_CHECK(opts->allow.empty());
}

BOOST_AUTO_TEST_CASE(bind_and_port_parsing)
{
    m_args.ForceSetArg("-sv2bind", "127.0.0.1:12345");
    {
        const auto opts{sv2::ReadOptions(m_args, ChainType::REGTEST)};
        BOOST_REQUIRE(opts);
        BOOST_REQUIRE_EQUAL(opts->binds.size(), 1u);
        BOOST_CHECK_EQUAL(opts->binds.front().ToStringAddrPort(), "127.0.0.1:12345");
    }

    // A bind without a port picks up -sv2port.
    m_args.ForceSetArg("-sv2bind", "127.0.0.1");
    m_args.ForceSetArg("-sv2port", "54321");
    {
        const auto opts{sv2::ReadOptions(m_args, ChainType::REGTEST)};
        BOOST_REQUIRE(opts);
        BOOST_CHECK_EQUAL(opts->binds.front().GetPort(), 54321);
    }
}

BOOST_AUTO_TEST_CASE(invalid_values_are_rejected)
{
    // A hostname is refused rather than resolved: a listener should not depend
    // on name resolution, and it is far more likely to be a mistake.
    m_args.ForceSetArg("-sv2bind", "not a host");
    BOOST_CHECK(!sv2::ReadOptions(m_args, ChainType::REGTEST));
    m_args.ForceSetArg("-sv2bind", "");

    m_args.ForceSetArg("-sv2allowip", "999.0.0.1/8");
    BOOST_CHECK(!sv2::ReadOptions(m_args, ChainType::REGTEST));
    m_args.ForceSetArg("-sv2allowip", "");

    m_args.ForceSetArg("-sv2maxclients", "0");
    BOOST_CHECK(!sv2::ReadOptions(m_args, ChainType::REGTEST));
    m_args.ForceSetArg("-sv2maxclients", "16");

    m_args.ForceSetArg("-sv2handshaketimeout", "0");
    BOOST_CHECK(!sv2::ReadOptions(m_args, ChainType::REGTEST));
    m_args.ForceSetArg("-sv2handshaketimeout", "10");

    // The send buffer has to hold one whole block, because that is what
    // RequestTransactionData.Success will carry.
    m_args.ForceSetArg("-sv2maxsendbuffer", "4");
    BOOST_CHECK(!sv2::ReadOptions(m_args, ChainType::REGTEST));
    m_args.ForceSetArg("-sv2maxsendbuffer", "32");

    m_args.ForceSetArg("-sv2certvalidity", "0");
    BOOST_CHECK(!sv2::ReadOptions(m_args, ChainType::REGTEST));
}

BOOST_AUTO_TEST_CASE(allowip_parsing)
{
    m_args.ForceSetArg("-sv2allowip", "10.0.0.0/8");
    const auto opts{sv2::ReadOptions(m_args, ChainType::REGTEST)};
    BOOST_REQUIRE(opts);
    BOOST_REQUIRE_EQUAL(opts->allow.size(), 1u);
    BOOST_CHECK(opts->allow.front().Match(LookupNumeric("10.1.2.3", 0)));
    BOOST_CHECK(!opts->allow.front().Match(LookupNumeric("11.1.2.3", 0)));
}

BOOST_AUTO_TEST_CASE(key_is_created_then_reused)
{
    const fs::path path{m_args.GetDataDirNet() / "sv2_test_key"};
    BOOST_REQUIRE(!fs::exists(path));

    const auto created{sv2::LoadOrCreateKey(path)};
    BOOST_REQUIRE(created);
    BOOST_CHECK(created->IsValid());
    BOOST_CHECK(fs::exists(path));

    // A restart must reuse the same identity, otherwise every client would
    // have to be reconfigured.
    const auto loaded{sv2::LoadOrCreateKey(path)};
    BOOST_REQUIRE(loaded);
    BOOST_CHECK(created->GetPubKey() == loaded->GetPubKey());
}

BOOST_AUTO_TEST_CASE(corrupt_key_file_is_an_error)
{
    // Silently regenerating here would change the server identity without
    // anyone noticing, so a short or invalid file has to be reported.
    const fs::path path{m_args.GetDataDirNet() / "sv2_short_key"};
    {
        std::ofstream out{path, std::ios::binary};
        out << "too short";
    }
    BOOST_CHECK(!sv2::LoadOrCreateKey(path));
}

BOOST_AUTO_TEST_CASE(certificate_covers_the_static_key)
{
    CKey authority;
    authority.MakeNewKey(/*fCompressedIn=*/true);
    CKey static_key;
    static_key.MakeNewKey(/*fCompressedIn=*/true);

    const int64_t now{GetTime()};
    const auto cert{sv2::MakeCertificate(authority, static_key, now, /*validity_days=*/1)};
    BOOST_REQUIRE(cert);

    const XOnlyPubKey authority_pub{authority.GetPubKey()};
    const XOnlyPubKey static_pub{static_key.GetPubKey()};
    BOOST_CHECK(cert->Verify(static_pub, authority_pub, now));
    // Valid for the requested window and no longer.
    BOOST_CHECK(cert->Verify(static_pub, authority_pub, now + 23 * 60 * 60));
    BOOST_CHECK(!cert->Verify(static_pub, authority_pub, now + 25 * 60 * 60));

    // It must not vouch for a different static key.
    CKey other;
    other.MakeNewKey(/*fCompressedIn=*/true);
    BOOST_CHECK(!cert->Verify(XOnlyPubKey{other.GetPubKey()}, authority_pub, now));
}

BOOST_AUTO_TEST_SUITE_END()
