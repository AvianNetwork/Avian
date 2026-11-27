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
- `sendall` leaves asset coins alone. It collected them with the AVN coins, and
  the transaction it built was invalid for any wallet holding an asset.

PSBT
----

Partially signed transactions are new in 5.x, and three places still expected
signatures without `SIGHASH_FORKID`, which every Avian signature carries:

- A PSBT holding a partial signature could not be decoded ("Signature is not a
  valid encoding"), so a multisig could not be signed by passing a PSBT from
  one signer to the next.
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
unsigned.

Credits
=======

Thanks to everyone who directly contributed to this release:

