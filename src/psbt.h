// Copyright (c) 2009-2022 The Bitcoin Core developers
// Copyright (c) 2025 The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef AVIAN_PSBT_H
#define AVIAN_PSBT_H

#include <map>
#include <vector>

#include <amount.h>
#include <boost/optional.hpp>
#include <policy/feerate.h>
#include <uint256.h>

#include "crypto/common.h"
#include "primitives/transaction.h"
#include "pubkey.h"
#include "script/script.h"
#include "script/sign.h"
#include "serialize.h"

// BIP 174 PSBT format constants
const unsigned char PSBT_MAGIC_BYTES[5] = {0x70, 0x73, 0x62, 0x74, 0xff}; // 'psbt' + 0xff

// Global section key types
const unsigned char PSBT_GLOBAL_UNSIGNED_TX = 0x00;
const unsigned char PSBT_GLOBAL_VERSION = 0xfb;

// Input section key types (BIP 174)
const unsigned char PSBT_IN_NON_WITNESS_UTXO = 0x00;
const unsigned char PSBT_IN_WITNESS_UTXO = 0x01;
const unsigned char PSBT_IN_PARTIAL_SIG = 0x02;
const unsigned char PSBT_IN_SIGHASH = 0x03;
const unsigned char PSBT_IN_REDEEM_SCRIPT = 0x04;
const unsigned char PSBT_IN_WITNESS_SCRIPT = 0x05;
const unsigned char PSBT_IN_BIP32_DERIVATION = 0x06;
const unsigned char PSBT_IN_FINAL_SCRIPTSIG = 0x07;
const unsigned char PSBT_IN_FINAL_SCRIPTWITNESS = 0x08;

// Output section key types (BIP 174)
const unsigned char PSBT_OUT_REDEEM_SCRIPT = 0x00;
const unsigned char PSBT_OUT_WITNESS_SCRIPT = 0x01;
const unsigned char PSBT_OUT_BIP32_DERIVATION = 0x02;

// Serialization helpers: use vector-based key/value READWRITE to work with both
// CDataStream (runtime serialization) and CSizeComputer (size calculation).

/** PSBTInput holds all the input-related information for signing */
struct PSBTInput {
    CTransactionRef utxo;                                       // The non-witness serialization of the previous output
    CTxOut txout;                                               // The output being spent
    std::map<CPubKey, std::vector<unsigned char>> partial_sigs; // Partial signatures from different signers
    std::vector<unsigned char> final_script_sig;                // Final scriptSig after signing
    std::vector<unsigned char> final_script_witness;            // Final scriptWitness after signing

    CScript redeem_script;                                     // Redeem script (if any)
    CScript witness_script;                                    // Witness script (if any)
    std::map<CPubKey, std::vector<unsigned char>> hd_keypaths; // BIP32 derivations: pubkey -> serialized derivation data

    std::map<std::vector<unsigned char>, std::vector<unsigned char>> unknown; // Unknown fields

    PSBTInput() : utxo(MakeTransactionRef()) {}

    bool IsSigned() const
    {
        return !final_script_sig.empty() || !final_script_witness.empty();
    }

    // Merge another PSBTInput into this one
    void Merge(const PSBTInput& input);

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action)
    {
        // Serialize/deserialize PSBT input as key-value map (BIP 174)
        if (!ser_action.ForRead()) {
            // Writing: build key/value vectors and use READWRITE so CSizeComputer works
            std::vector<unsigned char> key;
            std::vector<unsigned char> value;

            // PSBT_IN_NON_WITNESS_UTXO (0x00) - the full previous transaction (serialized without witness data per BIP 174)
            // Only write if the transaction has actual inputs or outputs (not just the default empty one)
            if (utxo && (utxo->vin.size() > 0 || utxo->vout.size() > 0)) {
                key.clear();
                key.push_back(PSBT_IN_NON_WITNESS_UTXO);
                CDataStream ds(SER_NETWORK, PROTOCOL_VERSION | SERIALIZE_TRANSACTION_NO_WITNESS);
                ds << *utxo;
                value.assign(ds.begin(), ds.end());
                READWRITE(key);
                READWRITE(value);
            }

            // PSBT_IN_WITNESS_UTXO (0x01) - the output being spent (for witness inputs)
            // Write if txout has been set (nValue != -1 is a heuristic; could also check scriptPubKey)
            if (txout.nValue >= 0) {
                key.clear();
                key.push_back(PSBT_IN_WITNESS_UTXO);
                CDataStream ds(SER_NETWORK, PROTOCOL_VERSION);
                ds << txout;
                value.assign(ds.begin(), ds.end());
                READWRITE(key);
                READWRITE(value);
            }

            // PSBT_IN_FINAL_SCRIPTSIG (0x07)
            if (!final_script_sig.empty()) {
                key.clear();
                key.push_back(PSBT_IN_FINAL_SCRIPTSIG);
                value = final_script_sig;
                READWRITE(key);
                READWRITE(value);
            }

            // PSBT_IN_FINAL_SCRIPTWITNESS (0x08)
            if (!final_script_witness.empty()) {
                key.clear();
                key.push_back(PSBT_IN_FINAL_SCRIPTWITNESS);
                value = final_script_witness;
                READWRITE(key);
                READWRITE(value);
            }

            // PSBT_IN_PARTIAL_SIG (0x02) - key is type + pubkey bytes, value is signature
            for (const auto& sig_pair : partial_sigs) {
                key.clear();
                key.push_back(PSBT_IN_PARTIAL_SIG);
                const std::vector<unsigned char> pubkey_data(sig_pair.first.begin(), sig_pair.first.end());
                key.insert(key.end(), pubkey_data.begin(), pubkey_data.end());
                value = sig_pair.second;
                READWRITE(key);
                READWRITE(value);
            }

            // PSBT_IN_REDEEM_SCRIPT (0x04)
            if (!redeem_script.empty()) {
                key.clear();
                key.push_back(PSBT_IN_REDEEM_SCRIPT);
                CDataStream ds(SER_NETWORK, PROTOCOL_VERSION);
                ds << redeem_script;
                value.assign(ds.begin(), ds.end());
                READWRITE(key);
                READWRITE(value);
            }

            // PSBT_IN_WITNESS_SCRIPT (0x05)
            if (!witness_script.empty()) {
                key.clear();
                key.push_back(PSBT_IN_WITNESS_SCRIPT);
                CDataStream ds(SER_NETWORK, PROTOCOL_VERSION);
                ds << witness_script;
                value.assign(ds.begin(), ds.end());
                READWRITE(key);
                READWRITE(value);
            }

            // PSBT_IN_BIP32_DERIVATION (0x06) - key is type + pubkey bytes, value is serialized derivation data
            for (const auto& kv : hd_keypaths) {
                key.clear();
                key.push_back(PSBT_IN_BIP32_DERIVATION);
                const std::vector<unsigned char> pubkey_data(kv.first.begin(), kv.first.end());
                key.insert(key.end(), pubkey_data.begin(), pubkey_data.end());
                value = kv.second;
                READWRITE(key);
                READWRITE(value);
            }

            // Unknown fields (keys already include type byte and any key data)
            for (const auto& unknown_pair : unknown) {
                key = unknown_pair.first;
                value = unknown_pair.second;
                READWRITE(key);
                READWRITE(value);
            }

            // Map terminator: write empty key
            key.clear();
            READWRITE(key);
        } else {
            // Reading: read key/value pairs until empty key terminator
            std::vector<unsigned char> key;
            std::vector<unsigned char> value;
            while (true) {
                READWRITE(key);
                if (key.empty()) break;
                READWRITE(value);

                unsigned char key_type = key[0];
                if (key_type == PSBT_IN_NON_WITNESS_UTXO && key.size() == 1) {
                    CDataStream ds(value, SER_NETWORK, PROTOCOL_VERSION | SERIALIZE_TRANSACTION_NO_WITNESS);
                    CMutableTransaction prev_tx;
                    ds >> prev_tx;
                    utxo = MakeTransactionRef(prev_tx);
                } else if (key_type == PSBT_IN_WITNESS_UTXO && key.size() == 1) {
                    CDataStream ds(value, SER_NETWORK, PROTOCOL_VERSION);
                    ds >> txout;
                } else if (key_type == PSBT_IN_PARTIAL_SIG && key.size() > 1) {
                    std::vector<unsigned char> pubkey_data(key.begin() + 1, key.end());
                    CPubKey pubkey(pubkey_data);
                    partial_sigs[pubkey] = value;
                } else if (key_type == PSBT_IN_FINAL_SCRIPTSIG && key.size() == 1) {
                    final_script_sig = value;
                } else if (key_type == PSBT_IN_FINAL_SCRIPTWITNESS && key.size() == 1) {
                    final_script_witness = value;
                } else if (key_type == PSBT_IN_REDEEM_SCRIPT && key.size() == 1) {
                    CDataStream ds(value, SER_NETWORK, PROTOCOL_VERSION);
                    ds >> redeem_script;
                } else if (key_type == PSBT_IN_WITNESS_SCRIPT && key.size() == 1) {
                    CDataStream ds(value, SER_NETWORK, PROTOCOL_VERSION);
                    ds >> witness_script;
                } else if (key_type == PSBT_IN_BIP32_DERIVATION && key.size() > 1) {
                    std::vector<unsigned char> pubkey_data(key.begin() + 1, key.end());
                    CPubKey pubkey(pubkey_data);
                    hd_keypaths[pubkey] = value;
                } else {
                    unknown[key] = value;
                }
            }
        }
    }
};

/** PSBTOutput holds all the output-related information */
struct PSBTOutput {
    CScript redeem_script;  // Redeemed output script
    CScript witness_script; // Witness script

    std::map<std::vector<unsigned char>, std::vector<unsigned char>> unknown; // Unknown fields

    PSBTOutput() {}

    // Merge another PSBTOutput into this one
    void Merge(const PSBTOutput& output);

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action)
    {
        // Serialize/deserialize PSBT output as key-value map (BIP 174)
        if (!ser_action.ForRead()) {
            std::vector<unsigned char> key;
            std::vector<unsigned char> value;

            // PSBT_OUT_REDEEM_SCRIPT (0x00)
            if (!redeem_script.empty()) {
                key.clear();
                key.push_back(PSBT_OUT_REDEEM_SCRIPT);
                CDataStream ds(SER_NETWORK, PROTOCOL_VERSION);
                ds << redeem_script;
                value.assign(ds.begin(), ds.end());
                READWRITE(key);
                READWRITE(value);
            }

            // PSBT_OUT_WITNESS_SCRIPT (0x01)
            if (!witness_script.empty()) {
                key.clear();
                key.push_back(PSBT_OUT_WITNESS_SCRIPT);
                CDataStream ds(SER_NETWORK, PROTOCOL_VERSION);
                ds << witness_script;
                value.assign(ds.begin(), ds.end());
                READWRITE(key);
                READWRITE(value);
            }

            // Unknown fields
            for (const auto& unknown_pair : unknown) {
                key = unknown_pair.first;
                value = unknown_pair.second;
                READWRITE(key);
                READWRITE(value);
            }

            // Map terminator (empty key)
            key.clear();
            READWRITE(key);
        } else {
            std::vector<unsigned char> key;
            std::vector<unsigned char> value;
            while (true) {
                READWRITE(key);
                if (key.empty()) break;
                READWRITE(value);

                unsigned char key_type = key[0];
                if (key_type == PSBT_OUT_REDEEM_SCRIPT && key.size() == 1) {
                    CDataStream ds(value, SER_NETWORK, PROTOCOL_VERSION);
                    ds >> redeem_script;
                } else if (key_type == PSBT_OUT_WITNESS_SCRIPT && key.size() == 1) {
                    CDataStream ds(value, SER_NETWORK, PROTOCOL_VERSION);
                    ds >> witness_script;
                } else {
                    unknown[key] = value;
                }
            }
        }
    }
};

/**
 * PartiallySignedTransaction represents a transaction that may not be fully signed yet.
 * It follows BIP 174 specification.
 */
struct PartiallySignedTransaction {
    CMutableTransaction tx;
    std::vector<PSBTInput> inputs;
    std::vector<PSBTOutput> outputs;
    std::map<std::vector<unsigned char>, std::vector<unsigned char>> unknown; // Unknown fields
    uint32_t m_version = 0;

    PartiallySignedTransaction() {}

    explicit PartiallySignedTransaction(const CMutableTransaction& txIn) : tx(txIn)
    {
        inputs.resize(txIn.vin.size());
        outputs.resize(txIn.vout.size());
        ClearUnsignedTxScripts();
    }

    bool IsNull() const { return tx.vin.empty() && tx.vout.empty(); }

    bool IsSigned() const
    {
        for (const auto& input : inputs) {
            if (!input.IsSigned()) {
                return false;
            }
        }
        return true;
    }

    size_t GetSerializeSize() const
    {
        return ::GetSerializeSize(*this, SER_NETWORK, PROTOCOL_VERSION);
    }

    std::string GetHex() const;
    static PartiallySignedTransaction FromHex(const std::string& hex);

    /** Merge another PSBT into this one. Requires same underlying transaction. */
    void Merge(const PartiallySignedTransaction& psbt);

    /** Add an input to this PSBT. */
    void AddInput(const CTxIn& txin);

    /** Add an output to this PSBT. */
    void AddOutput(const CTxOut& txout);

    void ClearUnsignedTxScripts();

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action)
    {
        // Write/read PSBT magic bytes (5 bytes: "psbt" + 0xff, no size prefix per BIP 174)
        // Serialize each byte individually to avoid compact size encoding
        uint8_t magic0 = PSBT_MAGIC_BYTES[0];
        uint8_t magic1 = PSBT_MAGIC_BYTES[1];
        uint8_t magic2 = PSBT_MAGIC_BYTES[2];
        uint8_t magic3 = PSBT_MAGIC_BYTES[3];
        uint8_t magic4 = PSBT_MAGIC_BYTES[4];

        READWRITE(magic0);
        READWRITE(magic1);
        READWRITE(magic2);
        READWRITE(magic3);
        READWRITE(magic4);

        // Verify magic bytes when reading
        if (ser_action.ForRead()) {
            if (magic0 != PSBT_MAGIC_BYTES[0] || magic1 != PSBT_MAGIC_BYTES[1] ||
                magic2 != PSBT_MAGIC_BYTES[2] || magic3 != PSBT_MAGIC_BYTES[3] ||
                magic4 != PSBT_MAGIC_BYTES[4]) {
                throw std::runtime_error("Invalid PSBT magic bytes");
            }
        }

        // Global section: key-value map format (BIP 174)
        if (!ser_action.ForRead()) {
            ClearUnsignedTxScripts();
            std::vector<unsigned char> key;
            std::vector<unsigned char> value;

            // PSBT_GLOBAL_UNSIGNED_TX (0x00)
            key.clear();
            key.push_back(PSBT_GLOBAL_UNSIGNED_TX);
            {
                CDataStream ds(SER_NETWORK, PROTOCOL_VERSION);
                ds << tx;
                value.assign(ds.begin(), ds.end());
            }
            READWRITE(key);
            READWRITE(value);

            // Unknown global fields
            for (const auto& unknown_pair : unknown) {
                key = unknown_pair.first;
                value = unknown_pair.second;
                READWRITE(key);
                READWRITE(value);
            }

            // Global section terminator (empty key)
            key.clear();
            READWRITE(key);
        } else {
            std::vector<unsigned char> key;
            std::vector<unsigned char> value;
            while (true) {
                READWRITE(key);
                if (key.empty()) break;
                READWRITE(value);
                unsigned char key_type = key[0];
                if (key_type == PSBT_GLOBAL_UNSIGNED_TX && key.size() == 1) {
                    CDataStream ds(value, SER_NETWORK, PROTOCOL_VERSION);
                    ds >> tx;
                } else if (key_type == PSBT_GLOBAL_VERSION && key.size() == 1) {
                    if (value.size() != 4) {
                        throw std::runtime_error("Invalid PSBT version encoding");
                    }
                    uint32_t version = ReadLE32(value.data());
                    if (version != 0) {
                        throw std::runtime_error("Unsupported PSBT version");
                    }
                    m_version = version;
                } else {
                    unknown[key] = value;
                }
            }
        }

        // When deserializing, resize inputs and outputs to match transaction
        if (ser_action.ForRead()) {
            inputs.resize(tx.vin.size());
            outputs.resize(tx.vout.size());
        }

        // Inputs section
        for (auto& input : inputs) {
            READWRITE(input);
        }

        // Outputs section
        for (auto& output : outputs) {
            READWRITE(output);
        }
    }
};

/** Combine multiple PSBTs into one, merging partial signatures and data from each */
bool CombinePSBTs(PartiallySignedTransaction& out, const std::vector<PartiallySignedTransaction>& psbts);

/** Finalize a PSBT by extracting final signatures from partial signatures */
bool FinalizePSBT(PartiallySignedTransaction& psbtx);

/** Finalize a PSBT and extract the final transaction */
bool FinalizeAndExtractPSBT(PartiallySignedTransaction& psbtx, CMutableTransaction& result);

/**
 * Enumerates the processing roles defined by BIP 174.
 * Keeping the ordering aligned with upstream (creator < updater < signer < finalizer < extractor)
 * allows simple comparisons when determining the "next" role for a PSBT.
 */
enum class PSBTRole {
    CREATOR = 0,
    UPDATER,
    SIGNER,
    FINALIZER,
    EXTRACTOR,
};

/** Holds an analysis of one input from a PSBT. */
struct PSBTInputAnalysis {
    bool has_utxo{false};
    bool is_final{false};
    PSBTRole next{PSBTRole::CREATOR};

    std::vector<CKeyID> missing_pubkeys; //!< Reserved for future use
    std::vector<CKeyID> missing_sigs;    //!< Reserved for future use
    uint160 missing_redeem_script{};     //!< Reserved for future use
    uint256 missing_witness_script{};    //!< Reserved for future use
};

/** Holds the results of AnalyzePSBT (miscellaneous information about a PSBT). */
struct PSBTAnalysis {
    boost::optional<size_t> estimated_vsize;     //!< Estimated virtual size of the transaction
    boost::optional<CFeeRate> estimated_feerate; //!< Estimated feerate (fee / vsize)
    boost::optional<CAmount> fee;                //!< Total fee paid by the transaction
    std::vector<PSBTInputAnalysis> inputs;       //!< Information about the individual inputs
    PSBTRole next{PSBTRole::CREATOR};            //!< Which BIP 174 role should operate next
    std::string error;                           //!< Non-empty when the PSBT is malformed

    void SetInvalid(const std::string& err_msg)
    {
        estimated_vsize = boost::none;
        estimated_feerate = boost::none;
        fee = boost::none;
        inputs.clear();
        next = PSBTRole::CREATOR;
        error = err_msg;
    }
};

/** Provides assorted information about where a PSBT is in the signing workflow. */
PSBTAnalysis AnalyzePSBT(PartiallySignedTransaction psbtx);

#endif // AVIAN_PSBT_H
