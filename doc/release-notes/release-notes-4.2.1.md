# Avian Core 4.2.1 Release Notes

## Overview

Avian Core 4.2.1 is a **consensus release** for the 4.2 series. It fixes an
asset-transfer integer-overflow bug present in every prior release, and is
required for all nodes ahead of a scheduled activation.

A matching **v5.0.4** release carries the same fix for the 5.x series. Both
activate at the same mainnet block, so the two codebases begin enforcing the
rule together and stay on one chain.

---

## Upgrading

Shut down the running node, wait for it to stop completely, then install this
version over it. No reindex or resync is required.

**Upgrade before the activation block below.** Every node that validates blocks
— pools, exchanges, and wallets — must be running 4.2.1 (or v5.0.4) by then.
Nodes still on an earlier version after that height risk following a divergent
chain if a malformed transaction is mined.

### Activation

The new consensus rule activates at **mainnet block 5,270,000**.

---

## Significant Changes

### Consensus: fix asset transfer amount integer overflow

`CheckTxAssets` accumulated the per-asset input and output totals of a
transaction without bounds-checking the individual amounts. A transfer carrying
an `nAmount` beyond `MAX_MONEY` could wrap the signed 64-bit accumulator so that
the input total appeared to equal the output total, letting the transaction pass
the balance check and create assets from nothing. This code path is present in
4.2.0 — the version most of the network runs — so the fix belongs here, not
only in 5.x.

The fix rejects any per-transfer amount outside `MoneyRange`, and guards the
running total with a pre-addition check (`nAmount > MAX_MONEY - current`) so the
overflowing addition — undefined behaviour in C++, not merely wraparound — is
never executed. It is applied to both the input and output sides.

The rule is gated on a fixed activation height (`nAssetTransferOverflowFixHeight`
= mainnet block **5,270,000**), identical to the height used by v5.0.4. A plain
height comparison enforces the same rule across both codebases, whereas
version-bit signalling would need two separate implementations to agree and may
never reach threshold while most hashpower is on 4.2.0, and a timestamp gate
would inherit the difference where 5.x reads node wall-clock time and 4.2.0
reads chain-tip block time.

New rejection reasons: `bad-txns-asset-input-amount-out-of-range`,
`bad-txns-asset-inputs-amount-overflow`,
`bad-txns-asset-transfer-amount-out-of-range`, and
`bad-txns-asset-outputs-amount-overflow`.

Regression tests confirm a malformed transfer is accepted below the activation
height (legacy behaviour) and rejected at or after it, and that a set of
individually in-range outputs whose sum exceeds `MAX_MONEY` is rejected by the
running-total guard.

### Build fixes

- **depends**: the zlib source URL now points at the permanent GitHub release
  (zlib.net only hosts the current version, so `zlib-1.3.1.tar.gz` began
  returning 404 and broke from-scratch builds). The tarball is byte-identical,
  so the recorded checksum is unchanged. `FALLBACK_DOWNLOAD_PATH` is now set, so
  a future dead source URL falls back to a mirror instead of failing outright.
- **build**: added a missing `<deque>` include so the test suite compiles under
  GCC 13 and newer toolchains.

---

## Compatibility

Avian Core is supported and tested on Linux (kernel 3.17+), macOS 13+, and
Windows 10+. It should also work on most other Unix-like systems but is less
frequently tested there.
