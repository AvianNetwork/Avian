// Copyright (c) 2024-present The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

// RIP-25: ML-DSA-44 fuzz targets that check answers, not only memory safety.
//
// The mldsa44 target feeds the verifier arbitrary bytes and discards the result.
// The targets here know what the result must be:
//
//   mldsa44_verify_differential  mutates a valid signature and asserts that the
//                                CPU-dispatched verifier and the portable C
//                                reference agree, and accept only the original.
//   mldsa44_spend                derives a key, signs a fuzzed transaction through
//                                the production signer and asserts the consensus
//                                verifier accepts it and rejects a tampered copy.
//   mldsa44_descriptor           parses mldsa44() descriptors and asserts the
//                                parse, expand, cache, address and sign paths
//                                are consistent with each other.
//
// All three need a WITH_LIBOQS=ON build and return immediately without one.

#include <addresstype.h>
#include <chainparams.h>
#include <consensus/amount.h>
#include <crypto/mldsa.h>
#include <key.h>
#include <key_io.h>
#include <outputtype.h>
#include <policy/policy.h>
#include <pqkey.h>
#include <primitives/transaction.h>
#include <random.h>
#include <script/descriptor.h>
#include <script/interpreter.h>
#include <script/script.h>
#include <script/script_error.h>
#include <script/sign.h>
#include <script/signingprovider.h>
#include <span.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <test/fuzz/util.h>
#include <uint256.h>
#include <util/chaintype.h>
#include <util/string.h>

#ifdef HAVE_LIBOQS
#include <oqs/rand.h>
#endif

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace {

using Bytes = std::vector<unsigned char>;

// ML-DSA signing is randomized. Route liboqs's randomness through a stream that
// each target reseeds from its input, so a given input always produces the same
// signature and a failing input reproduces.
std::optional<FastRandomContext> g_signing_rng;

#ifdef HAVE_LIBOQS
void SigningRandomBytes(uint8_t* out, size_t len)
{
    g_signing_rng->fillrand(std::as_writable_bytes(std::span{out, len}));
}
#endif

void SeedSigningRng(const uint256& seed)
{
    g_signing_rng.emplace(seed);
#ifdef HAVE_LIBOQS
    OQS_randombytes_custom_algorithm(&SigningRandomBytes);
#endif
}

std::array<uint8_t, mldsa::SEED_SIZE> ConsumeSeed(FuzzedDataProvider& fdp)
{
    std::array<uint8_t, mldsa::SEED_SIZE> seed{};
    const Bytes bytes = ConsumeFixedLengthByteVector(fdp, seed.size());
    std::copy(bytes.begin(), bytes.end(), seed.begin());
    return seed;
}

// XOR `mask` into `target` starting at `offset`, ignoring what does not fit.
void XorAt(std::span<uint8_t> target, size_t offset, const Bytes& mask)
{
    for (size_t i = 0; i < mask.size() && offset + i < target.size(); ++i) target[offset + i] ^= mask[i];
}

bool VerifySpend(const CMutableTransaction& mtx, unsigned int n_in, const CScript& spk, CAmount amount, unsigned int flags, ScriptError& err)
{
    const CTransaction tx{mtx};
    err = SCRIPT_ERR_UNKNOWN_ERROR;
    return VerifyScript(tx.vin[n_in].scriptSig, spk, &tx.vin[n_in].scriptWitness, flags,
                        TransactionSignatureChecker(&tx, n_in, amount, MissingDataBehavior::ASSERT_FAIL), &err);
}

constexpr unsigned int CONSENSUS_FLAGS{SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_WITNESS | SCRIPT_VERIFY_MLDSA44};

// Sign input `n_in` of `mtx` as a spend of the witness-v2 output `spk` with the
// production signer, and assert the result is the canonical [sig, pubkey]
// witness that the consensus verifier and both ML-DSA backends accept.
void SignAndCheckSpend(CMutableTransaction& mtx, unsigned int n_in, const CScript& spk, CAmount amount, const SigningProvider& provider)
{
    mtx.vin[n_in].scriptSig.clear();
    mtx.vin[n_in].scriptWitness.SetNull();

    SignatureData sigdata;
    assert(ProduceSignature(provider, MutableTransactionSignatureCreator(mtx, n_in, amount, SIGHASH_ALL), spk, sigdata));
    UpdateInput(mtx.vin[n_in], sigdata);

    const auto& stack = mtx.vin[n_in].scriptWitness.stack;
    assert(mtx.vin[n_in].scriptSig.empty());
    assert(stack.size() == 2);
    assert(stack[0].size() == mldsa::SIG_SIZE);
    assert(stack[1].size() == mldsa::PUBKEY_SIZE);

    ScriptError err;
    assert(VerifySpend(mtx, n_in, spk, amount, CONSENSUS_FLAGS, err));
    assert(err == SCRIPT_ERR_OK);
    assert(VerifySpend(mtx, n_in, spk, amount, STANDARD_SCRIPT_VERIFY_FLAGS, err));

    // The same signature at the crypto layer, on both backends.
    const uint256 sighash = SignatureHash(spk, mtx, n_in, SIGHASH_ALL | SIGHASH_FORKID, amount, SigVersion::WITNESS_V0);
    const std::span<const uint8_t, mldsa::SIG_SIZE> sig{stack[0].data(), mldsa::SIG_SIZE};
    const std::span<const uint8_t, mldsa::PUBKEY_SIZE> pk{stack[1].data(), mldsa::PUBKEY_SIZE};
    assert(mldsa::Verify(sig, sighash, GetMLDsa44DomainContext(), pk));
    assert(mldsa::VerifyReference(sig, sighash, GetMLDsa44DomainContext(), pk));
}

// ---------------------------------------------------------------------------
// mldsa44_verify_differential
// ---------------------------------------------------------------------------

struct ValidCase {
    std::array<uint8_t, mldsa::PUBKEY_SIZE> pk;
    std::array<uint8_t, mldsa::SIG_SIZE> sig;
    Bytes msg;
    Bytes ctx;
};
ValidCase g_valid;

// Signature layout (FIPS 204, ML-DSA-44): c_tilde (32) || z (2304) || hints (84).
constexpr size_t CTILDE_SIZE{32};
constexpr size_t HINT_SIZE{84};
constexpr size_t Z_SIZE{mldsa::SIG_SIZE - CTILDE_SIZE - HINT_SIZE};

void initialize_mldsa44_verify_differential()
{
    if (!mldsa::IsAvailable()) return;
    SeedSigningRng(uint256::ONE);

    std::array<uint8_t, mldsa::SEED_SIZE> seed;
    for (size_t i = 0; i < seed.size(); ++i) seed[i] = static_cast<uint8_t>(0x51 + i);
    std::array<uint8_t, mldsa::SECRETKEY_SIZE> sk;
    assert(mldsa::KeyGenFromSeed(g_valid.pk, sk, seed));

    g_valid.msg.resize(32);
    for (size_t i = 0; i < g_valid.msg.size(); ++i) g_valid.msg[i] = static_cast<uint8_t>(0xC0 + i);
    const std::string ctx{"AVN/ML-DSA-44/fuzz"};
    g_valid.ctx.assign(ctx.begin(), ctx.end());

    assert(mldsa::Sign(g_valid.sig, g_valid.msg, g_valid.ctx, sk));
    assert(mldsa::Verify(g_valid.sig, g_valid.msg, g_valid.ctx, g_valid.pk));
    assert(mldsa::VerifyReference(g_valid.sig, g_valid.msg, g_valid.ctx, g_valid.pk));
}

void initialize_mldsa44_spend()
{
    SelectParams(ChainType::REGTEST);
    SeedSigningRng(uint256::ONE);
}

void initialize_mldsa44_descriptor()
{
    static ECC_Context ecc_context{};
    SelectParams(ChainType::REGTEST);
    SeedSigningRng(uint256::ONE);
}

} // namespace

FUZZ_TARGET(mldsa44_verify_differential, .init = initialize_mldsa44_verify_differential)
{
    if (!mldsa::IsAvailable()) return;
    FuzzedDataProvider fdp(buffer.data(), buffer.size());

    // Start from a valid (signature, public key, message, context) and let the
    // input edit it. The empty input is therefore a signature that verifies, and
    // small inputs are near-valid signatures that get past the cheap structural
    // checks into the arithmetic. Whole-value replacement keeps arbitrary inputs
    // reachable as well.
    auto sig = g_valid.sig;
    auto pk = g_valid.pk;
    Bytes msg = g_valid.msg;
    Bytes ctx = g_valid.ctx;

    LIMITED_WHILE(fdp.ConsumeBool(), 64)
    {
        CallOneOf(
            fdp,
            [&] { XorAt(std::span{sig}.first(CTILDE_SIZE), fdp.ConsumeIntegralInRange<size_t>(0, CTILDE_SIZE - 1), ConsumeRandomLengthByteVector(fdp, CTILDE_SIZE)); },
            [&] { XorAt(std::span{sig}.subspan(CTILDE_SIZE, Z_SIZE), fdp.ConsumeIntegralInRange<size_t>(0, Z_SIZE - 1), ConsumeRandomLengthByteVector(fdp, 64)); },
            [&] { XorAt(std::span{sig}.last(HINT_SIZE), fdp.ConsumeIntegralInRange<size_t>(0, HINT_SIZE - 1), ConsumeRandomLengthByteVector(fdp, HINT_SIZE)); },
            [&] { XorAt(pk, fdp.ConsumeIntegralInRange<size_t>(0, pk.size() - 1), ConsumeRandomLengthByteVector(fdp, 64)); },
            [&] { msg = ConsumeRandomLengthByteVector(fdp, 128); },
            // One past the FIPS 204 limit of 255 so the length gate is reached.
            [&] { ctx = ConsumeRandomLengthByteVector(fdp, 256); },
            [&] {
                const Bytes raw = fdp.ConsumeBytes<unsigned char>(sig.size());
                sig.fill(0);
                std::copy(raw.begin(), raw.end(), sig.begin());
            },
            [&] {
                const Bytes raw = fdp.ConsumeBytes<unsigned char>(pk.size());
                pk.fill(0);
                std::copy(raw.begin(), raw.end(), pk.begin());
            });
    }

    const bool unchanged = sig == g_valid.sig && pk == g_valid.pk && msg == g_valid.msg && ctx == g_valid.ctx;
    const bool dispatched = mldsa::Verify(sig, msg, ctx, pk);
    const bool reference = mldsa::VerifyReference(sig, msg, ctx, pk);

    // A disagreement here would let two CPUs disagree on a block.
    assert(dispatched == reference);
    // ML-DSA is strongly unforgeable: nothing but the original tuple may verify.
    assert(dispatched == unchanged);
}

FUZZ_TARGET(mldsa44_spend, .init = initialize_mldsa44_spend)
{
    if (!mldsa::IsAvailable()) return;
    FuzzedDataProvider fdp(buffer.data(), buffer.size());
    SeedSigningRng(ConsumeUInt256(fdp));

    auto key = std::make_shared<CPQKey>();
    assert(key->SetSeed(ConsumeSeed(fdp)));
    const uint256 program = key->GetPubKey().GetWitnessProgram();
    FlatSigningProvider provider;
    provider.pq_keys.emplace(program, key);

    const CScript spk = GetScriptForDestination(WitnessV2MLDsa44{program});
    assert(spk == (CScript() << OP_2 << Bytes(program.begin(), program.end())));

    CMutableTransaction mtx = ConsumeTransaction(fdp, std::nullopt);
    if (mtx.vin.empty()) return;
    const unsigned int n_in = fdp.ConsumeIntegralInRange<unsigned int>(0, mtx.vin.size() - 1);
    const CAmount amount = ConsumeMoney(fdp);

    SignAndCheckSpend(mtx, n_in, spk, amount, provider);

    // The key also signs and verifies messages and contexts of any length.
    {
        const Bytes msg = ConsumeRandomLengthByteVector(fdp, 128);
        const Bytes ctx = ConsumeRandomLengthByteVector(fdp, 255);
        Bytes sig;
        assert(key->Sign(sig, msg, ctx));
        const std::span<const uint8_t, mldsa::SIG_SIZE> s{sig.data(), mldsa::SIG_SIZE};
        assert(mldsa::Verify(s, msg, ctx, key->GetPubKey().GetData()));
        assert(mldsa::VerifyReference(s, msg, ctx, key->GetPubKey().GetData()));
    }

    // Change one thing the signature commits to, or one thing about the witness,
    // and the spend must fail with the matching error.
    CMutableTransaction bad = mtx;
    CAmount bad_amount = amount;
    ScriptError expected = SCRIPT_ERR_PQ_SIGNATURE_VERIFY_FAILED;
    const uint32_t delta = fdp.ConsumeIntegralInRange<uint32_t>(1, 0xFFFFFFFF);
    const uint8_t byte_delta = fdp.ConsumeIntegralInRange<uint8_t>(1, 0xFF);
    auto& bad_stack = bad.vin[n_in].scriptWitness.stack;
    CallOneOf(
        fdp,
        [&] { bad_amount = amount ^ static_cast<CAmount>(delta); },
        [&] { bad.nLockTime ^= delta; },
        [&] { bad.version ^= delta; },
        [&] { bad.vin[n_in].nSequence ^= delta; },
        [&] { bad.vin[n_in].prevout.n ^= delta; },
        [&] { bad.vout.emplace_back(ConsumeMoney(fdp), CScript{}); },
        [&] { bad_stack[0][fdp.ConsumeIntegralInRange<size_t>(0, mldsa::SIG_SIZE - 1)] ^= byte_delta; },
        [&] {
            bad_stack[1][fdp.ConsumeIntegralInRange<size_t>(0, mldsa::PUBKEY_SIZE - 1)] ^= byte_delta;
            expected = SCRIPT_ERR_PQ_WITNESS_PROGRAM_MISMATCH;
        },
        [&] {
            bad_stack[0].push_back(SIGHASH_ALL | SIGHASH_FORKID);
            expected = SCRIPT_ERR_PQ_SIGNATURE_SIZE;
        },
        [&] {
            bad_stack[1].pop_back();
            expected = SCRIPT_ERR_PQ_PUBKEY_SIZE;
        },
        [&] {
            bad_stack.emplace_back();
            expected = SCRIPT_ERR_WITNESS_PROGRAM_MISMATCH;
        },
        [&] {
            bad.vin[n_in].scriptSig = CScript() << OP_TRUE;
            expected = SCRIPT_ERR_WITNESS_MALLEATED;
        });

    ScriptError err;
    assert(!VerifySpend(bad, n_in, spk, bad_amount, CONSENSUS_FLAGS, err));
    assert(err == expected);
}

FUZZ_TARGET(mldsa44_descriptor, .init = initialize_mldsa44_descriptor)
{
    if (!mldsa::IsAvailable()) return;
    FuzzedDataProvider fdp(buffer.data(), buffer.size());
    SeedSigningRng(ConsumeUInt256(fdp));
    std::string error;

    // Arbitrary argument text: the parser must reject or accept it cleanly, and
    // whatever it accepts must expand to a witness-v2 output or not at all.
    if (fdp.ConsumeBool()) {
        FlatSigningProvider keys;
        const auto parsed = Parse("mldsa44(" + fdp.ConsumeRandomLengthString(160) + ")", keys, error);
        for (const auto& desc : parsed) {
            assert(desc);
            assert(desc->IsRange());
            assert(desc->GetOutputType() == OutputType::PQ);
            (void)desc->ToString();
            std::vector<CScript> scripts;
            FlatSigningProvider out;
            if (desc->Expand(fdp.ConsumeIntegralInRange<int>(0, 0x7FFFFFFF), keys, scripts, out)) {
                int version{-1};
                Bytes program;
                assert(scripts.size() == 1 && scripts[0].IsWitnessProgram(version, program));
                assert(version == 2 && program.size() == 32);
            }
        }
        return;
    }

    // A well-formed descriptor over a fuzzed root key and path.
    CExtKey root;
    const Bytes root_seed = ConsumeFixedLengthByteVector(fdp, 32);
    root.SetSeed(MakeByteSpan(root_seed));
    const bool apostrophe = fdp.ConsumeBool();
    const std::string hardened_marker{apostrophe ? "'" : "h"};
    std::string path;
    const int depth = fdp.ConsumeIntegralInRange<int>(1, 5);
    for (int i = 0; i < depth; ++i) {
        path += "/" + util::ToString(fdp.ConsumeIntegralInRange<uint32_t>(0, 0x7FFFFFFF));
        if (fdp.ConsumeBool()) path += hardened_marker;
    }
    path += "/*" + hardened_marker;

    FlatSigningProvider keys;
    const auto parsed = Parse("mldsa44(" + EncodeExtKey(root) + path + ")", keys, error);
    assert(parsed.size() == 1);
    const Descriptor& desc = *parsed[0];
    assert(desc.IsRange());
    assert(desc.IsSolvable());
    assert(desc.GetOutputType() == OutputType::PQ);
    assert(desc.MaxSatisfactionElems() == 2);

    // Public and private string forms parse back to the same descriptor.
    const std::string pub_str = desc.ToString();
    FlatSigningProvider pub_keys;
    const auto reparsed = Parse(pub_str, pub_keys, error, /*require_checksum=*/true);
    assert(reparsed.size() == 1 && reparsed[0]->ToString() == pub_str);
    std::string priv_str;
    assert(desc.ToPrivateString(keys, priv_str));
    FlatSigningProvider priv_keys;
    const auto reparsed_priv = Parse(priv_str, priv_keys, error, /*require_checksum=*/true);
    assert(reparsed_priv.size() == 1 && reparsed_priv[0]->ToString() == pub_str);

    // Expansion yields one witness-v2 output and the key that controls it.
    const int pos = fdp.ConsumeIntegralInRange<int>(0, 0x7FFFFFFF);
    DescriptorCache cache;
    std::vector<CScript> scripts;
    FlatSigningProvider out;
    assert(desc.Expand(pos, keys, scripts, out, &cache));
    assert(scripts.size() == 1);
    assert(static_cast<int64_t>(scripts[0].size()) == desc.ScriptSize());
    int version{-1};
    Bytes program_bytes;
    assert(scripts[0].IsWitnessProgram(version, program_bytes));
    assert(version == 2 && program_bytes.size() == 32);
    const uint256 program{program_bytes};
    CPQKey derived;
    assert(out.GetPQKey(program, derived));
    assert(derived.GetPubKey().GetWitnessProgram() == program);

    // The same output from the private-key reparse, from the cache, and from
    // ExpandPrivate; none from the public form, because every step that reaches
    // an ML-DSA key is hardened.
    {
        std::vector<CScript> again;
        FlatSigningProvider again_out;
        assert(reparsed_priv[0]->Expand(pos, priv_keys, again, again_out));
        assert(again == scripts);

        std::vector<CScript> cached;
        FlatSigningProvider cached_out;
        assert(desc.ExpandFromCache(pos, cache, cached, cached_out));
        assert(cached == scripts);
        assert(cached_out.HavePQKey(program));

        FlatSigningProvider priv_out;
        desc.ExpandPrivate(pos, keys, priv_out);
        CPQKey from_private;
        assert(priv_out.GetPQKey(program, from_private));
        assert(from_private.GetPubKey() == derived.GetPubKey());

        std::vector<CScript> none;
        FlatSigningProvider none_out;
        assert(!reparsed[0]->Expand(pos, pub_keys, none, none_out));
    }

    // Address round trip, and the address-form descriptor inferred for it.
    CTxDestination dest;
    assert(ExtractDestination(scripts[0], dest));
    assert(std::holds_alternative<WitnessV2MLDsa44>(dest));
    assert(DecodeDestination(EncodeDestination(dest)) == dest);
    assert(GetScriptForDestination(dest) == scripts[0]);
    const auto inferred = InferDescriptor(scripts[0], out);
    assert(inferred);
    assert(inferred->GetOutputType() == OutputType::PQ);
    assert(inferred->MaxSatisfactionWeight(true) == desc.MaxSatisfactionWeight(true));
    assert(inferred->MaxSatisfactionElems() == desc.MaxSatisfactionElems());

    // The derived key spends the derived output.
    CMutableTransaction mtx = ConsumeTransaction(fdp, std::nullopt, /*max_num_in=*/3, /*max_num_out=*/3);
    if (mtx.vin.empty()) return;
    const unsigned int n_in = fdp.ConsumeIntegralInRange<unsigned int>(0, mtx.vin.size() - 1);
    SignAndCheckSpend(mtx, n_in, scripts[0], ConsumeMoney(fdp), out);
}
