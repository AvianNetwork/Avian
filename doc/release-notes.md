(note: this is a temporary file, to be added-to by anybody, and moved to
release-notes/ at release time)

Avian Core version *version* is now available from:

  <https://github.com/AvianNetwork/Avian/releases>

This release includes the following changes since the last release:

Notable changes
===============

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

The extension currently uses the experimental identifier `0x4001`, which the
Stratum v2 specification forbids in production equipment, so this is suitable
for development and testnet only until a registry allocation is assigned.
`doc/sv2.md` has the full configuration surface, the extension's wire format and
the security considerations, the most important of which is that exposing the
Stratum v2 port is equivalent to exposing an RPC port. It binds to loopback by
default.

Credits
=======

Thanks to everyone who directly contributed to this release:

