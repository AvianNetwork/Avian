// Copyright (c) 2026 The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_SV2_OPTIONS_H
#define BITCOIN_SV2_OPTIONS_H

#include <key.h>
#include <netaddress.h>
#include <primitives/block.h>
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

//! What a client must negotiate before it may ask for templates.
enum class ExtensionPolicy {
    //! The client must send RequestExtensions and include everything in
    //! `required_extensions`, or it is disconnected.
    REQUIRE,
    //! RequestExtensions is answered if sent, but is not demanded.
    OPTIONAL,
    //! Avian extensions are not advertised at all. For proving that an
    //! unmodified client can still complete a session.
    OFF,
};

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

    ExtensionPolicy extension_policy{ExtensionPolicy::REQUIRE};

    //! Identifier for Avian's multi-PoW extension.
    //!
    //! Overridable for development and migration only. The default lies in
    //! the spec's experimental range, which production equipment must not
    //! use, so this has to become a registry-allocated value before mainnet.
    uint16_t extension_id{AVIAN_EXTENSION_ID};

    //! Algorithms this server will build templates for. The first is what a
    //! connection gets if it does not select one.
    std::vector<POW_TYPE> pow_algos{POW_TYPE_X16RT};

    //! Which nVersion bits a client may roll. Always zero, and deliberately
    //! not configurable.
    //!
    //! There is no durable safe subset to expose. Bits 16-23 hold the
    //! algorithm, so rolling them either trips bad-algo-id or silently
    //! switches a share to the other algorithm and the wrong pow limit. Bits
    //! 29-31 are VERSIONBITS_TOP_MASK, which Avian also uses to signal
    //! assets. What is left is BIP9 deployment space, where bits 2, 11, 12
    //! and 28 are already assigned and the rest is where future deployments
    //! will land, so a mask that is safe today would not stay safe.
    //!
    //! Version rolling exists to give ASICs search space past nonce and
    //! extranonce at very high hash rates. X16RT and MinotaurX are GPU and
    //! CPU algorithms where nonce plus extranonce is ample, so this buys
    //! nothing to set against that. The field stays in the wire format, and
    //! CheckSolution honours a non-zero value, so a later protocol version
    //! can use it without a format change.
    uint32_t version_rolling_mask{0};

    //! How many solutions a client may have refused before it is dropped. A
    //! client failing repeatedly is almost always rolling a field it was told
    //! not to, and will never find a valid block.
    size_t max_rejects{10};

    //! How many templates to keep live per client, newest included.
    //!
    //! A refresh supersedes a template but does not make it worthless: work
    //! already handed to a miner is still valid for the same tip, and refusing
    //! a block found on it would throw away a real block. Avian refreshes on
    //! the same cadence as it produces blocks, so keeping only the newest
    //! would discard any solution whose job predates the last refresh.
    //!
    //! Cheap to raise. Templates hold CTransactionRef, so retaining one costs
    //! a vector of shared pointers rather than a second copy of the block, and
    //! the serialized transaction list, which is the one expensive member, is
    //! released as soon as a template stops being the newest. Templates for a
    //! superseded tip are dropped outright, since those really are worthless.
    size_t max_templates{3};

    //! How often to rebuild templates when the tip has not moved, which is
    //! what picks up new fees. Clamped so an X16RT client always gets a fresh
    //! template before its epoch boundary expires.
    std::chrono::seconds refresh_interval{30};

    //! How far above a template's own timestamp a client may roll ntime, and
    //! how much margin to leave below the consensus future-time limit.
    int64_t ntime_forward{600};
    int64_t ntime_margin{60};

    //! Avian extensions this server implements. Extension 0x0001 itself is
    //! always supported and is not listed here.
    std::vector<uint16_t> supported_extensions;

    //! Extensions a client must request. A client that omits one is told so
    //! and disconnected if it does not retry.
    std::vector<uint16_t> required_extensions;

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
