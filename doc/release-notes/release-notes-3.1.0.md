Avian Core version *3.1.0* is now available from:

  <https://github.com/AvianNetwork/Avian/releases/tag/v3.1.0>

This is a major release containing bug fixes and enhancements for 3.0.  It is highly recommended that users 
upgrade to this version.

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

It is not recommended that users downgrade their version.  Version 3.0.0 and later contain
changes that *will* fork the chain, users not running 3.1.0 (or later) will be not
be able to participate in this fork process and will be left on the old chain which 
will not be valid.

Compatibility
==============

Avian Core is extensively tested on multiple operating systems using
the Linux kernel, macOS 10.11+, and Windows 10 (x64) and later. Windows XP is not supported.

Avian Core should also work on most other Unix-like systems but is not
frequently tested on them.

Avian Core has been tested with macOS 10.14 Mojave, but it is recommended that *developers*
do not update to Mojave.  There is an incompatibility with Berkeley-db 4.8.30 that causes
the binaries to seg-fault.  There is a workaround, but as of this release users should
not update to Mojave (see build-OSX.md for current status of this issue).  There are no
known issues running the release binaries on Mojave.

Notable changes
==============
- Rewrite of block difficulty retargetter algorithm GetNextWorkRequiredLWMA3()
- Rewrite of CheckProofOfWork to include powLimit from multiple algos, if they differ
- Small tweaks here and there to support other changes mentioned
-  RPC command output fixes towards industry standards
- Bumped version number and protocol version
- Checkpoint added for block 275972

Credits
==============

Thanks to everyone who directly contributed to this release:

- Most importantly - The Avian Community!
- Shafil Alam
- decentralisedcoder
