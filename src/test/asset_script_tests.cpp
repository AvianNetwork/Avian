// Copyright (c) 2026-present The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// What 4.x said about an asset script on its own, and 5.0.x had lost: which ones
// are unspendable, which address a qualifier tag or a restriction names, and the
// asset_data object in the script JSON.

#include <addresstype.h>
#include <assets/assets.h>
#include <assets/assettypes.h>
#include <coins.h>
#include <consensus/amount.h>
#include <consensus/tx_verify.h>
#include <consensus/validation.h>
#include <core_io.h>
#include <key_io.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <span.h>
#include <streams.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <univalue.h>
#include <util/strencodings.h>

#include <boost/test/unit_test.hpp>

#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace {
PKHash Owner()
{
    return PKHash{uint160{ParseHex("00112233445566778899aabbccddeeff00112233")}};
}

CScript Transfer(const std::string& name, CAmount amount)
{
    CScript script{GetScriptForDestination(Owner())};
    CAssetTransfer{name, amount}.ConstructTransaction(script);
    return script;
}

CScript AddressData(const std::string& name, int8_t flag)
{
    CScript script{CScript{} << OP_AVN_ASSET << ToByteVector(Owner())};
    CNullAssetTxData{name, flag}.ConstructTransaction(script);
    return script;
}

CScript GlobalRestriction(const std::string& name, int8_t flag)
{
    CScript script;
    CNullAssetTxData{name, flag}.ConstructGlobalRestrictionTransaction(script);
    return script;
}

CScript Verifier(const std::string& verifier)
{
    CScript script;
    CNullAssetTxVerifierString{verifier}.ConstructTransaction(script);
    return script;
}

UniValue Describe(const CScript& script)
{
    UniValue out{UniValue::VOBJ};
    ScriptToUniv(script, out, /*include_hex=*/true, /*include_address=*/true);
    return out;
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(asset_script_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(unspendable)
{
    // Anything that begins with OP_AVN_ASSET
    BOOST_CHECK(AddressData("#KYC", 1).IsUnspendable());
    BOOST_CHECK(AddressData("$TOKEN", 0).IsUnspendable());
    BOOST_CHECK(GlobalRestriction("$TOKEN", 1).IsUnspendable());
    BOOST_CHECK(Verifier("KYC").IsUnspendable());

    // An asset script with nothing in it
    BOOST_CHECK(Transfer("AVIANTEST", 0).IsUnspendable());
    CScript reissue{GetScriptForDestination(Owner())};
    CReissueAsset{"AVIANTEST", 0, /*nUnits=*/-1, /*nReissuable=*/1, /*strIPFSHash=*/"", /*strANSID=*/""}.ConstructTransaction(reissue);
    BOOST_CHECK(reissue.IsAssetScript());
    BOOST_CHECK(reissue.IsUnspendable());

    // An asset script that carries something
    BOOST_CHECK(!Transfer("AVIANTEST", 1).IsUnspendable());
    CScript issue{GetScriptForDestination(Owner())};
    CNewAsset{"AVIANTEST", 21000 * COIN}.ConstructTransaction(issue);
    BOOST_CHECK(!issue.IsUnspendable());
    CScript owner_token{GetScriptForDestination(Owner())};
    CNewAsset{"AVIANTEST", 21000 * COIN}.ConstructOwnerTransaction(owner_token);
    BOOST_CHECK(!owner_token.IsUnspendable());

    // What it was before
    BOOST_CHECK(!GetScriptForDestination(Owner()).IsUnspendable());
    BOOST_CHECK((CScript{} << OP_RETURN).IsUnspendable());
    BOOST_CHECK(!CScript{}.IsUnspendable());

    // A zero-amount transfer cut short does not deserialize, and 4.x then let it be.
    const CScript whole{Transfer("AVIANTEST", 0)};
    int type{0};
    bool is_owner{false};
    int start{0};
    BOOST_REQUIRE(whole.IsAssetScript(type, is_owner, start));
    const CScript cut{whole.begin(), whole.begin() + start + 4};
    BOOST_CHECK(cut.IsAssetScript());
    BOOST_CHECK(!cut.IsUnspendable());
}

BOOST_AUTO_TEST_CASE(unspendable_stays_out_of_the_utxo_set)
{
    CCoinsView base;
    CCoinsViewCache view{&base};
    const Txid txid{Txid::FromUint256(uint256::ONE)};

    view.AddCoin(COutPoint{txid, 0}, Coin{CTxOut{0, AddressData("#KYC", 1)}, 1, false}, false);
    view.AddCoin(COutPoint{txid, 1}, Coin{CTxOut{0, Transfer("AVIANTEST", 0)}, 1, false}, false);
    view.AddCoin(COutPoint{txid, 2}, Coin{CTxOut{0, Transfer("AVIANTEST", 5 * COIN)}, 1, false}, false);

    BOOST_CHECK(!view.HaveCoin(COutPoint{txid, 0}));
    BOOST_CHECK(!view.HaveCoin(COutPoint{txid, 1}));
    BOOST_CHECK(view.HaveCoin(COutPoint{txid, 2}));
}

// A node that ran 5.0.x has these outputs in its chainstate. A view that hands one out
// stands in for that: AddCoin() itself would refuse it.
BOOST_AUTO_TEST_CASE(leftover_unspendable_coin_cannot_be_spent)
{
    struct OldChainstate : CCoinsView {
        std::map<COutPoint, Coin> coins;
        std::optional<Coin> GetCoin(const COutPoint& outpoint) const override
        {
            const auto it = coins.find(outpoint);
            if (it == coins.end()) return std::nullopt;
            return it->second;
        }
    };

    const Txid txid{Txid::FromUint256(uint256::ONE)};
    OldChainstate old;
    old.coins.emplace(COutPoint{txid, 0}, Coin{CTxOut{0, AddressData("#KYC", 1)}, 1, false});
    old.coins.emplace(COutPoint{txid, 1}, Coin{CTxOut{0, Transfer("AVIANTEST", 0)}, 1, false});
    old.coins.emplace(COutPoint{txid, 2}, Coin{CTxOut{COIN, GetScriptForDestination(Owner())}, 1, false});
    CCoinsViewCache view{&old};

    const auto spend = [&](uint32_t n) {
        CMutableTransaction mtx;
        mtx.vin.emplace_back(COutPoint{txid, n});
        mtx.vout.emplace_back(0, GetScriptForDestination(Owner()));
        TxValidationState state;
        CAmount fee{0};
        const bool valid{Consensus::CheckTxInputs(CTransaction{mtx}, state, view, /*nSpendHeight=*/200, fee)};
        return valid ? std::string{} : state.GetRejectReason();
    };

    // The view does hold them: this is the upgraded node, not the freshly synced one
    BOOST_CHECK(view.HaveCoin(COutPoint{txid, 0}));
    BOOST_CHECK(view.HaveCoin(COutPoint{txid, 1}));

    BOOST_CHECK_EQUAL(spend(0), "bad-txns-inputs-missingorspent");
    BOOST_CHECK_EQUAL(spend(1), "bad-txns-inputs-missingorspent");
    BOOST_CHECK_EQUAL(spend(2), "");
    // What a node without the coin answers
    BOOST_CHECK_EQUAL(spend(3), "bad-txns-inputs-missingorspent");
}

BOOST_AUTO_TEST_CASE(tag_and_restriction_address)
{
    const PKHash owner{Owner()};
    const std::string owner_address{EncodeDestination(owner)};

    for (const CScript& script : {AddressData("#KYC", 1), AddressData("$TOKEN", 0)}) {
        BOOST_CHECK(script.IsNullAssetTxDataScript());

        CTxDestination dest;
        BOOST_CHECK(ExtractDestination(script, dest));
        BOOST_CHECK(dest == CTxDestination{owner});
        // The address names who is tagged. It does not give the script back, so code that
        // rebuilds outputs from destinations (fundrawtransaction, bumpfee) keeps the script.
        BOOST_CHECK(GetScriptForDestination(dest) != script);

        CNullAssetTxData data;
        std::string address;
        BOOST_CHECK(AssetNullDataFromScript(script, data, address));
        BOOST_CHECK_EQUAL(address, owner_address);
    }

    // A global restriction and a verifier string have no address
    for (const CScript& script : {GlobalRestriction("$TOKEN", 1), Verifier("KYC")}) {
        CTxDestination dest;
        BOOST_CHECK(!ExtractDestination(script, dest));
        BOOST_CHECK(GetScriptForDestination(dest) == script);
    }

    // Nor does tag data with something other than pushes after the marker opcode
    CScript not_push_only{AddressData("#KYC", 1)};
    not_push_only << OP_DROP;
    CTxDestination dest;
    BOOST_CHECK(!ExtractDestination(not_push_only, dest));

    // An asset transfer still resolves to its holder
    BOOST_CHECK(ExtractDestination(Transfer("AVIANTEST", 5 * COIN), dest));
    BOOST_CHECK(dest == CTxDestination{owner});
}

BOOST_AUTO_TEST_CASE(asset_data_to_univ)
{
    const std::string owner_address{EncodeDestination(Owner())};

    {
        const UniValue out{Describe(AddressData("#KYC", 1))};
        BOOST_CHECK_EQUAL(out["type"].get_str(), "restricted_asset_data");
        BOOST_CHECK_EQUAL(out["address"].get_str(), owner_address);
        const UniValue& data{out["asset_data"]};
        BOOST_CHECK_EQUAL(data["asset_name"].get_str(), "#KYC");
        BOOST_CHECK_EQUAL(data["qualifier_type"].get_str(), "adding qualifier");
        BOOST_CHECK_EQUAL(data["address"].get_str(), owner_address);
    }
    {
        const UniValue data = Describe(AddressData("#KYC", 0))["asset_data"];
        BOOST_CHECK_EQUAL(data["qualifier_type"].get_str(), "removing qualifier");
    }
    {
        const UniValue data = Describe(AddressData("$TOKEN", 1))["asset_data"];
        BOOST_CHECK_EQUAL(data["asset_name"].get_str(), "$TOKEN");
        BOOST_CHECK_EQUAL(data["restricted_type"].get_str(), "freezing address");
        BOOST_CHECK_EQUAL(data["address"].get_str(), owner_address);
    }
    {
        const UniValue out{Describe(GlobalRestriction("$TOKEN", 0))};
        BOOST_CHECK_EQUAL(out["type"].get_str(), "restricted_asset_data");
        BOOST_CHECK(!out.exists("address"));
        const UniValue& data{out["asset_data"]};
        BOOST_CHECK_EQUAL(data["restricted_name"].get_str(), "$TOKEN");
        BOOST_CHECK_EQUAL(data["restricted_type"].get_str(), "unfreezing");
        BOOST_CHECK_EQUAL(data["address"].get_str(), "all addresses");
    }
    {
        const UniValue out{Describe(Verifier("KYC"))};
        BOOST_CHECK(!out.exists("address"));
        BOOST_CHECK_EQUAL(out["asset_data"]["verifier_string"].get_str(), "KYC");
    }
    {
        // Ordinary and asset outputs get no asset_data
        BOOST_CHECK(!Describe(GetScriptForDestination(Owner())).exists("asset_data"));
        const UniValue out{Describe(Transfer("AVIANTEST", 5 * COIN))};
        BOOST_CHECK(!out.exists("asset_data"));
        BOOST_CHECK_EQUAL(out["asset"]["name"].get_str(), "AVIANTEST");
    }
}

// A reissue payload truncated before strANSID. 4.x reads strANSID
// unconditionally and rejects this; 5.0.x skipped the field when no bytes
// remained and accepted (and would mint) an output a 4.x node rejects.
BOOST_AUTO_TEST_CASE(reissue_truncated_before_ansid_is_rejected)
{
    CReissueAsset reissue{"REISSUETEST", 100 * COIN, /*nUnits=*/0, /*nReissuable=*/1,
                          /*strIPFSHash=*/"", /*strANSID=*/""};
    DataStream ss{};
    ss << reissue;
    const std::vector<unsigned char> full(UCharCast(ss.data()), UCharCast(ss.data() + ss.size()));

    // strANSID is serialised last; empty is a single 0x00 length byte. Dropping
    // it leaves a payload that ends exactly at the IPFS field.
    BOOST_REQUIRE(!full.empty());
    BOOST_REQUIRE_EQUAL(full.back(), 0x00);
    const std::vector<unsigned char> truncated{full.begin(), full.end() - 1};

    // A reissue script carrying the given payload, with no trailing opcode so the
    // extracted stream ends exactly where the payload does.
    const auto reissue_script = [](const std::vector<unsigned char>& payload) {
        std::vector<unsigned char> msg{AVN_R, AVN_V, AVN_N, AVN_R}; // "rvnr"
        msg.insert(msg.end(), payload.begin(), payload.end());
        CScript script{GetScriptForDestination(Owner())};
        script << OP_AVN_ASSET << msg;
        return script;
    };

    CReissueAsset parsed;
    std::string address;

    // The whole payload still parses.
    BOOST_CHECK(reissue_script(full).IsAssetScript());
    BOOST_CHECK(ReissueAssetFromScript(reissue_script(full), parsed, address));
    BOOST_CHECK_EQUAL(parsed.strName, "REISSUETEST");

    // Truncated before strANSID: still recognised as a reissue script, but must
    // fail to deserialize, matching 4.x.
    BOOST_CHECK(reissue_script(truncated).IsAssetScript());
    BOOST_CHECK(!ReissueAssetFromScript(reissue_script(truncated), parsed, address));
}

// 4.x finds the burn address in more scripts than Solver() does here: a key
// hash pushed by any push opcode, and a push-only OP_AVN_ASSET script. Its
// pools mine such a burn, so it has to be found here too.
BOOST_AUTO_TEST_CASE(burn_output_is_read_as_4x_reads_it)
{
    const CAmount amount{GetBurnAmount(AssetType::ROOT)};
    const CTxDestination burn_dest{DecodeDestination(GetBurnAddress(AssetType::ROOT))};
    const std::vector<unsigned char> burn{ToByteVector(std::get<PKHash>(burn_dest))};
    const std::vector<unsigned char> other{ToByteVector(Owner())};

    const auto pushdata1 = [](const std::vector<unsigned char>& data) {
        std::vector<unsigned char> push{OP_PUSHDATA1, static_cast<unsigned char>(data.size())};
        push.insert(push.end(), data.begin(), data.end());
        return push;
    };
    const auto pushdata2 = [](const std::vector<unsigned char>& data) {
        std::vector<unsigned char> push{OP_PUSHDATA2, static_cast<unsigned char>(data.size()), 0};
        push.insert(push.end(), data.begin(), data.end());
        return push;
    };
    const auto p2pkh = [](const std::vector<unsigned char>& push) {
        CScript script{CScript{} << OP_DUP << OP_HASH160};
        script.insert(script.end(), push.begin(), push.end());
        return script << OP_EQUALVERIFY << OP_CHECKSIG;
    };
    const auto is_burn = [&](const CScript& script) { return CheckIssueBurnTx(CTxOut{amount, script}, AssetType::ROOT); };

    // The ordinary burn
    BOOST_CHECK(is_burn(GetScriptForDestination(burn_dest)));
    BOOST_CHECK(!is_burn(GetScriptForDestination(Owner())));
    BOOST_CHECK(!CheckIssueBurnTx(CTxOut{amount - 1, GetScriptForDestination(burn_dest)}, AssetType::ROOT));

    // The hash pushed another way
    BOOST_CHECK(is_burn(p2pkh(pushdata1(burn))));
    BOOST_CHECK(is_burn(p2pkh(pushdata2(burn))));
    BOOST_CHECK(!is_burn(p2pkh(pushdata1(other))));
    BOOST_CHECK(!is_burn(p2pkh(pushdata1(burn)) << OP_NOP));
    BOOST_CHECK(!is_burn(CScript{} << OP_DUP << OP_HASH160 << burn << OP_EQUAL << OP_CHECKSIG));

    // A push-only OP_AVN_ASSET script of 23 bytes or more holds the hash in bytes 2 to 21
    BOOST_CHECK(is_burn(CScript{} << OP_AVN_ASSET << burn << OP_0));
    std::vector<unsigned char> longer{burn};
    longer.push_back(0x01);
    BOOST_CHECK(is_burn(CScript{} << OP_AVN_ASSET << longer));
    BOOST_CHECK(!is_burn(CScript{} << OP_AVN_ASSET << burn));
    BOOST_CHECK(!is_burn(CScript{} << OP_AVN_ASSET << other << OP_0));
    BOOST_CHECK(!is_burn(CScript{} << OP_AVN_ASSET << burn << OP_DROP));
    BOOST_CHECK(!is_burn(CScript{} << OP_AVN_ASSET << OP_RESERVED << burn << OP_0));

    // Not an address in 4.x either
    BOOST_CHECK(!is_burn(CScript{} << OP_0 << burn));
    BOOST_CHECK(!is_burn(CScript{} << OP_RETURN << burn));
    BOOST_CHECK(!is_burn(CScript{}));

    // A reissue burn goes the same way
    const CAmount reissue_amount{GetReissueAssetBurnAmount()};
    const CTxDestination reissue_dest{DecodeDestination(GetBurnAddress(AssetType::REISSUE))};
    const std::vector<unsigned char> reissue_burn{ToByteVector(std::get<PKHash>(reissue_dest))};
    BOOST_CHECK(CheckReissueBurnTx(CTxOut{reissue_amount, GetScriptForDestination(reissue_dest)}));
    BOOST_CHECK(CheckReissueBurnTx(CTxOut{reissue_amount, p2pkh(pushdata1(reissue_burn))}));
    BOOST_CHECK(CheckReissueBurnTx(CTxOut{reissue_amount, CScript{} << OP_AVN_ASSET << reissue_burn << OP_0}));
    BOOST_CHECK(!CheckReissueBurnTx(CTxOut{reissue_amount, p2pkh(pushdata1(burn))}));
}

BOOST_AUTO_TEST_SUITE_END()
