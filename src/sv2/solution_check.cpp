// Copyright (c) 2026 The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <sv2/solution_check.h>

#include <arith_uint256.h>
#include <chain.h>
#include <consensus/amount.h>
#include <consensus/consensus.h>
#include <consensus/tx_verify.h>
#include <consensus/validation.h>
#include <hash.h>
#include <pow.h>
#include <script/script.h>
#include <streams.h>

#include <algorithm>
#include <numeric>

namespace sv2 {

const char* SolutionErrorString(SolutionError error)
{
    switch (error) {
    case SolutionError::NONE: return "ok";
    case SolutionError::UNKNOWN_TEMPLATE: return "unknown-template";
    case SolutionError::VERSION_MISMATCH: return "version-mismatch";
    case SolutionError::BAD_ALGO: return "bad-algo-id";
    case SolutionError::NTIME_OUT_OF_RANGE: return "ntime-out-of-range";
    case SolutionError::NTIME_WRONG_EPOCH: return "ntime-wrong-epoch";
    case SolutionError::COINBASE_TOO_LARGE: return "coinbase-too-large";
    case SolutionError::COINBASE_MALFORMED: return "coinbase-malformed";
    case SolutionError::COINBASE_NON_CANONICAL: return "coinbase-non-canonical";
    case SolutionError::COINBASE_BAD_SCRIPTSIG: return "coinbase-bad-scriptsig";
    case SolutionError::COINBASE_BAD_FIELDS: return "coinbase-bad-fields";
    case SolutionError::COINBASE_BAD_WITNESS: return "coinbase-bad-witness";
    case SolutionError::MISSING_REQUIRED_OUTPUT: return "missing-required-output";
    case SolutionError::BAD_COMMITMENT: return "bad-commitment";
    case SolutionError::VALUE_TOO_HIGH: return "value-too-high";
    case SolutionError::TOO_MANY_SIGOPS: return "too-many-sigops";
    case SolutionError::BLOCK_TOO_LARGE: return "block-too-large";
    case SolutionError::POW_INSUFFICIENT: return "pow-insufficient";
    }
    return "unknown";
}

namespace {

//! The index of the highest-numbered output carrying the BIP141 commitment
//! magic, mirroring GetWitnessCommitmentIndex, which takes a whole block.
int CommitmentIndex(const CTransaction& coinbase)
{
    int found{NO_WITNESS_COMMITMENT};
    for (size_t o = 0; o < coinbase.vout.size(); ++o) {
        const CScript& script{coinbase.vout[o].scriptPubKey};
        if (script.size() >= MINIMUM_WITNESS_COMMITMENT && script[0] == OP_RETURN &&
            script[1] == 0x24 && script[2] == 0xaa && script[3] == 0x21 && script[4] == 0xa9 &&
            script[5] == 0xed) {
            found = static_cast<int>(o);
        }
    }
    return found;
}

//! Fold a merkle path onto a leaf. The coinbase is always leftmost, so every
//! sibling is a right-hand one.
uint256 FoldMerklePath(const uint256& leaf, const std::vector<uint256>& path)
{
    uint256 current{leaf};
    for (const uint256& sibling : path) {
        current = Hash(current, sibling);
    }
    return current;
}

} // namespace

SolutionCheck CheckSolution(const SolutionContext& ctx, const SubmitSolution& solution, int64_t now,
                            const Consensus::Params& consensus, const PoWHasher& hasher)
{
    SolutionCheck result;
    const auto fail{[&result](SolutionError error) {
        result.error = error;
        return result;
    }};

    // Tier 1: fixed-size field checks. No parsing, no hashing.

    // Only bits the rolling mask permits may differ. The mask never opens bits
    // 16-23, so this single comparison is also what keeps the algorithm, and
    // therefore nBits and the pow limit, consistent with what was issued.
    if (((solution.version ^ ctx.version) & ~ctx.version_rolling_mask) != 0) {
        return fail(SolutionError::VERSION_MISMATCH);
    }

    // Defence in depth: redundant while the mask excludes the algorithm bits,
    // but it is the check that survives any future change to the mask.
    const uint32_t algo{(solution.version >> 16) & 0xff};
    if (algo >= static_cast<uint32_t>(NUM_BLOCK_TYPES) || algo != static_cast<uint32_t>(ctx.pow_type)) {
        return fail(SolutionError::BAD_ALGO);
    }

    if (solution.ntime < ctx.ntime_min || solution.ntime > ctx.ntime_max) {
        return fail(SolutionError::NTIME_OUT_OF_RANGE);
    }
    // Re-evaluated now rather than at issue time, so a long-lived template
    // cannot be used to smuggle a timestamp the chain would reject.
    if (static_cast<int64_t>(solution.ntime) > now + MAX_FUTURE_BLOCK_TIME_DUAL_ALGO) {
        return fail(SolutionError::NTIME_OUT_OF_RANGE);
    }
    // X16RT rederives its hash chain every 128 seconds. Implied by the bounds
    // above, but kept explicit so the failure is legible in the logs.
    if (ctx.pow_type == POW_TYPE_X16RT && (solution.ntime & TIME_MASK) != (ctx.ntime & TIME_MASK)) {
        return fail(SolutionError::NTIME_WRONG_EPOCH);
    }

    if (solution.coinbase_tx.size() > ctx.max_coinbase_size) {
        return fail(SolutionError::COINBASE_TOO_LARGE);
    }

    // Tier 2: one pass over a bounded buffer.

    CMutableTransaction coinbase;
    try {
        SpanReader reader{std::span<const unsigned char>{solution.coinbase_tx}};
        reader >> TX_WITH_WITNESS(coinbase);
        if (!reader.empty()) return fail(SolutionError::COINBASE_NON_CANONICAL);
    } catch (const std::ios_base::failure&) {
        return fail(SolutionError::COINBASE_MALFORMED);
    }

    // Re-serialize and compare, which rejects non-canonical encodings: a
    // witness marker on a transaction with no witness, non-minimal
    // CompactSize, and similar tricks that would change the txid a pool
    // computes without changing the one we do.
    {
        DataStream round_trip{};
        round_trip << TX_WITH_WITNESS(coinbase);
        if (round_trip.size() != solution.coinbase_tx.size() ||
            !std::equal(solution.coinbase_tx.begin(), solution.coinbase_tx.end(),
                        UCharCast(round_trip.data()))) {
            return fail(SolutionError::COINBASE_NON_CANONICAL);
        }
    }

    if (coinbase.vin.size() != 1 || !coinbase.vin[0].prevout.IsNull() || coinbase.vout.empty()) {
        return fail(SolutionError::COINBASE_MALFORMED);
    }

    // BIP34, and the same bounds CheckTransaction enforces. Checking the
    // prefix here heads off a bad-cb-height rejection that would otherwise
    // only surface deep inside block connection.
    const CScript& script_sig{coinbase.vin[0].scriptSig};
    if (script_sig.size() < 2 || script_sig.size() > 100) {
        return fail(SolutionError::COINBASE_BAD_SCRIPTSIG);
    }
    if (script_sig.size() < ctx.coinbase_prefix.size() ||
        !std::equal(ctx.coinbase_prefix.begin(), ctx.coinbase_prefix.end(), script_sig.begin())) {
        return fail(SolutionError::COINBASE_BAD_SCRIPTSIG);
    }

    // All three were advertised, so strict equality is entirely in spec.
    if (static_cast<uint32_t>(coinbase.version) != ctx.coinbase_tx_version ||
        coinbase.nLockTime != ctx.coinbase_tx_locktime ||
        coinbase.vin[0].nSequence != ctx.coinbase_tx_input_sequence) {
        return fail(SolutionError::COINBASE_BAD_FIELDS);
    }

    // The BIP141 witness reserved value. Without it the commitment cannot
    // match, and that would only be discovered inside ConnectBlock.
    const auto& witness{coinbase.vin[0].scriptWitness.stack};
    if (witness.size() != 1 || witness[0].size() != 32 ||
        std::any_of(witness[0].begin(), witness[0].end(), [](unsigned char c) { return c != 0; })) {
        return fail(SolutionError::COINBASE_BAD_WITNESS);
    }

    // Every output we told the client to include must be present verbatim.
    // Stricter than consensus, which would tolerate a larger founder payment
    // or a different order, but we specified exactly what to carry and a
    // client that did something else has a bug worth reporting.
    for (const CTxOut& required : ctx.required_outputs) {
        if (std::find(coinbase.vout.begin(), coinbase.vout.end(), required) == coinbase.vout.end()) {
            return fail(SolutionError::MISSING_REQUIRED_OUTPUT);
        }
    }

    // The commitment must also be the highest-indexed one bearing the magic,
    // because that is the one consensus reads.
    if (!ctx.required_outputs.empty()) {
        const int commit{CommitmentIndex(CTransaction{coinbase})};
        if (commit != NO_WITNESS_COMMITMENT) {
            const CTxOut& found{coinbase.vout[commit]};
            const bool matches{std::any_of(
                ctx.required_outputs.begin(), ctx.required_outputs.end(),
                [&found](const CTxOut& required) { return required == found; })};
            if (!matches) return fail(SolutionError::BAD_COMMITMENT);
        }
    }

    const CAmount required_total{std::accumulate(
        ctx.required_outputs.begin(), ctx.required_outputs.end(), CAmount{0},
        [](CAmount acc, const CTxOut& out) { return acc + out.nValue; })};
    CAmount total{0};
    for (const CTxOut& out : coinbase.vout) {
        if (out.nValue < 0 || out.nValue > MAX_MONEY) return fail(SolutionError::VALUE_TOO_HIGH);
        total += out.nValue;
        if (total > MAX_MONEY) return fail(SolutionError::VALUE_TOO_HIGH);
    }
    if (total > ctx.value_remaining + required_total) {
        return fail(SolutionError::VALUE_TOO_HIGH);
    }

    const unsigned int sigops{GetLegacySigOpCount(CTransaction{coinbase})};
    if (sigops > ctx.max_coinbase_sigops) {
        return fail(SolutionError::TOO_MANY_SIGOPS);
    }

    const size_t coinbase_weight{static_cast<size_t>(
        ::GetSerializeSize(TX_NO_WITNESS(coinbase)) * (WITNESS_SCALE_FACTOR - 1) +
        ::GetSerializeSize(TX_WITH_WITNESS(coinbase)))};
    if (coinbase_weight > ctx.max_coinbase_weight) {
        return fail(SolutionError::BLOCK_TOO_LARGE);
    }

    // Tier 3: cheap hashing. A handful of SHA256d compressions.

    result.coinbase = MakeTransactionRef(coinbase);
    const uint256 coinbase_txid{result.coinbase->GetHash().ToUint256()};

    result.header.nVersion = static_cast<int32_t>(solution.version);
    result.header.hashPrevBlock = ctx.prev_hash;
    result.header.hashMerkleRoot = FoldMerklePath(coinbase_txid, ctx.merkle_path);
    result.header.nTime = solution.ntime;
    result.header.nBits = ctx.nbits;
    result.header.nNonce = solution.nonce;

    // Tier 4: the one expensive operation, reached only by a submission that
    // has passed everything above.
    const uint256 pow{hasher(result.header)};

    // Replicates CheckProofOfWorkDualAlgo, which is static in pow.cpp. The
    // per-algorithm limit matters: MinotaurX's floor is 256 times easier than
    // X16RT's, so using the wrong one would accept or reject wrongly.
    const uint256 limit{algo < consensus.powTypeLimits.size() ? consensus.powTypeLimits[algo]
                                                             : consensus.powLimit};
    const auto target{DeriveTarget(ctx.nbits, limit)};
    if (!target || UintToArith256(pow) > *target) {
        return fail(SolutionError::POW_INSUFFICIENT);
    }

    return result;
}

} // namespace sv2
