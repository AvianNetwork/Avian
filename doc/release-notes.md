v5.0.0 Release Notes
====================

Avian Core version v5.0.0 is now available from:

  <https://github.com/AvianNetwork/Avian/releases>

This is a major release that rebases the entire Avian codebase onto Bitcoin
Core v30.2, bringing years of upstream improvements while preserving all
Avian-specific features including assets, dual-algorithm mining, and
replay protection.

Please report bugs using the issue tracker at GitHub:

  <https://github.com/AvianNetwork/Avian/issues>

How to Upgrade
==============

If you are running an older version, shut it down. Wait until it has completely
shut down (which might take a few minutes in some cases), then run the
installer (on Windows) or just copy over `/Applications/Avian-Qt` (on macOS)
or `aviand`/`avian-qt` (on Linux).

Compatibility
=============

Avian Core is supported and tested on operating systems using the
Linux Kernel 3.17+, macOS 13+, and Windows 10+. Avian Core should also
work on most other Unix-like systems but is not as frequently tested on
them. It is not recommended to use Avian Core on unsupported systems.

Notable changes
===============

### Bitcoin Core v30.2 Rebase

The entire codebase has been rebased onto Bitcoin Core v30.2, bringing:

- CMake-based build system (replaces autotools)
- Modern P2P networking, mempool, and validation engine
- Descriptor wallets and modern wallet architecture
- Compact block relay (BIP 152)
- I2P and CJDNS network support
- Updated libsecp256k1 with improved performance
- Improved fee estimation algorithms
- Full SegWit support

### Asset Layer

- Complete rewrite of asset consensus validation for the Bitcoin Core 30.2
  transaction and UTXO framework
- Asset issuance, transfer, and reissue RPCs (`issue`, `transfer`, `reissue`,
  `listmyassets`)
- Asset database persistence and synchronization fixes
- PSBT (Partially Signed Bitcoin Transaction) support for asset transactions
- Full GUI for asset management: create, transfer, reissue dialogs
- Address, timestamp, and spent index support for asset queries

### Mining & Consensus

- Dual-algorithm mining (X16RT / MinotaurX) fully preserved
- Per-algorithm difficulty and hashrate RPCs
- PowCache for efficient proof-of-work hash caching (`powcache.dat`)
- SIGHASH_FORKID (UAHF) replay protection maintained
- Max reorg depth enforcement preserved

### Post-Quantum Addresses (RIP-25)

Avian v5.0.0 introduces support for ML-DSA-44 (FIPS 204) post-quantum signature
addresses as specified in RIP-25.

- New `pq` address type using witness version 2 (`avn1p…` bech32 addresses)
- Keypairs are derived deterministically from a 32-byte HD seed using the
  ML-DSA-44 lattice-based signature scheme (NIST FIPS 204 / liboqs 0.12.0)
- `getnewaddress "" "pq"` generates a post-quantum address
- `getaddressinfo` returns `"ispostquantum": true` for PQ addresses
- Qt receive dialog includes a "Post-Quantum (ML-DSA-44)" address type option
- `DEPLOYMENT_MLDSA44` BIP9 deployment (bit 11): **`NEVER_ACTIVE` on mainnet**,
  `ALWAYS_ACTIVE` on testnet, testnet4, and regtest
- Post-quantum addresses carry larger transactions than secp256k1: public keys
  are 1312 bytes and signatures are 2420 bytes — this is an inherent tradeoff
  of lattice-based post-quantum security
- liboqs is built by default. To build without post-quantum support, pass
  `-DWITH_LIBOQS=OFF` to cmake. Without liboqs all other functionality is
  unaffected.

Mainnet activation is intentionally deferred pending community signalling via
the BIP9 mechanism.

### Wallet

- BIP44 coin type 921 for Avian HD wallet derivation paths
- Legacy wallet import with proper BIP44 keymeta path handling
- Wallet migration fixes for orphaned watch-only transactions and HD keys
- UTXO consolidation RPC (`consolidateutxos`)

### GUI

- Dark and light theme switching with custom CSS theming
- New Konnect font
- Paper wallet generation and printing
- Asset management UI with sidebar navigation
- Asset create, transfer, reissue, and UTXO duster dialogs
- Full rebrand from Bitcoin to Avian across all user-facing strings and
  currency units (AVN/avn)

### Build & Infrastructure

- Deterministic Guix builds for all 7 platforms:
  - `x86_64-linux-gnu`, `arm-linux-gnueabihf`, `aarch64-linux-gnu`
  - `riscv64-linux-gnu`, `powerpc64-linux-gnu`
  - `x86_64-w64-mingw32` (Windows)
  - `x86_64-apple-darwin` (macOS)
- All Guix release builds include post-quantum (ML-DSA-44) support via liboqs
- CI pipeline for Linux, macOS, and Windows
- CUPS dependency for Qt print support (paper wallets)
- Translation system rebranded and integrated with Weblate

### Network & Protocol

- BIP324 v2 encrypted transport disabled (incompatible with SIGHASH_FORKID)
- Pre-VERACK message filtering for peer compatibility
- Avian-specific protocol messaging and service flags

Credits
=======

Thanks to The Bitcoin Core developers for the upstream codebase, the original
Ravencoin developers, and the Avian Core team for the Avian implementation.

Special thanks to [ALENOC](https://github.com/ALENOC) for the RIP-25 ML-DSA-44
post-quantum signature implementation.

As well as to everyone that helped with translations on
[Weblate](https://weblate.avn.network/).
