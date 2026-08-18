v5.0.3 Release Notes
====================

Avian Core version v5.0.3 is now available from:

  <https://github.com/AvianNetwork/Avian/releases>

This is a **consensus hotfix release** and is a **mandatory update for everyone
running any 5.0.x version**. See [Consensus](#consensus) below.

Please report bugs using the issue tracker at GitHub:

  <https://github.com/AvianNetwork/Avian/issues>

How to Upgrade
==============

If you are running an older version, shut it down. Wait until it has completely
shut down (which might take a few minutes in some cases), then run the
installer (on Windows) or just copy over `/Applications/Avian-Qt` (on macOS)
or `aviand`/`avian-qt` (on Linux).

### Required step after upgrading

Nodes running 5.0.0 – 5.0.2 followed a divergent fork. After starting v5.0.3,
run the following once from the debug console (Qt: **Window → Console**) or via
`avian-cli`:

```
invalidateblock 00000000044c8acb0abc8c663f22ed74843bf226ed61ef12876f1bc59b870074
```

This marks the first block of the divergent fork as invalid and lets the node
reorganise back onto the canonical chain. **Without this step the node will
need to resync from genesis.** No reindex is required once it has been run.

Compatibility
=============

Avian Core is supported and tested on operating systems using the
Linux Kernel 3.17+, macOS 13+, and Windows 10+. Avian Core should also
work on most other Unix-like systems but is not as frequently tested on
them. It is not recommended to use Avian Core on unsupported systems.

Notable changes
===============

### Consensus

#### Reject unique-asset issuance that creates an owner token

A unique asset has no owner token of its own — it inherits authority from its
parent root `ROOT!` owner, which is *transferred*, not created, during the
issuance. Avian Core 4.2.0 enforced this in consensus via
`VerifyNewUniqueAsset`, but during the Bitcoin Core rewrite that validator was
left with no call sites, so 5.0.x silently accepted a unique issuance carrying
a `NAME#unique!` owner-creation output that 4.2.0 rejects. The first such
transaction split the chain.

The rule is restored on the unique path in `CheckTxAssets`, rejecting any
transaction on the `IsNewUniqueAsset` path that contains a
`TX_NEW_ASSET`+`fIsOwner` output with
`bad-txns-issue-unique-has-owner-creation-output`. It is applied
unconditionally (no activation height) so that it matches 4.2.0
retroactively — the canonical chain never contained such a transaction, so
this can only invalidate the divergent fork.

#### Restore the asset structural-validation layer dropped in the rewrite

The unique-owner split above was one symptom of a broader problem. Avian 4.2.0
validates assets in two consensus layers: `CheckTransaction` runs the
structural `Verify*` validators (burn present, owner token present and
correctly named, output counts, parent owner held), and `CheckTxAssets` runs
the field/contextual checks. The Bitcoin Core rewrite replaced
`CheckTransaction` with stock `consensus/tx_check.cpp`, which has no asset
logic, so every `Verify*` validator became dead code. Only the field layer
survived.

The absence of the structural layer also let 5.x accept — and 4.2.0 reject —
transactions that:

- reissue (mint more of) any reissuable asset **without** holding its owner
  token and **without** paying the burn (unauthorised inflation);
- issue root/sub assets with no or incorrect burn, a mismatched owner token
  name, or (for sub-assets) without the parent owner token;
- issue uniques with no 5 AVN burn or without the parent owner;
- issue restricted/qualifier/message-channel assets with no burn.

Each of these is a chain-split vector, and several are inflation or theft bugs.
The existing (unchanged) `Verify*` validators are now re-wired into the live
`CheckTxAssets` dispatch, one per issuance/reissue branch. These are purely
additive rejections: they can only make the malicious transactions invalid,
never make a previously-invalid transaction valid, and the canonical 4.2.0
chain already satisfies them. The explicit unique and restricted owner-output
guards are kept for defence in depth.

Related wallet fix: the owner-token creation output was gated on the asset type
not being `RESTRICTED`, so the wallet and GUI built an illegal owner token for
`UNIQUE` and `MSGCHANNEL` issuance — the same defect that split the chain.
Owner creation is now restricted to `ROOT` and `SUB` only.

### Wallet

#### Fee defaults corrected for the Bitcoin Core maxtxfee check

Bitcoin Core 27+ (the base for 5.x) added a hard `MAX_FEE_EXCEEDED` check in
`CreateTransaction` that the 4.x wallet lacked. With the inherited fallback fee
of 1,025,000 sat/kvB, any transaction larger than roughly 9.7 kvB exceeded the
0.1 AVN `maxtxfee` cap and was refused outright — which broke pool payout
transactions with many inputs and outputs.

- `DEFAULT_FALLBACK_FEE`: 1,025,000 → 20,000 sat/kvB. A 20 kvB pool payout now
  costs 0.004 AVN instead of failing with `MAX_FEE_EXCEEDED`.
- `DEFAULT_TRANSACTION_MINFEE`: 1,000,000 → 1,000 sat/kvB, still 10x above the
  Avian relay minimum fee of 100 sat/kvB.

#### settxfee no longer deprecated

The deprecation gate on `settxfee` (inherited from the Bitcoin Core v31
roadmap) has been removed. With `estimatesmartfee` non-functional on
thin-mempool networks, `settxfee` is a required operational tool and now works
without `-deprecatedrpc=settxfee`.

### Avian Name Service (ANS v2)

#### AIP-0009 CBOR profile records and AIP-0010 multi-chain addresses

ANS records now support two new types alongside the existing `ADDR` (`0x0`)
type:

- `PROFILE` (`0x2`, AIP-0009 §3.3) — a compact CBOR identity record carrying a
  payment address, display name, avatar, website URL, and banner. Avatar and
  banner may be either a URL/CID string or inline binary image data.
- `XADDR` (`0x1`, AIP-0010) — an external/cross-chain address, available on
  sub-assets of `.AVN` names.

A minimal CBOR decoder (`src/assets/cbor.h`) has been added for parsing profile
payloads, and the `ans_v2` BIP9 deployment gates the new record types. ANS v2
is always active on testnet and testnet4.

#### New and improved RPCs

- `resolveavn` — resolve a `.AVN` name to its payment address.
- `whoisavn` — return the full record for a `.AVN` name. It now includes a
  `holds` field listing regular `X.AVN` tokens held at the queried address, and
  normalises ANS profile addresses to hash160 before comparison so that bech32
  and legacy addresses for the same key match correctly in `registered_as`.
- `myansnames` — list the `.AVN` names owned by the wallet.

#### GUI support for ANS

The asset creation and reissue dialogs gained ANS support: the ANS box is
enabled and locked automatically when the asset name ends in `.AVN`
(quantity 1, 0 units, as required), `PROFILE` is offered as the default record
type, and sub-assets of `.AVN` names can carry AIP-0010 `XADDR` records. The
send dialog resolves `.AVN` names to addresses inline, falling back to the
holder of the `NAME.AVN!` owner token when no ANS record is present (requires
`-assetindex`). The address validator accepts in-progress ANS names without
prematurely accepting them as base58/bech32 addresses.

Reserved base names such as `AVN.AVN` and `RVN.AVN` are blocked in the UI only;
they are deliberately not blocked at the consensus level so that pre-ANS assets
on mainnet remain valid.

#### ANS info dialog

The ANS information dialog has been rebuilt as a styled `QDialog` with a teal
header and grid layout in place of a plain `QMessageBox`. Avatar, banner, and
URL fields open in the system browser when they contain `ipfs://` or
`http(s)://` values.

### RPC

#### New RPC: getburnaddresses

`getburnaddresses` returns the burn address and required fee for each asset
operation type, removing the need for callers to hard-code them.

#### getdeploymentinfo covers Avian network upgrades

`getdeploymentinfo` now reports the Avian timestamp-based network upgrades
(`x16rt`, `dual_algo`, `assets`, `flight_plans`, `ans_v1`) alongside the BIP9
deployments, including `activation_time`, `activation_datetime` (ISO 8601), and
the activation height (located by binary search). Additional fixes:

- BIP9 deployments that are `NEVER_ACTIVE` (`mldsa44`, `ans_v2` on mainnet) are
  now shown with status `never_active` instead of being silently omitted.
- `superseded_by` is only emitted on timestamp upgrades when the target
  deployment is actually enabled on the current network.
- The `testdummy` BIP9 deployment has been removed from the output.

### Node

#### Fix missing assets after bootstrapping without an assets directory

When the asset database was empty but the UTXO set was already synced — the
usual state after restoring from a bootstrap that did not include an `assets/`
directory — historical assets were silently missing. Startup now detects this
case and triggers an automatic asset reindex.

### Networks

#### Avian testnet4 (v5) enabled

Testnet4 is now a real Avian test network rather than a stub inheriting Bitcoin
Core defaults:

- Fresh data directory `testnet4_v5`, P2P port 28770, RPC port 28771.
- A PoW-valid genesis block, so startup block reads pass header checks.
- All Avian timestamp upgrades (`x16rt`, `dual_algo`, `assets`, `flight_plans`,
  `ans_v1`) pre-activated, and `ans_v2` always active for testing.

### GUI

#### Asset coin control and dialog improvements

- `assetcontroldialog`: fixed Qt6 signal syntax, added an admin token filter,
  fixed checkbox state handling, and corrected column labels.
- `assetsdialog`: the asset dropdown is pre-filled from the selection context.
- `createassetdialog` and `reissueassetdialog`: insufficient coin-control funds
  now raise a critical message box before the transaction build is attempted.
- `sendassetsentry`: the quantity spinner step now respects the asset's
  decimals.
- Asset transaction construction honours user-preselected inputs
  (`m_allow_other_inputs`) and respects `setAssetsSelected` in
  `CreateReissueAssetTransaction`; coin selection supports asset inputs
  alongside AVN inputs.

Known issues
============

Two lower-severity divergences from 4.2.0 were identified while fixing the
consensus issues above and are **not** addressed in this release, as they
require careful activation handling:

- the assets-active predicate uses the 5.x node's wall-clock time where 4.2.0
  uses the chain-tip block time;
- ANS activation uses BIP9 in 5.x where 4.2.0 uses a timestamp.

Both are tracked for a follow-up release.
