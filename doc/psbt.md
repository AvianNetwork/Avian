# PSBT Howto for Avian

Since Avian v0.3.0 (feature/psbt-wallet-integration), the node and wallet ship
with a complete RPC interface for Partially Signed Bitcoin Transactions (PSBTs)
as specified in [BIP 174](https://github.com/bitcoin/bips/blob/master/bip-0174.mediawiki).

This document mirrors Bitcoin Core’s PSBT how-to, but every command, example,
and workflow is expressed in Avian terms (`avian-cli`, Avian-specific address
types, and SIGHASH_FORKID semantics). The intent is to be the single source of
truth for PSBT usage in Avian.

## PSBT in general

PSBT is an interchange format for transactions that are not fully signed yet.
It bundles the unsigned transaction together with metadata that lets various
participants collaborate until the transaction is complete. Typical scenarios
include hardware wallets, air-gapped signers, multisig setups, and CoinJoin
style workflows.

### Overall workflow

A completed transaction generally passes through the following BIP 174 roles:

1. **Creator** – Proposes a transaction by listing inputs and outputs.
2. **Updater** – Adds UTXO data, scripts, and key paths required for signing.
3. **Signer** – Reviews the proposal and contributes partial signatures for the
   keys they control.
4. **Finalizer** – Turns the partial signatures and scripts into final
   `scriptSig`/`scriptWitness` data.
5. **Extractor** – Serializes the fully signed transaction for broadcast.

Roles can be combined. For example, Avian’s wallet often acts as Creator,
Updater, Signer, and Finalizer in one step, while air-gapped devices stick to
the Signer role.

Combiners merge metadata produced in parallel by different participants so that
the next role sees the latest information. Avian exposes a `combinepsbt` helper
for this purpose.

## PSBT in Avian

### RPCs

All RPCs below live in `src/rpc/psbt.cpp` and are accessible through
`avian-cli` or the GUI console.

- **`converttopsbt` (Creator)** – Utility RPC that converts an unsigned raw
  transaction into PSBT format, dropping any existing signatures.
- **`createpsbt` (Creator)** – Takes arrays of inputs/outputs and returns the
  bare PSBT. Equivalent to `createrawtransaction` followed by `converttopsbt`.
- **`walletcreatefundedpsbt` (Creator, Updater)** – Wallet helper that adds
  inputs, change, fee estimation, and metadata in one call. Internally similar
  to `createrawtransaction` → `fundrawtransaction` → `converttopsbt`.
- **`walletprocesspsbt` (Updater, Signer, Finalizer)** – Enriches a PSBT with
  wallet-known data, optionally signs inputs, and finalizes when possible.
- **`utxoupdatepsbt` (Updater)** – Populates SegWit UTXO data straight from the
  node’s UTXO set.
- **`finalizepsbt` (Finalizer, Extractor)** – Completes remaining inputs and,
  when every input is final, returns broadcast-ready hex.
- **`combinepsbt` (Combiner)** – Merges PSBTs that describe the same unsigned
  transaction, accumulating metadata and signatures.
- **`joinpsbts` (Creator)** – Concatenates the inputs and outputs of multiple
  PSBTs to build CoinJoin-style transactions in a deterministic way.
- **`decodepsbt`** – Diagnostic RPC that prints every known PSBT field along
  with fee/weight information when possible.
- **`analyzepsbt`** – Reports the next action required for each input,
  estimates fees, and tells you whether the PSBT is complete.

> **GUI tip:** The Send dialog exposes a “Create Unsigned PSBT” button that
> internally calls `walletcreatefundedpsbt` followed by `presentPSBT`. Use it
> whenever you prefer clipboard-friendly PSBTs instead of immediately broadcasting.

### Avian-specific notes

- Avian uses `SIGHASH_FORKID` for replay protection. PSBTs produced here carry
  that flag, making them incompatible with Bitcoin or other forks.
- Address strings begin with `R`/`r` (legacy) or `A` (SegWit) and must match
  your network (mainnet/testnet/regtest).
- Command examples use `avian-cli`. Replace with RPC over HTTP or the GUI
  console as needed.

## Example workflow: multisig with multiple Avian nodes

Alice, Bob, and Carol want to spend funds from a 2-of-3 multisig address using
only Avian Core instances.

### Setup

1. Each participant creates a new address using `avian-cli getnewaddress` and
   notes the associated public key from `avian-cli getaddressinfo`.
2. They run `avian-cli addmultisigaddress 2 ["Kalice","Kbob","Kcarol"]` to
   register the redeem script with their wallets. For watch-only tracking they
   can also call `avian-cli importaddress` with the multisig address.
3. Funds are sent to the multisig address. All three nodes see the UTXO as
   watch-only.

### Spending

1. Carol prepares the spend:

   ```bash
   avian-cli walletcreatefundedpsbt [] '{"Asend":1.25}' 0 '{"includeWatching":true,"subtractFeeFromOutputs":[0]}'
   ```

   The resulting PSBT `P` contains inputs, change, fees, and metadata but no
   signatures.

2. Carol signs her inputs and forwards the PSBT to Bob:

   ```bash
   avian-cli walletprocesspsbt "$P"
   ```

3. Bob inspects `decodepsbt "$P2"` to verify destinations and fees, then signs
   with `walletprocesspsbt`. Any remaining signer repeats the same step.

4. Once the PSBT has enough signatures, anyone can run:

   ```bash
   avian-cli finalizepsbt "$Pfinal"
   avian-cli sendrawtransaction "<hex>"
   ```

   Alternatively, if signers worked in parallel, collect their partially signed
   PSBTs and merge them with `avian-cli combinepsbt` before finalizing.

### CoinJoin / PSBT joining

For CoinJoin-style collaboration, each participant creates their own inputs
and outputs in a PSBT, then a coordinator runs `avian-cli joinpsbts '["psbt1","psbt2",...]'`
to build the aggregate transaction before distributing it back for signing.

## Troubleshooting checklist

- **Missing UTXO data** – Run `utxoupdatepsbt` on a fully synced node or make
  sure every signer includes witness/non-witness UTXO info.
- **“Incomplete” after signing** – At least one input still lacks
  `final_script_sig` or `final_script_witness`. Inspect `analyzepsbt` to see
  which signer is missing.
- **Mismatched fee/outputs** – Always run `decodepsbt` before signing and
  verify the `fee` and `outputs` objects match expectations.
- **GUI button seemingly does nothing** – The “Create Unsigned PSBT” option
  copies the base64 PSBT to your clipboard and shows a confirmation dialog.
  Paste the clipboard contents into a file or external signer.

## References

- [BIP 174 specification](https://github.com/bitcoin/bips/blob/master/bip-0174.mediawiki)
- Avian Core source files: `src/psbt.cpp`, `src/psbt.h`, `src/rpc/psbt.cpp`
- `doc/psbt.md` (this file) – kept intentionally aligned with
  `Bitcoin/doc/psbt.md` for ease of maintenance.

With this single reference in place, all previous PSBT scratch notes,
implementation logs, and ad-hoc guides have been retired from the repository.
