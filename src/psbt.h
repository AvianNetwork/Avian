// Copyright (c) 2009-2022 The Bitcoin Core developers
// Copyright (c) 2025 The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef AVIAN_PSBT_H
#define AVIAN_PSBT_H

#include <map>
#include <vector>

#include "primitives/transaction.h"
#include "pubkey.h"
#include "script/script.h"
#include "script/sign.h"
#include "serialize.h"

// BIP 174 PSBT format constants
const unsigned char PSBT_MAGIC_BYTES[5] = {0x70, 0x73, 0x62, 0x74, 0xff}; // 'psbt' + 0xff
const unsigned char PSBT_GLOBAL_UNSIGNED_TX = 0x00;
const unsigned char PSBT_INPUT_PREVIOUS_TXID = 0x00;
const unsigned char PSBT_INPUT_PREVIOUS_TXINDEX = 0x01;
const unsigned char PSBT_INPUT_SEQUENCE = 0x04;
const unsigned char PSBT_INPUT_REQUIRED_TIME_LOCKTIME = 0x05;
const unsigned char PSBT_INPUT_REQUIRED_HEIGHT_LOCKTIME = 0x06;

/** PSBTInput holds all the input-related information for signing */
struct PSBTInput {
    CTransaction* utxo = nullptr;                               // The non-witness serialization of the previous output
    CTxOut txout;                                               // The output being spent
    std::map<CPubKey, std::vector<unsigned char>> partial_sigs; // Partial signatures from different signers
    std::vector<unsigned char> final_script_sig;                // Final scriptSig after signing
    std::vector<unsigned char> final_script_witness;            // Final scriptWitness after signing

    std::map<std::vector<unsigned char>, std::vector<unsigned char>> unknown; // Unknown fields

    PSBTInput() {}

    bool IsSigned() const
    {
        return !final_script_sig.empty() || !final_script_witness.empty();
    }

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action)
    {
        READWRITE(utxo);
        READWRITE(txout);
        READWRITE(partial_sigs);
        READWRITE(final_script_sig);
        READWRITE(final_script_witness);
        READWRITE(unknown);
    }
};

/** PSBTOutput holds all the output-related information */
struct PSBTOutput {
    CScript redeem_script;  // Redeemed output script
    CScript witness_script; // Witness script

    std::map<std::vector<unsigned char>, std::vector<unsigned char>> unknown; // Unknown fields

    PSBTOutput() {}

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action)
    {
        READWRITE(redeem_script);
        READWRITE(witness_script);
        READWRITE(unknown);
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

    PartiallySignedTransaction() {}

    explicit PartiallySignedTransaction(const CMutableTransaction& txIn) : tx(txIn)
    {
        inputs.resize(txIn.vin.size());
        outputs.resize(txIn.vout.size());
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

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action)
    {
        // Write/read PSBT magic bytes
        std::vector<unsigned char> magic_bytes(PSBT_MAGIC_BYTES, PSBT_MAGIC_BYTES + 5);
        READWRITE(magic_bytes);

        // Global section: unsigned transaction
        unsigned char key_type = PSBT_GLOBAL_UNSIGNED_TX;
        std::vector<unsigned char> key;
        key.push_back(key_type);
        READWRITE(key);

        READWRITE(tx);

        // Unknown fields in global section
        READWRITE(unknown);

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

#endif // AVIAN_PSBT_H
