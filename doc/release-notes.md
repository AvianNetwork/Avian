(note: this is a temporary file, to be added-to by anybody, and moved to
release-notes/ at release time)

Avian Core version *version* is now available from:

  <https://github.com/AvianNetwork/Avian/releases>

This release includes the following changes since the last release:

Notable changes
===============

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
  freezes a 5.0.x node already stored stay as they are: `-reindexassets` does
  not replay them, so a node that has processed any needs a full `-reindex`.

Reward snapshots
----------------

A snapshot requested with `requestsnapshot` is now taken when its height is
reached. 5.0.x stored the request and never took the snapshot. The asset
database is written to disk first, so a snapshot holds the balances as of its
own block.

`distributereward` still pays every batch at once when it can. When it cannot,
because the wallet is locked or short of funds, or the node stops part way, the
distribution stays registered and the wallet it was asked of tries again each
time it sees a new block, as 4.x did, until every batch has been sent. A
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

- Reissuing an asset twice before the asset database was next written stored
  an arbitrary block height and hash for it, which `getassetdata` then showed.
  The height and hash the asset was issued at are kept.

No longer recorded
------------------

- Issuing a message channel or a qualifier no longer adds an owner entry with
  an empty name and address to the asset database.

Credits
=======

Thanks to everyone who directly contributed to this release:

P2P
---

- A header or block that forks from the active chain below the maximum
  reorganization depth is refused as before, but the peer that sent it is no
  longer disconnected and discouraged. The refusal depends on this node's own
  tip and peers and says nothing against the peer, which 4.x gave a ban score
  of 1 in 100.

