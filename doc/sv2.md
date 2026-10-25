# Stratum v2 Template Provider

`aviand` can serve the Stratum v2 Template Distribution Protocol. This document
is both the operator guide and the specification of Avian's protocol extension,
because that extension is what a third-party pool or miner has to implement and
what a registry allocation request will freeze.

Nothing here is enabled by default, and a node started without `-sv2` behaves
exactly as it did before.

## What this is, and what it is not

`aviand` acts as a **Template Provider**. It builds block templates, announces
tip changes, serves transaction data, and accepts finished blocks. It is not a
pool: it has no concept of a worker, a share, a difficulty adjustment per miner,
or a payout.

```
aviand                          separate pool process
  block construction              worker management
  transaction selection           share difficulty and accounting
  consensus rules                 payouts
  per-algorithm targets           the Stratum v2 Mining Protocol
  founder payment
  template generation
  block submission
```

The existing JSON-RPC `getblocktemplate` and `submitblock` are untouched and
continue to work alongside this.

## Enabling it

```
-sv2                            serve the Template Distribution Protocol (default: off)
-sv2bind=<addr>[:<port>]        where to listen; repeatable (default: loopback)
-sv2port=<port>                 default port for binds that omit one
-sv2allowip=<ip>                subnet allowed to connect; repeatable
```

Default ports are 8442 (mainnet), 18442 (testnet), 28442 (testnet4), 38442
(signet) and 18447 (regtest).

Algorithms and protocol policy:

```
-sv2powalgo=<algo>              x16rt or minotaurx; repeatable. The first given is
                                used by a client that does not select one.
-sv2extensionpolicy=<policy>    require, optional or off (default: require)
-sv2versionmask=<hex>           version-rolling mask (default: 0, see below)
-sv2maxrejects=<n>              drop a client after this many refused solutions
```

Timing and limits:

```
-sv2interval=<n>                seconds between refreshes when the tip has not moved
-sv2ntimeforward=<n>            how far above a template's own timestamp ntime may roll
-sv2timemargin=<n>              margin left below the consensus future-time limit
-sv2maxclients=<n>
-sv2maxclientsperip=<n>
-sv2handshaketimeout=<n>
-sv2maxsendbuffer=<n>           MiB; must hold one whole block
```

Keys, which are generated mode 0600 on first run:

```
-sv2staticprivkey=<file>        default: <datadir>/sv2_static_key
-sv2authoritykey=<file>         default: <datadir>/sv2_authority_key
-sv2certvalidity=<n>            certificate lifetime in days
```

## Security

**Exposing the Stratum v2 port is equivalent to exposing an RPC port.** It
binds to loopback by default, and that default should be changed only
deliberately.

The Noise NX handshake authenticates the **server to the client**, not the
client to the server. A client learns that it is talking to a node holding a
static key endorsed by the pool authority key; the node learns nothing about the
client. Noise is therefore transport security, not access control. Use
`-sv2allowip`, which is checked at accept time before any cryptography runs.

There is **no cheap pre-filter for Avian's proof of work**. SHA256d chains let a
server do a quick partial check before committing to real work, but X16RT and
MinotaurX have no such form: the only way to know whether a submission is valid
is to compute the whole hash. The node's defence is the validation ladder in
`src/sv2/solution_check.cpp`, which rejects on every cheap field check first, a
rate limit on submissions, and the loopback default. If the port is exposed to
an untrusted network, those three are the whole defence.

## Session flow

```
            client                                node
               |                                   |
               |  Noise NX message 1 (64 bytes)    |
               | --------------------------------> |
               |  Noise NX message 2 (234 bytes)   |
               | <-------------------------------- |
               |                                   |
               |  SetupConnection                  |
               | --------------------------------> |
               |  SetupConnection.Success          |
               | <-------------------------------- |
               |                                   |
               |  RequestExtensions [0x4001]       |
               | --------------------------------> |
               |  RequestExtensions.Success        |
               | <-------------------------------- |
               |                                   |
               |  AvianSelectAlgorithm             |
               | --------------------------------> |
               |  AvianSelectAlgorithm.Success     |
               | <-------------------------------- |
               |                                   |
               |  CoinbaseOutputConstraints        |
               | --------------------------------> |
               |  NewTemplate                      |
               | <-------------------------------- |
               |  SetNewPrevHash                   |
               | <-------------------------------- |
               |  AvianTimeConstraints             |
               | <-------------------------------- |
               |                                   |
               |  RequestTransactionData           |
               | --------------------------------> |
               |  RequestTransactionData.Success   |
               | <-------------------------------- |
               |                                   |
               |  SubmitSolution                   |
               | --------------------------------> |
```

`SetupConnection.protocol` must be 2 (Template Distribution).
`AvianSelectAlgorithm` is sent once per connection, after
`RequestExtensions.Success` and before `CoinbaseOutputConstraints`. The
algorithm is immutable thereafter, because one connection carries one `nBits`
and `target` stream.

`CoinbaseOutputConstraints` must arrive before the node will issue a template,
because the coinbase size and sigop budget it declares are what the block
assembler reserves.

`SetNewPrevHash` accompanies a template built on a tip the client has not seen.
An in-place refresh that picks up new fees sends only `NewTemplate` and
`AvianTimeConstraints`, so a client must not assume a fixed number of messages
per round of work.

The cryptography is the Stratum v2 spec's `Noise_NX_Secp256k1+EllSwift_ChaChaPoly_SHA256`
unchanged. Its key agreement is BIP324's `v2_ecdh`, down to the
`bip324_ellswift_xonly_ecdh` tag and the role-dependent key ordering.

## The Avian extension

`extension_type = 0x4001`, negotiated through the standard Extensions
Negotiation extension `0x0001`.

> **0x4001 is temporary and MUST NOT be used for production deployments.**

The Stratum v2 specification reserves `0x4000`–`0x7fff` for experimental use and
states that experimental values must not be present in production equipment.
A registry allocation at stratumprotocol.org is therefore a **mainnet release
blocker**, not a nice-to-have. Until one is assigned, this extension is suitable
for development, regtest and testnet only.

`-sv2extensionid` can override the identifier for interoperability testing. It
is hidden, validated `<= 0x7fff`, and **rejected on mainnet**, so a mainnet
node's wire identity is fixed by the constant alone.

### Why an extension is needed at all

Stock Template Distribution already carries most of what Avian needs.
`NewTemplate.version` contains `pow_type << 16`, and `SetNewPrevHash.nbits` and
`.target` are already per-algorithm. Three things are missing, and each is one
field:

1. A client cannot **ask** for an algorithm. Without that the node can only pin
   one per listening port.
2. There is no **upper bound on ntime**. The spec defines only
   `ntime >= ntime_start`. X16RT's hash chain is derived from
   `SHA256d(nTime & 0xffffff80)`, so a miner rolling ntime past a 128-second
   boundary silently computes the wrong hash function.
3. There is no way to tell a client **which version bits it may roll**. BIP320's
   mask covers bits 13–28, which fully contains Avian's algorithm field at bits
   16–23.

Every one of those failure modes is silent. A miner hits none of them as an
error; it just burns hashrate and looks unlucky. That is why the extension is
required by default rather than optional.

### Messages

All integers are little-endian, as everywhere in Stratum v2.

| `msg_type` | Message | Direction |
| --- | --- | --- |
| `0x00` | `AvianSelectAlgorithm` | client → node |
| `0x01` | `AvianSelectAlgorithm.Success` | node → client |
| `0x02` | `AvianSelectAlgorithm.Error` | node → client |
| `0x03` | `AvianTimeConstraints` | node → client |

**`AvianSelectAlgorithm`**

| Field | Type | Meaning |
| --- | --- | --- |
| `pow_type` | `U8` | 0 = X16RT, 1 = MinotaurX |

**`AvianSelectAlgorithm.Success`**

| Field | Type | Meaning |
| --- | --- | --- |
| `pow_type` | `U8` | the algorithm now bound to this connection |
| `version_rolling_mask` | `U32` | bits of `nVersion` the client may change |

`version_rolling_mask` is `0` in this version, and the node ANDs any configured
value with `~0x00ff0000` so that bits 16–23 can never be opened. A client must
submit `version` exactly as the template gave it unless the mask says otherwise.

**`AvianSelectAlgorithm.Error`**

| Field | Type | Meaning |
| --- | --- | --- |
| `error_code` | `STR0_255` | why the algorithm was refused |

**`AvianTimeConstraints`**

| Field | Type | Meaning |
| --- | --- | --- |
| `template_id` | `U64` | the template these bounds apply to |
| `ntime_min` | `U32` | lowest acceptable `ntime`, inclusive |
| `ntime_max` | `U32` | highest acceptable `ntime`, inclusive |

Sent after every `NewTemplate`. Both bounds are **inclusive**, and they are
absolute values rather than a mask over `ntime_start`, because an in-place
refresh does not re-send `SetNewPrevHash` and so has nothing to anchor a mask
to.

`ntime_max` is the minimum of:

- X16RT only: the end of the template's 128-second epoch,
  `(nTime & 0xffffff80) + 127`
- `now + MAX_FUTURE_BLOCK_TIME_DUAL_ALGO - sv2timemargin`, where Avian's future
  limit is 1350 seconds, not Bitcoin's 7200
- `nTime + sv2ntimeforward`
- on chains that allow minimum-difficulty blocks, which is regtest and testnet,
  **`nTime` itself**: there `nBits` is a function of `nTime`, so rolling at all
  risks `bad-diff` even though the template's `nBits` was correct when issued

The node re-evaluates the future-time term when a solution arrives, so a
long-lived template cannot be used to smuggle a timestamp the chain would
reject.

## What a client must reproduce

The node rebuilds the header from the template and the submitted coinbase, so a
client's coinbase has to match what it was told. All of this is checked before
any proof of work is computed:

- `coinbase_tx_version`, `coinbase_tx_locktime` and
  `coinbase_tx_input_sequence` exactly as advertised
- one input with a null prevout, and a `scriptSig` of 2 to 100 bytes that
  **starts with `coinbase_prefix`**, which is the BIP34 height push
- `vin[0]` witness stack of exactly one 32-byte zero item, the BIP141 reserved
  value
- every output in `coinbase_tx_outputs` present **verbatim**, with the witness
  commitment the highest-indexed output bearing the `aa21a9ed` magic
- output total no more than `coinbase_tx_value_remaining` plus the required
  outputs
- a canonical encoding: the node re-serializes and byte-compares

Two of those are stricter than consensus on purpose. Avian's **founder payment**
is checked only in `ConnectBlock`, not in `ContextualCheckBlock`, so a client
that dropped that output would otherwise produce a block the node accepts at
header level and relays before validation refuses it. The same is true of the
witness commitment. Rejecting both here turns a wasted block into a log line.

`coinbase_tx_value_remaining` is already net of the founder payment, so no
further subtraction is needed:

```
   coinbase value
         |
   +-----+------+
   |            |
founder      remaining
(mandatory)  (the pool controls this)
```

## Caveats

1. **No unmodified Stratum v2 client can validate an Avian share.** Avian's
   block hash is the X16RT or MinotaurX proof-of-work hash, not SHA256d, however
   byte-perfect the wire format is. Making the extension required is what turns
   that from a silent failure into an explicit one.
2. **`extension_type = 0x4001` is experimental.** Registry allocation is a
   mainnet release blocker. See above.
3. **Version rolling is unsupported** in this version. The mask is 0.
4. **There is no cheap pre-filter for Avian's proof of work.** Rate limiting and
   the loopback default are the defence for that final hash.
5. **Exposing the Stratum v2 port is equivalent to exposing RPC.**
6. **There is no `SubmitSolution.Error` in the Template Distribution Protocol.**
   A refused solution can only be logged and counted, because inventing a reply
   message would break a stock client's dispatch. Run with `-debug=sv2` to see
   the reason. A client that keeps failing is dropped after `-sv2maxrejects`,
   since one rolling a field it was told not to will never find a valid block
   and has no other way of being told.
7. **A server cannot proactively announce a required extension.**
   `RequestExtensions.Error` is only a response, and it does name the required
   extension, so a client that negotiates the wrong thing can correct itself.
   A client that never negotiates at all can only be disconnected, with a log
   line and no on-wire explanation. That is the cost of `require`.

## Testing

```
build/bin/test_avian --run_test=sv2_messages_tests,sv2_noise_tests,sv2_transport_tests,sv2_options_tests,sv2_connman_tests,sv2_solution_check_tests,sv2_template_provider_tests
test/functional/mining_sv2.py
```

`test/functional/test_framework/sv2.py` is a complete Template Distribution
client in Python, built on the framework's existing BIP324 primitives. It is a
second reader of the wire format, and the starting point for anyone wanting to
drive the Template Provider from a script.

The regtest fork activates at genesis+1, so both algorithms are reachable there
and `mining_sv2.py` mines a block with each.
