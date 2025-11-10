# PSBT (Partially Signed Bitcoin Transaction) Implementation

## Overview

This document describes the PSBT (BIP 174) implementation in Avian, enabling multi-party transaction workflows and hardware wallet integration.

## What is PSBT?

PSBT is a standardized binary format for transactions at various stages of completion:

- **Unsigned**: Created but not yet signed
- **Partially signed**: Some inputs signed, some awaiting signatures
- **Fully signed**: All inputs signed, ready to broadcast

## Use Cases

### 1. Multi-Signature Transactions

Multiple parties sign a transaction independently without exposing private keys to each other.

**Workflow:**

```
Party A (Creator) → Create unsigned PSBT
                ↓
Party B (Signer) → Sign their inputs
                ↓
Party C (Signer) → Sign their inputs
                ↓
Party D (Finalizer) → Finalize and broadcast
```

### 2. Hardware Wallet Integration

Devices stay offline until needed for signing, improving security.

**Workflow:**

```
Online: createpsbt → utxoupdatepsbt
           ↓
Offline: walletprocesspsbt (on hardware device)
           ↓
Online: finalizepsbt → sendrawtransaction
```

### 3. Transaction Inspection Before Signing

Signers can review transaction details before committing signatures.

**Workflow:**

```
createpsbt → decodepsbt (review details) → walletprocesspsbt → sendrawtransaction
```

## RPC Commands

### createpsbt

Create an unsigned PSBT from inputs and outputs.

**Syntax:**

```
createpsbt [{"txid":"id","vout":n},...] [{"address":amount},...] [locktime]
```

**Example:**

```bash
createpsbt '[{"txid":"abc123","vout":0}]' '[{"AN4rK3ZzCKjzw6cxX3bGpfXeZb1EGwSm1z":1.5}]'
```

### decodepsbt

Display PSBT contents in human-readable JSON format.

**Syntax:**

```
decodepsbt "psbt"
```

**Example:**

```bash
decodepsbt "cHNidP8BAHECAAAAAbDJgx..."
```

**Output Fields:**

- `tx`: The underlying transaction
- `inputs`: Array with input details and signatures
- `outputs`: Array with output details
- `unknown`: Any unrecognized fields

### walletprocesspsbt

Sign PSBT inputs using wallet private keys.

**Syntax:**

```
walletprocesspsbt "psbt" [sign] ["sighashtype"]
```

**Parameters:**

- `sign` (boolean, optional): Also sign inputs (default: true)
- `sighashtype` (string, optional): Signature type (default: "ALL")

**Example:**

```bash
walletprocesspsbt "cHNidP8BAHECAAAAAbDJgx..." true "ALL"
```

**Output:**

```json
{
  "psbt": "cHNidP8BAHECAAAAAbDJgx...",
  "complete": false
}
```

### finalizepsbt

Convert a fully-signed PSBT to a transaction ready for broadcast.

**Syntax:**

```
finalizepsbt "psbt" [extract]
```

**Parameters:**

- `extract` (boolean, optional): Return transaction hex if complete (default: true)

**Example:**

```bash
finalizepsbt "cHNidP8BAHECAAAAAbDJgx..."
```

**Output:**

```json
{
  "hex": "020000000...",
  "complete": true
}
```

### analyzepsbt

Check PSBT completeness and report missing signatures/keys.

**Syntax:**

```
analyzepsbt "psbt"
```

**Output:**

- `inputs`: Array with completion status for each input
- `complete`: Boolean indicating if all inputs are signed

### utxoupdatepsbt

Add UTXO data to PSBT from external sources.

**Syntax:**

```
utxoupdatepsbt "psbt" "[{"txid":"id","vout":n,"scriptPubKey":"hex"}]"
```

## Multi-Party Workflow Example

### Scenario: 2-of-3 Multisig Transaction

**Setup:**

- Alice, Bob, and Charlie each have a key in a 2-of-3 multisig address
- They want to send 5 AVN to Dave
- Only 2 signatures are needed

**Step 1: Alice creates the PSBT**

```bash
psbt=$(createpsbt '[{"txid":"abc123","vout":0}]' '[{"dave_address":5}]')
```

**Step 2: Bob analyzes and signs**

```bash
# First, check what needs signing
analyzepsbt $psbt

# Sign the inputs
psbt=$(walletprocesspsbt $psbt true "ALL")
```

**Step 3: Charlie signs**

```bash
# Charlie also signs (now we have 2-of-3 signatures)
psbt=$(walletprocesspsbt $psbt true "ALL")
```

**Step 4: Finalize and broadcast**

```bash
# Convert to transaction
result=$(finalizepsbt $psbt)
tx_hex=$(echo $result | jq -r '.hex')

# Broadcast
sendrawtransaction $tx_hex
```

## Security Considerations

### Private Key Safety

- Private keys never leave the signing device
- PSBT is just transaction structure, no keys included
- Signers only see inputs/outputs they're responsible for

### Verification

Always use `decodepsbt` and `analyzepsbt` to verify transaction details before signing.

### Offline Signing

Hardware wallets can:

1. Receive PSBT via USB/QR code
2. Sign completely offline
3. Return signed PSBT without exposing keys

## Implementation Details

### File Structure

- `src/psbt.h`: Core PSBT data structures
- `src/psbt.cpp`: PSBT serialization/deserialization
- `src/rpc/psbt.cpp`: RPC command implementations
- `src/rpc/psbt.h`: RPC interface

### BIP 174 Compliance

This implementation follows BIP 174 specification:

- Magic bytes: `7073627400` (psbt\0)
- Proper key-value encoding
- Support for unknown fields
- Compatible with other implementations

## References

- BIP 174: https://github.com/bitcoin/bips/blob/master/bip-0174.mediawiki
- Bitcoin PSBT PR: https://github.com/bitcoin/bitcoin/pull/13557
- Avian Network: https://www.avn.network

## Future Enhancements

1. **HD Key Support**: Derive keys from HD seed during signing
2. **Script Templates**: Pre-defined multisig templates
3. **Fee Estimation**: Calculate fees before signing
4. **Timelocks**: Support absolute/relative timelocks in PSBT
5. **Cross-Chain Swaps**: Atomic swaps using PSBTs
