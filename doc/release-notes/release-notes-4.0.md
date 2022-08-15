Avian Core version *4.0.0* is now available from:

  <https://github.com/AvianNetwork/Avian/releases/tag/v4.0.0>

Our latest release "Avian Network v4.0.0 - Thorondor", this has many updates to the look of the wallet as well as Flight Plans and Assets activated in testnet only Please update when you can, this is not a mandatory upgrade but we would advise you update to take advantage of fixes and improvements it offers.

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

- Bring Avian up to date + Assets 
- Add nExtCoinType for BIP44 
- Add Peer Dialog to RPConsole 
- Add Paper Wallet 
- Add "Get My Words" to UI 
- Start work on Avian Flight Plans 
- Depends: Add 32-bit ARM support for QT 
- UI: Add wrapping window 
- CoinGecko API to fetch currency units and price 
- Add importPrivateKey to walletview 

Credits
==============

Thanks to everyone who directly contributed to this release:

- Most importantly - The Avian Community!
- Shafil Alam
- Craig Donnachie
