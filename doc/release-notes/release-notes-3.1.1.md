Avian Core version *3.1.1* is now available from:

  <https://github.com/AvianNetwork/Avian/releases/tag/v3.1.1>

Our latest maintenance release "Avian Network v3.1.1 - Tom Bombadil" that brings backports for security and performance along with tweaks here and there in the GUI is ready for release. Please can you all update when you can, this is not a mandatory upgrade but we would advise you update to take advantage of fixes and improvements it offers.

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

It is not recommended that users downgrade their version.  Version 3.1.0 and later contain
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
- GUI: Add out-of-the-box browsers. Can be changed and removed in options by @fdoving in #30
- set the correct url to the Avian webpage. by @fdoving in #28
- gui: encrypt wallet dialog, update to AVN by @fdoving in #33
- [backport] gui, wallet: random abort (segmentation fault) btc#9683 by @fdoving in #14
- build: Suppress -Wdeprecated-copy warnings by @fdoving in #15
- Drop buggy TableViewLastColumnResizingFixer class (btc#204) by @fdoving in #16
- Introduce a Shuffle for FastRandomContext and use it in wallet by @fdoving in #17
- Backport: Remove useless mapRequest tracking by @fdoving in #19
- depends: update zeromq to 4.3.4 by @fdoving in #20
- lockedpool: avoid sensitive data in core files (Linux and FreeBSD) by @fdoving in #21
- backport: wallet: Fix Char as Bool in Wallet by @fdoving in #23
- backport: gui: Set CConnman byte counters earlier to avoid uninitialized reads by @fdoving in #24
- backport: rpc: Fix data race (UB) in InterruptRPC() by @fdoving in #25
- backport: fix uninitialized read when stringifying an addrLocal by @fdoving in #26
- backport: Introduce a maximum size for locators. by @fdoving in #27
- depends: update expat to 2.4.1 by @fdoving in #31
- depends: libevent update by @fdoving in #32
- Contrib: Copy install_db4.sh from bitcoin by @fdoving in #37
- http: Release work queue after event base finish (btc#19033) by @fdoving in #38
- disable output to stdout by @fdoving in #36
- disable logging of diff by @fdoving in #39
- gui: set black text for price and increase to 30s update times. by @fdoving in #41
- Revert "gui: set black text for price and increase to 30s update times." by @alamshafil in #43
- gui: set black text for price and increase to 30s update times. by @fdoving in #42
- gui: text color for price info fixed for dark and light mode. by @fdoving in #44
- backport: [qt] Fix potential memory leak in newPossibleKey(ChangeCWallet *wallet) (bitcoin #10920) by @fdoving in #45
- translations: search and replace Raven/Avian by @fdoving in #49
- GUI: Enable High DPI Scaling by @fdoving in #50

Credits
==============

Thanks to everyone who directly contributed to this release:

- Most importantly - The Avian Community!
- Shafil Alam
- decentralisedcoder
- fdov
