(note: this is a temporary file, to be added-to by anybody, and moved to
release-notes/ at release time)

Avian Core version *version* is now available from:

  <https://github.com/AvianNetwork/Avian/releases>

This release includes the following changes since the last release:

Notable changes
===============

Post-quantum signatures (RIP-25)
--------------------------------

Two ML-DSA-44 (witness v2) consensus rules change. Mainnet is not affected: the
deployment is not scheduled there. Testnet, testnet4 and regtest are, because
the deployment is active on them.

- The signature context now includes the network's message start, and its tag
  is `AVN/ML-DSA-44/v2/`. Before, the context was built from the genesis hash
  alone, which testnet and testnet4 share, so a signature made for one verified
  on the other. A signature made by 5.0.5 or earlier does not verify under the
  new context. A block that holds a witness v2 spend signed by an earlier
  release is invalid to this one, and a transaction signed by this release is
  invalid to an earlier one. Upgrade every node of a test network together.
- A witness v2 program wrapped in P2SH is no longer charged a sigop cost. Such
  a program is not a RIP-25 output and no signature is verified for it, but it
  was charged as if one were.

`doc/rip-25.md` and the vectors in `src/test/data` are updated to match.

Credits
=======

Thanks to everyone who directly contributed to this release:

