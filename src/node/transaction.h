#pragma once
#include <string>

namespace node
{

enum class TransactionError {
    OK = 0,
    MISSING_INPUTS,
    ALREADY_IN_CHAIN,
    P2P_DISABLED,
    MEMPOOL_REJECTED,
    INVALID_PSBT,
    INVALID_PACKAGE,
    MEMPOOL_ERROR,
    UNKNOWN
};

std::string TransactionErrorString(TransactionError err);

} // namespace node