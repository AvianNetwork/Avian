// Copyright (c) 2025
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef AVIAN_WALLET_PSBWALLET_H
#define AVIAN_WALLET_PSBWALLET_H

#include <string>

class CWallet;
class COutPoint;
class PartiallySignedTransaction;

/**
 * Fill PSBT input fields that depend on wallet knowledge:
 *  - non_witness_utxo (full previous transaction)
 *  - witness_utxo (txout)
 *  - redeem_script for P2SH
 *  - hd_keypaths (BIP32 derivations) when available for HD wallets
 */
bool FillPSBTInputWalletData(const CWallet& wallet,
    const COutPoint& prevout,
    PartiallySignedTransaction& psbtx,
    unsigned int input_index);

/**
 * Ensure every PSBT input has a non-witness UTXO matching its referenced outpoint.
 * Tries the wallet first and falls back to GetTransaction() for lookups.
 * Optionally refreshes witness UTXO data for native witness scripts.
 * @return true on success, false (and fills error_out) when a lookup fails or data mismatches.
 */
bool EnsurePSBTInputUTXOs(const CWallet* wallet,
    PartiallySignedTransaction& psbtx,
    std::string& error_out);

#endif // AVIAN_WALLET_PSBWALLET_H
