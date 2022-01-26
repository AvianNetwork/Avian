// Copyright (c) 2009-2016 The Bitcoin Core developers
// Copyright (c) 2017-2019 The Raven Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef AVIAN_RPCREGISTER_H
#define AVIAN_RPCREGISTER_H

/** These are in one header file to avoid creating tons of single-function
 * headers for everything under src/rpc/ */
class CRPCTable;

/** Register block chain RPC commands */
void RegisterBlockchainRPCCommands(CRPCTable &tableRPC);
/** Register P2P networking RPC commands */
void RegisterNetRPCCommands(CRPCTable &tableRPC);
/** Register miscellaneous RPC commands */
void RegisterMiscRPCCommands(CRPCTable &tableRPC);
/** Register mining RPC commands */
void RegisterMiningRPCCommands(CRPCTable &tableRPC);
/** Register raw transaction RPC commands */
void RegisterRawTransactionRPCCommands(CRPCTable &tableRPC);
/** Register asset RPC commands */
void RegisterAssetRPCCommands(CRPCTable &tableRPC);
/** Register message RPC commands */
void RegisterMessageRPCCommands(CRPCTable &tableRPC);
/** Register rewards RPC commands */
void RegisterRewardsRPCCommands(CRPCTable &tableRPC);
/** Register flight plan RPC commands */
void RegisterFlightPlanRPCCommands(CRPCTable &tableRPC);

static inline void RegisterAllCoreRPCCommands(CRPCTable &t)
{
    RegisterBlockchainRPCCommands(t);
    RegisterNetRPCCommands(t);
    RegisterMiscRPCCommands(t);
    RegisterMiningRPCCommands(t);
    RegisterRawTransactionRPCCommands(t);
    RegisterAssetRPCCommands(t);
    RegisterMessageRPCCommands(t);
    RegisterRewardsRPCCommands(t);
    RegisterFlightPlanRPCCommands(t);
}

#endif
