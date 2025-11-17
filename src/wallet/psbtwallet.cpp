#include "psbtwallet.h"

#include "chainparams.h"
#include "psbt.h"
#include "tinyformat.h"
#include "validation.h"
#include "wallet.h"

#include <limits>
#include <sstream>

// Local copy of the BIP32 hardened index limit (same value as used in wallet.cpp)
static const uint32_t BIP32_HARDENED_KEY_LIMIT = 0x80000000U;

/** Parse a textual HD keypath like "m/44'/921'/0'/0/5'" into BIP32 indices. */
static bool ParseHDKeypath(const std::string& keypath, std::vector<uint32_t>& out)
{
    out.clear();
    if (keypath.empty() || keypath[0] != 'm') {
        return false;
    }

    // Split on '/'
    std::vector<std::string> parts;
    std::stringstream ss(keypath);
    std::string item;
    while (std::getline(ss, item, '/')) {
        parts.push_back(item);
    }
    if (parts.size() < 2) {
        // Must at least be m/x
        return false;
    }

    for (size_t i = 1; i < parts.size(); ++i) {
        std::string p = parts[i];
        if (p.empty()) continue;

        bool hardened = false;
        char last = p.back();
        if (last == '\'' || last == 'h' || last == 'H') {
            hardened = true;
            p.pop_back();
        }

        if (p.empty()) {
            return false;
        }

        char* endp = nullptr;
        errno = 0;
        unsigned long idx = strtoul(p.c_str(), &endp, 10);
        if (errno != 0 || endp == p.c_str() || *endp != '\0' || idx > std::numeric_limits<uint32_t>::max()) {
            return false;
        }

        uint32_t index = static_cast<uint32_t>(idx);
        if (hardened) {
            index |= BIP32_HARDENED_KEY_LIMIT;
        }
        out.push_back(index);
    }

    return !out.empty();
}

/** Build BIP174 key origin data: 4-byte parent fingerprint + 4-byte indices. */
static bool BuildBIP32Origin(const CKeyMetadata& meta, std::vector<unsigned char>& out)
{
    out.clear();
    if (meta.hdKeypath.empty() || meta.hd_seed_id.IsNull()) {
        return false;
    }

    std::vector<uint32_t> indices;
    if (!ParseHDKeypath(meta.hdKeypath, indices)) {
        return false;
    }

    // Use first 4 bytes of hd_seed_id (hash160) as fingerprint, big endian.
    const unsigned char* id = meta.hd_seed_id.begin();
    if (!id) {
        return false;
    }
    out.reserve(4 + 4 * indices.size());
    out.push_back(id[0]);
    out.push_back(id[1]);
    out.push_back(id[2]);
    out.push_back(id[3]);

    for (uint32_t idx : indices) {
        out.push_back((idx >> 24) & 0xFF);
        out.push_back((idx >> 16) & 0xFF);
        out.push_back((idx >> 8) & 0xFF);
        out.push_back(idx & 0xFF);
    }

    return true;
}

bool FillPSBTInputWalletData(const CWallet& wallet,
    const COutPoint& prevout,
    PartiallySignedTransaction& psbtx,
    unsigned int input_index)
{
    if (input_index >= psbtx.inputs.size()) {
        return false;
    }

    auto it = wallet.mapWallet.find(prevout.hash);
    if (it == wallet.mapWallet.end()) {
        return false;
    }
    const CWalletTx& wtxPrev = it->second;

    // Non-witness UTXO (full previous transaction)
    psbtx.inputs[input_index].utxo = wtxPrev.tx;

    if (prevout.n >= wtxPrev.tx->vout.size()) {
        return false;
    }

    const CTxOut& prevOut = wtxPrev.tx->vout[prevout.n];

    // If this is P2SH and the wallet knows the redeem script, include it and any HD keypaths.
    CScript scriptPubKey = prevOut.scriptPubKey;
    txnouttype type;
    std::vector<CTxDestination> vDest;
    int nRequired = 0;
    if (!ExtractDestinations(scriptPubKey, type, vDest, nRequired)) {
        return true; // UTXO fields filled; nothing more to do.
    }

    if (type == TX_SCRIPTHASH && !vDest.empty()) {
        const CTxDestination& dest = vDest[0];
        CScriptID scriptID(boost::get<CScriptID>(dest));
        CScript redeemScriptLocal;
        if (wallet.GetCScript(scriptID, redeemScriptLocal) && !redeemScriptLocal.empty()) {
            psbtx.inputs[input_index].redeem_script = redeemScriptLocal;
        }

        // HD keypaths: only if wallet supports HD and we have key metadata.
        if (wallet.CanSupportFeature(FEATURE_HD)) {
            auto mit = wallet.mapKeyMetadata.find(dest);
            if (mit != wallet.mapKeyMetadata.end()) {
                const CKeyMetadata& meta = mit->second;
                std::vector<unsigned char> origin;
                if (BuildBIP32Origin(meta, origin)) {
                    const CKeyID* keyID = boost::get<CKeyID>(&dest);
                    if (keyID != nullptr) {
                        CPubKey pubkey;
                        if (wallet.GetPubKey(*keyID, pubkey) && pubkey.IsFullyValid()) {
                            psbtx.inputs[input_index].hd_keypaths[pubkey] = origin;
                        }
                    }
                }
            }
        }
    }

    return true;
}

static bool LookupWalletOrChainTransaction(const CWallet* wallet, const uint256& txid, CTransactionRef& txOut)
{
    txOut.reset();

    if (wallet) {
        auto it = wallet->mapWallet.find(txid);
        if (it != wallet->mapWallet.end() && it->second.tx) {
            txOut = it->second.tx;
            return true;
        }
    }

    uint256 blockHash;
    if (GetTransaction(txid, txOut, Params().GetConsensus(), blockHash, /*fAllowSlow=*/true)) {
        return true;
    }

    return false;
}

bool EnsurePSBTInputUTXOs(const CWallet* wallet,
    PartiallySignedTransaction& psbtx,
    std::string& error_out)
{
    error_out.clear();

    if (psbtx.inputs.size() != psbtx.tx.vin.size()) {
        error_out = "PSBT input count does not match transaction inputs";
        return false;
    }

    for (size_t i = 0; i < psbtx.inputs.size(); ++i) {
        PSBTInput& input = psbtx.inputs[i];
        const CTxIn& txin = psbtx.tx.vin[i];

        bool needs_lookup = !input.utxo || input.utxo->GetHash() != txin.prevout.hash;
        if (!needs_lookup && txin.prevout.n >= input.utxo->vout.size()) {
            needs_lookup = true;
        }

        if (needs_lookup) {
            CTransactionRef txPrev;
            if (!LookupWalletOrChainTransaction(wallet, txin.prevout.hash, txPrev)) {
                error_out = strprintf("Missing previous transaction %s", txin.prevout.hash.ToString());
                return false;
            }
            if (!txPrev || txin.prevout.n >= txPrev->vout.size()) {
                error_out = strprintf("Previous transaction %s missing output %u", txin.prevout.hash.ToString(), txin.prevout.n);
                return false;
            }
            if (txPrev->GetHash() != txin.prevout.hash) {
                error_out = strprintf("Lookup returned mismatching transaction for %s", txin.prevout.hash.ToString());
                return false;
            }
            input.utxo = txPrev;
        }

        if (!input.utxo || txin.prevout.n >= input.utxo->vout.size()) {
            error_out = strprintf("Incomplete UTXO data for input %u", i);
            return false;
        }
    }

    return true;
}
