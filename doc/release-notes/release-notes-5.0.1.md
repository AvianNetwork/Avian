v5.0.1 Release Notes
====================

Avian Core version v5.0.1 is now available from:

  <https://github.com/AvianNetwork/Avian/releases>

This is a bug-fix release for v5.0.0.

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

### Bug Fixes

#### Reject bumpfee on transactions containing asset outputs

Transactions with asset outputs (`TRANSFER_ASSET`, `NEW_ASSET`,
`REISSUE_ASSET`) can no longer be fee-bumped via `bumpfee` or `psbtbumpfee`.
Previously, the fee bumping process would silently strip asset data from
outputs, resulting in loss of assets. If the transaction has not yet confirmed,
`abandontransaction` may be used to mark it as abandoned before rebroadcasting
with a higher fee - note that `abandontransaction` only works if the
transaction is not in the mempool and has not been confirmed.

#### Allow AVN payment and asset transfer to the same address in one transaction

`sendrawtransaction`/`createrawtransaction` no longer incorrectly rejects
outputs when an AVN payment and an asset transfer are sent to the same address
in a single transaction. Previously this raised a false "duplicate destination"
error or silently read the wrong output value.
