Avian Core version *4.0.1* is now available from:

  <https://github.com/AvianNetwork/Avian/releases/tag/v4.0.1>

Our latest maintenance release "Avian Network v4.0.1 - Thorondor Update". Please update when you can, this is not a mandatory upgrade but we would advise you update to take advantage of fixes and improvements it offers.

Please report bugs using the issue tracker at GitHub:

  <https://github.com/AvianNetwork/Avian/issues>

How to Upgrade
==============

If you are running an older version, shut it down. Wait until it has completely
shut down (which might take a few minutes for older versions), then run the 
installer (on Windows) or just copy over `/Applications/Avian-Qt` (on Mac)
or `aviand`/`avian-qt` (on Linux).

Downgrading warning
==============

It is not recommended that users downgrade their version.

Compatibility
==============

Avian Core is extensively tested on multiple operating systems using
the Linux kernel, macOS 10.11+, and Windows Vista and later. Windows XP is not supported.

Avian Core should also work on most other Unix-like systems but is not
frequently tested on them.

Notable changes
==============

- Correct bugs found in 4.0.0 by @cdonnachie in #68
- Bump version to 4.0.1
- Disable messaging check in Mainnet
- Speed up indexing
- Add new checkpoints
- Update nMinimumChainWork and defaultAssumeValid

Credits
==============

Thanks to everyone who directly contributed to this release:

- Craig Donnachie
