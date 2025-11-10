# PSBT Examples and Tutorials

## Prerequisites

- Avian wallet with spendable outputs
- Understanding of PSBT concepts from `psbt.md`
- Familiarity with Avian RPC commands

## Basic Examples

### Example 1: Create and Sign a Simple Transaction

**Goal**: Send 1 AVN to an address using PSBT

```bash
# 1. Create unsigned PSBT
PSBT=$(avian-cli createpsbt \
  '[{"txid":"abc123def456","vout":0}]' \
  '[{"AN4rK3ZzCKjzw6cxX3bGpfXeZb1EGwSm1z":1}]')

echo "Created PSBT: $PSBT"

# 2. Analyze the PSBT before signing
avian-cli analyzepsbt "$PSBT"

# 3. Sign the PSBT
SIGNED_PSBT=$(avian-cli walletprocesspsbt "$PSBT" true "ALL" | jq -r '.psbt')

# 4. Check if signing completed the transaction
RESULT=$(avian-cli finalizepsbt "$SIGNED_PSBT")
COMPLETE=$(echo "$RESULT" | jq -r '.complete')

if [ "$COMPLETE" = "true" ]; then
  HEX=$(echo "$RESULT" | jq -r '.hex')
  TXID=$(avian-cli sendrawtransaction "$HEX")
  echo "Transaction sent: $TXID"
else
  echo "Transaction not complete, still needs signatures"
fi
```

### Example 2: Two-of-Two Multisig

**Goal**: Setup and use a 2-of-2 multisig address

**Setup Phase** (run once):

```bash
# Alice generates a key
ALICE_KEY=$(avian-cli getnewaddress "" "legacy")
ALICE_PUBKEY=$(avian-cli validateaddress "$ALICE_KEY" | jq -r '.pubkey')

# Bob generates a key
BOB_KEY=$(avian-cli getnewaddress "" "legacy")
BOB_PUBKEY=$(avian-cli validateaddress "$BOB_KEY" | jq -r '.pubkey')

# Create 2-of-2 multisig address
MULTISIG=$(avian-cli addmultisigaddress 2 "[$ALICE_PUBKEY,$BOB_PUBKEY]")
MULTISIG_ADDR=$(echo "$MULTISIG" | jq -r '.address')

echo "2-of-2 Multisig Address: $MULTISIG_ADDR"
```

**Transaction Phase**:

```bash
# Fund the multisig (send coins to MULTISIG_ADDR first)

# Alice creates PSBT
ALICE_PSBT=$(avian-cli createpsbt \
  '[{"txid":"fund_tx_id","vout":0}]' \
  '[{"recipient_address":0.5}]')

echo "Created by Alice: $ALICE_PSBT"

# Alice signs
ALICE_SIGNED=$(avian-cli walletprocesspsbt "$ALICE_PSBT" true "ALL" | jq -r '.psbt')

# Pass to Bob (via email, QR code, etc.)
echo "PSBT for Bob: $ALICE_SIGNED"

# Bob analyzes before signing
avian-cli analyzepsbt "$ALICE_SIGNED"

# Bob signs
BOB_SIGNED=$(avian-cli walletprocesspsbt "$ALICE_SIGNED" true "ALL" | jq -r '.psbt')

# Finalize (can be done by either party)
FINAL=$(avian-cli finalizepsbt "$BOB_SIGNED")
echo $FINAL | jq '.'

# Extract and broadcast
if [ "$(echo $FINAL | jq -r '.complete')" = "true" ]; then
  HEX=$(echo "$FINAL" | jq -r '.hex')
  avian-cli sendrawtransaction "$HEX"
fi
```

### Example 3: Tiered Signing (3-of-5 Multisig)

**Goal**: Transaction requiring 3 out of 5 signatures

```bash
# Create 3-of-5 multisig address (setup phase)
KEYS=[]
for i in {1..5}; do
  KEY=$(avian-cli getnewaddress "" "legacy")
  PUBKEY=$(avian-cli validateaddress "$KEY" | jq -r '.pubkey')
  KEYS=$(echo "$KEYS" | jq ". += [\"$PUBKEY\"]")
done

MULTISIG=$(avian-cli addmultisigaddress 3 "$KEYS")
MULTISIG_ADDR=$(echo "$MULTISIG" | jq -r '.address')

# === Transaction Phase ===

# Create PSBT
PSBT=$(avian-cli createpsbt \
  '[{"txid":"tx_id","vout":0}]' \
  '[{"dest":5}]')

# Signer 1
PSBT=$(avian-cli walletprocesspsbt "$PSBT" true "ALL" | jq -r '.psbt')
echo "After Signer 1: $(avian-cli analyzepsbt $PSBT | jq '.complete')"

# Signer 2
PSBT=$(avian-cli walletprocesspsbt "$PSBT" true "ALL" | jq -r '.psbt')
echo "After Signer 2: $(avian-cli analyzepsbt $PSBT | jq '.complete')"

# Signer 3 - This should complete the 3-of-5
PSBT=$(avian-cli walletprocesspsbt "$PSBT" true "ALL" | jq -r '.psbt')
ANALYSIS=$(avian-cli analyzepsbt "$PSBT")

if [ "$(echo $ANALYSIS | jq -r '.complete')" = "true" ]; then
  HEX=$(avian-cli finalizepsbt "$PSBT" | jq -r '.hex')
  avian-cli sendrawtransaction "$HEX"
else
  echo "Need more signatures: $(echo $ANALYSIS | jq '.inputs[].missing_sigs')"
fi
```

## Advanced Scenarios

### Scenario 1: Corporate Transaction Approval

**Setup**: Company requiring CFO signature + 2 of 3 board members

```bash
# Create transaction with approval workflow

# Step 1: CFO creates PSBT
CFO_PSBT=$(avian-cli createpsbt \
  '[{"txid":"company_account","vout":0}]' \
  '[{"vendor_address":100}]')

# Step 2: CFO signs
CFO_PSBT=$(avian-cli walletprocesspsbt "$CFO_PSBT" true "ALL" | jq -r '.psbt')

# Step 3: Board member 1 reviews and signs
echo "PSBT Details:"
avian-cli decodepsbt "$CFO_PSBT" | jq '.tx'

BOARD_PSBT=$(avian-cli walletprocesspsbt "$CFO_PSBT" true "ALL" | jq -r '.psbt')

# Step 4: Board member 2 signs
BOARD_PSBT=$(avian-cli walletprocesspsbt "$BOARD_PSBT" true "ALL" | jq -r '.psbt')

# Step 5: Finalize when ready
FINAL=$(avian-cli finalizepsbt "$BOARD_PSBT")
if [ "$(echo $FINAL | jq -r '.complete')" = "true" ]; then
  HEX=$(echo "$FINAL" | jq -r '.hex')
  avian-cli sendrawtransaction "$HEX"
  echo "Transaction approved and sent"
fi
```

### Scenario 2: Offline Hardware Wallet Signing

**Setup**: Hardware wallet connected only when needed

```bash
#!/bin/bash

# Online machine (normal PC)
export ONLINE_HOST="normal.machine"

# Offline machine (hardware wallet)
export OFFLINE_HOST="secure.machine"

# Step 1: Online - Create PSBT
PSBT=$(ssh $ONLINE_HOST avian-cli createpsbt \
  '[{"txid":"online_utxo","vout":0}]' \
  '[{"recipient":10}]')

echo "PSBT created: $PSBT"

# Step 2: Transfer to offline machine (via QR/USB)
echo "Scan this QR code on offline device:"
echo "$PSBT" | qrencode -o - | cat

# Step 3: Offline - Sign PSBT (hardware wallet handles this)
SIGNED_PSBT=$(ssh $OFFLINE_HOST avian-cli walletprocesspsbt "$PSBT" true "ALL" | jq -r '.psbt')

# Step 4: Transfer back to online machine
echo "Signed PSBT from hardware: $SIGNED_PSBT"

# Step 5: Online - Finalize and broadcast
FINAL=$(ssh $ONLINE_HOST avian-cli finalizepsbt "$SIGNED_PSBT")
HEX=$(echo "$FINAL" | jq -r '.hex')
TXID=$(ssh $ONLINE_HOST avian-cli sendrawtransaction "$HEX")

echo "Transaction broadcast: $TXID"
```

### Scenario 3: UTXO Consolidation

**Goal**: Combine many small UTXOs into one large UTXO

```bash
#!/bin/bash

# Get all spendable outputs
UTXOS=$(avian-cli listunspent 1 9999)

# Build input array from UTXOs
INPUTS=$(echo "$UTXOS" | jq '[.[] | {"txid": .txid, "vout": .vout}]')

# Calculate total amount
TOTAL=$(echo "$UTXOS" | jq '[.[].amount] | add')

# Calculate fee estimate
FEE=$(echo "scale=8; $(echo $UTXOS | jq 'length') * 0.00001" | bc)

# Send amount minus fee to single address
DESTINATION=$(avian-cli getnewaddress "consolidated")
OUTPUTS="[{\"$DESTINATION\": $(echo "$TOTAL - $FEE" | bc)}]"

# Create consolidation PSBT
PSBT=$(avian-cli createpsbt "$INPUTS" "$OUTPUTS")

# Sign all inputs
SIGNED_PSBT=$(avian-cli walletprocesspsbt "$PSBT" true "ALL" | jq -r '.psbt')

# Verify complete
ANALYSIS=$(avian-cli analyzepsbt "$SIGNED_PSBT")
if [ "$(echo $ANALYSIS | jq -r '.complete')" = "true" ]; then
  HEX=$(avian-cli finalizepsbt "$SIGNED_PSBT" | jq -r '.hex')
  TXID=$(avian-cli sendrawtransaction "$HEX")
  echo "Consolidated $$(echo $TOTAL | jq) into single UTXO: $TXID"
else
  echo "Error: Could not sign all inputs"
fi
```

## Troubleshooting

### PSBT Not Complete After Signing

**Problem**: `finalizepsbt` reports `complete: false` after signing

**Solutions**:

1. Check which inputs are missing signatures:

   ```bash
   avian-cli analyzepsbt "$PSBT" | jq '.inputs[] | select(.missing_sigs)'
   ```

2. Verify you're using the correct private key:

   ```bash
   avian-cli getaddressesbyaccount "account_name"
   ```

3. Check script type matches:
   ```bash
   avian-cli decodepsbt "$PSBT" | jq '.inputs[] | .script_type'
   ```

### Invalid Signature Error

**Problem**: `"code": -25 - Script failed verification`

**Solutions**:

1. Ensure PSBT was created correctly with proper UTXOs
2. Verify using correct sighash type:

   ```bash
   avian-cli walletprocesspsbt "$PSBT" true "ALL|ANYONECANPAY"
   ```

3. Check UTXO still exists:
   ```bash
   avian-cli gettxout "txid" vout
   ```

### PSBT Too Large

**Problem**: PSBT size exceeds limits

**Solutions**:

1. Split into multiple transactions
2. Use raw multisig rather than native multisig
3. Optimize witness data size

## Testing

### Unit Test Example

```bash
# Test basic PSBT creation and finalization
TEST_PSBT=$(avian-cli createpsbt \
  '[{"txid":"abc123","vout":0}]' \
  '[{"AN4rK3ZzCKjzw6cxX3bGpfXeZb1EGwSm1z":1}]')

# Verify PSBT format
if [[ $TEST_PSBT =~ ^cHNidP8= ]]; then
  echo "✓ PSBT format valid (base64 encoded)"
else
  echo "✗ PSBT format invalid"
fi

# Test decoding
DECODE=$(avian-cli decodepsbt "$TEST_PSBT")
if [ $? -eq 0 ]; then
  echo "✓ PSBT decodes successfully"
else
  echo "✗ PSBT decode failed"
fi
```

## References

- Full BIP 174 spec: https://github.com/bitcoin/bips/blob/master/bip-0174.mediawiki
- PSBT documentation: `doc/psbt.md`
- Related commands: `createpsbt`, `decodepsbt`, `walletprocesspsbt`, `finalizepsbt`, `analyzepsbt`
