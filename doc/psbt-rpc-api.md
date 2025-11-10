# PSBT RPC API Reference

## Overview

Complete API reference for PSBT (BIP 174) RPC commands in Avian.

---

## createpsbt

Create an unsigned PSBT from transaction inputs and outputs.

### Syntax

```
createpsbt [inputs] [outputs] [locktime] [sequence]
```

### Parameters

| Name       | Type    | Required | Description                                                 |
| ---------- | ------- | -------- | ----------------------------------------------------------- |
| `inputs`   | array   | yes      | Array of previous transactions to spend                     |
| `outputs`  | array   | yes      | Object with destinations and amounts                        |
| `locktime` | integer | no       | Unix timestamp or block height (default: 0)                 |
| `sequence` | integer | no       | Sequence number for relative locktime (default: 4294967295) |

### Input Format

Each input is a JSON object with UTXO reference:

```json
{
  "txid": "hash", // Previous transaction ID
  "vout": 0 // Output index to spend
}
```

### Output Format

Object with address keys and amount values:

```json
{
  "address1": amount1,
  "address2": amount2
}
```

Special outputs:

- `"data"`: `"hex"` - OP_RETURN data output
- `"fee"`: `amount` - Explicit fee (subtracts from inputs)

### Result

Base64-encoded PSBT string.

### Examples

**Simple transaction:**

```bash
createpsbt '[{"txid":"abc123","vout":0}]' '[{"AN4rK3ZzCKjzw6cxX3bGpfXeZb1EGwSm1z":1.5}]'
```

**Multiple inputs and outputs:**

```bash
createpsbt \
  '[{"txid":"abc123","vout":0},{"txid":"def456","vout":1}]' \
  '{"address1":1.0,"address2":0.5,"address3":0.3}'
```

**With locktime:**

```bash
createpsbt \
  '[{"txid":"abc123","vout":0}]' \
  '[{"address":1}]' \
  1704067200
```

**With data output:**

```bash
createpsbt \
  '[{"txid":"abc123","vout":0}]' \
  '[{"address":0.9,"data":"48656c6c6f"}]'
```

### Error Codes

- `-3`: Invalid parameters
- `-8`: Insufficient inputs
- `-25`: Invalid address

---

## decodepsbt

Display contents of a PSBT in human-readable JSON format.

### Syntax

```
decodepsbt "psbt"
```

### Parameters

| Name   | Type   | Required | Description         |
| ------ | ------ | -------- | ------------------- |
| `psbt` | string | yes      | Base64-encoded PSBT |

### Result

```json
{
  "tx": {                    // Underlying transaction
    "txid": "...",
    "vin": [...],
    "vout": [...]
  },
  "inputs": [                // Array of input details
    {
      "non_witness_utxo": {...},      // Full UTXO (if available)
      "witness_utxo": {...},           // Witness UTXO (if available)
      "partial_signatures": {          // Signatures contributed so far
        "pubkey": "signature"
      },
      "sighash": "SIGHASH_ALL",        // Signature hash type
      "redeem_script": "hex",          // Redeem script if present
      "witness_script": "hex",         // Witness script if present
      "final_scriptSig": "hex",        // Final unlocking script
      "final_scriptWitness": [...],    // Final witness data
      "unknown": {}                    // Unrecognized fields
    }
  ],
  "outputs": [               // Array of output details
    {
      "redeem_script": "hex",
      "witness_script": "hex",
      "unknown": {}
    }
  ],
  "fee": amount,            // Fee if calculable
  "unknown": {}             // Global unknown fields
}
```

### Examples

```bash
decodepsbt "cHNidP8BAHECAAAAAbDJgx..."
```

**Parse output for analysis:**

```bash
avian-cli decodepsbt "$PSBT" | jq '.inputs[0].partial_signatures | length'
```

### Error Codes

- `-22`: Invalid hex format
- `-25`: Invalid PSBT format

---

## walletprocesspsbt

Sign PSBT inputs using wallet private keys.

### Syntax

```
walletprocesspsbt "psbt" [sign] [sighashtype] [finalize]
```

### Parameters

| Name          | Type    | Required | Description                                                           |
| ------------- | ------- | -------- | --------------------------------------------------------------------- |
| `psbt`        | string  | yes      | Base64-encoded PSBT                                                   |
| `sign`        | boolean | no       | Sign the inputs (default: true)                                       |
| `sighashtype` | string  | no       | Signature type: ALL, NONE, SINGLE, ALL\|ANYONECANPAY (default: "ALL") |
| `finalize`    | boolean | no       | Finalize after signing (default: false)                               |

### Result

```json
{
  "psbt": "base64_string",   // Updated PSBT
  "complete": true/false,     // Whether transaction is fully signed
  "finalized": true/false     // Whether finalization was attempted
}
```

### Sighash Types

| Type     | Description                                          |
| -------- | ---------------------------------------------------- | -------------------------------- |
| `ALL`    | Sign all inputs, all outputs                         |
| `NONE`   | Sign all inputs, no outputs (anyone can add outputs) |
| `SINGLE` | Sign all inputs, matching output only                |
| `ALL     | ANYONECANPAY`                                        | Only this input, all outputs     |
| `NONE    | ANYONECANPAY`                                        | Only this input, no outputs      |
| `SINGLE  | ANYONECANPAY`                                        | Only this input, matching output |

### Examples

**Sign with default parameters:**

```bash
walletprocesspsbt "cHNidP8BAHECAAAAAbDJgx..."
```

**Sign only without finalizing:**

```bash
walletprocesspsbt "cHNidP8BAHECAAAAAbDJgx..." true "ALL"
```

**Sign and finalize immediately:**

```bash
walletprocesspsbt "cHNidP8BAHECAAAAAbDJgx..." true "ALL" true
```

**Partial spend (ANYONECANPAY):**

```bash
walletprocesspsbt "cHNidP8BAHECAAAAAbDJgx..." true "ALL|ANYONECANPAY"
```

### Error Codes

- `-4`: Wallet access error
- `-13`: Wallet locked (need to unlock)
- `-22`: Invalid sighash type

---

## finalizepsbt

Convert a signed PSBT to a transaction ready for broadcast.

### Syntax

```
finalizepsbt "psbt" [extract]
```

### Parameters

| Name      | Type    | Required | Description                             |
| --------- | ------- | -------- | --------------------------------------- |
| `psbt`    | string  | yes      | Base64-encoded PSBT                     |
| `extract` | boolean | no       | Extract hex if complete (default: true) |

### Result

```json
{
  "psbt": "base64_string",   // Finalized PSBT
  "hex": "hex_string",       // Transaction hex (if complete and extract=true)
  "complete": true/false     // Whether finalization succeeded
}
```

### Finalization Rules

A PSBT is "complete" when:

1. All inputs have final_scriptSig or final_scriptWitness
2. No input has partial_signatures awaiting completion
3. All script requirements are met

### Examples

**Check if ready for broadcast:**

```bash
finalizepsbt "cHNidP8BAHECAAAAAbDJgx..."
```

**Get transaction without extracting hex:**

```bash
finalizepsbt "cHNidP8BAHECAAAAAbDJgx..." false
```

**Finalize and send:**

```bash
RESULT=$(avian-cli finalizepsbt "$PSBT")
if [ "$(echo $RESULT | jq -r '.complete')" = "true" ]; then
  HEX=$(echo $RESULT | jq -r '.hex')
  avian-cli sendrawtransaction "$HEX"
fi
```

### Error Codes

- `-25`: Invalid PSBT format
- `-22`: PSBT not complete (cannot finalize)

---

## analyzepsbt

Analyze PSBT completeness and report missing signatures/keys.

### Syntax

```
analyzepsbt "psbt"
```

### Parameters

| Name   | Type   | Required | Description         |
| ------ | ------ | -------- | ------------------- |
| `psbt` | string | yes      | Base64-encoded PSBT |

### Result

```json
{
  "inputs": [
    {
      "has_utxo": true/false,        // UTXO info available
      "is_final": true/false,         // Input is finalized
      "missing_pubkeys": [            // Public keys needed
        "pubkey1", "pubkey2"
      ],
      "missing_signatures": 2,        // Number of signatures needed
      "missing_redeem_script": true/false,  // Redeem script needed
      "missing_witness_script": true/false  // Witness script needed
    }
  ],
  "complete": true/false,            // All inputs complete
  "estimated_fee": 0.00001           // Fee estimate if calculable
}
```

### Examples

**Check multisig completion:**

```bash
analyzepsbt "cHNidP8BAHECAAAAAbDJgx..." | jq '.inputs[] | .missing_signatures'
```

**Monitor signing progress:**

```bash
for i in {1..5}; do
  echo "Attempt $i:"
  avian-cli analyzepsbt "$PSBT" | jq '.complete'
done
```

**Find incomplete inputs:**

```bash
analyzepsbt "$PSBT" | jq '.inputs[] | select(.is_final == false)'
```

### Error Codes

- `-22`: Invalid PSBT format
- `-25`: PSBT corrupted

---

## utxoupdatepsbt

Add UTXO (Unspent Transaction Output) data to PSBT.

### Syntax

```
utxoupdatepsbt "psbt" [utxos]
```

### Parameters

| Name    | Type   | Required | Description                               |
| ------- | ------ | -------- | ----------------------------------------- |
| `psbt`  | string | yes      | Base64-encoded PSBT                       |
| `utxos` | array  | no       | UTXO descriptors (auto-lookup if omitted) |

### UTXO Format

Each UTXO descriptor:

```json
{
  "txid": "transaction_hash",
  "vout": 0,
  "scriptPubKey": "hex_script",
  "amount": 1.5,
  "redeemScript": "hex_script", // For P2SH
  "witnessScript": "hex_script" // For P2WSH
}
```

### Result

```json
{
  "psbt": "base64_string", // Updated PSBT
  "updated": 3 // Number of inputs updated
}
```

### Examples

**Auto-lookup UTXOs from network:**

```bash
utxoupdatepsbt "cHNidP8BAHECAAAAAbDJgx..."
```

**Manual UTXO specification:**

```bash
utxoupdatepsbt "cHNidP8BAHECAAAAAbDJgx..." \
'[{
  "txid":"abc123",
  "vout":0,
  "scriptPubKey":"76a9...",
  "amount":1.5
}]'
```

**Update from blockchain lookup:**

```bash
for input in $(decodepsbt "$PSBT" | jq -r '.tx.vin[] | @base64'); do
  TXID=$(echo "$input" | base64 -d | jq -r '.txid')
  VOUT=$(echo "$input" | base64 -d | jq -r '.vout')
  UTXO=$(avian-cli gettxout "$TXID" $VOUT)
  # Use utxoupdatepsbt to add...
done
```

### Error Codes

- `-8`: UTXO not found
- `-22`: Invalid UTXO format

---

## Parameter Type Reference

| Type    | Format        | Example                              |
| ------- | ------------- | ------------------------------------ |
| amount  | decimal       | 1.5                                  |
| txid    | hex string    | "abc123def456..."                    |
| vout    | integer       | 0                                    |
| hex     | hex string    | "76a914..."                          |
| address | base58/bech32 | "AN4rK3ZzCKjzw6cxX3bGpfXeZb1EGwSm1z" |
| boolean | true/false    | true                                 |
| string  | text          | "hello"                              |
| object  | JSON          | {"key": "value"}                     |
| array   | JSON          | ["item1", "item2"]                   |

---

## Common Workflows

### Workflow 1: Create and Sign Immediately

```
createpsbt → walletprocesspsbt(sign=true) → finalizepsbt → sendrawtransaction
```

### Workflow 2: Multi-Party Signing

```
createpsbt → analyze/decode → [walletprocesspsbt]* → finalizepsbt → sendrawtransaction
```

### Workflow 3: Offline Hardware Wallet

```
createpsbt → [utxoupdatepsbt] → {transfer to hardware} → walletprocesspsbt → {transfer back} → finalizepsbt → sendrawtransaction
```

### Workflow 4: Complex Inspection

```
createpsbt → decodepsbt (review) → analyzepsbt (check) → walletprocesspsbt → decodepsbt (verify) → finalizepsbt
```

---

## Return Value Summary

| Command             | Returns                               |
| ------------------- | ------------------------------------- |
| `createpsbt`        | PSBT hex string                       |
| `decodepsbt`        | Decoded JSON object                   |
| `walletprocesspsbt` | `{psbt, complete, finalized}`         |
| `finalizepsbt`      | `{psbt, hex, complete}`               |
| `analyzepsbt`       | `{inputs[], complete, estimated_fee}` |
| `utxoupdatepsbt`    | `{psbt, updated}`                     |

---

## See Also

- `decodescript` - Decode script
- `createrawtransaction` - Non-PSBT method
- `signrawtransaction` - Non-PSBT signing
- `sendrawtransaction` - Broadcast transaction
- `gettxout` - Get UTXO information

---

## Notes

1. All PSBT strings are base64-encoded
2. Signatures require wallet access (unlock if necessary)
3. PSBT can only reference existing UTXOs on the network
4. Multiple signatures can be applied sequentially
5. Finalization is automatic during walletprocesspsbt if requested
