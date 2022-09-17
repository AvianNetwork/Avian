Avian Core version *4.0.3* is now available from:

  <https://github.com/AvianNetwork/Avian/releases/tag/v4.0.3>

This release includes new checkpoints for safety.

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
the Linux kernel, macOS 10.11+, and Windows Vista and later. 32-bit versions of Windows,
and Windows XP are not supported.

Avian Core should also work on most other Unix-like systems but is not
frequently tested on them.

Avian Core has been tested with macOS 10.14 Mojave, but it is recommended that developers
do not update to Mojave.  There is an incompatibility with Berkeley-db 4.8.30 that causes
the binaries to seg-fault.  There is a workaround, but as of this release users should
not update to Mojave (see build-OSX.md for current status of this issue).  There are no
known issues running the release binaries on Mojave.

Notable changes
==============

This release includes new checkpoints for safety:

- 940202 = 00000000ed69247f7ef177a14e44de41d9c1ba689cb930946ff773ebfe23f64c
- 952399 = 0000000000a11f354eacb65fee963df9818ee8884d8dd926da33921691ec9969

Credits
==============

Thanks to everyone who directly contributed to this release:

- Most importantly - The Avian Community!
- decentralisedcoder
- Craig Donnachie
