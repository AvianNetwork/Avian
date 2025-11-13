#include <node/transaction.h>

namespace node
{
std::string TransactionErrorString(TransactionError err)
{
    switch (err) {
    case TransactionError::OK:
        return "OK";
    case TransactionError::MISSING_INPUTS:
        return "Missing inputs";
    case TransactionError::ALREADY_IN_CHAIN:
        return "Already in chain";
    case TransactionError::P2P_DISABLED:
        return "P2P disabled";
    case TransactionError::MEMPOOL_REJECTED:
        return "Rejected by mempool";
    case TransactionError::INVALID_PSBT:
        return "Invalid PSBT";
    case TransactionError::INVALID_PACKAGE:
        return "Invalid package";
    case TransactionError::MEMPOOL_ERROR:
        return "Mempool error";
    default:
        return "Unknown error";
    }
}
} // namespace node
