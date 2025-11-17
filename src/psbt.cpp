// Copyright (c) 2009-2022 The Bitcoin Core developers
// Copyright (c) 2025 The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "psbt.h"
#include "script/sign.h"
#include "streams.h"
#include "tinyformat.h"
#include "utilstrencodings.h"

#include "consensus/consensus.h"
#include "policy/policy.h"

#include <algorithm>

std::string PartiallySignedTransaction::GetHex() const
{
    CDataStream sstream(SER_NETWORK, PROTOCOL_VERSION);
    sstream << *this;
    return HexStr(sstream.begin(), sstream.end());
}

PartiallySignedTransaction PartiallySignedTransaction::FromHex(const std::string& hex)
{
    std::vector<unsigned char> data = ParseHex(hex);
    CDataStream stream(data, SER_NETWORK, PROTOCOL_VERSION);
    PartiallySignedTransaction psbt;
    stream >> psbt;
    return psbt;
}

void PartiallySignedTransaction::ClearUnsignedTxScripts()
{
    for (auto& txin : tx.vin) {
        txin.scriptSig.clear();
        txin.scriptWitness.SetNull();
    }
}

void PSBTInput::Merge(const PSBTInput& input)
{
    // Merge partial signatures
    for (const auto& sig : input.partial_sigs) {
        if (partial_sigs.count(sig.first) && partial_sigs.at(sig.first) != sig.second) {
            // Conflicting signature for same pubkey
            throw std::runtime_error("Conflicting partial signatures for same pubkey");
        }
        partial_sigs.insert(sig);
    }

    // Merge UTXO data (prefer non-witness if present, otherwise witness)
    if (input.utxo && input.utxo->vin.size() + input.utxo->vout.size() > 0) {
        if (!utxo || utxo->vin.size() + utxo->vout.size() == 0) {
            utxo = input.utxo;
        }
    }

    // Merge witness UTXO (txout)
    if (input.txout.nValue >= 0) {
        if (txout.nValue < 0) {
            txout = input.txout;
        }
    }

    // Merge final script data
    if (!input.final_script_sig.empty()) {
        if (final_script_sig.empty()) {
            final_script_sig = input.final_script_sig;
        }
    }
    if (!input.final_script_witness.empty()) {
        if (final_script_witness.empty()) {
            final_script_witness = input.final_script_witness;
        }
    }

    // Merge redeem script
    if (!input.redeem_script.empty()) {
        if (redeem_script.empty()) {
            redeem_script = input.redeem_script;
        }
    }

    // Merge witness script
    if (!input.witness_script.empty()) {
        if (witness_script.empty()) {
            witness_script = input.witness_script;
        }
    }

    // Merge BIP32 derivations (pubkey -> derivation data)
    for (const auto& kv : input.hd_keypaths) {
        auto it = hd_keypaths.find(kv.first);
        if (it != hd_keypaths.end() && it->second != kv.second) {
            throw std::runtime_error("Conflicting BIP32 derivations for same pubkey");
        }
        hd_keypaths.insert(kv);
    }

    // Merge unknown fields
    for (const auto& unknown_pair : input.unknown) {
        if (unknown.count(unknown_pair.first) && unknown.at(unknown_pair.first) != unknown_pair.second) {
            throw std::runtime_error("Conflicting unknown fields in PSBT input");
        }
        unknown.insert(unknown_pair);
    }
}

void PSBTOutput::Merge(const PSBTOutput& output)
{
    // Merge redeem script
    if (!output.redeem_script.empty()) {
        if (redeem_script.empty()) {
            redeem_script = output.redeem_script;
        }
    }

    // Merge witness script
    if (!output.witness_script.empty()) {
        if (witness_script.empty()) {
            witness_script = output.witness_script;
        }
    }

    // Merge unknown fields
    for (const auto& unknown_pair : output.unknown) {
        if (unknown.count(unknown_pair.first) && unknown.at(unknown_pair.first) != unknown_pair.second) {
            throw std::runtime_error("Conflicting unknown fields in PSBT output");
        }
        unknown.insert(unknown_pair);
    }
}

void PartiallySignedTransaction::Merge(const PartiallySignedTransaction& psbt)
{
    // Check that underlying transaction has the same inputs/outputs
    if (psbt.inputs.size() != inputs.size() || psbt.outputs.size() != outputs.size()) {
        throw std::runtime_error("Cannot merge PSBTs with different input/output counts");
    }

    // Merge input data
    for (size_t i = 0; i < psbt.inputs.size(); ++i) {
        inputs[i].Merge(psbt.inputs[i]);
    }

    // Merge output data
    for (size_t i = 0; i < psbt.outputs.size(); ++i) {
        outputs[i].Merge(psbt.outputs[i]);
    }

    // Merge unknown fields at PSBT level
    for (const auto& unknown_pair : psbt.unknown) {
        if (unknown.count(unknown_pair.first) && unknown.at(unknown_pair.first) != unknown_pair.second) {
            throw std::runtime_error("Conflicting unknown fields in PSBT");
        }
        unknown.insert(unknown_pair);
    }
}

void PartiallySignedTransaction::AddInput(const CTxIn& input)
{
    // Check for duplicate inputs
    for (const auto& existing_input : tx.vin) {
        if (existing_input.prevout == input.prevout) {
            throw std::runtime_error("Input already exists in transaction");
        }
    }

    // Add input to transaction
    tx.vin.push_back(input);
    tx.vin.back().scriptSig.clear();
    tx.vin.back().scriptWitness.SetNull();

    // Add corresponding PSBTInput
    PSBTInput psbt_input;
    inputs.push_back(psbt_input);
}

void PartiallySignedTransaction::AddOutput(const CTxOut& output)
{
    // Add output to transaction
    tx.vout.push_back(output);

    // Add corresponding PSBTOutput
    PSBTOutput psbt_output;
    outputs.push_back(psbt_output);
}


bool CombinePSBTs(PartiallySignedTransaction& out, const std::vector<PartiallySignedTransaction>& psbts)
{
    if (psbts.empty()) {
        return false;
    }

    // All PSBTs must have the same underlying transaction
    out = psbts[0];

    // Merge all subsequent PSBTs
    for (size_t i = 1; i < psbts.size(); ++i) {
        const PartiallySignedTransaction& psbt = psbts[i];

        // Check that underlying transaction is the same
        if (psbt.tx.GetHash() != out.tx.GetHash()) {
            return false;
        }

        try {
            // Use the new Merge method to combine PSBTs
            out.Merge(psbt);
        } catch (const std::exception& e) {
            return false;
        }
    }

    return true;
}

bool FinalizePSBT(PartiallySignedTransaction& psbtx)
{
    // Finalize input signatures by extracting the final_script_sig to scriptSig
    // In Avian, we check if all inputs have been properly signed (either script or witness)
    bool complete = true;
    for (unsigned int i = 0; i < psbtx.tx.vin.size(); ++i) {
        // Input is complete if it has EITHER final_script_sig OR final_script_witness
        if (psbtx.inputs[i].final_script_sig.empty() && psbtx.inputs[i].final_script_witness.empty()) {
            complete = false;
            break;
        }
    }

    return complete;
}

bool FinalizeAndExtractPSBT(PartiallySignedTransaction& psbtx, CMutableTransaction& result)
{
    // It's not safe to extract a PSBT that isn't finalized, and there's no easy way to check
    //   whether a PSBT is finalized without finalizing it, so we just do this.
    if (!FinalizePSBT(psbtx)) {
        return false;
    }

    result = psbtx.tx;
    for (unsigned int i = 0; i < result.vin.size(); ++i) {
        // Convert final_script_sig (vector<unsigned char>) to CScript
        if (!psbtx.inputs[i].final_script_sig.empty()) {
            result.vin[i].scriptSig = CScript(psbtx.inputs[i].final_script_sig.begin(),
                psbtx.inputs[i].final_script_sig.end());
        }

        // Handle witness script - deserialize the stack elements from the serialized blob
        if (!psbtx.inputs[i].final_script_witness.empty()) {
            result.vin[i].scriptWitness.stack.clear();
            try {
                CDataStream ss(psbtx.inputs[i].final_script_witness, SER_NETWORK, PROTOCOL_VERSION);
                uint64_t stack_size;
                ss >> stack_size;
                for (uint64_t j = 0; j < stack_size && !ss.eof(); ++j) {
                    std::vector<unsigned char> item;
                    ss >> item;
                    result.vin[i].scriptWitness.stack.push_back(item);
                }
            } catch (const std::exception&) {
                // If deserialization fails, treat the entire blob as one witness item (fallback)
                result.vin[i].scriptWitness.stack.push_back(psbtx.inputs[i].final_script_witness);
            }
        }
    }
    return true;
}

PSBTAnalysis AnalyzePSBT(PartiallySignedTransaction psbtx)
{
    PSBTAnalysis result;

    if (psbtx.inputs.size() != psbtx.tx.vin.size()) {
        result.SetInvalid("PSBT input count does not match transaction inputs");
        return result;
    }

    result.inputs.resize(psbtx.tx.vin.size());
    result.next = PSBTRole::EXTRACTOR;

    bool calc_fee = true;
    bool all_final = !psbtx.inputs.empty();
    CAmount total_in = 0;

    for (size_t i = 0; i < psbtx.tx.vin.size(); ++i) {
        const CTxIn& txin = psbtx.tx.vin[i];
        const PSBTInput& input = psbtx.inputs[i];
        PSBTInputAnalysis& analysis = result.inputs[i];

        analysis.next = PSBTRole::EXTRACTOR;
        analysis.is_final = input.IsSigned();

        CAmount amount = -1;
        if (input.utxo && txin.prevout.n < input.utxo->vout.size()) {
            amount = input.utxo->vout[txin.prevout.n].nValue;
        } else if (input.txout.nValue >= 0) {
            amount = input.txout.nValue;
        }

        if (amount < 0) {
            calc_fee = false;
            analysis.has_utxo = false;
            analysis.next = PSBTRole::UPDATER;
            result.next = std::min(result.next, analysis.next);
            all_final = false;
            continue;
        }

        if (!MoneyRange(amount) || !MoneyRange(total_in + amount)) {
            result.SetInvalid(strprintf("PSBT is not valid. Input %u has invalid value", i));
            return result;
        }

        analysis.has_utxo = true;
        total_in += amount;

        if (!analysis.is_final) {
            all_final = false;
            analysis.next = PSBTRole::SIGNER;
        } else {
            analysis.next = PSBTRole::FINALIZER;
        }

        result.next = std::min(result.next, analysis.next);
    }

    if (psbtx.tx.vin.empty()) {
        result.next = PSBTRole::CREATOR;
    }

    if (calc_fee) {
        CAmount total_out = 0;
        for (const CTxOut& out : psbtx.tx.vout) {
            total_out += out.nValue;
            if (!MoneyRange(out.nValue) || !MoneyRange(total_out)) {
                result.SetInvalid("PSBT is not valid. Output amount invalid");
                return result;
            }
        }

        if (!MoneyRange(total_in) || !MoneyRange(total_in - total_out)) {
            result.SetInvalid("PSBT is not valid. Fee calculation failed");
            return result;
        }

        result.fee = total_in - total_out;
    }

    if (all_final && !psbtx.tx.vin.empty()) {
        CMutableTransaction extracted;
        PartiallySignedTransaction tmp = psbtx;
        if (FinalizeAndExtractPSBT(tmp, extracted)) {
            const CTransaction final_tx(extracted);
            size_t vsize = GetVirtualTransactionSize(final_tx);
            result.estimated_vsize = vsize;
            if (result.fee) {
                result.estimated_feerate = CFeeRate(*result.fee, vsize);
            }
            result.next = PSBTRole::EXTRACTOR;
        } else {
            // Inputs are marked final but extraction failed; treat as needing finalization
            result.next = PSBTRole::FINALIZER;
        }
    }

    return result;
}
