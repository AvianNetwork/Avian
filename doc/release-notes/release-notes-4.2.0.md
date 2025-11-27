# Avian Core 4.2.0 Release Notes

## Overview

Avian Core 4.2.0 "Longbottom Leaf" is a maintenance and modernization release focused on improving build infrastructure, dependency freshness, GUI responsiveness, and wallet performance ahead of new feature development.

**This is the final release of the 4.2.0 series, following three release candidates (RC1, RC2, RC3).**

---

## Significant Changes

### Build Infrastructure & Dependencies

- **Qt Framework Upgrade**: 5.7.0 → 5.12.11 (LTS)

  - Improved stability and performance on modern systems
  - Better cross-platform compatibility
  - Enhanced security patches from upstream

- **Updated Dependencies**: All depends system packages updated to current, secure versions

  - Ensures long-term stability and security

- **CI/CD Modernization**: GitHub Actions now builds binaries for:
  - Windows (x64)
  - Linux (x64)
  - macOS (x64)
  - aarch64 (ARM 64-bit)
  - arm32v7 (ARM 32-bit)

### Network & Protocol

- **Protocol Version Bump**

  - Ensures compatibility with 4.1.x and newer nodes only
  - Pre-4.1 wallets will no longer connect, preventing sync slowdowns and network inconsistencies

- **getblocktemplate Update**: Improved compliance with BIP22 specification

- **Chain Parameters**: PoW hash calculation now uses chain-specific parameters correctly

### GUI Improvements

- **Enhanced Startup Experience**

  - Splash screen now remains visible with real-time progress updates
  - Prevents UI blocking during wallet loading
  - Defers network initialization to speed up GUI construction
  - Displays RC version information on splash screen when applicable

- **UTXO Consolidation GUI Refactor**
  - Consolidation now runs on background thread using QtConcurrent::run()
  - Prevents UI freezing during long batch operations
  - Progress dialog fully responsive and cancelable
  - Uses QMetaObject::invokeMethod for thread-safe UI updates

### New RPC Commands

#### `getblockstats`

Returns comprehensive block-level statistics including:

- Transaction counts, sizes, and weights
- Fee information (min, max, average, percentiles)
- Input/output counts
- Segwit transaction statistics
- Developer fee tracking
- **New**: `powtype` field indicating the block's PoW algorithm (x16rt, minotaurx, or x16r for genesis)

Usage:

```
getblockstats <hash_or_height> [stats]
```

#### `listwatchonly`

Lists all imported watch-only addresses with their labels and metadata.

Usage:

```
listwatchonly
```

#### `removewatchonly`

Removes a watch-only address from wallet storage and address book.

Usage:

```
removewatchonly <address>
```

#### `getnodeaddresses`

Lists known peer node addresses for network diagnostics.

Usage:

```
getnodeaddresses [count]
```

#### `getindexinfo`

Displays the status of active blockchain indexes.

Usage:

```
getindexinfo
```

### Wallet & UTXO Management

#### New UTXO Consolidation System

Consolidates small UTXOs into larger ones to reduce future transaction costs and improve wallet performance.

**RPC Command**: `consolidateutxos`

```
consolidateutxos <address> [min_utxos] [max_batches] [min_amount] [max_amount]
```

Parameters:

- `address`: Destination address for consolidated outputs
- `min_utxos`: Minimum UTXOs required before consolidation (default: 2000)
- `max_batches`: Maximum number of batches to process (default: unlimited)
- `min_amount`: Minimum UTXO amount to include (default: 0.01 AVN)
- `max_amount`: Maximum UTXO amount to include (default: 25 AVN)

**GUI Integration**: New "Consolidate UTXO Transactions" dialog with:

- Configurable limits (min/max inputs, batch size, amount ranges)
- Real-time UTXO display and selection
- Progress tracking and cancellation support
- Thread-safe background processing

**Improvements in RC3**:

- Improved batch counting and progress accuracy
- Pre-calculates totalUTXOFiltered before consolidation loop
- Applies min/max filters consistently during counting
- Accurately accounts for maxBatches limit
- Displays precise progress (e.g., "1 of 1" when maxBatches=1)

#### New Watch-Only Address Management

- `listwatchonly`: Display all imported watch-only addresses with labels
- `removewatchonly`: Completely remove watch-only addresses from wallet storage and address book
- Automatic GUI sync via NotifyAddressBookChanged signal
- No wallet unlock required for watch-only operations

### Developer & Maintenance

- **NetworkUpgrade Struct**: Added for better handling of network upgrade logic
- **Mainnet Checkpoint Data**: Updated to the latest block
- **Help System**: Added powhashcache details to help messages

---

## Bug Fixes & Performance Improvements

- Fixed null pointer dereference in `getblockstats` for genesis block
- Improved wallet RPC command filtering and accuracy
- Performance optimizations in GUI startup sequence
- Better error handling in batch UTXO operations
- Fixed progress estimation for UTXO consolidation

---

## Testing Recommendations

### For Windows Users

- Test wallet startup time and responsiveness
- Verify UTXO consolidation with test wallets containing many small outputs
- Check splash screen progress updates during initial sync

### For Linux / macOS Users

- Verify cross-platform Qt 5.12.11 compatibility
- Test RPC commands (especially new ones: getblockstats, listwatchonly, removewatchonly)
- Confirm network syncing with mixed 4.1.x and 4.2.0 nodes

### For ARM Users (aarch64 / arm32v7)

- Verify binary builds and execution on ARM hardware
- Test wallet functionality and RPC operations
- Confirm network connectivity and syncing

### For RPC Users

- Test `getblockstats` with various block heights and filters
- Verify `listwatchonly` and `removewatchonly` operations
- Test `getnodeaddresses` and `getindexinfo`
- Confirm UTXO consolidation batch processing

---

## Upgrading

### From 4.1.x

- Backup your wallet before upgrading
- The protocol version bump means pre-4.1 nodes will disconnect from your node
- You can deploy 4.2.0; it will automatically only connect to 4.1.0+ peers
- UTXO consolidation is optional and accessed via GUI or RPC

### From Older Versions

- Direct upgrade from any prior version is supported
- If you are upgrading from a version much older than 4.1.0, ensure you are on the correct chain (mainnet vs testnet)
- If experiencing sync issues after upgrade, verify your peers and network connectivity
- Use `-reindex` flag if you encounter blockchain validation errors

---

## Known Issues

None known at this time. Please report any issues via GitHub Issues.

---

## Credits

The Avian Core development team thanks all contributors, testers, and the community for their feedback throughout the RC process.

---

## Version Information

- **Version**: 4.2.0
- **Release Date**: 2025-11-27
- **Previous Release**: 4.1.0
- **Network**: Mainnet

---

## Changelog Summary

- 1 new major feature (UTXO Consolidation System)
- 5 new RPC commands (getblockstats, listwatchonly, removewatchonly, getnodeaddresses, getindexinfo)
- 1 major dependency upgrade (Qt 5.12.11 LTS)
- 30+ bug fixes and improvements
- Full CI/CD modernization (5 platform builds)
