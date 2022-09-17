Avian Core version *4.1.0* is now available from:

  <https://github.com/AvianNetwork/Avian/releases/tag/v4.1.0>


Avian 4.1.0 includes the block reward dev fee, important security updates, improved network efficiency, faster loading speeds, and other new features.

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

It is not recommended that users downgrade their version. Version 4.1.0 and later contain
changes that *will* fork the chain, users not running 4.1.0 (or later) will be not
be able to participate in this fork process and will be left on the old chain which 
will not be valid.

Compatibility
==============

Avian Core is extensively tested on multiple operating systems using
the Linux kernel, macOS 11+, and Windows 10 and later. 32-bit versions of Windows,
and Windows XP are not supported.

Avian Core should also work on most other Unix-like systems but is not
frequently tested on them.

Notable changes
==============

Security Updates
----------------

### Peer management 
Avian will now tightly manage memory usage during high network traffic or when connected to slow peers.

### Avian Assets (not on mainnet)
- Avian will enforce asset coinbase checks and enforced values.
- Fix bug with in memory qualifier address checking

### Avian Flight Plans (testnet)
- Avian Flight Plans (active on testnet) now uses the secure POSIX `mkstemp` instead of the risky `tmpnam`.
- AFP will not include all lua libraries for security purposes
- Removed buggy and risky web3lib

### Bitcoin backports
This update includes many security backports:
- depends: update zeromq to 4.3.4 (#20)
- depends: update expat to 2.4.1 (#31)
- depends: libevent update (#32)
- lockedpool: avoid sensitive data in core files (Linux and FreeBSD) (#21)
- backport: gui, wallet: random abort (segmentation fault) btc#9683 (#14)
- backport: Remove useless mapRequest tracking (#19)
- backport: wallet: Fix Char as Bool in Wallet (#23)
- backport: gui: Set CConnman byte counters earlier to avoid uninitialized reads (#24)
- backport: rpc: Fix data race (UB) in InterruptRPC() (#25)
- backport: fix uninitialized read when stringifying an addrLocal (#26)
- backport: Introduce a maximum size for locators. (#27)
- http: Release work queue after event base finish (btc#19033) (#38)
- qt: Fix potential memory leak in newPossibleKey(ChangeCWallet *wallet) (#45)

Features
--------

### Block reward fee
This update inclues a 5% dev fee that was voted on and approved by the community.

### PoW cache
Avian will now cache proof-of-work hashes into a file called `powcache.dat` for improved wallet loading speed and faster syncing.

Before v4.1.0: 526.5 seconds / 8 minutes

With v4.1.0: 30.61 seconds (second run)

~ 94.1861% speed up in wallet loading time.

Inital sync (IBD) done around ~1hr 48 min using 8GB of bandwidth.

### Add pruning setting to the UI
This new setting allows a user to decrease the total disk space used down to ~1GB, by removing blocks that are no longer needed.

Features that need the entire blockchain, such as rescanning, or importing keys, will **not** function.

Reverting this setting will make the wallet **re-download** the whole blockchain.

### Wallet repair tab
This updates includes a new wallet repair tab in Help -> Debug Window. This tab allows you to rescan the blockchain, recover transactions, upgrade wallet.dat, and reindex without having to use the command line. Backport from Dash PR#342 with changes made for Avian.

### New RPC to get stuck transactions
A new RPC method has been added to help operators identify if any transactions are stuck (have not been mined and are not in the mempool).

This method is called: `liststucktransactions`

### Avian Name System (ANS) (testnet)
This update includes the basis for the Avian Name System based on the draft AIP-04. This feature will **only** be active for testing on testnet and regtest.

### Working regtest
This update fixes the regtest for Avian, allowing people to locally run a blockchain for testing purposes.

Minor Changes
-------------
- Print CreateNewBlock() when BENCH debug is set (#171)
- qi: Create CodeQL action to analyze code
- qt: Add direction to peers tab
- Qt: Add "Copy address" to context menu in peers tab
- Qt: Elide long strings in their middle in peers tab
- Qt: Use correct AVN browser URLs
- afp: Add help message for "flightplans" option
- afp: Remove use of marco to get path
- afp: Add list_flightplans RPC method
- afp: Improve RPC method list_flightplans
- Introduce proper AreMessagesDeployed() and AreRestrictedAssetsDeployed()
- qt: Remove unused dev action
- qt: Remove unused pool picker
- qt: Shrink size of tabs by reducing padding
- qt: Fix QComboBox bug
- qt: Restore Restricted Assets
- qt: New icon for restrictedAssetAction
- Add backward compatibility with ReadAddressUnspentIndex
- Disallow the creation of "RVN" asset
- afp: Do not include all lua libraries
- afp+rpc: Rename "call_function" to "call_flightplan"
- Restore FORKID to VerifyScript
- Fix nCoinCacheUsage
- Fix ComputeBlockVersion
- rpc: Restore FORKID to createrawtransaction
- qt: Make RPC console buttons look clickable
- qt: Add dark mode console
- qt: Rework Open URI dialog
- qt: Add rounded corners
- qt: Restore action menu icons
- backport: [qt] Add version to modal overlay
- backport: [qt] macOS darkmode
- backport: [qt] Reissue asset spinboxes; disable if unit is max.
- backport: [qt] Update asset list when qcombobox is shown.
- backport: [qt] Get my words, re-lock wallet if not bip44.
- backport: [qt] Get my words, auto close.
- backport: [qt] Create and reissue asset views add browse IPFS button
- Fix ToolBarSelectedTextColor for dark mode
- validation: Do not check PoW when downloading headers during IBD
- trival: Remove unused IsUAHFForAssetsenabled
- trival: Rename ambiguous UAHF to state FORKID
- ans: Add basic Avian Name System ID
- chain: Use correct BIP44 cointype for mainnet (921)
- qt: Fix menubar styling
- qt: Fix assets frame on overview page
- qt: Fix overview page
- qt: Add QScrollBar styling
- qt: Check wallet model to prevent segfault
- contrib: Fix optimize-pngs.py

Credits
==============

Thanks to everyone who directly contributed to this release:

- Most importantly - The Avian Community!
- Craig Donnachie
- decentralisedcoder 
- Shafil Alam
- fdov
- Furcalor
