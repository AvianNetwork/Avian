# RIP-25: ML-DSA-44 Post-Quantum Transaction Outputs

- **Status:** Draft (working document, not frozen)
- **Network:** Avian
- **Witness version:** 2
- **Signature scheme:** ML-DSA-44 (NIST FIPS 204)
- **Deployment:** BIP9 `mldsa44` (bit 11), `NEVER_ACTIVE` on mainnet
- **Scope of this version:** AVN outputs only. Assets and hybrid (ECDSA+ML-DSA) are out of scope and specified separately in later revisions.

> This document records the consensus rules of the ML-DSA-44 output type as they exist in
> Avian Core v5.0 and marks the decisions that must be resolved before the specification is
> frozen. Sections tagged **OPEN** describe current behavior that is expected to change; do not
> treat them as final. Once frozen, any consensus-affecting change requires updating this
> document and its test vectors together.

## Abstract

RIP-25 defines a SegWit witness version 2 output type whose spending authority is an ML-DSA-44
(lattice-based, FIPS 204) signature rather than an ECDSA signature. The output commits to a
32-byte hash of an ML-DSA-44 public key. A spend reveals the public key and a signature over the
transaction digest. Existing ECDSA outputs are unaffected.

## Motivation

ECDSA is vulnerable to a sufficiently large quantum computer. ML-DSA-44 is a NIST-standardized
signature scheme believed resistant to such attacks. RIP-25 gives Avian holders an optional,
quantum-resistant ownership type that coexists with legacy addresses, activated only by community
signalling and only for outputs created after activation. It does not migrate or threaten existing
funds.

## Constants

| Name | Value | Source |
|---|---|---|
| Public key length | 1312 bytes | `mldsa::PUBKEY_SIZE`, `CPQPubKey::SIZE` (`src/crypto/mldsa.h`) |
| Signature length | 2420 bytes | `mldsa::SIG_SIZE` (`src/crypto/mldsa.h`) |
| Seed length | 32 bytes | `mldsa::SEED_SIZE` |
| Secret key length | 2560 bytes | `mldsa::SECRETKEY_SIZE` |
| Witness program length | 32 bytes | `SHA256` output |
| Witness version | 2 | `OP_2` |
| Sighash type | `0x41` (`SIGHASH_ALL | SIGHASH_FORKID`) | `interpreter.cpp:1747` |

## Specification

### Output format

```
scriptPubKey = OP_2 <32-byte program>          (34 bytes total: 0x52 0x20 <program>)
program      = SHA256(public_key)              (single SHA256, not Hash160, not SHA256d)
```

`SHA256` is the plain 32-byte SHA-256 of the 1312-byte public key (`src/pqkey.cpp:19-25`).

### Address encoding

Witness version 2, encoded with bech32m per BIP350 (`src/key_io.cpp:69-76`, decode `:198-203`),
using the same human-readable part as all other Avian bech32 addresses (no distinct HRP for PQ).
Decoding accepts a v2 program only when its length is exactly 32 bytes.

### Witness structure

The witness stack for a RIP-25 input MUST contain exactly two elements, in this order
(`interpreter.cpp:2037-2041`):

```
witness = [ signature (2420 bytes),   // stack[0]
            public_key (1312 bytes) ] // stack[1]
```

Any other stack size fails with `SCRIPT_ERR_WITNESS_PROGRAM_MISMATCH`.

### Message construction (signature hash)

The message signed by ML-DSA-44 is a BIP143-style transaction digest (`interpreter.cpp:1742-1749`,
signing side `src/script/sign.cpp:98-121`):

```
scriptCode = OP_2 <program>                    (34 bytes)
sighash    = SignatureHash(scriptCode, tx, nIn, 0x41, amount, SigVersion::WITNESS_V0, txdata)
```

- The sighash type is fixed to `SIGHASH_ALL | SIGHASH_FORKID` (`0x41`) and is not encoded anywhere
  in the transaction. No other sighash mode is valid, and unlike ECDSA/Schnorr the witness carries
  **no** trailing sighash-type byte: the signature element is the bare `SIG_SIZE` (2420-byte) ML-DSA
  signature. A verifier MUST reject any witness whose signature element is not exactly 2420 bytes
  (`SCRIPT_ERR_PQ_SIGNATURE_SIZE`), which is what enforces the "no carried byte" half of this rule;
  a signature computed over any other hashtype's digest fails verification
  (`SCRIPT_ERR_PQ_SIGNATURE_VERIFY_FAILED`). Both halves are locked by
  `mldsa44_sighash_tests.cpp`.
- `amount` is the value of the output being spent (BIP143).
- Using the 34-byte `OP_2 <program>` scriptCode distinguishes a RIP-25 preimage from a witness v0
  (P2WPKH/P2WSH) preimage, because a v0 scriptCode can never take that shape.
- The 32-byte `sighash` is passed to ML-DSA-44 as the message in "pure" mode (the caller pre-hashes;
  ML-DSA's own HashML-DSA mode is not used), together with a non-empty FIPS-204 context string for
  domain separation. See **Domain separation**.

### Verification algorithm

When `SCRIPT_VERIFY_MLDSA44` is set (deployment active), a witness v2 / 32-byte-program input is
verified as follows (`interpreter.cpp:2027-2068`), failing on the first violated rule:

1. `stack.size() == 2`, else `SCRIPT_ERR_WITNESS_PROGRAM_MISMATCH`.
2. `public_key.size() == 1312`, else `SCRIPT_ERR_PQ_PUBKEY_SIZE`.
3. `signature.size() == 2420`, else `SCRIPT_ERR_PQ_SIGNATURE_SIZE`.
4. `SHA256(public_key) == program` (32-byte compare), else `SCRIPT_ERR_PQ_WITNESS_PROGRAM_MISMATCH`.
5. `ML-DSA-44.Verify(public_key, signature, sighash)` succeeds, else `SCRIPT_ERR_PQ_SIGNATURE_VERIFY_FAILED`.

On success the input is valid; no further script evaluation occurs (RIP-25 inputs never enter
`EvalScript`).

Error codes are defined at `src/script/script_error.h:90-94`.

### P2SH exclusion

A RIP-25 program wrapped in P2SH is not recognized (`!is_p2sh` guard, `interpreter.cpp:2027`).
RIP-25 outputs are native SegWit only.

### Pre-activation semantics

Before `SCRIPT_VERIFY_MLDSA44` is set, a witness v2 / 32-byte-program output is treated as an
unknown future upgrade and is **anyone-can-spend** at the consensus layer (`interpreter.cpp:2031-2036`
returns `true`), unless `SCRIPT_VERIFY_DISCOURAGE_UPGRADABLE_WITNESS_PROGRAM` is set (relay policy),
in which case it returns `SCRIPT_ERR_DISCOURAGE_UPGRADABLE_WITNESS_PROGRAM`.

Consequence: **RIP-25 outputs created before activation are not secure and must not be used to
hold value on mainnet.** The wallet enforces this by refusing to generate `pq` addresses until the
deployment is active (`src/wallet/rpc/addresses.cpp:69-75`).

### Key derivation

Wallets derive ML-DSA-44 keys through a descriptor:

```
mldsa44(<xpub>/25h/921h/0h/<0h|1h>/*h)         (purpose 25 = RIP-25, coin type 921 = Avian)
```

All steps are hardened; the final step must be `/*h` (`src/script/descriptor.cpp:2678-2717`,
`src/wallet/walletutil.cpp:63-75`). Let `leaf` be the 32-byte hardened BIP32 child private key at
the leaf. The keypair is then derived by this exact, normative algorithm:

```
DST = "AVN/RIP-25/ML-DSA-44/keygen/v1"        (30 ASCII bytes, no trailing NUL)
xi  = SHA256( DST || leaf )                    32-byte ML-DSA seed
(public_key, secret_key) = ML-DSA.KeyGen_internal(xi)   (FIPS 204 Algorithm 6)
```

`ML-DSA.KeyGen_internal` is the standard derandomized FIPS-204 key generation: `xi` is expanded as
`SHAKE256(xi || IntToBytes(k=4,1) || IntToBytes(l=4,1))` into `(rho, rho', K)`, the matrix `A` is
expanded from `rho`, the short vectors `s1, s2` are sampled from `rho'`, `t = A*s1 + s2` is split
into `(t1, t0)`, and `public_key = pkEncode(rho, t1)`,
`secret_key = skEncode(rho, tr, K, s1, s2, t0)` with `tr = SHAKE256(public_key)`. Because the
result is a standard function of `xi`, any compliant ML-DSA implementation reproduces the same
keypair; the derivation is independent of how randomness is drawn.

The versioned domain-separation tag (`.../v1`) makes the derivation explicit and lets any future
change be unambiguous (`.../v2`). The mapping is pinned by the known-answer vector below and by
`derivation_known_answer_vector` in `src/test/pqkey_tests.cpp`, so it cannot drift silently across a
liboqs upgrade, a struct-ABI change, or a change to the tag or the expansion.

Implementation: `src/crypto/mldsa.cpp` `KeyGenFromSeed` computes `xi` and runs `KeyGen_internal` by
calling the pinned liboqs reference primitives (`pqcrystals_ml_dsa_44_ref_*`) directly with `xi`,
with no process-global RNG state and no mutex. This replaces the earlier approach, which forced
determinism by temporarily swapping liboqs's global RNG for a SHA256 counter-mode KDF and so coupled
every address to liboqs's internal RNG call order.

Reference known-answer vector (`leaf = 0x00,0x01,...,0x1f`):

```
witness_program = SHA256(public_key) = e7fb04e58cb66a825e9f045ea60e6a6d72bbefddb93343321bc64cd03c4265b2
SHA256(secret_key)                   = 5e6259e974035d534b5007ee56b08beaf8d91a31d593cb786c8eede792e1b1c8
```

## Activation

RIP-25 activates via BIP9 versionbits, deployment `mldsa44`, bit 11
(`src/consensus/params.h:40`, `src/deploymentinfo.cpp:21-24`).

| Network | State | Threshold / period |
|---|---|---|
| mainnet | `NEVER_ACTIVE` | 36288 / 40320 (90%, ~2 weeks at 30s blocks) when set |
| testnet, testnet4 | `ALWAYS_ACTIVE` | 1512 / 2016 |
| regtest | `ALWAYS_ACTIVE` | 108 / 144 |

When the deployment is active at a block, `GetBlockScriptFlags` adds
`SCRIPT_VERIFY_MLDSA44 = 1U << 22` (`src/validation.cpp:2667-2670`, `interpreter.h:151-152`).

> The flag enforces pure ML-DSA-44 (no ECDSA component). A true hybrid (ECDSA+ML-DSA) scheme, if
> ever added, would be a separate future output type with its own flag; this one is named
> `SCRIPT_VERIFY_MLDSA44` accordingly.

**Mempool vs consensus asymmetry:** `SCRIPT_VERIFY_MLDSA44` is in `STANDARD_SCRIPT_VERIFY_FLAGS`
but not `MANDATORY_SCRIPT_VERIFY_FLAGS` (`src/policy/policy.h`). The mempool therefore enforces
RIP-25 verification on all networks regardless of activation, while consensus enforces it only once
the deployment is active. This is documented so implementers do not rely on mempool acceptance as
evidence of consensus validity before activation.

## Resource limits and DoS

A measured ML-DSA-44 verification costs ~16.5 microseconds, which is *cheaper* than an ECDSA
verification (~50 us). The dominant cost of a RIP-25 input is its size: the ~3.7 KB witness
(2420 + 1312 bytes) is already accounted for by transaction weight, and `MAX_BLOCK_WEIGHT` alone
bounds a block to roughly 2000 PQ verifications (~35 ms), which is well within the range the network
already tolerates for ECDSA. RIP-25 is therefore not a validation-time DoS vector on its own.

To make that bound explicit and to price the operation, each active ML-DSA-44 verification is charged
a **sigop cost** (`MLDSA44_SIGOP_COST = 50`, `src/script/script.h`), counted in `WitnessSigOps`
(`src/script/interpreter.cpp`) only when `SCRIPT_VERIFY_MLDSA44` is set. Charging it through the
existing sigop machinery reuses three limits at once, with no parallel accounting system:

- **Per block:** it counts toward `MAX_BLOCK_SIGOPS_COST` (80000), capping a block at 1600 PQ
  verifications.
- **Per transaction (policy):** it counts toward `MAX_STANDARD_TX_SIGOPS_COST` (16000), capping a
  standard transaction at 320 PQ verifications.
- **Fees:** `DEFAULT_BYTES_PER_SIGOP` inflates the virtual size by the sigop count, so a PQ spend
  pays a fee commensurate with its verification, not only its weight.

The value 50 matches the Tapscript per-checksig cost (`VALIDATION_WEIGHT_PER_SIGOP_PASSED`); it is a
conservative over-pricing given the measured ~16.5 us, chosen for a clean, consistent ceiling.
Because the charge is gated on `SCRIPT_VERIFY_MLDSA44`, pre-activation cost accounting is unchanged.

**Standardness.** `IsWitnessStandard` (`src/policy/policy.cpp`) has a witness-v2 branch requiring the
witness to be exactly the canonical `[signature (2420), public_key (1312)]`; any other shape is
non-standard and does not relay, so oversized or padded v2 witnesses cannot bloat the mempool. Both
the sigop cost and the standardness branch are covered by `mldsa44_sighash_tests.cpp`.

> A dedicated hybrid signature (a real ML-DSA + ECDSA output) would carry two signatures and could
> warrant a higher per-verification charge; that is a separate future output type and out of scope
> here.

## Domain separation

Every ML-DSA-44 signature is bound to an explicit domain-separation **context** through FIPS 204's
native context string (the `pre = 0x00 || len(ctx) || ctx` prefix prepended to the message before
signing and verification). The context is:

```
ctx = "AVN/RIP-25/ML-DSA-44/v1/" || genesis_block_hash        (24 + 32 = 56 bytes)
```

The genesis block hash is the canonical, immutable per-network identifier, so a signature made for
one network (e.g. testnet) can never verify on another (mainnet), independently of the transaction
contents. The versioned prefix separates RIP-25 from any other ML-DSA use and lets a future context
change be unambiguous (`.../v2`), and reserves room for a distinct context in a future asset-enabled
revision (AVN vs asset spends).

The context is a single process value, set once from the active network in `SelectParams()` and read
by both the consensus verifier and the signer via `GetMLDsa44DomainContext()`
(`src/script/interpreter.h`). It lives in the `bitcoin_consensus` layer because the script verifier
cannot depend on chainparams; the network-selection layer pushes the value in. Signing and verifying
therefore always agree, and there is no per-call-site value that could drift between mempool and
consensus. Enforced by `mldsa44_sighash_tests.cpp` (`domain_separation_binds_the_network`).

> Note: this is defence in depth. BIP143 already binds each input to its prevout, so a spend built on
> one chain does not have a valid preimage on another; the context makes the network binding explicit
> and independent of that, and generalises to non-network contexts.

## Open decisions (consolidated, must resolve before freeze)

1. **Key derivation:** RESOLVED. The liboqs-RNG-hijack is replaced by an explicit, versioned,
   standards-aligned derandomized keygen (`xi = SHA256(DST || leaf)`, then FIPS-204
   `KeyGen_internal(xi)`), pinned by a known-answer vector (see Key derivation).
2. **Domain separation:** RESOLVED. Every signature is bound to a FIPS-204 context string
   `"AVN/RIP-25/ML-DSA-44/v1/" || genesis_block_hash`, set once from the active network in
   `SelectParams()` and shared by signer and verifier, giving explicit per-network binding (see
   Domain separation).
3. **Resource limits:** RESOLVED. Each active ML-DSA-44 verification is charged
   `MLDSA44_SIGOP_COST = 50` sigops, bounding a block to 1600 and a standard transaction to 320 PQ
   verifications and tying fees to the cost; `IsWitnessStandard` requires the canonical two-element
   witness (see Resource limits and DoS).
4. **Sighash policy:** RESOLVED. `SIGHASH_ALL | SIGHASH_FORKID` is the permanent and only rule; no
   sighash-type byte is carried in the witness. The two magic `0x41` literals are replaced by the
   named `SIGHASH_ALL | SIGHASH_FORKID` on both the signing and verifying sides, and the rule is
   locked behaviorally by `mldsa44_sighash_tests.cpp` (see Signature message).
5. **liboqs requirement:** PARTIALLY RESOLVED. The build no longer silently downgrades: with
   `WITH_LIBOQS` ON (the default) a missing liboqs is now a `FATAL_ERROR`, and disabling
   post-quantum support requires an explicit `-DWITH_LIBOQS=OFF` (`cmake/liboqs.cmake`). The stub
   verifier still returns `false` when built that way (`src/crypto/mldsa.cpp`), so such a binary
   must not act as a validating node after activation. Remaining before freeze: pin liboqs to an
   exact revision (currently the 0.12.0 tag plus sha256 in `depends/packages/liboqs.mk`) and assert
   the release build cannot reach the stub path.

## Test vectors

To be committed at `src/test/data/rip25_vectors.json` and treated as normative. Each vector:

```
seed -> public_key -> witness_program -> address ->
unsigned_tx -> sighash -> signature -> final_tx -> txid / wtxid -> expected_verify_result
```

Vectors MUST be reproduced byte-for-byte across Linux, Windows and macOS on x86_64 and ARM64, and
across a clean liboqs rebuild. They pin the derivation and the digest so neither can drift silently.

## Reference implementation

- Consensus verify: `src/script/interpreter.cpp:2027-2068`
- Signature hash: `src/script/interpreter.cpp:1742-1749`
- Signing: `src/script/sign.cpp:98-121`
- Crypto wrapper / keygen: `src/crypto/mldsa.{h,cpp}`, keys `src/pqkey.{h,cpp}`
- Address type: `src/addresstype.h:132-144`, `src/script/solver.{h,cpp}`
- Descriptor: `src/script/descriptor.cpp:2161-2362`
- Activation: `src/consensus/params.h:40`, `src/kernel/chainparams.cpp`, `src/validation.cpp:2667-2670`
- Error codes: `src/script/script_error.h:90-94`

## Backwards compatibility

RIP-25 is a soft fork. Non-upgraded nodes see RIP-25 outputs as anyone-can-spend and continue to
follow the chain; they must upgrade (with liboqs) before activation to enforce the new rule and
avoid diverging. Existing ECDSA outputs remain valid and spendable unchanged. Adoption is voluntary.

## References

- NIST FIPS 204 (ML-DSA)
- BIP141 (SegWit), BIP143 (v0 signature digest), BIP350 (bech32m)
- Ravencoin RIP-25 origin: https://github.com/RavenProject/Ravencoin/pull/1281
- Avian v5.0.0 release notes: `doc/release-notes/release-notes-5.0.0.md`
