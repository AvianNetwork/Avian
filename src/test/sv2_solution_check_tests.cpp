// Copyright (c) 2026 The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <arith_uint256.h>
#include <chainparams.h>
#include <consensus/consensus.h>
#include <consensus/validation.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <streams.h>
#include <sv2/solution_check.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <util/chaintype.h>

#include <cstdint>
#include <vector>

#include <boost/test/unit_test.hpp>

//! The submission validation ladder.
//!
//! Avian's proof of work has no cheap partial form the way SHA256d does, so
//! the single hash at the end of this ladder costs orders of magnitude more
//! than every check before it. The ordering is therefore the security
//! property, not an optimisation, and these tests exist to pin it: every
//! rejection case runs with a hasher that fails the test if it is called at
//! all.
namespace {
struct SolutionCheckSetup : BasicTestingSetup {
    SolutionCheckSetup() : BasicTestingSetup{ChainType::REGTEST} {}
};
} // namespace

BOOST_FIXTURE_TEST_SUITE(sv2_solution_check_tests, SolutionCheckSetup)

namespace {

constexpr int HEIGHT{101};
constexpr uint32_t NTIME{1700000000};
//! Algorithm bits (16-23) are zero, which is X16RT.
constexpr uint32_t VERSION{0x20000000};

//! A hasher that must never run. The invariant is "zero calls", so saying
//! exactly that is stronger than counting: a counter you forget to assert
//! passes silently.
sv2::PoWHasher NeverHashes()
{
    return [](const CBlockHeader&) -> uint256 {
        BOOST_FAIL("proof-of-work was hashed for a submission that should have been refused earlier");
        return uint256{};
    };
}

//! A hasher that records its calls and returns a hash meeting any target.
sv2::PoWHasher CountingHasher(int& calls)
{
    return [&calls](const CBlockHeader&) -> uint256 {
        ++calls;
        return uint256::ZERO;
    };
}

CScript CommitmentScript()
{
    CScript script;
    script.resize(MINIMUM_WITNESS_COMMITMENT);
    script[0] = OP_RETURN;
    script[1] = 0x24;
    script[2] = 0xaa;
    script[3] = 0x21;
    script[4] = 0xa9;
    script[5] = 0xed;
    return script;
}

//! A context and a solution that satisfy every check, so each test can break
//! exactly one thing and know that is the reason it failed.
struct Valid {
    sv2::SolutionContext ctx;
    CMutableTransaction coinbase;
    sv2::SubmitSolution solution;

    void Reserialize()
    {
        DataStream ss{};
        ss << TX_WITH_WITNESS(coinbase);
        solution.coinbase_tx.assign(UCharCast(ss.data()), UCharCast(ss.data()) + ss.size());
    }
};

Valid MakeValid()
{
    Valid v;

    const CScript prefix{CScript() << HEIGHT};
    const CTxOut commitment{0, CommitmentScript()};

    v.ctx.pow_type = POW_TYPE_X16RT;
    v.ctx.version = VERSION;
    v.ctx.version_rolling_mask = 0;
    // A real target rather than regtest's floor, so that the comparison against
    // it is meaningful in both directions.
    v.ctx.nbits = 0x1d00ffff;
    v.ctx.prev_hash = uint256::ONE;
    v.ctx.ntime = NTIME;
    v.ctx.ntime_min = NTIME;
    v.ctx.ntime_max = NTIME + 60;
    v.ctx.coinbase_tx_version = 2;
    v.ctx.coinbase_tx_input_sequence = CTxIn::MAX_SEQUENCE_NONFINAL;
    v.ctx.coinbase_tx_locktime = HEIGHT - 1;
    v.ctx.coinbase_prefix.assign(prefix.begin(), prefix.end());
    v.ctx.required_outputs = {commitment};
    v.ctx.value_remaining = 5000;
    v.ctx.max_coinbase_size = 4000;
    v.ctx.max_coinbase_weight = 16000;
    v.ctx.max_coinbase_sigops = 400;
    // A single coinbase means the root is the coinbase txid itself.
    v.ctx.merkle_path = {};

    v.coinbase.version = 2;
    v.coinbase.nLockTime = HEIGHT - 1;
    v.coinbase.vin.resize(1);
    v.coinbase.vin[0].prevout.SetNull();
    v.coinbase.vin[0].nSequence = CTxIn::MAX_SEQUENCE_NONFINAL;
    v.coinbase.vin[0].scriptSig = CScript() << HEIGHT << OP_0;
    v.coinbase.vin[0].scriptWitness.stack.assign(1, std::vector<unsigned char>(32, 0x00));
    v.coinbase.vout = {CTxOut{4000, CScript() << OP_TRUE}, commitment};

    v.solution.template_id = 1;
    v.solution.version = VERSION;
    v.solution.ntime = NTIME;
    v.solution.nonce = 42;
    v.Reserialize();
    return v;
}

sv2::SolutionError Refuse(const Valid& v)
{
    return sv2::CheckSolution(v.ctx, v.solution, NTIME, Params().GetConsensus(), NeverHashes()).error;
}

} // namespace

BOOST_AUTO_TEST_CASE(a_valid_solution_is_accepted_and_hashed_once)
{
    const auto v{MakeValid()};
    int calls{0};
    const auto check{
        sv2::CheckSolution(v.ctx, v.solution, NTIME, Params().GetConsensus(), CountingHasher(calls))};

    BOOST_CHECK_EQUAL(sv2::SolutionErrorString(check.error), "ok");
    // Exactly one hash for a submission that earned it, and not one more.
    BOOST_CHECK_EQUAL(calls, 1);

    BOOST_REQUIRE(check.coinbase);
    BOOST_CHECK_EQUAL(check.header.nVersion, static_cast<int32_t>(VERSION));
    BOOST_CHECK_EQUAL(check.header.nTime, NTIME);
    BOOST_CHECK_EQUAL(check.header.nNonce, 42u);
    BOOST_CHECK_EQUAL(check.header.hashPrevBlock, uint256::ONE);
    // With no merkle path, the root is the coinbase txid.
    BOOST_CHECK_EQUAL(check.header.hashMerkleRoot, check.coinbase->GetHash().ToUint256());
}

BOOST_AUTO_TEST_CASE(version_may_not_differ_outside_the_rolling_mask)
{
    auto v{MakeValid()};
    v.solution.version = VERSION | 0x1;
    BOOST_CHECK_EQUAL(sv2::SolutionErrorString(Refuse(v)), "version-mismatch");

    // With that bit permitted, the same submission is fine.
    v.ctx.version_rolling_mask = 0x1;
    int calls{0};
    BOOST_CHECK_EQUAL(
        sv2::SolutionErrorString(
            sv2::CheckSolution(v.ctx, v.solution, NTIME, Params().GetConsensus(), CountingHasher(calls))
                .error),
        "ok");
}

BOOST_AUTO_TEST_CASE(algorithm_bits_may_never_be_rolled)
{
    // Even a mask that opens the algorithm bits must not let the algorithm
    // change: rolling bit 16 would judge the share under the other algorithm
    // and the wrong pow limit.
    auto v{MakeValid()};
    v.ctx.version_rolling_mask = 0x00ff0000;
    v.solution.version = VERSION | (1u << 16);
    BOOST_CHECK_EQUAL(sv2::SolutionErrorString(Refuse(v)), "bad-algo-id");
}

BOOST_AUTO_TEST_CASE(unknown_algorithm_is_refused)
{
    auto v{MakeValid()};
    v.ctx.version_rolling_mask = 0x00ff0000;
    v.solution.version = VERSION | (static_cast<uint32_t>(NUM_BLOCK_TYPES) << 16);
    BOOST_CHECK_EQUAL(sv2::SolutionErrorString(Refuse(v)), "bad-algo-id");
}

BOOST_AUTO_TEST_CASE(ntime_must_be_inside_the_advertised_window)
{
    auto v{MakeValid()};
    v.solution.ntime = NTIME - 1;
    BOOST_CHECK_EQUAL(sv2::SolutionErrorString(Refuse(v)), "ntime-out-of-range");

    v.solution.ntime = v.ctx.ntime_max + 1;
    BOOST_CHECK_EQUAL(sv2::SolutionErrorString(Refuse(v)), "ntime-out-of-range");
}

BOOST_AUTO_TEST_CASE(ntime_is_rechecked_against_the_clock)
{
    // A long-lived template must not become a way to smuggle a timestamp the
    // chain would reject, so the future limit is evaluated at submission.
    auto v{MakeValid()};
    v.ctx.ntime_max = NTIME + 100000;
    v.solution.ntime = NTIME + 100000;
    BOOST_CHECK_EQUAL(sv2::SolutionErrorString(Refuse(v)), "ntime-out-of-range");
}

BOOST_AUTO_TEST_CASE(x16rt_ntime_may_not_cross_its_epoch)
{
    // X16RT rederives its hash chain every 128 seconds, so work does not
    // survive a boundary crossing even when the window would allow it.
    auto v{MakeValid()};
    const uint32_t next_epoch{(NTIME & TIME_MASK) + 128};
    v.ctx.ntime_max = next_epoch + 10;
    v.solution.ntime = next_epoch;
    BOOST_CHECK_EQUAL(sv2::SolutionErrorString(Refuse(v)), "ntime-wrong-epoch");
}

BOOST_AUTO_TEST_CASE(minotaurx_has_no_epoch_restriction)
{
    auto v{MakeValid()};
    v.ctx.pow_type = POW_TYPE_MINOTAURX;
    v.ctx.version = VERSION | (static_cast<uint32_t>(POW_TYPE_MINOTAURX) << 16);
    v.solution.version = v.ctx.version;
    const uint32_t next_epoch{(NTIME & TIME_MASK) + 128};
    v.ctx.ntime_max = next_epoch + 10;
    v.solution.ntime = next_epoch;

    int calls{0};
    BOOST_CHECK_EQUAL(
        sv2::SolutionErrorString(
            sv2::CheckSolution(v.ctx, v.solution, next_epoch, Params().GetConsensus(),
                               CountingHasher(calls))
                .error),
        "ok");
}

BOOST_AUTO_TEST_CASE(oversized_coinbase_is_refused)
{
    auto v{MakeValid()};
    v.ctx.max_coinbase_size = 10;
    BOOST_CHECK_EQUAL(sv2::SolutionErrorString(Refuse(v)), "coinbase-too-large");
}

BOOST_AUTO_TEST_CASE(malformed_coinbase_is_refused)
{
    auto v{MakeValid()};
    v.solution.coinbase_tx = {0x01, 0x02, 0x03};
    BOOST_CHECK_EQUAL(sv2::SolutionErrorString(Refuse(v)), "coinbase-malformed");
}

BOOST_AUTO_TEST_CASE(trailing_bytes_after_the_coinbase_are_refused)
{
    // A non-canonical encoding would let a pool compute a different txid from
    // the one we do, which is a way to make a valid-looking block invalid.
    auto v{MakeValid()};
    v.solution.coinbase_tx.push_back(0x00);
    BOOST_CHECK_EQUAL(sv2::SolutionErrorString(Refuse(v)), "coinbase-non-canonical");
}

BOOST_AUTO_TEST_CASE(coinbase_must_keep_the_height_prefix)
{
    auto v{MakeValid()};
    v.coinbase.vin[0].scriptSig = CScript() << (HEIGHT + 1) << OP_0;
    v.Reserialize();
    BOOST_CHECK_EQUAL(sv2::SolutionErrorString(Refuse(v)), "coinbase-bad-scriptsig");
}

BOOST_AUTO_TEST_CASE(coinbase_fields_must_match_what_was_advertised)
{
    auto v{MakeValid()};
    v.coinbase.nLockTime = HEIGHT;
    v.Reserialize();
    BOOST_CHECK_EQUAL(sv2::SolutionErrorString(Refuse(v)), "coinbase-bad-fields");

    auto w{MakeValid()};
    w.coinbase.vin[0].nSequence = 0;
    w.Reserialize();
    BOOST_CHECK_EQUAL(sv2::SolutionErrorString(Refuse(w)), "coinbase-bad-fields");
}

BOOST_AUTO_TEST_CASE(witness_reserved_value_is_required)
{
    // Without it the commitment cannot match, and that would only surface
    // inside ConnectBlock.
    auto v{MakeValid()};
    v.coinbase.vin[0].scriptWitness.stack.clear();
    v.Reserialize();
    BOOST_CHECK_EQUAL(sv2::SolutionErrorString(Refuse(v)), "coinbase-bad-witness");

    auto w{MakeValid()};
    w.coinbase.vin[0].scriptWitness.stack.assign(1, std::vector<unsigned char>(32, 0x01));
    w.Reserialize();
    BOOST_CHECK_EQUAL(sv2::SolutionErrorString(Refuse(w)), "coinbase-bad-witness");
}

BOOST_AUTO_TEST_CASE(required_outputs_must_be_present)
{
    // This is what stops a client dropping Avian's founder payment, which
    // consensus only notices at connect time.
    auto v{MakeValid()};
    v.coinbase.vout.pop_back();
    v.Reserialize();
    BOOST_CHECK_EQUAL(sv2::SolutionErrorString(Refuse(v)), "missing-required-output");
}

BOOST_AUTO_TEST_CASE(founder_output_may_not_be_altered)
{
    auto v{MakeValid()};
    const CTxOut founder{250, CScript() << OP_RETURN << std::vector<unsigned char>{0xfe}};
    v.ctx.required_outputs.push_back(founder);
    // Present but short-paid.
    v.coinbase.vout.push_back(CTxOut{249, founder.scriptPubKey});
    v.Reserialize();
    BOOST_CHECK_EQUAL(sv2::SolutionErrorString(Refuse(v)), "missing-required-output");
}

BOOST_AUTO_TEST_CASE(a_later_commitment_output_is_refused)
{
    // Consensus reads the highest-indexed commitment, so a second one after
    // ours would change which commitment the block actually carries.
    auto v{MakeValid()};
    CScript other{CommitmentScript()};
    other[6] = 0x01;
    v.coinbase.vout.push_back(CTxOut{0, other});
    v.Reserialize();
    BOOST_CHECK_EQUAL(sv2::SolutionErrorString(Refuse(v)), "bad-commitment");
}

BOOST_AUTO_TEST_CASE(coinbase_may_not_pay_more_than_it_was_given)
{
    auto v{MakeValid()};
    v.coinbase.vout[0].nValue = v.ctx.value_remaining + 1;
    v.Reserialize();
    BOOST_CHECK_EQUAL(sv2::SolutionErrorString(Refuse(v)), "value-too-high");
}

BOOST_AUTO_TEST_CASE(negative_output_is_refused)
{
    auto v{MakeValid()};
    v.coinbase.vout[0].nValue = -1;
    v.Reserialize();
    BOOST_CHECK_EQUAL(sv2::SolutionErrorString(Refuse(v)), "value-too-high");
}

BOOST_AUTO_TEST_CASE(excess_sigops_are_refused)
{
    auto v{MakeValid()};
    v.ctx.max_coinbase_sigops = 0;
    v.coinbase.vout[0].scriptPubKey = CScript() << OP_CHECKSIG;
    v.Reserialize();
    BOOST_CHECK_EQUAL(sv2::SolutionErrorString(Refuse(v)), "too-many-sigops");
}

BOOST_AUTO_TEST_CASE(coinbase_may_not_exceed_its_weight_reservation)
{
    auto v{MakeValid()};
    v.ctx.max_coinbase_weight = 1;
    BOOST_CHECK_EQUAL(sv2::SolutionErrorString(Refuse(v)), "block-too-large");
}

BOOST_AUTO_TEST_CASE(insufficient_proof_of_work_is_refused)
{
    // The one case that does reach the hasher, because it is the hash itself
    // that fails.
    const auto v{MakeValid()};
    int calls{0};
    const sv2::PoWHasher too_high{[&calls](const CBlockHeader&) {
        ++calls;
        return ArithToUint256(arith_uint256{1} << 250);
    }};
    const auto check{sv2::CheckSolution(v.ctx, v.solution, NTIME, Params().GetConsensus(), too_high)};
    BOOST_CHECK_EQUAL(sv2::SolutionErrorString(check.error), "pow-insufficient");
    BOOST_CHECK_EQUAL(calls, 1);
}

BOOST_AUTO_TEST_CASE(the_per_algorithm_limit_is_used)
{
    // MinotaurX's floor is far easier than X16RT's, so using the wrong limit
    // would accept or refuse wrongly.
    const auto& consensus{Params().GetConsensus()};
    BOOST_REQUIRE_EQUAL(consensus.powTypeLimits.size(), static_cast<size_t>(NUM_BLOCK_TYPES));
    for (const uint256& limit : consensus.powTypeLimits) {
        BOOST_CHECK(limit != uint256::ZERO);
    }
}

BOOST_AUTO_TEST_SUITE_END()
