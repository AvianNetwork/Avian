(note: this is a temporary file, to be added-to by anybody, and moved to
release-notes at release time)

Avian Core version *version* is now available from:

  <https://avn.network/bin/avian-core-*version*/>

This is a new major version release, including new features, various bugfixes
and performance improvements, as well as updated translations.

Please report bugs using the issue tracker at GitHub:

  <https://github.com/AvianNetwork/Avian/issues>

To receive security and update notifications, please subscribe to:

  <https://avn.network/en/list/announcements/join/>

How to Upgrade
==============

If you are running an older version, shut it down. Wait until it has completely
shut down (which might take a few minutes for older versions), then run the
installer (on Windows) or just copy over `/Applications/Avian-Qt` (on Mac)
or `aviand`/`avian-qt` (on Linux).

The first time you run version 0.15.0, your chainstate database will be converted to a
new format, which will take anywhere from a few minutes to half an hour,
depending on the speed of your machine.

Note that the block database format also changed in version 0.8.0 and there is no
automatic upgrade code from before version 0.8 to version 0.15.0. Upgrading
directly from 0.7.x and earlier without redownloading the blockchain is not supported.
However, as usual, old wallet versions are still supported.

Downgrading warning
-------------------

The chainstate database for this release is not compatible with previous
releases, so if you run 0.15 and then decide to switch back to any
older version, you will need to run the old release with the `-reindex-chainstate`
option to rebuild the chainstate data structures in the old format.

If your node has pruning enabled, this will entail re-downloading and
processing the entire blockchain.

Compatibility
==============

Avian Core is extensively tested on multiple operating systems using
the Linux kernel, macOS 10.8+, and Windows Vista and later. Windows XP is not supported.

Avian Core should also work on most other Unix-like systems but is not
frequently tested on them.

Notable changes
===============

Miner block size limiting deprecated
------------------------------------

Though blockmaxweight has been preferred for limiting the size of blocks returned by
getblocktemplate since 0.13.0, blockmaxsize remained as an option for those who wished
to limit their block size directly. Using this option resulted in a few UI issues as
well as non-optimal fee selection and ever-so-slightly worse performance, and has thus
now been deprecated. Further, the blockmaxsize option is now used only to calculate an
implied blockmaxweight, instead of limiting block size directly. Any miners who wish
to limit their blocks by size, instead of by weight, will have to do so manually by
removing transactions from their block template directly.

HD-wallets by default
---------------------
Due to a backward-incompatible change in the wallet database, wallets created
with version 0.16.0 will be rejected by previous versions. Also, version 0.16.0
will only create hierarchical deterministic (HD) wallets.

Low-level RPC changes
----------------------
- The "currentblocksize" value in getmininginfo has been removed.
- The deprecated RPC `getinfo` was removed. It is recommended that the more specific RPCs are used:
  * `getblockchaininfo`
  * `getnetworkinfo`
  * `getwalletinfo`
  * `getmininginfo`

- `dumpwallet` no longer allows overwriting files. This is a security measure
  as well as prevents dangerous user mistakes.

Credits
=======

Thanks to everyone who directly contributed to this release:


As well as everyone that helped translating on [Transifex](https://www.transifex.com/projects/p/avian/).
