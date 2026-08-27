// Copyright (c) 2024-present The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

// RIP-25: fuzz the ML-DSA-44 (witness v2) consensus verification path.
//
// A generic script fuzzer almost never reaches this path by chance: it requires a
// witness-v2 32-byte program spent by a 2-element witness at the exact 2420/1312
// sizes whose pubkey hashes to the program. This target constructs a v2 program
// and shapes the witness toward those sizes, so it exercises: witness-stack
// parsing (element count + exact-length checks), the BIP143 ML-DSA sighash
// construction, the SHA256(pubkey)==program commitment, and -- in a WITH_LIBOQS=ON
// build -- the real mldsa::Verify wrapper fed malformed-but-plausibly-sized inputs.
//
// Acceptance: no crash, no out-of-bounds/UB (the fuzzer's ASan/UBSan), no
// unbounded allocation. The verifier must reject malformed inputs cleanly; a valid
// spend is astronomically unlikely from fuzzed bytes, so a true SCRIPT_ERR_OK is
// not expected (nor asserted).

#include <consensus/amount.h>
#include <crypto/mldsa.h>
#include <crypto/sha256.h>
#include <primitives/transaction.h>
#include <script/interpreter.h>
#include <script/script.h>
#include <script/script_error.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <test/fuzz/util.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace {
// A byte vector whose size is biased toward `target` (and small deltas around it),
// so the size checks and the verifier boundary are hit often, but arbitrary sizes
// still occur.
std::vector<unsigned char> ConsumeSizedElement(FuzzedDataProvider& fdp, size_t target)
{
    size_t size;
    if (fdp.ConsumeBool()) {
        const int delta = fdp.ConsumeIntegralInRange<int>(-2, 2);
        const long long biased = static_cast<long long>(target) + delta;
        size = biased < 0 ? 0 : static_cast<size_t>(biased);
    } else {
        size = fdp.ConsumeIntegralInRange<size_t>(0, target + 8);
    }
    return fdp.ConsumeBytes<unsigned char>(size);
}
} // namespace

FUZZ_TARGET(mldsa44)
{
    FuzzedDataProvider fdp(buffer.data(), buffer.size());

    const std::vector<unsigned char> pubkey = ConsumeSizedElement(fdp, mldsa::PUBKEY_SIZE);
    const std::vector<unsigned char> sig = ConsumeSizedElement(fdp, mldsa::SIG_SIZE);

    // The 32-byte witness program: sometimes the true SHA256(pubkey) so the
    // commitment check passes and control reaches the signature verify, sometimes
    // fuzzed so the mismatch path is exercised.
    std::vector<unsigned char> program(32, 0);
    if (fdp.ConsumeBool()) {
        CSHA256().Write(pubkey.data(), pubkey.size()).Finalize(program.data());
    } else {
        program = ConsumeFixedLengthByteVector(fdp, 32);
    }
    const CScript spk = CScript() << OP_2 << program;

    // Fuzz the witness-stack shape: element count and contents. Bias the first two
    // toward [sig, pubkey] so the canonical shape is reached, but allow extra /
    // missing / arbitrary elements.
    CScriptWitness wit;
    const int n = fdp.ConsumeIntegralInRange<int>(0, 4);
    for (int i = 0; i < n; ++i) {
        if (i == 0 && fdp.ConsumeBool()) {
            wit.stack.push_back(sig);
        } else if (i == 1 && fdp.ConsumeBool()) {
            wit.stack.push_back(pubkey);
        } else {
            wit.stack.push_back(ConsumeRandomLengthByteVector(fdp, /*max_length=*/mldsa::SIG_SIZE + 8));
        }
    }

    CMutableTransaction mtx;
    mtx.vin.resize(1);
    mtx.vin[0].scriptWitness = wit;
    mtx.vout.resize(1);
    mtx.vout[0].nValue = 1;
    const CTransaction tx{mtx};
    const CAmount amount = ConsumeMoney(fdp);

    const unsigned int flags = SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_WITNESS | SCRIPT_VERIFY_MLDSA44;
    ScriptError err = SCRIPT_ERR_OK;
    (void)VerifyScript(CScript(), spk, &tx.vin[0].scriptWitness, flags,
                       TransactionSignatureChecker(&tx, 0, amount, MissingDataBehavior::ASSERT_FAIL),
                       &err);
}
