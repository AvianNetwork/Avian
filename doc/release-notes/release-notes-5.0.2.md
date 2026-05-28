v5.0.2 Release Notes
====================

Avian Core version v5.0.2 is now available from:

  <https://github.com/AvianNetwork/Avian/releases>

This is a bug-fix release for v5.0.1.

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

### Chain Parameters

#### Updated assumevalid, minimumchainwork, and chainTxData

The hardcoded chain sync hints have been updated to block 4,909,756:

- `defaultAssumeValid` — `000000000afe7564cd746a69c516b407e3aa0357e95f5dc853b58d6bd9a98339`
- `nMinimumChainWork` — `000000000000000000000000000000000000000000000000467aa1ce12211f7a`
- `chainTxData` — updated to reflect 5,610,165 total transactions as of May 2026
  (previously set to September 2022 data)

These values improve initial block download performance for new nodes by
skipping script verification for all blocks prior to height 4,909,756.

### Build System

#### Qt upgraded from 6.7.3 to 6.8.3

The bundled Qt in the `depends` build system has been upgraded from 6.7.3 to
6.8.3, bringing upstream bug fixes and security patches.

#### Fix reproducible build failure due to GCC _Float16 native type (Guix)

The Qt `depends` build now patches out unconditional use of the `_Float16`
native type introduced in `qtbase` 6.x. When building with GCC 12+, the
compiler emits references to `GCC_12.0.0` versioned symbols for `_Float16`
arithmetic helpers, causing the Guix `check-symbols` script to reject the
resulting binaries as non-reproducible.

A new patch (`qtbase_avoid_native_float16.patch`) is applied during the Qt
`preprocess` step, replacing all `_Float16` literals and operations with
`float` equivalents. The previously attempted `QT_FEATURE_c_float16` CMake
option was ineffective and has been removed. This restores clean
`check-symbols` output and reproducible builds across all four Guix targets
(x86_64-linux-gnu, aarch64-linux-gnu, arm-linux-gnueabihf, dist-archive).

### GUI

#### Context menu styling, spinbox width, and theme icon colour fixes

Various Qt widget styling issues have been corrected: context menus now render
correctly in both light and dark themes, spinbox width is no longer clipped,
and theme icon colours are applied consistently across dialogs.

#### Restricted asset UI fixes

Several fixes to the restricted asset and qualifier dialogs in the Qt wallet:

- The Address List panel is now populated correctly after qualifier or
  restriction broadcast operations.
- Confirmation dialogs have been added to the Restrict Addresses and Assign
  Qualifier tabs, and the Check button on those tabs is now wired up correctly.
- The FreezeAddress combo box no longer omits restricted assets when the wallet
  holds both a base asset (e.g. `FOO`) and its owner token (`FOO!`).
- Combo boxes on the Assign/Remove Qualifier and Restrict Addresses tabs are now
  populated on dialog open.
- The asset balance list no longer goes blank when the wallet contains
  unconfirmed restricted asset UTXOs.
- The PSBT Operations dialog now shows the verifier string for restricted asset
  sends instead of a blank AVN send line.

### Asset Layer

#### Fix: listassets misses assets not yet flushed from memory to disk

`listassets` now includes assets that have been created but whose database
records have not yet been flushed from the in-memory cache to disk, preventing
them from being silently omitted from the results.

#### Fix: restricted asset transaction output ordering and owner token handling

Two related issues with restricted asset transaction construction have been
fixed:

- Output ordering and the change output position in `CreateAssetTransaction`
  were incorrect in certain cases and have been corrected.
- Restricted asset creation transactions that incorrectly included an owner
  token creation output are now rejected at validation.
- The dust check now exempts null asset scripts used in restricted asset
  creation, which were previously being incorrectly rejected.

#### Fix: reject restricted asset creation txs containing owner token outputs

Restricted asset creation transactions that contain an owner token creation
output (`NEW_ASSET` for the `!` owner) are now correctly rejected, preventing
malformed transactions from entering the mempool.

### RPC

#### New fields in listassets and getassetdata: has_ans and ans_id

`listassets` and `getassetdata` now return two additional fields:

- `has_ans` (bool): whether the asset has an associated Avian Name Service
  record.
- `ans_id` (string): the ANS identifier for the asset, if present.

### Wallet / PSBT

#### Create Unsigned PSBT support for asset creation and reissue dialogs

The asset creation and reissue dialogs in the Qt wallet now include a
"Create Unsigned PSBT" button, allowing hardware wallet users and
multi-signature setups to construct asset transactions without signing them
immediately. This mirrors the existing PSBT workflow available for standard
AVN sends.
