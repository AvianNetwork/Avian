// Copyright (c) 2026 The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <sv2/options.h>

#include <common/args.h>
#include <logging.h>
#include <netbase.h>
#include <pubkey.h>
#include <tinyformat.h>
#include <util/readwritefile.h>
#include <util/translation.h>

#include <algorithm>
#include <string>

namespace sv2 {

uint16_t DefaultPort(ChainType chain)
{
    switch (chain) {
    case ChainType::MAIN: return 8442;
    case ChainType::TESTNET: return 18442;
    case ChainType::TESTNET4: return 28442;
    case ChainType::SIGNET: return 38442;
    // Not 18445: that is regtest's onion service target (18444 + 1).
    case ChainType::REGTEST: return 18447;
    }
    assert(false);
}

util::Result<Options> ReadOptions(const ArgsManager& args, ChainType chain)
{
    Options opts;
    const uint16_t default_port{static_cast<uint16_t>(args.GetIntArg("-sv2port", DefaultPort(chain)))};

    for (const std::string& spec : args.GetArgs("-sv2bind")) {
        // Numeric only: a listener must not depend on name resolution, and a
        // hostname here is far more likely to be a mistake than an intent.
        const CService bind{LookupNumeric(spec, default_port)};
        if (!bind.IsValid()) {
            return util::Error{Untranslated(strprintf("Invalid -sv2bind address '%s'", spec))};
        }
        opts.binds.push_back(bind);
    }
    if (opts.binds.empty()) {
        // Loopback by default. The Noise NX handshake proves the server's
        // identity to the client, not the client's to the server, so exposing
        // this port is equivalent to exposing RPC.
        opts.binds.emplace_back(LookupNumeric("127.0.0.1", default_port));
    }

    for (const std::string& spec : args.GetArgs("-sv2allowip")) {
        const CSubNet subnet{LookupSubNet(spec)};
        if (!subnet.IsValid()) {
            return util::Error{Untranslated(strprintf("Invalid -sv2allowip subnet '%s'", spec))};
        }
        opts.allow.push_back(subnet);
    }

    const int64_t max_clients{args.GetIntArg("-sv2maxclients", opts.max_clients)};
    if (max_clients < 1) {
        return util::Error{Untranslated("-sv2maxclients must be at least 1")};
    }
    opts.max_clients = static_cast<size_t>(max_clients);

    const int64_t per_ip{args.GetIntArg("-sv2maxclientsperip", opts.max_clients_per_ip)};
    if (per_ip < 1) {
        return util::Error{Untranslated("-sv2maxclientsperip must be at least 1")};
    }
    opts.max_clients_per_ip = static_cast<size_t>(per_ip);

    const int64_t timeout{args.GetIntArg("-sv2handshaketimeout", opts.handshake_timeout.count())};
    if (timeout < 1) {
        return util::Error{Untranslated("-sv2handshaketimeout must be at least 1 second")};
    }
    opts.handshake_timeout = std::chrono::seconds{timeout};

    const int64_t send_buffer{args.GetIntArg("-sv2maxsendbuffer", opts.max_send_buffer >> 20)};
    // A single RequestTransactionData.Success carries a whole block, so this
    // cannot be tightened below the block size without stalling that message.
    if (send_buffer < 16) {
        return util::Error{Untranslated("-sv2maxsendbuffer must be at least 16 MiB, enough for one block")};
    }
    opts.max_send_buffer = static_cast<size_t>(send_buffer) << 20;

    const int64_t validity{args.GetIntArg("-sv2certvalidity", opts.cert_validity_days)};
    if (validity < 1) {
        return util::Error{Untranslated("-sv2certvalidity must be at least 1 day")};
    }
    opts.cert_validity_days = validity;

    opts.static_key_file = args.GetPathArg("-sv2staticprivkey", args.GetDataDirNet() / "sv2_static_key");
    opts.authority_key_file = args.GetPathArg("-sv2authoritykey", args.GetDataDirNet() / "sv2_authority_key");

    return opts;
}

util::Result<CKey> LoadOrCreateKey(const fs::path& path)
{
    if (fs::exists(path)) {
        const auto [ok, data]{ReadBinaryFile(path, 32)};
        if (!ok) {
            return util::Error{Untranslated(strprintf("Cannot read %s", fs::PathToString(path)))};
        }
        if (data.size() != 32) {
            return util::Error{Untranslated(strprintf("%s is not a 32-byte private key", fs::PathToString(path)))};
        }
        CKey key;
        key.Set(data.begin(), data.end(), /*fCompressedIn=*/true);
        if (!key.IsValid()) {
            return util::Error{Untranslated(strprintf("%s does not contain a valid private key", fs::PathToString(path)))};
        }
        return key;
    }

    CKey key;
    key.MakeNewKey(/*fCompressedIn=*/true);
    // umask is 0077 for the process, so this lands mode 0600.
    if (!WriteBinaryFile(path, std::string{UCharCast(key.begin()), UCharCast(key.end())})) {
        return util::Error{Untranslated(strprintf("Cannot write %s", fs::PathToString(path)))};
    }
    LogPrintf("sv2: generated a new private key at %s\n", fs::PathToString(path));
    return key;
}

util::Result<SignatureNoiseMessage> MakeCertificate(const CKey& authority, const CKey& static_key,
                                                    int64_t now, int64_t validity_days)
{
    SignatureNoiseMessage cert;
    cert.version = 0;
    cert.valid_from = static_cast<uint32_t>(now);
    cert.not_valid_after = static_cast<uint32_t>(now + validity_days * 24 * 60 * 60);

    const XOnlyPubKey static_pub{static_key.GetPubKey()};
    if (!cert.Sign(authority, static_pub)) {
        return util::Error{Untranslated("Cannot sign the Sv2 server certificate")};
    }
    return cert;
}

} // namespace sv2
