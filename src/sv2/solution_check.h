// Copyright (c) 2026 The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_SV2_SOLUTION_CHECK_H
#define BITCOIN_SV2_SOLUTION_CHECK_H

#include <consensus/params.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <sv2/messages.h>
#include <uint256.h>

#include <cstdint>
#include <functional>
#include <vector>

namespace sv2 {

//! What a template committed to, kept so that validating a solution against it
//! costs no block work.
struct SolutionContext {
    POW_TYPE pow_type{POW_TYPE_X16RT};
    //! nVersion as issued. A solution may differ only in bits the rolling mask
    //! permits, which never includes the algorithm bits.
    uint32_t version{0};
    uint32_t version_rolling_mask{0};
    uint32_t nbits{0};
    uint256 prev_hash;
    uint32_t ntime{0};
    uint32_t ntime_min{0};
    uint32_t ntime_max{0};

    //! Coinbase fields the client was told to reproduce.
    uint32_t coinbase_tx_version{0};
    uint32_t coinbase_tx_input_sequence{0};
    uint32_t coinbase_tx_locktime{0};
    std::vector<uint8_t> coinbase_prefix;
    //! Outputs the client must include verbatim: Avian's founder payment and
    //! the witness commitment.
    std::vector<CTxOut> required_outputs;
    //! Value the client may spend on outputs of its own.
    int64_t value_remaining{0};
    //! Ceilings for the finished coinbase: what the template's own coinbase
    //! needs plus what the client asked to add. Derived when the template is
    //! issued rather than guessed from a constant, because the base size
    //! depends on how many outputs are required.
    size_t max_coinbase_size{0};
    unsigned int max_coinbase_sigops{0};

    //! Weight ceiling for the coinbase, which is what the assembler left free
    //! for it.
    //!
    //! Checking the coinbase against its reservation gives the block limits
    //! transitively: the assembler already built the rest of the block to fit
    //! inside MAX_BLOCK_WEIGHT minus this reservation. Computing the block's
    //! own size instead would mean copying up to eight megabytes per
    //! template, for a weaker guarantee.
    size_t max_coinbase_weight{0};

    //! Ordered from the deepest, as NewTemplate carries it.
    std::vector<uint256> merkle_path;
};

//! Why a solution was refused.
//!
//! An enum rather than a string so tests can assert the exact reason instead
//! of matching log text, and so the counters stay cheap.
enum class SolutionError {
    NONE,
    UNKNOWN_TEMPLATE,
    VERSION_MISMATCH,
    BAD_ALGO,
    NTIME_OUT_OF_RANGE,
    NTIME_WRONG_EPOCH,
    COINBASE_TOO_LARGE,
    COINBASE_MALFORMED,
    COINBASE_NON_CANONICAL,
    COINBASE_BAD_SCRIPTSIG,
    COINBASE_BAD_FIELDS,
    COINBASE_BAD_WITNESS,
    MISSING_REQUIRED_OUTPUT,
    BAD_COMMITMENT,
    VALUE_TOO_HIGH,
    TOO_MANY_SIGOPS,
    BLOCK_TOO_LARGE,
    POW_INSUFFICIENT,
};

const char* SolutionErrorString(SolutionError error);

//! Computes the proof-of-work hash of a header.
//!
//! Injected rather than called directly so that tests and fuzz harnesses can
//! substitute something cheap. X16RT and MinotaurX cost orders of magnitude
//! more than SHA256d, and a fuzz target that ran them would manage single
//! digit executions per second.
using PoWHasher = std::function<uint256(const CBlockHeader&)>;

struct SolutionCheck {
    SolutionError error{SolutionError::NONE};
    //! Only meaningful when error is NONE.
    CBlockHeader header;
    CTransactionRef coinbase;
};

//! Validate a submitted solution against the template it claims, cheapest
//! check first.
//!
//! The ordering is the point. Avian's proof of work has no cheap partial form
//! the way SHA256d does, so the single hash at the end is thousands of times
//! more expensive than everything before it. Every field check that can reject
//! a frame must therefore happen first, or a few attacker bytes buy a
//! MinotaurX computation.
SolutionCheck CheckSolution(const SolutionContext& ctx, const SubmitSolution& solution, int64_t now,
                            const Consensus::Params& consensus, const PoWHasher& hasher);

} // namespace sv2

#endif // BITCOIN_SV2_SOLUTION_CHECK_H
