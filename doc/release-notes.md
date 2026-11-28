(note: this is a temporary file, to be added-to by anybody, and moved to
release-notes/ at release time)

Avian Core version *version* is now available from:

  <https://github.com/AvianNetwork/Avian/releases>

This release includes the following changes since the last release:

Notable changes
===============

Asset database
--------------

- A rebuild of the asset database writes the undo records of reissues, as a
  synced node has them. A database rebuilt from nothing had none, so when a
  block with a reissue was reorganized out, the units, IPFS hash, ANS id and
  verifier string the reissue had set stayed in place, and the node could go on
  to judge later transactions of that asset differently from the network.
- A block is not disconnected when its reissue undo record is there and cannot
  be read. The read result was ignored and the block was disconnected without
  it. 4.x fails the disconnect.
- The database records whether the address balances were kept (`-assetindex`).
  A node that had run with `-assetindex=0` and was started with the option on
  again served the balances it had last written, and could not disconnect a
  block that moved an asset it had no balance for, which stops a
  reorganization, and `listaddressesfortag` answered from the tags as they were.
  The balances are now read again from the UTXO set at that start, and the
  addresses of each tag from the tags of each address. This takes no block
  data, so it works on a pruned node. A database written by an earlier release
  carries no record and is taken to match the setting in use, and a run of an
  earlier release does not update the record: if the option was turned off and
  on again under an earlier release, start once with `-reindexassets`. A pruned
  node cannot use that option; there, start once with `-assetindex=0` and then
  with it on.

Proof-of-work cache
-------------------

- `powcache.dat` ends in a checksum, and a file that fails it is ignored. The
  file holds the hash each block header is known by. With a damaged entry the
  block it belongs to no longer matched its own index entry when read from
  disk, so it could not be served, rescanned or reorganized until the file was
  deleted. A file written by an earlier release has no checksum and is still
  read, once; the node writes the new format at shutdown.

Wallet
------

- The wallet does not pay to a Taproot (witness v1) or ML-DSA-44 (witness v2)
  address, or send change to one it is given, while that deployment is not
  active. Such an output has no spending rule yet, so any miner could take it.
  No Avian wallet hands out these addresses on mainnet, where neither is
  scheduled; the check covers one pasted from elsewhere. It applies to the
  send RPCs, `sendall`, `fundrawtransaction`, `walletcreatefundedpsbt` and the
  GUI. A transaction built by hand with `createrawtransaction` is not checked.
- An asset transaction the mempool refuses is reported as refused, with the
  reason, and is not kept. The wallet used to record it and report success, in
  the GUI and in the asset RPCs, and its inputs then counted as spent until the
  transaction was abandoned. 4.x reported the refusal.
- `sendall` leaves asset coins alone when it chooses the coins itself. It
  collected them with the AVN coins, and unless `send_max` happened to drop
  them, the transaction it built was invalid for a wallet holding an asset.
- The asset RPCs that use the wallet, and `distributereward`, wait until the
  wallet has processed the chain tip, as the other wallet RPCs do. Called
  straight after a block they worked from the wallet as it was before that
  block: `listmyassets` showed the earlier balance. Only `consolidateutxos`
  waited.

PSBT
----

Partially signed transactions are new in 5.x, and three places still expected
signatures without `SIGHASH_FORKID`, which every Avian signature carries:

- A PSBT holding a partial signature could not be decoded ("Signature is not a
  valid encoding"), so a multisig could not be signed by passing a PSBT from
  one signer to the next. The reverse now holds: a partial signature without
  `SIGHASH_FORKID`, which no Avian node accepts, is refused when the PSBT is
  decoded.
- A PSBT whose sighash field was set, as the GUI sets it when it signs, was
  refused by the next signer with "Specified sighash value does not match value
  stored in PSBT". The default type is now `ALL|FORKID`, in the GUI and in
  `walletprocesspsbt` and `descriptorprocesspsbt`, and is not written to the
  field.
- A fully signed PSBT was never reported complete: `walletprocesspsbt` answered
  `"complete": false` and gave no `hex`.

A PSBT input is no longer signed with `SIGHASH_SINGLE` when the transaction has
no output at that input's index. For a non-witness input that signature does
not commit to the transaction at all, and could be used to spend any other coin
of the same key. `signrawtransactionwithwallet` already leaves such an input
unsigned. The input stays unsigned and can be signed with another type; a
signature of that kind already in the PSBT is still finalized.

GUI
---

Fixes from a comparison of the asset pages, coin control and the PSBT dialog
with the 4.2.1 wallet:

- Each wallet's Create Assets, Manage Assets and Transfer Assets page has its
  own coin control. They shared one for the whole program, so with two wallets
  open, a custom change address set in one was used by the other without being
  shown there: its AVN change and the owner token it handed back went to that
  address.
- An owner token or a message channel token can be transferred again. The
  amount was filled in as 0.00000001 where the wallet requires 1, in a field
  that cannot be edited.
- A sub-qualifier can be issued from a parent qualifier coin that holds more
  than one. The transaction returned one of them and spent them all, and the
  node refused it. This also affected `issuequalifierasset`.
- Manage Assets offers a restricted asset (`$NAME`) for reissue to the wallet
  that holds the owner token `NAME!`, as 4.x did. It was missing from the list,
  which put the verifier string out of reach. A verifier string typed for one
  asset is dropped when another is selected, and the confirmation shows the
  string a reissue sets.
- A locked asset coin is not spent, in the GUI or by the asset RPCs, and the
  overview, `listmyassets` and `myansnames` leave it out as 4.x does. The asset
  coin control shows it as locked, where it can be unlocked. It was selected
  like any other. In the tree view of that dialog, a coin at an address that
  holds several can be selected again.
- The asset coin control forgets its selection once the transfer is sent. The
  coins it named were spent, and the next transfer failed on them.
- The PSBT dialog says what an asset output does: an issue with its units and
  reissuable flag, a reissue with what it changes, the name record either of
  them sets, the creation of an owner token, a tag added to or removed from an
  address, a freeze. All of these read "Transfers" or were left as an empty
  line. Its warning before broadcasting a transaction signed with
  `ANYONECANPAY` now appears; it looked only at partial signatures, which a
  signed input no longer has.

Credits
=======

Thanks to everyone who directly contributed to this release:

