// Copyright (c) 2026 The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_SV2_OPTIONS_H
#define BITCOIN_SV2_OPTIONS_H

#include <key.h>
#include <netaddress.h>
#include <sv2/noise.h>
#include <util/chaintype.h>
#include <util/fs.h>
#include <util/result.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class ArgsManager;

namespace sv2 {

//! Default listening port per chain.
//!
//! These avoid three sets of ports, not two: the P2P port, the RPC port, and
//! the Tor onion service target, which init derives as the P2P port plus one
//! and binds on loopback. Missing that third one made the regtest default
//! collide with 18445 and stopped the node listening at all.
uint16_t DefaultPort(ChainType chain);

//! How the Template Provider listener is configured.
struct Options {
    //! Addresses to listen on. Defaults to loopback, because the Noise NX
    //! handshake authenticates the server to the client and not the other way
    //! round, so it is not an access control.
    std::vector<CService> binds;

    //! Subnets permitted to connect, checked at accept before any crypto. An
    //! empty list means loopback only.
    std::vector<CSubNet> allow;

    size_t max_clients{16};
    size_t max_clients_per_ip{4};
    std::chrono::seconds handshake_timeout{10};

    //! Caps on buffered bytes per client. The receive cap only has to hold one
    //! frame plus framing; the send cap must exceed a whole block, which is
    //! what RequestTransactionData.Success will eventually carry.
    size_t max_recv_buffer{1 << 20};
    size_t max_send_buffer{size_t{32} << 20};

    //! Long-term key proving this server's identity, and the key that signs a
    //! certificate for it. Both are generated on first run.
    fs::path static_key_file;
    fs::path authority_key_file;
    int64_t cert_validity_days{3650};
};

//! Read the -sv2* options. Returns an error string rather than throwing, so
//! init can report it as an InitError.
util::Result<Options> ReadOptions(const ArgsManager& args, ChainType chain);

//! Load a secp256k1 private key from `path`, creating one if the file does not
//! exist. The file holds the raw 32-byte key and is created with the process
//! umask, which init sets to 0077.
util::Result<CKey> LoadOrCreateKey(const fs::path& path);

//! Sign a certificate for `static_key` with `authority`, valid from now for
//! `validity_days`.
util::Result<SignatureNoiseMessage> MakeCertificate(const CKey& authority, const CKey& static_key,
                                                   int64_t now, int64_t validity_days);

} // namespace sv2

#endif // BITCOIN_SV2_OPTIONS_H
