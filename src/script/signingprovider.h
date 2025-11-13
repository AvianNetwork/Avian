#pragma once
#include <key.h> // CKey
#include <map>
#include <optional>
#include <pubkey.h>           // CPubKey, CKeyID
#include <script/keyorigin.h> // KeyOriginInfo (your shim)
#include <script/script.h>    // CScript

// Minimal SigningProvider interface (enough for PSBT v24 paths we use)
class SigningProvider
{
public:
    virtual ~SigningProvider() = default;

    // Return true if found; leave out Taproot/Schnorr for now.
    virtual bool GetCScript(const CScriptID& scriptid, CScript& script) const { return false; }
    virtual bool GetPubKey(const CKeyID& keyid, CPubKey& pubkey) const { return false; }
    virtual bool GetKey(const CKeyID& keyid, CKey& key) const { return false; }
    virtual bool GetKeyOrigin(const CKeyID& keyid, KeyOriginInfo& info) const { return false; }
};

// Flat (in-memory) provider used by PSBT utils/finalizer
struct FlatSigningProvider : public SigningProvider {
    std::map<CKeyID, CKey> keys;
    std::map<CKeyID, CPubKey> pubkeys;
    std::map<CScriptID, CScript> scripts;
    std::map<CKeyID, KeyOriginInfo> origins;

    bool GetCScript(const CScriptID& id, CScript& out) const override
    {
        auto it = scripts.find(id);
        if (it == scripts.end()) return false;
        out = it->second;
        return true;
    }
    bool GetPubKey(const CKeyID& id, CPubKey& out) const override
    {
        auto it = pubkeys.find(id);
        if (it == pubkeys.end()) return false;
        out = it->second;
        return true;
    }
    bool GetKey(const CKeyID& id, CKey& out) const override
    {
        auto it = keys.find(id);
        if (it == keys.end()) return false;
        out = it->second;
        return true;
    }
    bool GetKeyOrigin(const CKeyID& id, KeyOriginInfo& out) const override
    {
        auto it = origins.find(id);
        if (it == origins.end()) return false;
        out = it->second;
        return true;
    }
};
