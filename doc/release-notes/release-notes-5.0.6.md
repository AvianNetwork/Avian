v5.0.6 Release Notes
====================

Avian Core version v5.0.6 is now available from:

  <https://github.com/AvianNetwork/Avian/releases>

This is a **consensus release**. It closes the remaining places where a 5.x
node accepted or refused a block differently from the 4.x series: how asset
scripts are parsed and classified, how a burn output is recognised, how
witness v1 outputs are validated, and how the mempool and a reorganization
track asset state. It follows v5.0.5, which restored the asset rules in
`CheckTransaction()`, and completes that work.

Every node that validates blocks should upgrade, and pools first. A pool on an
earlier 5.x release can build a block that the 4.x majority rejects, or reject
a block the majority accepts, and in either case forks off the chain.

Please report bugs using the issue tracker at GitHub:

  <https://github.com/AvianNetwork/Avian/issues>

How to Upgrade
==============

If you are running an older version, shut it down. Wait until it has completely
shut down (which might take a few minutes in some cases), then run the
installer (on Windows) or just copy over `/Applications/Avian-Qt` (on macOS)
or `aviand`/`avian-qt` (on Linux).

No reindex or resync is required, with one exception below. Upgrading from
v5.0.5 does not rebuild the asset database, because its on-disk format is
unchanged. Upgrading from v5.0.0 to v5.0.4 rebuilds it once at the first start,
as v5.0.5 did; see the v5.0.5 notes. The node does not answer RPC calls while
a rebuild runs.

The exception is a pruned node coming from v5.0.0 to v5.0.4. The rebuild reads
every block, and a pruned node has deleted some, so it stops at start and says
so. It has to download the chain again with `-reindex`.

### Activation

The restored parse and classification rules have no activation height: they
apply to every block. The whole mainnet chain up to block 5,425,627 was
validated under this release and no block was refused, so a node that upgrades
stays on the chain it is on.

Gating the Taproot script flag on its (never active) deployment is strictly
more permissive for witness v1 outputs, so it refuses nothing in the history.

The post-quantum (RIP-25) changes below affect only the test networks, where
the deployment is active. They change nothing on mainnet, where it is not
scheduled.

Compatibility
=============

Avian Core is supported and tested on operating systems using the
Linux Kernel 3.17+, macOS 13+, and Windows 10+. Avian Core should also
work on most other Unix-like systems but is not as frequently tested on
them. It is not recommended to use Avian Core on unsupported systems.

Notable changes
===============

Asset script parsing and classification parity with 4.x
-------------------------------------------------------

Three script-layer divergences from 4.x are closed. Each let a 5.x node read a
crafted asset output differently from 4.x, which changes which blocks the two
accept:

- `OP_AVN_ASSET` now ends script parsing. 4.x treats everything after the asset
  marker as that opcode's data; 5.x kept executing the bytes after it as
  opcodes, which changed the execution result, the sigop count and the
  signature hash for a script with a non-canonical tail. The canonical asset
  output, which ends in `OP_DROP`, is unaffected.
- The optional IPFS or TXID hash is read exactly as 4.x reads it: consumed
  whenever the field is present, whatever its type byte, with the type byte
  only deciding whether the `Qm` marker is prepended. 5.x gated the read on the
  type byte and so consumed a different number of bytes for the same new-asset,
  transfer or reissue payload.
- A reissue payload truncated before its ANS id is rejected, as 4.x rejects it.
  5.x read that field only when bytes remained, so it minted from an output the
  4.x majority refuses. 5.x always writes the field, so valid scripts still
  carry it.

Two classification rules that 4.x does not have are gated behind the ANS v2
deployment, which is not scheduled, instead of being matched unconditionally:
the 3-byte `OP_PUSHDATA2` asset layout, and the ANS v2 PROFILE identifier.
5.x matched both with no gate, so it classified as an asset, or rejected, an
output that no 4.x node sees the same way.

The burn output of an issuance, a reissue or a tag is found in every script
4.x finds it in. 4.x reads the burn address from a pay-to-pubkey-hash script
whatever opcode pushes the hash, and from a push-only `OP_AVN_ASSET` script of
23 bytes or more. 5.x read it from the exact 25-byte script only, and refused
the transaction, and the block, when a burn was written another way.

Taproot deployment honoured
---------------------------

`GetBlockScriptFlags` set `SCRIPT_VERIFY_TAPROOT` on every block, although the
Taproot deployment is never active on any Avian network. A witness v1 output
was therefore validated under BIP341/342 on 5.x, while 4.x treats it as an
unknown upgrade that anyone can spend. A 5.x node would reject a witness v1
spend the 4.x majority accepts and fork off. The flag is now gated on the
deployment, like DERSIG, CLTV and CSV.

Mempool and reorg asset consistency
-----------------------------------

The mempool and the reorganization paths now track asset state as 4.x does:

- The mempool's asset-conflict maps are populated on accept. Before, they were
  declared and cleared but never filled, so a block could not evict a
  transaction it invalidated through a verifier, qualifier or freeze change, a
  chained reissue could enter the mempool, and two transactions adding the same
  tag or globally freezing the same asset could both sit unconfirmed although
  no block can hold both. These are now refused at mempool accept and evicted
  when a block invalidates them, together with the transactions that spend
  them.
- The rule of one unconfirmed reissue per asset applies to the mempool only.
  4.x also applies it when it validates a block, so a 4.x node that holds one
  reissue of an asset refuses a block that confirms a different one. A 5.x
  node never refuses a block for what its own mempool holds. Nor is the rule
  read back from disk at start, which 4.x does: it is set again as the mempool
  loads, so a transaction that does not come back leaves nothing behind.
- A reissue of a restricted asset that carries a verifier string is recorded
  as changing it even when the string is the same, as in 4.x. With two
  reissues of one asset in a block, 5.x could otherwise end on a different
  verifier string from 4.x.
- Block-template validation runs the contextual asset checks. `TestBlockValidity`
  had no asset cache, so an asset-invalid template was caught only later, after
  the miner had worked on it.
- A reissue undo entry is recorded only when the reissue changed something, and
  it keeps the previous ANS id. Before, a no-op reissue restored the wrong
  value on a reorg across two reissues of one asset, and a reorged-out ANS
  change was not undone.
- A mempool transaction that fails the asset rules when a block template is
  checked is removed from the mempool, with the transactions that spend it,
  and the template is built again. Before, every template failed until the
  transaction expired, up to 336 hours later, and the node could mine nothing.
  4.x has the same weakness.
- The names of a unique-asset issuance are tracked in the mempool like every
  other issued name. A second transaction that issues one of them is refused,
  and the transaction is evicted when a block issues one first. As for every
  other kind of issuance, this means an unconfirmed unique-asset issuance
  cannot be replaced by a higher-fee version of itself. A child transaction
  can still pay for one that is already in the mempool.
- A package of transactions is not evaluated when one of them carries an
  asset output, a tag, a freeze or a verifier string. The asset checks of
  mempool acceptance read state that is recorded as each transaction is
  added, so inside a package two issuances of one name, or two reissues of one
  asset, passed together. Asset transactions are accepted one at a time, as on
  4.x, which has no package acceptance. `testmempoolaccept` given several
  transactions of which one is an asset transaction answers with a
  `package-error` for each.

Asset database
--------------

- `-reindexassets`, and the automatic rebuild a node runs when its asset
  database and chain state disagree, now refuse to run on a pruned node before
  erasing anything, and fail on a block they cannot read instead of skipping
  it and reporting success. The old behaviour left a database missing every
  asset in an unreadable block, after which the node rejected the next transfer
  of one and forked. Startup now stops when the rebuild fails rather than
  running on with an incomplete database.
- The metadata pass of the rebuild keeps each block file open across reads
  instead of reopening it for every block. On Windows this cuts the mainnet
  pass from minutes to seconds. It is safe because the one case that can delete
  block files, a pruned node, is now refused up front.
- The rebuild runs before the node connects any block that is on disk and not
  yet in the chain. It ran after, so a node stopped in the middle of a flush,
  most likely during the first sync, could check such a block against asset
  state older than its UTXO set, find a valid block invalid, and stay below it
  until `reconsiderblock` was run.
- `-reindex-chainstate` now empties the asset and restricted databases, as
  `-reindex` does. They were left in place, so the first issuance connected
  again was refused as already used and the node stopped at that block. The
  same holds when the chainstate directory has been removed by hand. A node
  that an earlier 5.x release left stopped this way keeps that block marked
  invalid: start it once with `-reindex`, or with `-reindex-chainstate` and
  then run `reconsiderblock` on the block it stopped at.
- On a pruned node `-reindexassets` is refused at start, before anything is
  erased, and the node starts normally without it. A pruned node whose asset
  database does need rebuilding cannot do it, since the blocks are gone, and
  says so at start: it has to download the chain again with `-reindex`.

Wallet
------

- Building an asset issue, transfer or reissue transaction no longer risks a
  deadlock against the block-connection handler. The two took `cs_main` and
  `cs_wallet` in opposite orders, so a reissue or tag change issued in the same
  moment a block arrived could hang the node. The builders now take the locks
  in the handler's order.
- Change never goes to an ML-DSA-44 output before that deployment is active,
  and `getrawchangeaddress` refuses the `pq` type until then, as `getnewaddress`
  does. `getnewaddress` now refuses before it derives the address; it used to
  leave the address in the address book. Before activation such an output has
  no spending rule. On mainnet the deployment is not scheduled.

GUI
---

- The send-asset form shows the balance of an administrator (owner) asset
  instead of `(unknown)`. An owner token is a single indivisible unit, which
  the form now reports as a balance of 1.

RPC
---

- `listassets` shows an asset's reissued amount, units and flags as soon as the
  reissue is in a block, before the next flush of the asset database, matching
  what `getassetdata` already reported. It read only the committed database and
  the uncommitted new issuances, not the uncommitted reissues, so a
  just-reissued asset kept its previous amount until the database was next
  written.

Post-quantum signatures (RIP-25)
--------------------------------

ML-DSA-44 (witness v2) is not deployed on mainnet and this release does not
schedule it. The changes affect only the test networks, where it is active:

- The signature context now includes the network's message start, and its tag
  is `AVN/ML-DSA-44/v2/`. Before, the context was built from the genesis hash
  alone, which testnet and testnet4 share, so a signature made for one verified
  on the other. A signature made by v5.0.5 or earlier does not verify under the
  new context. Upgrade every node of a test network together.
- A witness v2 program wrapped in P2SH is no longer charged a sigop cost. Such
  a program is not a RIP-25 output and no signature is verified for it, but it
  was charged as if one were.
- Every step of an `mldsa44()` descriptor derivation path must be hardened.
- Building with post-quantum support checks the liboqs version it finds.

`doc/rip-25.md` and the known-answer and consensus vectors in `src/test/data`
are updated to match. The suite now has conformance vectors that check the
verify result, a backend agreement check, and fuzz targets that check results
rather than only that the code does not crash.

P2P
---

- A header that names a proof-of-work algorithm which does not exist is
  refused before its difficulty limit is looked up. The lookup read past the
  end of a two-entry table. The header was refused afterwards either way.

Mining
------

- Removing a transaction from the mempool while building a block template
  takes the validation lock as well as the mempool lock.
- A node serving Stratum v2 templates no longer stops when a template cannot
  be built. The error left the thread that asked for the template and ended
  the process. The client is kept, the failure is logged, and the build is
  tried again a second later.

Tests and CI
------------

- The unit test suite runs in CI on ARM64 Linux and macOS as well as x86-64.

Credits
=======

Thanks to everyone who directly contributed to this release.
