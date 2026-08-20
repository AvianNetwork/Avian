v5.0.4 Release Notes
====================

Avian Core version v5.0.4 is now available from:

  <https://github.com/AvianNetwork/Avian/releases>

This is a **consensus release** and upgrading is required for all nodes ahead of
a scheduled activation. It fixes an asset-transfer integer-overflow bug that is
present in every prior release. A matching **v4.2.1** release carries the same
fix for the v4.x series; both activate at the same block so the two codebases
enforce the rule together.

Please report bugs using the issue tracker at GitHub:

  <https://github.com/AvianNetwork/Avian/issues>

How to Upgrade
==============

If you are running an older version, shut it down. Wait until it has completely
shut down (which might take a few minutes in some cases), then run the
installer (on Windows) or just copy over `/Applications/Avian-Qt` (on macOS)
or `aviand`/`avian-qt` (on Linux).

Upgrade before the activation block below. No reindex or resync is required.

### Activation

The new consensus rule activates at **mainnet block 5,270,000**. Every node that
validates blocks — pools, exchanges, and wallets alike — must be running
v5.0.4 (or v4.2.1) by then. Nodes still on an earlier version after that height
risk following a divergent chain if a malformed transaction is mined.

Compatibility
=============

Avian Core is supported and tested on operating systems using the
Linux Kernel 3.17+, macOS 13+, and Windows 10+. Avian Core should also
work on most other Unix-like systems but is not as frequently tested on
them. It is not recommended to use Avian Core on unsupported systems.

Notable changes
===============

### Consensus

#### Fix asset transfer amount integer overflow

`CheckTxAssets` accumulated the per-asset input and output totals of a
transaction without bounds-checking the individual amounts. A transfer carrying
an `nAmount` beyond `MAX_MONEY` could wrap the signed 64-bit accumulator so that
the input total appeared to equal the output total, letting the transaction pass
the balance check and create assets from nothing.

The fix rejects any per-transfer amount outside `MoneyRange`, and guards the
running total with a pre-addition check (`nAmount > MAX_MONEY - current`) so the
overflowing addition — undefined behaviour in C++, not merely wraparound — is
never executed. The check is applied to both the input and output sides of the
transaction.

The rule is gated on a fixed activation height (`nAssetTransferOverflowFixHeight`
= mainnet block **5,270,000**) rather than BIP9 signalling. A plain height
comparison enforces identically across the v5.x and v4.2.x codebases, whereas
version-bit signalling would need two separate implementations to agree, and a
timestamp gate would inherit the existing difference where v5.x reads node
wall-clock time and v4.2.0 reads chain-tip block time. The matching **v4.2.1**
release uses the same height so both series begin enforcing at the same block.

New rejection reasons: `bad-txns-asset-input-amount-out-of-range`,
`bad-txns-asset-inputs-amount-overflow`,
`bad-txns-asset-transfer-amount-out-of-range`, and
`bad-txns-asset-outputs-amount-overflow`.

### Chain Parameters

#### Updated assumevalid, minimumchainwork, and chainTxData

The hardcoded chain sync hints, deferred from the v5.0.3 hotfix, have been
updated to block 5,140,000:

- `defaultAssumeValid` — `000000000a095023e60fce2539c16e02832ab19782c87bdae4556c493b77c95b`
- `nMinimumChainWork` — `0000000000000000000000000000000000000000000000004690cd3cda006245`
- `chainTxData` — updated to reflect 5,847,417 total transactions as of
  August 2026

These values improve initial block download performance for new nodes by
skipping script verification for all blocks prior to height 5,140,000.

### Networks

#### testnet4 activation height

The asset transfer overflow check is active from genesis on testnet, testnet4,
signet, and regtest, so the new consensus behaviour can be exercised there
without waiting for a height.

### Tests

Added regression coverage for the overflow fix: unit tests confirm a malformed
transfer is accepted below the activation height (legacy behaviour) and rejected
at or after it, and that a set of individually in-range outputs whose sum
exceeds `MAX_MONEY` is rejected by the running-total guard.
