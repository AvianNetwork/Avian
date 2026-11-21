// Copyright (c) 2026-present The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WALLET_REWARDS_H
#define BITCOIN_WALLET_REWARDS_H

#include <uint256.h>

#include <string>
#include <utility>
#include <vector>

class CRewardSnapshot;

namespace wallet {
class CWallet;
struct WalletContext;

/**
 * Sends the batches of a reward distribution that have not been sent yet, in order, and
 * records each. It stops at the first batch that cannot be sent, leaving the reason in
 * the distribution's status, and can be called again to go on from there.
 *
 * @param hash the key the distribution is stored under, which is its hash as first created
 * @param sent_txids the transactions this call sent
 * @param error an RPC error code and a message, when false is returned
 * @return true when every batch has been sent
 */
bool ContinueRewardDistribution(CWallet& wallet, const uint256& hash, CRewardSnapshot& snapshot,
                                const std::string& change_address, std::vector<std::string>& sent_txids,
                                std::pair<int, std::string>& error);

/**
 * Goes on with the distributions that distributereward registered for a loaded wallet
 * and could not finish: the wallet was locked, funds were short, the node stopped. Run
 * from the scheduler, it does something only when a wallet has seen a new block, which
 * is when 4.x made the same attempt.
 */
void MaybeDistributeRewards(WalletContext& context);
} // namespace wallet

#endif // BITCOIN_WALLET_REWARDS_H
