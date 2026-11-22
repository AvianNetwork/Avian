v5.0.5 Release Notes
====================

Avian Core version v5.0.5 is now available from:

  <https://github.com/AvianNetwork/Avian/releases>

This is a **consensus release**. It makes the 5.x series enforce the asset
rules that 4.x enforces and 5.0.0 to 5.0.4 did not, so that the two series
accept and refuse the same blocks. It also carries the asset transfer overflow
fix of v5.0.4, which has been in force since mainnet block 5,270,000. v5.0.4
was tagged without a binary release, so this is the first release of the 5.x
series that has the fix.

Every node that validates blocks should upgrade, and pools first. A pool on
5.0.0 to 5.0.4 accepts transactions that these rules refuse, and a block it
mines with one is rejected by 4.x nodes and by this version. For the 4.x
series the overflow fix is in v4.2.1. v4.2.0 and earlier do not have it.

Please report bugs using the issue tracker at GitHub:

  <https://github.com/AvianNetwork/Avian/issues>

How to Upgrade
==============

If you are running an older version, shut it down. Wait until it has completely
shut down (which might take a few minutes in some cases), then run the
installer (on Windows) or just copy over `/Applications/Avian-Qt` (on macOS)
or `aviand`/`avian-qt` (on Linux).

No reindex or resync is required. At the first start with this version the
asset database is rebuilt from the blocks on disk, which on mainnet takes a
minute or two with a fast disk. The node does not answer RPC calls until that
is done. See "Asset database" below.

### Activation

The restored asset rules have no activation height: they apply to every block.
The whole mainnet chain up to block 5,411,445 was validated under them and no
block was refused, so a node that upgrades stays on the chain it is on.

The overflow rule of v5.0.4 keeps its height, **mainnet block 5,270,000**, which
has passed.

Compatibility
=============

Avian Core is supported and tested on operating systems using the
Linux Kernel 3.17+, macOS 13+, and Windows 10+. Avian Core should also
work on most other Unix-like systems but is not as frequently tested on
them. It is not recommended to use Avian Core on unsupported systems.

Notable changes
===============

Asset transfer overflow fix
---------------------------

The fix that was tagged as v5.0.4 is part of this release. A transfer amount
outside the money range, or a set of transfers whose total overflows, is
refused from mainnet block 5,270,000. `release-notes-5.0.4.md` has the details.
v4.2.1 enforces the same rule from the same height.

Asset consensus rules restored from 4.x
---------------------------------------

5.0.x did not enforce the asset rules that 4.x had in `CheckTransaction()`, so a
5.0.x node accepted transactions and blocks that a 4.x node rejects. They are
enforced again, for the mempool and for every block:

- a coinbase carries no asset outputs
- asset outputs carry 0 AVN
- a unique asset transfer is exactly 1, a qualifier transfer 1 to 10
- one tag change per asset and address, one global change per asset, one
  verifier string per transaction
- a tag or a freeze travels with the transfer of the owner token, or of the
  qualifier, that authorises it
- adding a qualifier tag pays the burn fee
- the owner token of an issuance has the name and the address of its asset
- a restricted issuance carries a verifier string, and a verifier string is
  only found with a restricted issuance or reissuance

Two related rules are back as well:

- A script that begins with `OP_AVN_ASSET` (tags, freezes, verifier strings),
  or an asset script with an amount of zero, is unspendable and is not put in
  the UTXO set. A node upgraded in place keeps the entries it already stored
  for such outputs. A spend of one is refused as a missing input, which is what
  a 4.x node and a freshly synced node answer, so no rebuild is needed to stay
  on the same chain. `gettxoutsetinfo` will differ from a freshly synced node
  until the chainstate is rebuilt with `-reindex-chainstate`. The wallet no
  longer offers such an output as an asset coin.
- A qualifier tag or an address freeze is recorded against the address it
  names. 5.0.x recorded every one against an empty address. The tags and
  freezes a 5.0.x node already stored are put right at the first start with
  this version: see "Asset database" below.

Reward snapshots
----------------

A snapshot requested with `requestsnapshot` is now taken when its height is
reached. 5.0.x stored the request and never took the snapshot. The asset
database is written to disk first, so a snapshot holds the balances as of its
own block.

`distributereward` still pays every batch at once when it can. When it cannot,
because the wallet is locked or short of funds, or the node stops part way, the
distribution stays registered and the wallet it was asked of tries again each
time it sees a new block, as 4.x did, until every batch has been sent. The
batches sent later use the change address `distributereward` was given. A
distribution registered by an earlier 5.0.x is not picked up this way.

- `canceldistribution` is new, and withdraws a registered distribution so that
  nothing more is sent for it. 4.x had no way to do that. The batches already
  sent are listed and remembered: asking for the same distribution again sends
  only the batches that never went out.
- A batch is marked in the wallet with its distribution and number, in the
  same write as the transaction. A batch that went out just before the node
  stopped is found by that mark and is not sent a second time.
- A distribution stops, with status 6, when a batch it sent was abandoned or
  has been displaced by a conflicting transaction. Such a batch has paid
  nobody, and used to be counted as sent.
- A holder whose address is not valid is named in the log when it is left out
  of an AVN distribution. It used to be left out silently.

RPC
---

- Outputs of type `restricted_asset_data` have an `asset_data` object again in
  `decodescript`, `decoderawtransaction`, `getrawtransaction` and the other
  calls that describe a script, with the fields 4.x gave it. Tag and freeze
  outputs also report the `address` they apply to.
- `fundrawtransaction` and `bumpfee` keep an output whose script its address
  cannot reproduce. `fundrawtransaction` used to replace an asset transfer with
  a plain payment to its holder.
- `avian-cli -named` now converts four asset arguments it knew by the wrong
  name and so passed as strings, which the server refused: `onlytotal` of
  `listaddressesbyasset` and `listassetbalancesbyaddress`, and `new_units` of
  `reissue` and `reissuerestrictedasset`.
- `createrawtransaction` and `createpsbt` build the asset operations 4.x had,
  where 5.0.x built `transfer` alone: `transferwithmessage`, `issue`,
  `issue_unique`, `reissue`, `issue_restricted`, `reissue_restricted`,
  `issue_qualifier`, `tag_addresses`, `untag_addresses`, `freeze_addresses`,
  `unfreeze_addresses`, `freeze_asset` and `unfreeze_asset`. The help lists the
  data each takes. Four things differ from 4.x, each where 4.x was wrong:
  - `transferwithmessage` builds its output. 4.x accepted it and built nothing.
  - `issue_unique` takes `ipfs_hashes` and `ans_ids` each on its own. 4.x used
    the hashes only when both were given.
  - `reissue` and `reissue_restricted` leave the units as they are unless
    `new_units` is given. 4.x always sent 0, which only an asset of 0 units
    accepts.
  - `reissue_restricted` adds a verifier output only when a verifier string is
    given. 4.x added an empty output otherwise.
  A `transfer` may name several assets again, one output each.
- `-zmqpubrawmessage=<address>` is back: every asset message in a connected
  block is published on the `rawmessage` topic as the JSON 4.x sent. See
  `doc/zmq.md`.
- A message is stored with the height of its block, where 5.0.x stored 0.
- The help of `-assetindex` gives its real default, which is on.

Messages
--------

- A message is marked `ORPHAN` when the block that carried it is disconnected,
  as in 4.x. 5.0.x left it as it was.

Testnet and regtest
-------------------

- Two burn addresses of the testnet and regtest set had a bad checksum, so a
  qualifier tag could not be added and a message channel could not be issued
  there. They are the addresses 4.x used again.
- The burn output of a reissue was checked against the mainnet address on
  every network, so no reissue was valid on testnet or regtest.

Neither changes anything on mainnet.

GUI
---

- The wallet window stays usable while the chain is catching up with large
  wallets open. Each wallet refreshed its balances and the confirmations of
  every transaction four times a second, taking the validation lock once for
  each transaction, which left the window no time to respond and slowed the
  sync itself. While the chain catches up the balances are now refreshed every
  ten seconds and the confirmations when it has caught up, and the lock is
  taken once a block.

Asset database
--------------

- The asset database now records a version. A database written by 5.0.x has
  none, and is rebuilt from the blocks on disk at the first start with this
  version, which is what moves the tags and freezes 5.0.x stored under an
  empty address to the addresses they name. The rebuild is automatic and needs
  no `-reindex`. The node does not answer RPC calls until it is done, which on
  mainnet takes a minute or two with a fast disk.
- That rebuild, which is also what `-reindexassets` runs and what a node does
  by itself when the asset database and the chain state disagree after an
  unclean shutdown, now restores everything a full `-reindex` does. In 5.0.x
  it lost every qualifier tag, address freeze and global freeze, and a
  verifier string changed by a reissue went back to the one the asset was
  issued with. It also left each asset credited to the address it was issued
  to, in `listaddressesbyasset` and in reward snapshots, when that address had
  since sent all of it away.
- A rebuild that is interrupted is started again at the next start. 5.0.x
  could take a partly rebuilt database for a finished one.
- A rebuild keeps the undo data of reissues, so a reorganization across a
  reissue after one still restores the asset as it was.
- Reissuing an asset twice before the asset database was next written stored
  an arbitrary block height and hash for it, which `getassetdata` then showed.
  The height and hash the asset was issued at are kept.

P2P
---

- A header or block that forks from the active chain below the maximum
  reorganization depth is refused as before, but the peer that sent it is no
  longer disconnected and discouraged. The refusal depends on this node's own
  tip and peers and says nothing against the peer, which 4.x gave a ban score
  of 1 in 100.

No longer recorded
------------------

- Issuing a message channel or a qualifier no longer adds an owner entry with
  an empty name and address to the asset database.

Stratum v2 Template Provider
----------------------------

`aviand` can now serve the Stratum v2 Template Distribution Protocol, so a pool
can take templates over an encrypted Stratum v2 connection instead of polling
`getblocktemplate`. It acts as a Template Provider only: block construction,
transaction selection and block submission stay in the node, while worker
management, share accounting and payouts stay in a separate pool process. The
existing JSON-RPC mining interface is untouched, and a node started without
`-sv2` behaves exactly as before.

Avian's two proof-of-work algorithms are handled by a small required protocol
extension. A client selects X16RT or MinotaurX for the lifetime of a connection,
and the node tells it the exact `ntime` window the work is valid for, which
matters because X16RT rederives its hash chain every 128 seconds and stock
Stratum v2 has no upper bound on `ntime`. Version rolling is not offered in this
version, because Avian keeps the algorithm identifier in `nVersion` bits 16-23
where BIP320's mask would otherwise let a miner change it.

The same extension also carries the reply the Template Distribution Protocol
does not define. Its message table ends at `SubmitSolution`, so a pool that
submitted a block learned nothing: not that the node took it, not why it did
not, and not even that it was heard. A node now answers every submission it
can, either with the block hash validation accepted or with a named reason it
was refused, so an operator debugging a pool reads the reason instead of
inferring it from a tip that never moved.

The extension uses identifier `0x4001`, allocated in Avian's own extension
registry rather than Stratum v2's: an `extension_type` need only be unique among
peers that can share a connection, and an Avian connection and a Bitcoin one
never do. `doc/sv2.md` is that registry, and also has the full configuration
surface, the extension's wire format and the security considerations, the most
important of which is that exposing the Stratum v2 port is equivalent to
exposing an RPC port. It binds to loopback by default.

Mining interface (IPC)
----------------------

- A template request made through the multiprocess Mining interface now
  carries its proof-of-work algorithm and its coinbase output script. Both were
  dropped, so a client of `avian-node` got an X16RT template that paid the
  default script, whatever it had asked for.

Post-quantum signatures (RIP-25)
--------------------------------

ML-DSA-44 (witness v2) is not deployed on mainnet and this release does not
schedule it. The work since v5.0.4 prepares it:

- `doc/rip-25.md` specifies the rules, and the test suite has known-answer and
  consensus vectors for them.
- A key is derived from its seed by a fixed, versioned algorithm. Post-quantum
  keys and addresses made on a test network by an earlier version are not
  carried forward.
- A signature is bound to the network it was made for, and a post-quantum
  spend counts towards the signature operation limits.
- The wallet can spend post-quantum coins, and keeps the change of a payment
  to a post-quantum address in a post-quantum output.
- Building with post-quantum support, which is the default, requires liboqs.
  Configure fails when it is missing, where it used to turn the support off
  with a warning. `-DWITH_LIBOQS=OFF` builds without it, and such a node
  refuses to start on a network where the deployment can activate.

Tests and CI
------------

- The unit test suite is localized to Avian and runs in CI, with the lint
  suite and a fuzz build with a smoke run.
