// Copyright (c) 2017-2020 The Raven Core developers
// Copyright (c) 2022 The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <regex>
#include <script/script.h>
#include <streams.h>
#include <primitives/transaction.h>
#include <script/solver.h>
#include <addresstype.h>
#include <key_io.h>
#include <common/args.h>
#include <base58.h>
#include <validation.h>
#include <tinyformat.h>
#include <consensus/validation.h>
#include <rpc/protocol.h>
#include <net.h>
#include <logging.h>
#include <coins.h>
#include <memusage.h>
#include <span.h>
#include <util/strencodings.h>
#include <util/moneystr.h>
#include <util/time.h>
#include <util/translation.h>
#include <assets/assets.h>
#include <assets/assetdb.h>
#include <assets/assettypes.h>
#include <deploymentstatus.h>
#include <txmempool.h>
#include <assets/ans.h>
#include <assets/LibBoolEE.h>
#include <assets/restricteddb.h>
#include <protocol.h>
#include <util/chaintype.h>
#include <univalue.h>

// Compatibility: old error() function logged a message and returned false.
// Removed in BTC 30.2. Define as macro wrapping LogError.
#define error(...) ([&]() -> bool { LogError(__VA_ARGS__); return false; }())

// Helper: split string by delimiter characters (replaces boost::split + boost::is_any_of)
static std::vector<std::string> SplitString(const std::string& str, const std::string& delimiters) {
    std::vector<std::string> parts;
    size_t start = 0, end;
    while ((end = str.find_first_of(delimiters, start)) != std::string::npos) {
        parts.push_back(str.substr(start, end - start));
        start = end + 1;
    }
    parts.push_back(str.substr(start));
    return parts;
}

#define SIX_MONTHS 15780000 // Six months worth of seconds

#define OFFSET_THREE 3
#define OFFSET_FOUR 4
#define OFFSET_TWENTY_THREE 23


std::map<uint256, std::string> mapReissuedTx;
std::map<std::string, uint256> mapReissuedAssets;

// Asset global state definitions
CAssetsCache* passets = nullptr;
CAssetsDB* passetsdb = nullptr;
CLRUCache<std::string, CDatabasedAssetData>* passetsCache = nullptr;
CRestrictedDB* prestricteddb = nullptr;
CLRUCache<std::string, CNullAssetTxVerifierString>* passetsVerifierCache = nullptr;
CLRUCache<std::string, int8_t>* passetsQualifierCache = nullptr;
CLRUCache<std::string, int8_t>* passetsRestrictionCache = nullptr;
CLRUCache<std::string, int8_t>* passetsGlobalRestrictionCache = nullptr;
bool fAssetIndex = false;

static bool IsUpgradeActive(Consensus::UpgradeIndex idx)
{
    const auto& upgrade = Params().GetConsensus().vUpgrades[idx];
    return upgrade.nTimestamp != std::numeric_limits<uint32_t>::max() &&
           GetTime() >= static_cast<int64_t>(upgrade.nTimestamp);
}

bool AreAssetsDeployed()
{
    return IsUpgradeActive(Consensus::UPGRADE_AVIAN_ASSETS);
}

bool AreMessagesDeployed()
{
    return IsUpgradeActive(Consensus::UPGRADE_AVIAN_ASSETS);
}

bool AreRestrictedAssetsDeployed()
{
    return IsUpgradeActive(Consensus::UPGRADE_AVIAN_ASSETS);
}

bool IsAvianNameSystemDeployed(const CBlockIndex* pindexPrev, VersionBitsCache& vbc)
{
    // AIP-0009: Full BIP9 state-machine check via DeploymentActiveAfter.
    // Handles ALWAYS_ACTIVE, NEVER_ACTIVE, and real miner-signalling state.
    return DeploymentActiveAfter(pindexPrev, Params().GetConsensus(), Consensus::DEPLOYMENT_ANS_V2, vbc);
}

bool IsAvianNameSystemDeployed()
{
    // Fast-path without a block index: only correct for NEVER_ACTIVE and ALWAYS_ACTIVE.
    // Callers with block context (consensus validation) should use the pindexPrev overload.
    const auto& dep = Params().GetConsensus().vDeployments[Consensus::DEPLOYMENT_ANS_V2];
    if (dep.nStartTime == Consensus::BIP9Deployment::NEVER_ACTIVE) return false;
    if (dep.nStartTime == Consensus::BIP9Deployment::ALWAYS_ACTIVE) return true;
    // BIP9 signalling state requires a CBlockIndex — unknown without one.
    return false;
}

// excluding owner tag ('!')
static const auto MAX_NAME_LENGTH = 31;
static const auto MAX_CHANNEL_NAME_LENGTH = 12;

// min lengths are expressed by quantifiers
static const std::regex ROOT_NAME_CHARACTERS("^[A-Z0-9._]{3,}$");
static const std::regex SUB_NAME_CHARACTERS("^[A-Z0-9._]+$");
static const std::regex UNIQUE_TAG_CHARACTERS("^[-A-Za-z0-9@$%&*()[\\]{}_.?:]+$");
static const std::regex MSG_CHANNEL_TAG_CHARACTERS("^[A-Za-z0-9_]+$");
static const std::regex VOTE_TAG_CHARACTERS("^[A-Z0-9._]+$");

// Restricted assets
static const std::regex QUALIFIER_NAME_CHARACTERS("#[A-Z0-9._]{3,}$");
static const std::regex SUB_QUALIFIER_NAME_CHARACTERS("#[A-Z0-9._]+$");
static const std::regex RESTRICTED_NAME_CHARACTERS("\\$[A-Z0-9._]{3,}$");

static const std::regex DOUBLE_PUNCTUATION("^.*[._]{2,}.*$");
static const std::regex LEADING_PUNCTUATION("^[._].*$");
static const std::regex TRAILING_PUNCTUATION("^.*[._]$");
static const std::regex QUALIFIER_LEADING_PUNCTUATION("^[#\\$][._].*$"); // Used for qualifier assets, and restricted asset only

static const std::string SUB_NAME_DELIMITER = "/";
static const std::string UNIQUE_TAG_DELIMITER = "#";
static const std::string MSG_CHANNEL_TAG_DELIMITER = "~";
static const std::string VOTE_TAG_DELIMITER = "^";
// static const std::string RESTRICTED_TAG_DELIMITER = "$"; // TODO: re-enable when restricted assets use this

static const std::regex UNIQUE_INDICATOR(R"(^[^^~#!]+#[^~#!\/]+$)");
static const std::regex MSG_CHANNEL_INDICATOR(R"(^[^^~#!]+~[^~#!\/]+$)");
static const std::regex OWNER_INDICATOR(R"(^[^^~#!]+!$)");
static const std::regex VOTE_INDICATOR(R"(^[^^~#!]+\^[^~#!\/]+$)");

static const std::regex QUALIFIER_INDICATOR("^[#][A-Z0-9._]{3,}$"); // Starts with #
static const std::regex SUB_QUALIFIER_INDICATOR("^#[A-Z0-9._]+\\/#[A-Z0-9._]+$"); // Starts with #
static const std::regex RESTRICTED_INDICATOR("^[\\$][A-Z0-9._]{3,}$"); // Starts with $

static const std::regex AVIAN_NAMES("^RVN$|^AVN$|^AVIAN$|^AVIAN$|^#AVN$|^#AVIAN$|^#AVIAN$");

bool IsRootNameValid(const std::string& name)
{
    return std::regex_match(name, ROOT_NAME_CHARACTERS)
        && !std::regex_match(name, DOUBLE_PUNCTUATION)
        && !std::regex_match(name, LEADING_PUNCTUATION)
        && !std::regex_match(name, TRAILING_PUNCTUATION)
        && !std::regex_match(name, AVIAN_NAMES);
}

bool IsQualifierNameValid(const std::string& name)
{
    return std::regex_match(name, QUALIFIER_NAME_CHARACTERS)
           && !std::regex_match(name, DOUBLE_PUNCTUATION)
           && !std::regex_match(name, QUALIFIER_LEADING_PUNCTUATION)
           && !std::regex_match(name, TRAILING_PUNCTUATION)
           && !std::regex_match(name, AVIAN_NAMES);
}

bool IsRestrictedNameValid(const std::string& name)
{
    return std::regex_match(name, RESTRICTED_NAME_CHARACTERS)
           && !std::regex_match(name, DOUBLE_PUNCTUATION)
           && !std::regex_match(name, LEADING_PUNCTUATION)
           && !std::regex_match(name, TRAILING_PUNCTUATION)
           && !std::regex_match(name, AVIAN_NAMES);
}

bool IsSubQualifierNameValid(const std::string& name)
{
    return std::regex_match(name, SUB_QUALIFIER_NAME_CHARACTERS)
           && !std::regex_match(name, DOUBLE_PUNCTUATION)
           && !std::regex_match(name, LEADING_PUNCTUATION)
           && !std::regex_match(name, TRAILING_PUNCTUATION);
}

bool IsSubNameValid(const std::string& name)
{
    return std::regex_match(name, SUB_NAME_CHARACTERS)
        && !std::regex_match(name, DOUBLE_PUNCTUATION)
        && !std::regex_match(name, LEADING_PUNCTUATION)
        && !std::regex_match(name, TRAILING_PUNCTUATION);
}

bool IsUniqueTagValid(const std::string& tag)
{
    return std::regex_match(tag, UNIQUE_TAG_CHARACTERS);
}

bool IsVoteTagValid(const std::string& tag)
{
    return std::regex_match(tag, VOTE_TAG_CHARACTERS);
}

bool IsMsgChannelTagValid(const std::string &tag)
{
    return std::regex_match(tag, MSG_CHANNEL_TAG_CHARACTERS)
        && !std::regex_match(tag, DOUBLE_PUNCTUATION)
        && !std::regex_match(tag, LEADING_PUNCTUATION)
        && !std::regex_match(tag, TRAILING_PUNCTUATION);
}

bool IsNameValidBeforeTag(const std::string& name)
{
    std::vector<std::string> parts;
    parts = SplitString(name, SUB_NAME_DELIMITER);

    if (!IsRootNameValid(parts.front())) return false;

    if (parts.size() > 1)
    {
        for (unsigned long i = 1; i < parts.size(); i++)
        {
            if (!IsSubNameValid(parts[i])) return false;
        }
    }

    return true;
}

bool IsQualifierNameValidBeforeTag(const std::string& name)
{
    std::vector<std::string> parts;
    parts = SplitString(name, SUB_NAME_DELIMITER);

    if (!IsQualifierNameValid(parts.front())) return false;

    // Qualifiers can only have one sub qualifier under it
    if (parts.size() > 2) {
        return false;
    }

    if (parts.size() > 1)
    {

        for (unsigned long i = 1; i < parts.size(); i++)
        {
            if (!IsSubQualifierNameValid(parts[i])) return false;
        }
    }

    return true;
}

bool IsAssetNameASubasset(const std::string& name)
{
    std::vector<std::string> parts;
    parts = SplitString(name, SUB_NAME_DELIMITER);

    if (!IsRootNameValid(parts.front())) return false;

    return parts.size() > 1;
}

bool IsAssetNameASubQualifier(const std::string& name)
{
    std::vector<std::string> parts;
    parts = SplitString(name, SUB_NAME_DELIMITER);

    if (!IsQualifierNameValid(parts.front())) return false;

    return parts.size() > 1;
}


bool IsAssetNameValid(const std::string& name, AssetType& assetType, std::string& error)
{
    // Do a max length check first to stop the possibility of a stack exhaustion.
    // We check for a value that is larger than the max asset name
    if (name.length() > 40)
        return false;

    assetType = AssetType::INVALID;
    if (std::regex_match(name, UNIQUE_INDICATOR))
    {
        bool ret = IsTypeCheckNameValid(AssetType::UNIQUE, name, error);
        if (ret)
            assetType = AssetType::UNIQUE;

        return ret;
    }
    else if (std::regex_match(name, MSG_CHANNEL_INDICATOR))
    {
        bool ret = IsTypeCheckNameValid(AssetType::MSGCHANNEL, name, error);
        if (ret)
            assetType = AssetType::MSGCHANNEL;

        return ret;
    }
    else if (std::regex_match(name, OWNER_INDICATOR))
    {
        bool ret = IsTypeCheckNameValid(AssetType::OWNER, name, error);
        if (ret)
            assetType = AssetType::OWNER;

        return ret;
    }
    else if (std::regex_match(name, VOTE_INDICATOR))
    {
        bool ret = IsTypeCheckNameValid(AssetType::VOTE, name, error);
        if (ret)
            assetType = AssetType::VOTE;

        return ret;
    }
    else if (std::regex_match(name, QUALIFIER_INDICATOR))
    {
        bool ret = IsTypeCheckNameValid(AssetType::QUALIFIER, name, error);
        if (ret) {
            if (IsAssetNameASubQualifier(name))
                assetType = AssetType::SUB_QUALIFIER;
            else
                assetType = AssetType::QUALIFIER;
        }

        return ret;
    }
    else if (std::regex_match(name, SUB_QUALIFIER_INDICATOR))
    {
        bool ret = IsTypeCheckNameValid(AssetType::SUB_QUALIFIER, name, error);
        if (ret) {
            if (IsAssetNameASubQualifier(name))
                assetType = AssetType::SUB_QUALIFIER;
        }

        return ret;
    }
    else if (std::regex_match(name, RESTRICTED_INDICATOR))
    {
        bool ret = IsTypeCheckNameValid(AssetType::RESTRICTED, name, error);
        if (ret)
            assetType = AssetType::RESTRICTED;

        return ret;
    }
    else
    {
        auto type = IsAssetNameASubasset(name) ? AssetType::SUB : AssetType::ROOT;
        bool ret = IsTypeCheckNameValid(type, name, error);
        if (ret)
            assetType = type;

        return ret;
    }
}

bool IsAssetNameValid(const std::string& name)
{
    AssetType _assetType;
    std::string _error;
    return IsAssetNameValid(name, _assetType, _error);
}

bool IsAssetNameValid(const std::string& name, AssetType& assetType)
{
    std::string _error;
    return IsAssetNameValid(name, assetType, _error);
}

bool IsAssetNameARoot(const std::string& name)
{
    AssetType type;
    return IsAssetNameValid(name, type) && type == AssetType::ROOT;
}

bool IsAssetNameAnOwner(const std::string& name)
{
    return IsAssetNameValid(name) && std::regex_match(name, OWNER_INDICATOR);
}

bool IsAssetNameAnRestricted(const std::string& name)
{
    return IsAssetNameValid(name) && std::regex_match(name, RESTRICTED_INDICATOR);
}

bool IsAssetNameAQualifier(const std::string& name, bool fOnlyQualifiers)
{
    if (fOnlyQualifiers) {
        return IsAssetNameValid(name) && std::regex_match(name, QUALIFIER_INDICATOR);
    }

    return IsAssetNameValid(name) && (std::regex_match(name, QUALIFIER_INDICATOR) || std::regex_match(name, SUB_QUALIFIER_INDICATOR));
}

bool IsAssetNameAnMsgChannel(const std::string& name)
{
    return IsAssetNameValid(name) && std::regex_match(name, MSG_CHANNEL_INDICATOR);
}

// TODO get the string translated below
bool IsTypeCheckNameValid(const AssetType type, const std::string& name, std::string& error)
{
    if (type == AssetType::UNIQUE) {
        if (name.size() > MAX_NAME_LENGTH) { error = "Name is greater than max length of " + std::to_string(MAX_NAME_LENGTH); return false; }
        std::vector<std::string> parts;
        parts = SplitString(name, UNIQUE_TAG_DELIMITER);
        bool valid = IsNameValidBeforeTag(parts.front()) && IsUniqueTagValid(parts.back());
        if (!valid) { error = "Unique name contains invalid characters (Valid characters are: A-Z a-z 0-9 @ $ % & * ( ) [ ] { } _ . ? : -)";  return false; }
        return true;
    } else if (type == AssetType::MSGCHANNEL) {
        if (name.size() > MAX_NAME_LENGTH) { error = "Name is greater than max length of " + std::to_string(MAX_NAME_LENGTH); return false; }
        std::vector<std::string> parts;
        parts = SplitString(name, MSG_CHANNEL_TAG_DELIMITER);
        bool valid = IsNameValidBeforeTag(parts.front()) && IsMsgChannelTagValid(parts.back());
        if (parts.back().size() > MAX_CHANNEL_NAME_LENGTH) { error = "Channel name is greater than max length of " + std::to_string(MAX_CHANNEL_NAME_LENGTH); return false; }
        if (!valid) { error = "Message Channel name contains invalid characters (Valid characters are: A-Z 0-9 _ .) (special characters can't be the first or last characters)";  return false; }
        return true;
    } else if (type == AssetType::OWNER) {
        if (name.size() > MAX_NAME_LENGTH) { error = "Name is greater than max length of " + std::to_string(MAX_NAME_LENGTH); return false; }
        bool valid = IsNameValidBeforeTag(name.substr(0, name.size() - 1));
        if (!valid) { error = "Owner name contains invalid characters (Valid characters are: A-Z 0-9 _ .) (special characters can't be the first or last characters)";  return false; }
        return true;
    } else if (type == AssetType::VOTE) {
        if (name.size() > MAX_NAME_LENGTH) { error = "Name is greater than max length of " + std::to_string(MAX_NAME_LENGTH); return false; }
        std::vector<std::string> parts;
        parts = SplitString(name, VOTE_TAG_DELIMITER);
        bool valid = IsNameValidBeforeTag(parts.front()) && IsVoteTagValid(parts.back());
        if (!valid) { error = "Vote name contains invalid characters (Valid characters are: A-Z 0-9 _ .) (special characters can't be the first or last characters)";  return false; }
        return true;
    } else if (type == AssetType::QUALIFIER || type == AssetType::SUB_QUALIFIER) {
        if (name.size() > MAX_NAME_LENGTH) { error = "Name is greater than max length of " + std::to_string(MAX_NAME_LENGTH); return false; }
        bool valid = IsQualifierNameValidBeforeTag(name);
        if (!valid) { error = "Qualifier name contains invalid characters (Valid characters are: A-Z 0-9 _ .) (# must be the first character, _ . special characters can't be the first or last characters)";  return false; }
        return true;
    } else if (type == AssetType::RESTRICTED) {
        if (name.size() > MAX_NAME_LENGTH) { error = "Name is greater than max length of " + std::to_string(MAX_NAME_LENGTH); return false; }
        bool valid = IsRestrictedNameValid(name);
        if (!valid) { error = "Restricted name contains invalid characters (Valid characters are: A-Z 0-9 _ .) ($ must be the first character, _ . special characters can't be the first or last characters)";  return false; }
        return true;
    } else {
        if (name.size() > MAX_NAME_LENGTH - 1) { error = "Name is greater than max length of " + std::to_string(MAX_NAME_LENGTH - 1); return false; }  //Assets and sub-assets need to leave one extra char for OWNER indicator
        if (!IsAssetNameASubasset(name) && name.size() < MIN_ASSET_LENGTH) { error = "Name must be contain " + std::to_string(MIN_ASSET_LENGTH) + " characters"; return false; }
        bool valid = IsNameValidBeforeTag(name);
        if (!valid && IsAssetNameASubasset(name) && name.size() < 3) { error = "Name must have at least 3 characters (Valid characters are: A-Z 0-9 _ .)";  return false; }
        if (!valid) { error = "Name contains invalid characters (Valid characters are: A-Z 0-9 _ .) (special characters can't be the first or last characters)";  return false; }
        return true;
    }
}

std::string RestrictedNameToOwnerName(const std::string& name)
{
    if (!IsAssetNameAnRestricted(name)) {
        return "";
    }

    std::string temp_owner = name.substr(1,name.length());
    temp_owner = temp_owner + OWNER_TAG;

    return temp_owner;
}

std::string GetParentName(const std::string& name)
{
    AssetType type;
    if (!IsAssetNameValid(name, type))
        return "";

    auto index = std::string::npos;
    if (type == AssetType::SUB) {
        index = name.find_last_of(SUB_NAME_DELIMITER);
    } else if (type == AssetType::UNIQUE) {
        index = name.find_last_of(UNIQUE_TAG_DELIMITER);
    } else if (type == AssetType::MSGCHANNEL) {
        index = name.find_last_of(MSG_CHANNEL_TAG_DELIMITER);
    } else if (type == AssetType::VOTE) {
        index = name.find_last_of(VOTE_TAG_DELIMITER);
    } else if (type == AssetType::ROOT) {
        return name;
    } else if (type == AssetType::QUALIFIER) {
        return name;
    } else if (type == AssetType::SUB_QUALIFIER) {
        index = name.find_last_of(SUB_NAME_DELIMITER);
    } else if (type == AssetType::RESTRICTED) {
        return name;
    }

    if (std::string::npos != index)
    {
        return name.substr(0, index);
    }

    return name;
}

std::string GetUniqueAssetName(const std::string& parent, const std::string& tag)
{
    std::string unique = parent + "#" + tag;

    AssetType type;
    if (!IsAssetNameValid(unique, type)) {
        return "";
    }

    if (type != AssetType::UNIQUE)
        return "";

    return unique;
}

bool CNewAsset::IsNull() const
{
    return strName == "";
}

CNewAsset::CNewAsset(const CNewAsset& asset)
{
    this->strName = asset.strName;
    this->nAmount = asset.nAmount;
    this->units = asset.units;
    this->nHasIPFS = asset.nHasIPFS;
    this->strIPFSHash = asset.strIPFSHash;
    this->nHasANS = asset.nHasANS;
    this->strANSID = asset.strANSID;
    this->nReissuable = asset.nReissuable;
}

CNewAsset& CNewAsset::operator=(const CNewAsset& asset)
{
    this->strName = asset.strName;
    this->nAmount = asset.nAmount;
    this->units = asset.units;
    this->nHasIPFS = asset.nHasIPFS;
    this->strIPFSHash = asset.strIPFSHash;
    this->nHasANS = asset.nHasANS;
    this->strANSID = asset.strANSID;
    this->nReissuable = asset.nReissuable;
    return *this;
}

std::string CNewAsset::ToString()
{
    std::stringstream ss;
    ss << "Printing an asset" << "\n";
    ss << "name : " << strName << "\n";
    ss << "amount : " << nAmount << "\n";
    ss << "units : " << std::to_string(units) << "\n";
    ss << "reissuable : " << std::to_string(nReissuable) << "\n";
    ss << "has_ipfs : " << std::to_string(nHasIPFS) << "\n";
    ss << "has_ans : " << std::to_string(nHasANS) << "\n";

    if (nHasIPFS)
        ss << "ipfs_hash : " << strIPFSHash;

    if (nHasANS)
        ss << "ans_id : " << strANSID;

    return ss.str();
}

// IPFS
CNewAsset::CNewAsset(const std::string& strName, const CAmount& nAmount, const int& units, const int& nReissuable, const int& nHasIPFS, const std::string& strIPFSHash)
{
    this->SetNull();
    this->strName = strName;
    this->nAmount = nAmount;
    this->units = int8_t(units);
    this->nReissuable = int8_t(nReissuable);
    this->nHasIPFS = int8_t(nHasIPFS);
    this->strIPFSHash = strIPFSHash;
    this->nHasANS = int8_t(DEFAULT_HAS_ANS);
    this->strANSID = DEFAULT_ANS;
}

// IPFS + ANS
CNewAsset::CNewAsset(const std::string& strName, const CAmount& nAmount, const int& units, const int& nReissuable, const int& nHasIPFS, const std::string& strIPFSHash, const int& nHasANS, const std::string& strANSID)
{
    this->SetNull();
    this->strName = strName;
    this->nAmount = nAmount;
    this->units = int8_t(units);
    this->nReissuable = int8_t(nReissuable);
    this->nHasIPFS = int8_t(nHasIPFS);
    this->strIPFSHash = strIPFSHash;
    this->nHasANS = int8_t(nHasANS);
    this->strANSID = strANSID;
}

CNewAsset::CNewAsset(const std::string& strName, const CAmount& nAmount)
{
    this->SetNull();
    this->strName = strName;
    this->nAmount = nAmount;
    this->units = int8_t(DEFAULT_UNITS);
    this->nReissuable = int8_t(DEFAULT_REISSUABLE);
    this->nHasIPFS = int8_t(DEFAULT_HAS_IPFS);
    this->strIPFSHash = DEFAULT_IPFS;
    this->nHasANS = int8_t(DEFAULT_HAS_ANS);
    this->strANSID = DEFAULT_ANS;
}

CDatabasedAssetData::CDatabasedAssetData(const CNewAsset& asset, const int& nHeight, const uint256& blockHash)
{
    this->SetNull();
    this->asset = asset;
    this->nHeight = nHeight;
    this->blockHash = blockHash;
}

CDatabasedAssetData::CDatabasedAssetData()
{
    this->SetNull();
}

/**
 * Constructs a CScript that carries the asset name and quantity and adds to to the end of the given script
 * @param script - This script needs to be a pay to address script
 */
void CNewAsset::ConstructTransaction(CScript& script) const
{
    DataStream ssAsset{};
    ssAsset << *this;

    std::vector<unsigned char> vchMessage;
    vchMessage.push_back(AVN_R); // r
    vchMessage.push_back(AVN_V); // v
    vchMessage.push_back(AVN_N); // n
    vchMessage.push_back(AVN_Q); // q

    vchMessage.insert(vchMessage.end(), UCharCast(ssAsset.data()), UCharCast(ssAsset.data() + ssAsset.size()));
    script << OP_AVN_ASSET << ToByteVector(vchMessage) << OP_DROP;
}

void CNewAsset::ConstructOwnerTransaction(CScript& script) const
{
    DataStream ssOwner{};
    ssOwner << std::string(this->strName + OWNER_TAG);

    std::vector<unsigned char> vchMessage;
    vchMessage.push_back(AVN_R); // r
    vchMessage.push_back(AVN_V); // v
    vchMessage.push_back(AVN_N); // n
    vchMessage.push_back(AVN_O); // o

    vchMessage.insert(vchMessage.end(), UCharCast(ssOwner.data()), UCharCast(ssOwner.data() + ssOwner.size()));
    script << OP_AVN_ASSET << ToByteVector(vchMessage) << OP_DROP;
}

bool AssetFromTransaction(const CTransaction& tx, CNewAsset& asset, std::string& strAddress)
{
    // Check to see if the transaction is an new asset issue tx
    if (!IsNewAsset(tx))
        return false;

    // Get the scriptPubKey from the last tx in vout
    CScript scriptPubKey = tx.vout[tx.vout.size() - 1].scriptPubKey;

    return AssetFromScript(scriptPubKey, asset, strAddress);
}

bool MsgChannelAssetFromTransaction(const CTransaction& tx, CNewAsset& asset, std::string& strAddress)
{
    // Check to see if the transaction is an new asset issue tx
    if (!IsNewMsgChannelAsset(tx))
        return false;

    // Get the scriptPubKey from the last tx in vout
    CScript scriptPubKey = tx.vout[tx.vout.size() - 1].scriptPubKey;

    return MsgChannelAssetFromScript(scriptPubKey, asset, strAddress);
}

bool QualifierAssetFromTransaction(const CTransaction& tx, CNewAsset& asset, std::string& strAddress)
{
    // Check to see if the transaction is an new asset qualifier issue tx
    if (!IsNewQualifierAsset(tx))
        return false;

    // Get the scriptPubKey from the last tx in vout
    CScript scriptPubKey = tx.vout[tx.vout.size() - 1].scriptPubKey;

    return QualifierAssetFromScript(scriptPubKey, asset, strAddress);
}
bool RestrictedAssetFromTransaction(const CTransaction& tx, CNewAsset& asset, std::string& strAddress)
{
    // Check to see if the transaction is an new asset qualifier issue tx
    if (!IsNewRestrictedAsset(tx))
        return false;

    // Get the scriptPubKey from the last tx in vout
    CScript scriptPubKey = tx.vout[tx.vout.size() - 1].scriptPubKey;

    return RestrictedAssetFromScript(scriptPubKey, asset, strAddress);
}

bool ReissueAssetFromTransaction(const CTransaction& tx, CReissueAsset& reissue, std::string& strAddress)
{
    // Check to see if the transaction is a reissue tx
    if (!IsReissueAsset(tx))
        return false;

    // Get the scriptPubKey from the last tx in vout
    CScript scriptPubKey = tx.vout[tx.vout.size() - 1].scriptPubKey;

    return ReissueAssetFromScript(scriptPubKey, reissue, strAddress);
}

bool UniqueAssetFromTransaction(const CTransaction& tx, CNewAsset& asset, std::string& strAddress)
{
    // Check to see if the transaction is an new asset issue tx
    if (!IsNewUniqueAsset(tx))
        return false;

    // Get the scriptPubKey from the last tx in vout
    CScript scriptPubKey = tx.vout[tx.vout.size() - 1].scriptPubKey;

    return AssetFromScript(scriptPubKey, asset, strAddress);
}

bool IsNewOwnerTxValid(const CTransaction& tx, const std::string& assetName, const std::string& address, std::string& errorMsg)
{
    // TODO when ready to ship. Put the owner validation code in own method if needed
    std::string ownerName;
    std::string ownerAddress;
    if (!OwnerFromTransaction(tx, ownerName, ownerAddress)) {
        errorMsg = "bad-txns-bad-owner";
        return false;
    }

    int size = ownerName.size();

    if (ownerAddress != address) {
        errorMsg = "bad-txns-owner-address-mismatch";
        return false;
    }

    if (size < OWNER_LENGTH + MIN_ASSET_LENGTH) {
        errorMsg = "bad-txns-owner-asset-length";
        return false;
    }

    if (ownerName != std::string(assetName + OWNER_TAG)) {
        errorMsg = "bad-txns-owner-name-mismatch";
        return false;
    }

    return true;
}

bool OwnerFromTransaction(const CTransaction& tx, std::string& ownerName, std::string& strAddress)
{
    // Check to see if the transaction is an new asset issue tx
    if (!IsNewAsset(tx))
        return false;

    // Get the scriptPubKey from the last tx in vout
    CScript scriptPubKey = tx.vout[tx.vout.size() - 2].scriptPubKey;

    return OwnerAssetFromScript(scriptPubKey, ownerName, strAddress);
}

bool TransferAssetFromScript(const CScript& scriptPubKey, CAssetTransfer& assetTransfer, std::string& strAddress)
{
    int nStartingIndex = 0;
    if (!IsScriptTransferAsset(scriptPubKey, nStartingIndex)) {
        return false;
    }

    CTxDestination destination;
    ExtractDestination(scriptPubKey, destination);

    strAddress = EncodeDestination(destination);

    std::vector<unsigned char> vchTransferAsset;

    vchTransferAsset.insert(vchTransferAsset.end(), scriptPubKey.begin() + nStartingIndex, scriptPubKey.end());

    DataStream ssAsset{vchTransferAsset};

    try {
        ssAsset >> assetTransfer;
    } catch(std::exception& e) {
        error("Failed to get the transfer asset from the stream: %s", e.what());
        return false;
    }

    return true;
}

bool AssetFromScript(const CScript& scriptPubKey, CNewAsset& assetNew, std::string& strAddress)
{
    int nStartingIndex = 0;
    if (!IsScriptNewAsset(scriptPubKey, nStartingIndex))
        return false;

    CTxDestination destination;
    ExtractDestination(scriptPubKey, destination);

    strAddress = EncodeDestination(destination);

    std::vector<unsigned char> vchNewAsset;
    vchNewAsset.insert(vchNewAsset.end(), scriptPubKey.begin() + nStartingIndex, scriptPubKey.end());
    DataStream ssAsset{vchNewAsset};

    try {
        ssAsset >> assetNew;
    } catch(std::exception& e) {
        error("Failed to get the asset from the stream: %s", e.what());
        return false;
    }

    return true;
}

bool MsgChannelAssetFromScript(const CScript& scriptPubKey, CNewAsset& assetNew, std::string& strAddress)
{
    int nStartingIndex = 0;
    if (!IsScriptNewMsgChannelAsset(scriptPubKey, nStartingIndex))
        return false;

    CTxDestination destination;
    ExtractDestination(scriptPubKey, destination);

    strAddress = EncodeDestination(destination);

    std::vector<unsigned char> vchNewAsset;
    vchNewAsset.insert(vchNewAsset.end(), scriptPubKey.begin() + nStartingIndex, scriptPubKey.end());
    DataStream ssAsset{vchNewAsset};

    try {
        ssAsset >> assetNew;
    } catch(std::exception& e) {
        error("Failed to get the msg channel asset from the stream: %s", e.what());
        return false;
    }

    return true;
}

bool QualifierAssetFromScript(const CScript& scriptPubKey, CNewAsset& assetNew, std::string& strAddress)
{
    int nStartingIndex = 0;
    if (!IsScriptNewQualifierAsset(scriptPubKey, nStartingIndex))
        return false;

    CTxDestination destination;
    ExtractDestination(scriptPubKey, destination);

    strAddress = EncodeDestination(destination);

    std::vector<unsigned char> vchNewAsset;
    vchNewAsset.insert(vchNewAsset.end(), scriptPubKey.begin() + nStartingIndex, scriptPubKey.end());
    DataStream ssAsset{vchNewAsset};

    try {
        ssAsset >> assetNew;
    } catch(std::exception& e) {
        error("Failed to get the qualifier asset from the stream: %s", e.what());
        return false;
    }

    return true;
}

bool RestrictedAssetFromScript(const CScript& scriptPubKey, CNewAsset& assetNew, std::string& strAddress)
{
    int nStartingIndex = 0;
    if (!IsScriptNewRestrictedAsset(scriptPubKey, nStartingIndex))
        return false;

    CTxDestination destination;
    ExtractDestination(scriptPubKey, destination);

    strAddress = EncodeDestination(destination);

    std::vector<unsigned char> vchNewAsset;
    vchNewAsset.insert(vchNewAsset.end(), scriptPubKey.begin() + nStartingIndex, scriptPubKey.end());
    DataStream ssAsset{vchNewAsset};

    try {
        ssAsset >> assetNew;
    } catch(std::exception& e) {
        error("Failed to get the restricted asset from the stream: %s", e.what());
        return false;
    }

    return true;
}

bool OwnerAssetFromScript(const CScript& scriptPubKey, std::string& assetName, std::string& strAddress)
{
    int nStartingIndex = 0;
    if (!IsScriptOwnerAsset(scriptPubKey, nStartingIndex))
        return false;

    CTxDestination destination;
    ExtractDestination(scriptPubKey, destination);

    strAddress = EncodeDestination(destination);

    std::vector<unsigned char> vchOwnerAsset;
    vchOwnerAsset.insert(vchOwnerAsset.end(), scriptPubKey.begin() + nStartingIndex, scriptPubKey.end());
    DataStream ssOwner{vchOwnerAsset};

    try {
        ssOwner >> assetName;
    } catch(std::exception& e) {
        error("Failed to get the owner asset from the stream: %s", e.what());
        return false;
    }

    return true;
}

bool ReissueAssetFromScript(const CScript& scriptPubKey, CReissueAsset& reissue, std::string& strAddress)
{
    int nStartingIndex = 0;
    if (!IsScriptReissueAsset(scriptPubKey, nStartingIndex))
        return false;

    CTxDestination destination;
    ExtractDestination(scriptPubKey, destination);

    strAddress = EncodeDestination(destination);

    std::vector<unsigned char> vchReissueAsset;
    vchReissueAsset.insert(vchReissueAsset.end(), scriptPubKey.begin() + nStartingIndex, scriptPubKey.end());
    DataStream ssReissue{vchReissueAsset};

    try {
        ssReissue >> reissue;
    } catch(std::exception& e) {
        error("Failed to get the reissue asset from the stream: %s", e.what());
        return false;
    }

    return true;
}

bool AssetNullDataFromScript(const CScript& scriptPubKey, CNullAssetTxData& assetData, std::string& strAddress)
{
    if (!scriptPubKey.IsNullAssetTxDataScript()) {
        return false;
    }

    CTxDestination destination;
    ExtractDestination(scriptPubKey, destination);

    strAddress = EncodeDestination(destination);

    std::vector<unsigned char> vchAssetData;
    vchAssetData.insert(vchAssetData.end(), scriptPubKey.begin() + OFFSET_TWENTY_THREE, scriptPubKey.end());
    DataStream ssData{vchAssetData};

    try {
        ssData >> assetData;
    } catch(std::exception& e) {
        error("Failed to get the null asset tx data from the stream: %s", e.what());
        return false;
    }

    return true;
}

bool GlobalAssetNullDataFromScript(const CScript& scriptPubKey, CNullAssetTxData& assetData)
{
    if (!scriptPubKey.IsNullGlobalRestrictionAssetTxDataScript()) {
        return false;
    }

    std::vector<unsigned char> vchAssetData;
    vchAssetData.insert(vchAssetData.end(), scriptPubKey.begin() + OFFSET_FOUR, scriptPubKey.end());
    DataStream ssData{vchAssetData};

    try {
        ssData >> assetData;
    } catch(std::exception& e) {
        error("Failed to get the global restriction asset tx data from the stream: %s", e.what());
        return false;
    }

    return true;
}

bool AssetNullVerifierDataFromScript(const CScript& scriptPubKey, CNullAssetTxVerifierString& verifierData)
{
    if (!scriptPubKey.IsNullAssetVerifierTxDataScript()) {
        return false;
    }

    std::vector<unsigned char> vchAssetData;
    vchAssetData.insert(vchAssetData.end(), scriptPubKey.begin() + OFFSET_THREE, scriptPubKey.end());
    DataStream ssData{vchAssetData};

    try {
        ssData >> verifierData;
    } catch(std::exception& e) {
        error("Failed to get the verifier string from the stream: %s", e.what());
        return false;
    }

    return true;
}

//! Call VerifyNewAsset if this function returns true
bool IsNewAsset(const CTransaction& tx)
{
    // New Asset transaction will always have at least three outputs.
    // 1. Owner Token output
    // 2. Issue Asset output
    // 3. AVN Burn Fee
    if (tx.vout.size() < 3) {
        return false;
    }

    // Check for the assets data CTxOut. This will always be the last output in the transaction
    if (!CheckIssueDataTx(tx.vout[tx.vout.size() - 1]))
        return false;

    // Check to make sure the owner asset is created
    if (!CheckOwnerDataTx(tx.vout[tx.vout.size() - 2]))
        return false;

    // Don't overlap with IsNewUniqueAsset()
    CScript script = tx.vout[tx.vout.size() - 1].scriptPubKey;
    if (IsScriptNewUniqueAsset(script)|| IsScriptNewRestrictedAsset(script))
        return false;

    return true;
}

//! Make sure to call VerifyNewUniqueAsset if this call returns true
bool IsNewUniqueAsset(const CTransaction& tx)
{
    // Check trailing outpoint for issue data with unique asset name
    if (!CheckIssueDataTx(tx.vout[tx.vout.size() - 1]))
        return false;

    if (!IsScriptNewUniqueAsset(tx.vout[tx.vout.size() - 1].scriptPubKey))
        return false;

    return true;
}

//! Call this function after IsNewUniqueAsset
bool VerifyNewUniqueAsset(const CTransaction& tx, std::string& strError)
{
    // Must contain at least 3 outpoints (AVN burn, owner change and one or more new unique assets that share a root (should be in trailing position))
    if (tx.vout.size() < 3) {
        strError  = "bad-txns-unique-vout-size-to-small";
        return false;
    }

    // check for (and count) new unique asset outpoints.  make sure they share a root.
    std::set<std::string> setUniqueAssets;
    std::string assetRoot = "";
    int assetOutpointCount = 0;

    for (auto out : tx.vout) {
        if (IsScriptNewUniqueAsset(out.scriptPubKey)) {
            CNewAsset asset;
            std::string address;
            if (!AssetFromScript(out.scriptPubKey, asset, address)) {
                strError = "bad-txns-issue-unique-asset-from-script";
                return false;
            }
            std::string root = GetParentName(asset.strName);
            if (assetRoot.compare("") == 0)
                assetRoot = root;
            if (assetRoot.compare(root) != 0) {
                strError = "bad-txns-issue-unique-asset-compare-failed";
                return false;
            }

            // Check for duplicate unique assets in the same transaction
            if (setUniqueAssets.count(asset.strName)) {
                strError = "bad-txns-issue-unique-duplicate-name-in-same-tx";
                return false;
            }

            setUniqueAssets.insert(asset.strName);
            assetOutpointCount += 1;
        }
    }

    if (assetOutpointCount == 0) {
        strError = "bad-txns-issue-unique-asset-bad-outpoint-count";
        return false;
    }

    // check for burn outpoint (must account for each new asset)
    bool fBurnOutpointFound = false;
    for (auto out : tx.vout) {
        if (CheckIssueBurnTx(out, AssetType::UNIQUE, assetOutpointCount)) {
            fBurnOutpointFound = true;
            break;
        }
    }

    if (!fBurnOutpointFound) {
        strError = "bad-txns-issue-unique-asset-burn-outpoints-not-found";
        return false;
    }

    // check for owner change outpoint that matches root
    bool fOwnerOutFound = false;
    for (auto out : tx.vout) {
        CAssetTransfer transfer;
        std::string transferAddress;
        if (TransferAssetFromScript(out.scriptPubKey, transfer, transferAddress)) {
            if (assetRoot + OWNER_TAG == transfer.strName) {
                fOwnerOutFound = true;
                break;
            }
        }
    }

    if (!fOwnerOutFound) {
        strError = "bad-txns-issue-unique-asset-missing-owner-asset";
        return false;
    }

    // Loop through all of the vouts and make sure only the expected asset creations are taking place
    int nTransfers = 0;
    int nOwners = 0;
    int nIssues = 0;
    int nReissues = 0;
    GetTxOutAssetTypes(tx.vout, nIssues, nReissues, nTransfers, nOwners);

    if (nOwners > 0 || nReissues > 0 || nIssues != assetOutpointCount) {
        strError = "bad-txns-failed-unique-asset-formatting-check";
        return false;
    }

    return true;
}

//! To be called on CTransactions where IsNewAsset returns true
bool VerifyNewAsset(const CTransaction& tx, std::string& strError) {
    // Issuing an Asset must contain at least 3 CTxOut( Avian Burn Tx, Any Number of other Outputs ..., Owner Asset Tx, New Asset Tx)
    if (tx.vout.size() < 3) {
        strError = "bad-txns-issue-vout-size-to-small";
        return false;
    }

    // Check for the assets data CTxOut. This will always be the last output in the transaction
    if (!CheckIssueDataTx(tx.vout[tx.vout.size() - 1])) {
        strError = "bad-txns-issue-data-not-found";
        return false;
    }

    // Check to make sure the owner asset is created
    if (!CheckOwnerDataTx(tx.vout[tx.vout.size() - 2])) {
        strError = "bad-txns-issue-owner-data-not-found";
        return false;
    }

    // Get the asset type
    CNewAsset asset;
    std::string address;
    if (!AssetFromScript(tx.vout[tx.vout.size() - 1].scriptPubKey, asset, address)) {
        strError = "bad-txns-issue-serialzation-failed";
        return error("%s : Failed to get new asset from transaction: %s", __func__, tx.GetHash().GetHex());
    }

    AssetType assetType;
    IsAssetNameValid(asset.strName, assetType);

    std::string strOwnerName;
    if (!OwnerAssetFromScript(tx.vout[tx.vout.size() - 2].scriptPubKey, strOwnerName, address)) {
        strError = "bad-txns-issue-owner-serialzation-failed";
        return false;
    }

    if (strOwnerName != asset.strName + OWNER_TAG) {
        strError = "bad-txns-issue-owner-name-doesn't-match";
        return false;
    }

    // Check for the Burn CTxOut in one of the vouts ( This is needed because the change CTxOut is places in a random position in the CWalletTx
    bool fFoundIssueBurnTx = false;
    for (auto out : tx.vout) {
        if (CheckIssueBurnTx(out, assetType)) {
            fFoundIssueBurnTx = true;
            break;
        }
    }

    if (!fFoundIssueBurnTx) {
        strError = "bad-txns-issue-burn-not-found";
        return false;
    }

    if (assetType == AssetType::SUB) {
        std::string root = GetParentName(asset.strName);
        bool fOwnerOutFound = false;
        for (auto out : tx.vout) {
            CAssetTransfer transfer;
            std::string transferAddress;
            if (TransferAssetFromScript(out.scriptPubKey, transfer, transferAddress)) {
                if (root + OWNER_TAG == transfer.strName) {
                    fOwnerOutFound = true;
                    break;
                }
            }
        }

        if (!fOwnerOutFound) {
            strError = "bad-txns-issue-new-asset-missing-owner-asset";
            return false;
        }
    }

    // Loop through all of the vouts and make sure only the expected asset creations are taking place
    int nTransfers = 0;
    int nOwners = 0;
    int nIssues = 0;
    int nReissues = 0;
    GetTxOutAssetTypes(tx.vout, nIssues, nReissues, nTransfers, nOwners);

    if (nOwners != 1 || nIssues != 1 || nReissues > 0) {
        strError = "bad-txns-failed-issue-asset-formatting-check";
        return false;
    }

    return true;
}

//! Make sure to call VerifyNewUniqueAsset if this call returns true
bool IsNewMsgChannelAsset(const CTransaction& tx)
{
    // Check trailing outpoint for issue data with unique asset name
    if (!CheckIssueDataTx(tx.vout[tx.vout.size() - 1]))
        return false;

    if (!IsScriptNewMsgChannelAsset(tx.vout[tx.vout.size() - 1].scriptPubKey))
        return false;

    return true;
}

//! To be called on CTransactions where IsNewAsset returns true
bool VerifyNewMsgChannelAsset(const CTransaction& tx, std::string &strError)
{
    // Issuing an Asset must contain at least 3 CTxOut( Avian Burn Tx, Any Number of other Outputs ..., Owner Asset Tx, New Asset Tx)
    if (tx.vout.size() < 3) {
        strError  = "bad-txns-issue-msgchannel-vout-size-to-small";
        return false;
    }

    // Check for the assets data CTxOut. This will always be the last output in the transaction
    if (!CheckIssueDataTx(tx.vout[tx.vout.size() - 1])) {
        strError  = "bad-txns-issue-data-not-found";
        return false;
    }

    // Get the asset type
    CNewAsset asset;
    std::string address;
    if (!MsgChannelAssetFromScript(tx.vout[tx.vout.size() - 1].scriptPubKey, asset, address)) {
        strError = "bad-txns-issue-msgchannel-serialzation-failed";
        return error("%s : Failed to get new msgchannel asset from transaction: %s", __func__, tx.GetHash().GetHex());
    }

    AssetType assetType;
    IsAssetNameValid(asset.strName, assetType);

    // Check for the Burn CTxOut in one of the vouts ( This is needed because the change CTxOut is places in a random position in the CWalletTx
    bool fFoundIssueBurnTx = false;
    for (auto out : tx.vout) {
        if (CheckIssueBurnTx(out, AssetType::MSGCHANNEL)) {
            fFoundIssueBurnTx = true;
            break;
        }
    }

    if (!fFoundIssueBurnTx) {
        strError = "bad-txns-issue-msgchannel-burn-not-found";
        return false;
    }

    // check for owner change outpoint that matches root
    std::string root = GetParentName(asset.strName);
    bool fOwnerOutFound = false;
    for (auto out : tx.vout) {
        CAssetTransfer transfer;
        std::string transferAddress;
        if (TransferAssetFromScript(out.scriptPubKey, transfer, transferAddress)) {
            if (root + OWNER_TAG == transfer.strName) {
                fOwnerOutFound = true;
                break;
            }
        }
    }

    if (!fOwnerOutFound) {
        strError = "bad-txns-issue-msg-channel-asset-bad-owner-asset";
        return false;
    }

    // Loop through all of the vouts and make sure only the expected asset creations are taking place
    int nTransfers = 0;
    int nOwners = 0;
    int nIssues = 0;
    int nReissues = 0;
    GetTxOutAssetTypes(tx.vout, nIssues, nReissues, nTransfers, nOwners);

    if (nOwners != 0 || nIssues != 1 || nReissues > 0) {
        strError = "bad-txns-failed-issue-msgchannel-asset-formatting-check";
        return false;
    }

    return true;
}

//! Make sure to call VerifyNewQualifierAsset if this call returns true
bool IsNewQualifierAsset(const CTransaction& tx)
{
    // Check trailing outpoint for issue data with unique asset name
    if (!CheckIssueDataTx(tx.vout[tx.vout.size() - 1]))
        return false;

    if (!IsScriptNewQualifierAsset(tx.vout[tx.vout.size() - 1].scriptPubKey))
        return false;

    return true;
}

//! To be called on CTransactions where IsNewQualifierAsset returns true
bool VerifyNewQualfierAsset(const CTransaction& tx, std::string &strError)
{
    // Issuing an Asset must contain at least 2 CTxOut( Avian Burn Tx, New Asset Tx, Any Number of other Outputs...)
    if (tx.vout.size() < 2) {
        strError  = "bad-txns-issue-qualifier-vout-size-to-small";
        return false;
    }

    // Check for the assets data CTxOut. This will always be the last output in the transaction
    if (!CheckIssueDataTx(tx.vout[tx.vout.size() - 1])) {
        strError  = "bad-txns-issue-qualifider-data-not-found";
        return false;
    }

    // Get the asset type
    CNewAsset asset;
    std::string address;
    if (!QualifierAssetFromScript(tx.vout[tx.vout.size() - 1].scriptPubKey, asset, address)) {
        strError = "bad-txns-issue-qualifier-serialzation-failed";
        return error("%s : Failed to get new qualifier asset from transaction: %s", __func__, tx.GetHash().GetHex());
    }

    AssetType assetType;
    IsAssetNameValid(asset.strName, assetType);

    // Check for the Burn CTxOut in one of the vouts ( This is needed because the change CTxOut is places in a random position in the CWalletTx
    bool fFoundIssueBurnTx = false;
    for (auto out : tx.vout) {
        if (CheckIssueBurnTx(out, assetType)) {
            fFoundIssueBurnTx = true;
            break;
        }
    }

    if (!fFoundIssueBurnTx) {
        strError = "bad-txns-issue-qualifier-burn-not-found";
        return false;
    }

    if (assetType == AssetType::SUB_QUALIFIER) {
        // Check that there is an asset transfer with the parent name, qualifier use just the parent name, they don't use not parent + !
        bool fOwnerOutFound = false;
        std::string root = GetParentName(asset.strName);
        for (auto out : tx.vout) {
            CAssetTransfer transfer;
            std::string transferAddress;
            if (TransferAssetFromScript(out.scriptPubKey, transfer, transferAddress)) {
                if (root == transfer.strName) {
                    fOwnerOutFound = true;
                    break;
                }
            }
        }

        if (!fOwnerOutFound) {
            strError  = "bad-txns-issue-sub-qualifier-parent-outpoint-not-found";
            return false;
        }
    }

    // Loop through all of the vouts and make sure only the expected asset creations are taking place
    int nTransfers = 0;
    int nOwners = 0;
    int nIssues = 0;
    int nReissues = 0;
    GetTxOutAssetTypes(tx.vout, nIssues, nReissues, nTransfers, nOwners);

    if (nOwners != 0 || nIssues != 1 || nReissues > 0) {
        strError = "bad-txns-failed-issue-asset-formatting-check";
        return false;
    }

    return true;
}

//! Make sure to call VerifyNewAsset if this call returns true
bool IsNewRestrictedAsset(const CTransaction& tx)
{
    // Check trailing outpoint for issue data with unique asset name
    if (!CheckIssueDataTx(tx.vout[tx.vout.size() - 1]))
        return false;

    if (!IsScriptNewRestrictedAsset(tx.vout[tx.vout.size() - 1].scriptPubKey))
        return false;

    return true;
}

//! To be called on CTransactions where IsNewRestrictedAsset returns true
bool VerifyNewRestrictedAsset(const CTransaction& tx, std::string& strError) {
    // Issuing a restricted asset must cointain at least 4 CTxOut(Avian Burn Tx, Asset Creation, Root Owner Token Transfer, and CNullAssetTxVerifierString)
    if (tx.vout.size() < 4) {
        strError = "bad-txns-issue-restricted-vout-size-to-small";
        return false;
    }

    // Check for the assets data CTxOut. This will always be the last output in the transaction
    if (!CheckIssueDataTx(tx.vout[tx.vout.size() - 1])) {
        strError = "bad-txns-issue-restricted-data-not-found";
        return false;
    }

    // Get the asset type
    CNewAsset asset;
    std::string address;
    if (!RestrictedAssetFromScript(tx.vout[tx.vout.size() - 1].scriptPubKey, asset, address)) {
        strError = "bad-txns-issue-restricted-serialization-failed";
        return error("%s : Failed to get new restricted asset from transaction: %s", __func__, tx.GetHash().GetHex());
    }

    AssetType assetType;
    IsAssetNameValid(asset.strName, assetType);

    // Check for the Burn CTxOut in one of the vouts ( This is needed because the change CTxOut is places in a random position in the CWalletTx
    bool fFoundIssueBurnTx = false;
    for (auto out : tx.vout) {
        if (CheckIssueBurnTx(out, assetType)) {
            fFoundIssueBurnTx = true;
            break;
        }
    }

    if (!fFoundIssueBurnTx) {
        strError = "bad-txns-issue-restricted-burn-not-found";
        return false;
    }

    // Check that there is an asset transfer with the parent name, restricted assets use the root owner token. So issuing $TOKEN requires TOKEN!
    bool fRootOwnerOutFound = false;
    std::string root = GetParentName(asset.strName);
    std::string strippedRoot = root.substr(1, root.size() -1) + OWNER_TAG; // $TOKEN checks for TOKEN!
    for (auto out : tx.vout) {
        CAssetTransfer transfer;
        std::string transferAddress;
        if (TransferAssetFromScript(out.scriptPubKey, transfer, transferAddress)) {
            if (strippedRoot == transfer.strName) {
                fRootOwnerOutFound = true;
                break;
            }
        }
    }

    if (!fRootOwnerOutFound) {
        strError  = "bad-txns-issue-restricted-root-owner-token-outpoint-not-found";
        return false;
    }

    // Check to make sure we can get the verifier string from the transaction
    CNullAssetTxVerifierString verifier;
    if (!GetVerifierStringFromTx(tx, verifier, strError)) {
        return false;
    }

    // TODO is verifier string valid check, this happen automatically when processing the nullasset tx outputs

    // Loop through all of the vouts and make sure only the expected asset creations are taking place
    int nTransfers = 0;
    int nOwners = 0;
    int nIssues = 0;
    int nReissues = 0;
    GetTxOutAssetTypes(tx.vout, nIssues, nReissues, nTransfers, nOwners);

    if (nOwners != 0 || nIssues != 1 || nReissues > 0) {
        strError = "bad-txns-failed-issue-asset-formatting-check";
        return false;
    }

    return true;
}

bool GetVerifierStringFromTx(const CTransaction& tx, CNullAssetTxVerifierString& verifier, std::string& strError, bool& fNotFound)
{
    fNotFound = false;
    bool found = false;
    int count = 0;
    for (auto out : tx.vout) {
        if (out.scriptPubKey.IsNullAssetVerifierTxDataScript()) {
            count++;

            if (count > 1) {
                strError = _("Multiple verifier strings found in transaction");
                return false;
            }
            if (!AssetNullVerifierDataFromScript(out.scriptPubKey, verifier)) {
                strError = _("Failed to get verifier string from output: ") + out.ToString();
                return false;
            }

            found = true;
        }
    }

    // Set error message, for if it returns false
    if (!found) {
        fNotFound = true;
        strError = _("Verifier string not found");
    }

    return found && count == 1;
}

bool GetVerifierStringFromTx(const CTransaction& tx, CNullAssetTxVerifierString& verifier, std::string& strError)
{
    bool fNotFound = false;
    return GetVerifierStringFromTx(tx, verifier, strError, fNotFound);
}

bool IsReissueAsset(const CTransaction& tx)
{
    // Check for the reissue asset data CTxOut. This will always be the last output in the transaction
    if (!CheckReissueDataTx(tx.vout[tx.vout.size() - 1]))
        return false;

    return true;
}

//! To be called on CTransactions where IsReissueAsset returns true
bool VerifyReissueAsset(const CTransaction& tx, std::string& strError)
{
    // Reissuing an Asset must contain at least 3 CTxOut ( Avian Burn Tx, Any Number of other Outputs ..., Reissue Asset Tx, Owner Asset Change Tx)
    if (tx.vout.size() < 3) {
        strError  = "bad-txns-vout-size-to-small";
        return false;
    }

    // Check for the reissue asset data CTxOut. This will always be the last output in the transaction
    if (!CheckReissueDataTx(tx.vout[tx.vout.size() - 1])) {
        strError  = "bad-txns-reissue-data-not-found";
        return false;
    }

    CReissueAsset reissue;
    std::string address;
    if (!ReissueAssetFromScript(tx.vout[tx.vout.size() - 1].scriptPubKey, reissue, address)) {
        strError  = "bad-txns-reissue-serialization-failed";
        return false;
    }

    // Reissuing a regular asset checks the reissue_asset_name + "!"
    AssetType asset_type = AssetType::INVALID;
    IsAssetNameValid(reissue.strName, asset_type);

    // This is going to be the asset name that we need to verify that the owner token of was added to the transaction
    std::string asset_name_to_check = reissue.strName;

    // If the asset type is restricted, remove the $ from the name, so we can check for the correct owner token transfer
    if (asset_type == AssetType::RESTRICTED) {
        asset_name_to_check = reissue.strName.substr(1, reissue.strName.size() -1);
    }

    // Check that there is an asset transfer, this will be the owner asset change
    bool fOwnerOutFound = false;
    for (auto out : tx.vout) {
        CAssetTransfer transfer;
        std::string transferAddress;
        if (TransferAssetFromScript(out.scriptPubKey, transfer, transferAddress)) {
            if (asset_name_to_check + OWNER_TAG == transfer.strName) {
                fOwnerOutFound = true;
                break;
            }
        }
    }

    if (!fOwnerOutFound) {
        strError  = "bad-txns-reissue-owner-outpoint-not-found";
        return false;
    }

    // Check for the Burn CTxOut in one of the vouts ( This is needed because the change CTxOut is placed in a random position in the CWalletTx
    bool fFoundReissueBurnTx = false;
    for (auto out : tx.vout) {
        if (CheckReissueBurnTx(out)) {
            fFoundReissueBurnTx = true;
            break;
        }
    }

    if (!fFoundReissueBurnTx) {
        strError = "bad-txns-reissue-burn-outpoint-not-found";
        return false;
    }

    // Loop through all of the vouts and make sure only the expected asset creations are taking place
    int nTransfers = 0;
    int nOwners = 0;
    int nIssues = 0;
    int nReissues = 0;
    GetTxOutAssetTypes(tx.vout, nIssues, nReissues, nTransfers, nOwners);

    if (nOwners > 0 || nReissues != 1 || nIssues > 0) {
        strError = "bad-txns-failed-reissue-asset-formatting-check";
        return false;
    }

    return true;
}

bool CheckAddingTagBurnFee(const CTransaction& tx, const int& count)
{
    // check for burn outpoint )
    bool fBurnOutpointFound = false;
    for (auto out : tx.vout) {
        if (CheckIssueBurnTx(out, AssetType::NULL_ADD_QUALIFIER, count)) {
            fBurnOutpointFound = true;
            break;
        }
    }

   return fBurnOutpointFound;
}

CAssetTransfer::CAssetTransfer(const std::string& strAssetName, const CAmount& nAmount, const std::string& message, const int64_t& nExpireTime)
{
    SetNull();
    this->strName = strAssetName;
    this->nAmount = nAmount;
    this->message = message;
    if (!message.empty()) {
        if (nExpireTime) {
            this->nExpireTime = nExpireTime;
        } else {
            this->nExpireTime = 0;
        }
    }
}

bool CAssetTransfer::IsValid(std::string& strError) const
{
    // Don't use this function with any sort of consensus checks
    // All of these checks are run with ContextualCheckTransferAsset also

    strError = "";

    if (!IsAssetNameValid(std::string(strName))) {
        strError = "Invalid parameter: asset_name must only consist of valid characters and have a size between 3 and 30 characters. See help for more details.";
        return false;
    }

    // this function is only being called in createrawtranasction, so it is fine to have a contextual check here
    // if this gets called anywhere else, we will need to move this to a Contextual function
    if (nAmount <= 0) {
        strError = "Invalid parameter: asset amount can't be equal to or less than zero.";
        return false;
    }

    if (message.empty() && nExpireTime > 0) {
        strError = "Invalid parameter: asset transfer expiration time requires a message to be attached to the transfer";
        return false;
    }

    if (nExpireTime < 0) {
        strError = "Invalid parameter: expiration time must be a positive value";
        return false;
    }

    if (message.size() && !CheckEncoded(message, strError)) {
        return false;
    }

    return true;
}

bool CAssetTransfer::ContextualCheckAgainstVerifyString(CAssetsCache *assetCache, const std::string& address, std::string& strError) const
{
    // Get the verifier string
    CNullAssetTxVerifierString verifier;
    if (!assetCache->GetAssetVerifierStringIfExists(this->strName, verifier, true)) {
        // This shouldn't ever happen, but if it does we need to know
        strError = _("Verifier String doesn't exist for asset: ") + this->strName;
        return false;
    }

    if (!ContextualCheckVerifierString(assetCache, verifier.verifier_string, address, strError))
        return false;

    return true;
}

void CAssetTransfer::ConstructTransaction(CScript& script) const
{
    DataStream ssTransfer{};
    ssTransfer << *this;

    std::vector<unsigned char> vchMessage;
    vchMessage.push_back(AVN_R); // r
    vchMessage.push_back(AVN_V); // v
    vchMessage.push_back(AVN_N); // n
    vchMessage.push_back(AVN_T); // t

    vchMessage.insert(vchMessage.end(), UCharCast(ssTransfer.data()), UCharCast(ssTransfer.data() + ssTransfer.size()));
    script << OP_AVN_ASSET << ToByteVector(vchMessage) << OP_DROP;
}

CReissueAsset::CReissueAsset(const std::string &strAssetName, const CAmount &nAmount, const int &nUnits, const int &nReissuable,
                             const std::string &strIPFSHash, const std::string &strANSID)
{
    SetNull();
    this->strName = strAssetName;
    this->strIPFSHash = strIPFSHash;
    this->strANSID = strANSID;
    this->nReissuable = int8_t(nReissuable);
    this->nAmount = nAmount;
    this->nUnits = nUnits;
}

void CReissueAsset::ConstructTransaction(CScript& script) const
{
    DataStream ssReissue{};
    ssReissue << *this;

    std::vector<unsigned char> vchMessage;
    vchMessage.push_back(AVN_R); // r
    vchMessage.push_back(AVN_V); // v
    vchMessage.push_back(AVN_N); // n
    vchMessage.push_back(AVN_R); // r

    vchMessage.insert(vchMessage.end(), UCharCast(ssReissue.data()), UCharCast(ssReissue.data() + ssReissue.size()));
    script << OP_AVN_ASSET << ToByteVector(vchMessage) << OP_DROP;
}

bool CReissueAsset::IsNull() const
{
    return strName == "" || nAmount < 0;
}

bool CAssetsCache::AddTransferAsset(const CAssetTransfer& transferAsset, const std::string& address, const COutPoint& out, const CTxOut& txOut)
{
    AddToAssetBalance(transferAsset.strName, address, transferAsset.nAmount);

    // Add to cache so we can save to database
    CAssetCacheNewTransfer newTransfer(transferAsset, address, out);

    if (setNewTransferAssetsToRemove.count(newTransfer))
        setNewTransferAssetsToRemove.erase(newTransfer);

    setNewTransferAssetsToAdd.insert(newTransfer);

    return true;
}

void CAssetsCache::AddToAssetBalance(const std::string& strName, const std::string& address, const CAmount& nAmount)
{
    if (fAssetIndex) {
        auto pair = std::make_pair(strName, address);
        // Add to map address -> amount map

        // Get the best amount
        if (!GetBestAssetAddressAmount(*this, strName, address))
            mapAssetsAddressAmount.insert(make_pair(pair, 0));

        // Add the new amount to the balance
        if (IsAssetNameAnOwner(strName))
            mapAssetsAddressAmount.at(pair) = OWNER_ASSET_AMOUNT;
        else
            mapAssetsAddressAmount.at(pair) += nAmount;
    }
}

bool CAssetsCache::TrySpendCoin(const COutPoint& out, const CTxOut& txOut)
{
    // Placeholder strings that will get set if you successfully get the transfer or asset from the script
    std::string address = "";
    std::string assetName = "";
    CAmount nAmount = -1;

    // Get the asset tx data
    int nType = -1;
    bool fIsOwner = false;
    if (txOut.scriptPubKey.IsAssetScript(nType, fIsOwner)) {

        // Get the New Asset or Transfer Asset from the scriptPubKey
        if (nType == TX_NEW_ASSET && !fIsOwner) {
            CNewAsset asset;
            if (AssetFromScript(txOut.scriptPubKey, asset, address)) {
                assetName = asset.strName;
                nAmount = asset.nAmount;
            }
        } else if (nType == TX_TRANSFER_ASSET) {
            CAssetTransfer transfer;
            if (TransferAssetFromScript(txOut.scriptPubKey, transfer, address)) {
                assetName = transfer.strName;
                nAmount = transfer.nAmount;
            }
        } else if (nType == TX_NEW_ASSET && fIsOwner) {
            if (!OwnerAssetFromScript(txOut.scriptPubKey, assetName, address))
                return error("%s : ERROR Failed to get owner asset from the OutPoint: %s", __func__,
                             out.ToString());
            nAmount = OWNER_ASSET_AMOUNT;
        } else if (nType == TX_REISSUE_ASSET) {
            CReissueAsset reissue;
            if (ReissueAssetFromScript(txOut.scriptPubKey, reissue, address)) {
                assetName = reissue.strName;
                nAmount = reissue.nAmount;
            }
        }
    } else {
        // If it isn't an asset tx return true, we only fail if an error occurs
        return true;
    }

    // If we got the address and the assetName, proceed to remove it from the database, and in memory objects
    if (address != "" && assetName != "") {
        if (fAssetIndex && nAmount > 0) {
            CAssetCacheSpendAsset spend(assetName, address, nAmount);
            if (GetBestAssetAddressAmount(*this, assetName, address)) {
                auto pair = make_pair(assetName, address);
                if (mapAssetsAddressAmount.count(pair))
                    mapAssetsAddressAmount.at(pair) -= nAmount;

                if (mapAssetsAddressAmount.at(pair) < 0)
                    mapAssetsAddressAmount.at(pair) = 0;

                // Update the cache so we can save to database
                vSpentAssets.push_back(spend);
            }
        }
    } else {
        return error("%s : ERROR Failed to get asset from the OutPoint: %s", __func__, out.ToString());
    }

    return true;
}

bool CAssetsCache::ContainsAsset(const CNewAsset& asset)
{
    return CheckIfAssetExists(asset.strName);
}

bool CAssetsCache::ContainsAsset(const std::string& assetName)
{
    return CheckIfAssetExists(assetName);
}

bool CAssetsCache::UndoAssetCoin(const Coin& coin, const COutPoint& out)
{
    std::string strAddress = "";
    std::string assetName = "";
    CAmount nAmount = 0;

    // Get the asset tx from the script
    int nType = -1;
    bool fIsOwner = false;
    if(coin.out.scriptPubKey.IsAssetScript(nType, fIsOwner)) {

        if (nType == TX_NEW_ASSET && !fIsOwner) {
            CNewAsset asset;
            if (!AssetFromScript(coin.out.scriptPubKey, asset, strAddress)) {
                return error("%s : Failed to get asset from script while trying to undo asset spend. OutPoint : %s",
                             __func__,
                             out.ToString());
            }
            assetName = asset.strName;

            nAmount = asset.nAmount;
        } else if (nType == TX_TRANSFER_ASSET) {
            CAssetTransfer transfer;
            if (!TransferAssetFromScript(coin.out.scriptPubKey, transfer, strAddress))
                return error(
                        "%s : Failed to get transfer asset from script while trying to undo asset spend. OutPoint : %s",
                        __func__,
                        out.ToString());

            assetName = transfer.strName;
            nAmount = transfer.nAmount;
        } else if (nType == TX_NEW_ASSET && fIsOwner) {
            std::string ownerName;
            if (!OwnerAssetFromScript(coin.out.scriptPubKey, ownerName, strAddress))
                return error(
                        "%s : Failed to get owner asset from script while trying to undo asset spend. OutPoint : %s",
                        __func__, out.ToString());
            assetName = ownerName;
            nAmount = OWNER_ASSET_AMOUNT;
        } else if (nType == TX_REISSUE_ASSET) {
            CReissueAsset reissue;
            if (!ReissueAssetFromScript(coin.out.scriptPubKey, reissue, strAddress))
                return error(
                        "%s : Failed to get reissue asset from script while trying to undo asset spend. OutPoint : %s",
                        __func__, out.ToString());
            assetName = reissue.strName;
            nAmount = reissue.nAmount;
        }
    }

    if (assetName == "" || strAddress == "" || nAmount == 0)
        return error("%s : AssetName, Address or nAmount is invalid., Asset Name: %s, Address: %s, Amount: %d", __func__, assetName, strAddress, nAmount);

    if (!AddBackSpentAsset(coin, assetName, strAddress, nAmount, out))
        return error("%s : Failed to add back the spent asset. OutPoint : %s", __func__, out.ToString());

    return true;
}

//! Changes Memory Only
bool CAssetsCache::AddBackSpentAsset(const Coin& coin, const std::string& assetName, const std::string& address, const CAmount& nAmount, const COutPoint& out)
{
    if (fAssetIndex) {
        // Update the assets address balance
        auto pair = std::make_pair(assetName, address);

        // Get the map address amount from database if the map doesn't have it already
        if (!GetBestAssetAddressAmount(*this, assetName, address))
            mapAssetsAddressAmount.insert(std::make_pair(pair, 0));

        mapAssetsAddressAmount.at(pair) += nAmount;
    }

    // Add the undoAmount to the vector so we know what changes are dirty and what needs to be saved to database
    CAssetCacheUndoAssetAmount undoAmount(assetName, address, nAmount);
    vUndoAssetAmount.push_back(undoAmount);

    return true;
}

//! Changes Memory Only
bool CAssetsCache::UndoTransfer(const CAssetTransfer& transfer, const std::string& address, const COutPoint& outToRemove)
{
    if (fAssetIndex) {
        // Make sure we are in a valid state to undo the transfer of the asset
        if (!GetBestAssetAddressAmount(*this, transfer.strName, address))
            return error("%s : Failed to get the assets address balance from the database. Asset : %s Address : %s",
                         __func__, transfer.strName, address);

        auto pair = std::make_pair(transfer.strName, address);
        if (!mapAssetsAddressAmount.count(pair))
            return error(
                    "%s : Tried undoing a transfer and the map of address amount didn't have the asset address pair. Asset : %s Address : %s",
                    __func__, transfer.strName, address);

        if (mapAssetsAddressAmount.at(pair) < transfer.nAmount)
            return error(
                    "%s : Tried undoing a transfer and the map of address amount had less than the amount we are trying to undo. Asset : %s Address : %s",
                    __func__, transfer.strName, address);

        // Change the in memory balance of the asset at the address
        mapAssetsAddressAmount[pair] -= transfer.nAmount;
    }

    return true;
}

//! Changes Memory Only
bool CAssetsCache::RemoveNewAsset(const CNewAsset& asset, const std::string address)
{
    if (!CheckIfAssetExists(asset.strName))
        return error("%s : Tried removing an asset that didn't exist. Asset Name : %s", __func__, asset.strName);

    CAssetCacheNewAsset newAsset(asset, address, 0 , uint256());

    if (setNewAssetsToAdd.count(newAsset))
        setNewAssetsToAdd.erase(newAsset);

    setNewAssetsToRemove.insert(newAsset);

    if (fAssetIndex)
        mapAssetsAddressAmount[std::make_pair(asset.strName, address)] = 0;

    return true;
}

//! Changes Memory Only
bool CAssetsCache::AddNewAsset(const CNewAsset& asset, const std::string address, const int& nHeight, const uint256& blockHash)
{
    if(CheckIfAssetExists(asset.strName))
        return error("%s: Tried adding new asset, but it already existed in the set of assets: %s", __func__, asset.strName);

    CAssetCacheNewAsset newAsset(asset, address, nHeight, blockHash);

    if (setNewAssetsToRemove.count(newAsset))
        setNewAssetsToRemove.erase(newAsset);

    setNewAssetsToAdd.insert(newAsset);

    if (fAssetIndex) {
        // Insert the asset into the assests address amount map
        mapAssetsAddressAmount[std::make_pair(asset.strName, address)] = asset.nAmount;
    }

    return true;
}

//! Changes Memory Only
bool CAssetsCache::AddReissueAsset(const CReissueAsset& reissue, const std::string address, const COutPoint& out)
{
    auto pair = std::make_pair(reissue.strName, address);

    CNewAsset asset;
    int assetHeight;
    uint256 assetBlockHash;
    if (!GetAssetMetaDataIfExists(reissue.strName, asset, assetHeight, assetBlockHash))
        return error("%s: Failed to get the original asset that is getting reissued. Asset Name : %s",
                     __func__, reissue.strName);

    // Insert the reissue information into the reissue map
    if (!mapReissuedAssetData.count(reissue.strName)) {
        asset.nAmount += reissue.nAmount;
        asset.nReissuable = reissue.nReissuable;

        if (reissue.nUnits != -1)
            asset.units = reissue.nUnits;

        if (reissue.strIPFSHash != "") {
            asset.nHasIPFS = 1;
            asset.strIPFSHash = reissue.strIPFSHash;
        }

        if (reissue.strANSID != "") {
            asset.nHasANS = 1;
            asset.strANSID = reissue.strANSID;
        }

        mapReissuedAssetData.insert(make_pair(reissue.strName, asset));
    } else {
        mapReissuedAssetData.at(reissue.strName).nAmount += reissue.nAmount;
        mapReissuedAssetData.at(reissue.strName).nReissuable = reissue.nReissuable;

        if (reissue.nUnits != -1) {
            mapReissuedAssetData.at(reissue.strName).units = reissue.nUnits;
        }

        if (reissue.strIPFSHash != "") {
            mapReissuedAssetData.at(reissue.strName).nHasIPFS = 1;
            mapReissuedAssetData.at(reissue.strName).strIPFSHash = reissue.strIPFSHash;
        }

        if (reissue.strANSID != "") {
            mapReissuedAssetData.at(reissue.strName).nHasANS = 1;
            mapReissuedAssetData.at(reissue.strName).strANSID = reissue.strANSID;
        }
    }

    CAssetCacheReissueAsset reissueAsset(reissue, address, out, assetHeight, assetBlockHash);

    if (setNewReissueToRemove.count(reissueAsset))
        setNewReissueToRemove.erase(reissueAsset);

    setNewReissueToAdd.insert(reissueAsset);

    if (fAssetIndex) {
        // Add the reissued amount to the address amount map
        if (!GetBestAssetAddressAmount(*this, reissue.strName, address))
            mapAssetsAddressAmount.insert(make_pair(pair, 0));

        // Add the reissued amount to the amount in the map
        mapAssetsAddressAmount[pair] += reissue.nAmount;
    }

    return true;

}

//! Changes Memory Only
bool CAssetsCache::RemoveReissueAsset(const CReissueAsset& reissue, const std::string address, const COutPoint& out, const std::vector<std::pair<std::string, CBlockAssetUndo> >& vUndoData)
{
    auto pair = std::make_pair(reissue.strName, address);

    CNewAsset assetData;
    int height;
    uint256 blockHash;
    if (!GetAssetMetaDataIfExists(reissue.strName, assetData, height, blockHash))
        return error("%s: Tried undoing reissue of an asset, but that asset didn't exist: %s", __func__, reissue.strName);

    // Change the asset data by undoing what was reissued
    assetData.nAmount -= reissue.nAmount;
    assetData.nReissuable = 1;

    bool fVerifierStringChanged = false;
    std::string verifierString = "";
    // Find the ipfs hash in the undoblock data and restore the ipfs hash to its previous hash
    for (auto undoItem : vUndoData) {
        if (undoItem.first == reissue.strName) {
            if (undoItem.second.fChangedIPFS)
                assetData.strIPFSHash = undoItem.second.strIPFS;
            if (undoItem.second.fChangedANS)
                assetData.strANSID = undoItem.second.strANSID;
            if(undoItem.second.fChangedUnits)
                assetData.units = undoItem.second.nUnits;
            if (assetData.strIPFSHash == "")
                assetData.nHasIPFS = 0;
            if (assetData.strANSID == "")
                assetData.nHasANS = 0;
            if (undoItem.second.fChangedVerifierString) {
                fVerifierStringChanged = true;
                verifierString = undoItem.second.verifierString;

            }
            break;
        }
    }

    mapReissuedAssetData[assetData.strName] = assetData;

    CAssetCacheReissueAsset reissueAsset(reissue, address, out, height, blockHash);

    if (setNewReissueToAdd.count(reissueAsset))
        setNewReissueToAdd.erase(reissueAsset);

    setNewReissueToRemove.insert(reissueAsset);

    // If the verifier string was changed by this reissue, undo the change
    if (fVerifierStringChanged) {
        RemoveRestrictedVerifier(assetData.strName, verifierString, true);
    }

    if (fAssetIndex) {
        // Get the best amount form the database or dirty cache
        if (!GetBestAssetAddressAmount(*this, reissue.strName, address)) {
            if (reissueAsset.reissue.nAmount != 0)
                return error("%s : Trying to undo reissue of an asset but the assets amount isn't in the database",
                         __func__);
        }
        mapAssetsAddressAmount[pair] -= reissue.nAmount;

        if (mapAssetsAddressAmount[pair] < 0)
            return error("%s : Tried undoing reissue of an asset, but the assets amount went negative: %s", __func__,
                         reissue.strName);
    }

    return true;
}

//! Changes Memory Only
bool CAssetsCache::AddOwnerAsset(const std::string& assetsName, const std::string address)
{
    // Update the cache
    CAssetCacheNewOwner newOwner(assetsName, address);

    if (setNewOwnerAssetsToRemove.count(newOwner))
        setNewOwnerAssetsToRemove.erase(newOwner);

    setNewOwnerAssetsToAdd.insert(newOwner);

    if (fAssetIndex) {
        // Insert the asset into the assests address amount map
        mapAssetsAddressAmount[std::make_pair(assetsName, address)] = OWNER_ASSET_AMOUNT;
    }

    return true;
}

//! Changes Memory Only
bool CAssetsCache::RemoveOwnerAsset(const std::string& assetsName, const std::string address)
{
    // Update the cache
    CAssetCacheNewOwner newOwner(assetsName, address);
    if (setNewOwnerAssetsToAdd.count(newOwner))
        setNewOwnerAssetsToAdd.erase(newOwner);

    setNewOwnerAssetsToRemove.insert(newOwner);

    if (fAssetIndex) {
        auto pair = std::make_pair(assetsName, address);
        mapAssetsAddressAmount[pair] = 0;
    }

    return true;
}

//! Changes Memory Only
bool CAssetsCache::RemoveTransfer(const CAssetTransfer &transfer, const std::string &address, const COutPoint &out)
{
    if (!UndoTransfer(transfer, address, out))
        return error("%s : Failed to undo the transfer", __func__);

    CAssetCacheNewTransfer newTransfer(transfer, address, out);
    if (setNewTransferAssetsToAdd.count(newTransfer))
        setNewTransferAssetsToAdd.erase(newTransfer);

    setNewTransferAssetsToRemove.insert(newTransfer);

    return true;
}

//! Changes Memory Only, this only called when adding a block to the chain
bool CAssetsCache::AddQualifierAddress(const std::string& assetName, const std::string& address, const QualifierType type)
{
    CAssetCacheQualifierAddress newQualifier(assetName, address, type);

    // We are adding a qualifier that was in a transaction, so, if the set of qualifiers
    // that contains qualifiers to undo contains the same qualfier assetName, and address, erase it
    if (setNewQualifierAddressToRemove.count(newQualifier)) {
        setNewQualifierAddressToRemove.erase(newQualifier);
    }

    // If the set of qualifiers from transactions contains our qualifier already, we need to overwrite it
    if (setNewQualifierAddressToAdd.count(newQualifier)) {
        setNewQualifierAddressToAdd.erase(newQualifier);
    }

    if (IsAssetNameASubQualifier(assetName)) {
        if (type == QualifierType::ADD_QUALIFIER) {
            mapRootQualifierAddressesAdd[CAssetCacheRootQualifierChecker(GetParentName(assetName), address)].insert(assetName);
            mapRootQualifierAddressesRemove[CAssetCacheRootQualifierChecker(GetParentName(assetName), address)].erase(assetName);
        } else {
            mapRootQualifierAddressesRemove[CAssetCacheRootQualifierChecker(GetParentName(assetName), address)].insert(assetName);
            mapRootQualifierAddressesAdd[CAssetCacheRootQualifierChecker(GetParentName(assetName), address)].erase(assetName);
        }
    }

    setNewQualifierAddressToAdd.insert(newQualifier);

    return true;
}

//! Changes Memory Only, this is only called when undoing a block from the chain
bool CAssetsCache::RemoveQualifierAddress(const std::string& assetName, const std::string& address, const QualifierType type)
{
    CAssetCacheQualifierAddress newQualifier(assetName, address, type);

    // We are adding a qualifier that was in a transaction, so, if the set of qualifiers
    // that contains qualifiers to undo contains the same qualfier assetName, and address, erase it
    if (setNewQualifierAddressToAdd.count(newQualifier)) {
        setNewQualifierAddressToAdd.erase(newQualifier);
    }

    // If the set of qualifiers from transactions contains our qualifier already, we need to overwrite it
    if (setNewQualifierAddressToRemove.count(newQualifier)) {
        setNewQualifierAddressToRemove.erase(newQualifier);
    }

    if (IsAssetNameASubQualifier(assetName)) {
        if (type == QualifierType::ADD_QUALIFIER) {
            // When undoing a add, we want to remove it
            mapRootQualifierAddressesRemove[CAssetCacheRootQualifierChecker(GetParentName(assetName), address)].insert(assetName);
            mapRootQualifierAddressesAdd[CAssetCacheRootQualifierChecker(GetParentName(assetName), address)].erase(assetName);
        } else {
            // When undoing a remove, we want to add it
            mapRootQualifierAddressesAdd[CAssetCacheRootQualifierChecker(GetParentName(assetName), address)].insert(assetName);
            mapRootQualifierAddressesRemove[CAssetCacheRootQualifierChecker(GetParentName(assetName), address)].erase(assetName);
        }
    }

    setNewQualifierAddressToRemove.insert(newQualifier);

    return true;
}


//! Changes Memory Only, this only called when adding a block to the chain
bool CAssetsCache::AddRestrictedAddress(const std::string& assetName, const std::string& address, const RestrictedType type)
{
    CAssetCacheRestrictedAddress newRestricted(assetName, address, type);

    // We are adding a restricted address that was in a transaction, so, if the set of restricted addresses
    // to undo contains our restricted address. Erase it
    if (setNewRestrictedAddressToRemove.count(newRestricted)) {
        setNewRestrictedAddressToRemove.erase(newRestricted);
    }

    // If the set of restricted addresses from transactions contains our restricted asset address already, we need to overwrite it
    if (setNewRestrictedAddressToAdd.count(newRestricted)) {
        setNewRestrictedAddressToAdd.erase(newRestricted);
    }

    setNewRestrictedAddressToAdd.insert(newRestricted);

    return true;
}

//! Changes Memory Only, this is only called when undoing a block from the chain
bool CAssetsCache::RemoveRestrictedAddress(const std::string& assetName, const std::string& address, const RestrictedType type)
{
    CAssetCacheRestrictedAddress newRestricted(assetName, address, type);

    // We are undoing a restricted address transaction, so if the set that contains restricted address from new block
    // contains this restricted address, erase it.
    if (setNewRestrictedAddressToAdd.count(newRestricted)) {
        setNewRestrictedAddressToAdd.erase(newRestricted);
    }

    // If the set of restricted address to undo contains our restricted address already, we need to overwrite it
    if (setNewRestrictedAddressToRemove.count(newRestricted)) {
        setNewRestrictedAddressToRemove.erase(newRestricted);
    }

    setNewRestrictedAddressToRemove.insert(newRestricted);

    return true;
}

//! Changes Memory Only, this only called when adding a block to the chain
bool CAssetsCache::AddGlobalRestricted(const std::string& assetName, const RestrictedType type)
{
    CAssetCacheRestrictedGlobal newGlobalRestriction(assetName, type);

    // We are adding a global restriction transaction, so if the set the contains undo global restrictions,
    // contains this global restriction, erase it
    if (setNewRestrictedGlobalToRemove.count(newGlobalRestriction)) {
        setNewRestrictedGlobalToRemove.erase(newGlobalRestriction);
    }

    // If the set of global restrictions to add already contains our set, overwrite it
    if (setNewRestrictedGlobalToAdd.count(newGlobalRestriction)) {
        setNewRestrictedGlobalToAdd.erase(newGlobalRestriction);
    }

    setNewRestrictedGlobalToAdd.insert(newGlobalRestriction);

    return true;
}

//! Changes Memory Only, this is only called when undoing a block from the chain
bool CAssetsCache::RemoveGlobalRestricted(const std::string& assetName, const RestrictedType type)
{
    CAssetCacheRestrictedGlobal newGlobalRestriction(assetName, type);

    // We are undoing a global restriction transaction, so if the set the contains new global restrictions,
    // contains this global restriction, erase it
    if (setNewRestrictedGlobalToAdd.count(newGlobalRestriction)) {
        setNewRestrictedGlobalToAdd.erase(newGlobalRestriction);
    }

    // If the set of global restrictions to undo already contains our set, overwrite it
    if (setNewRestrictedGlobalToRemove.count(newGlobalRestriction)) {
        setNewRestrictedGlobalToRemove.erase(newGlobalRestriction);
    }

    setNewRestrictedGlobalToRemove.insert(newGlobalRestriction);

    return true;
}

//! Changes Memory Only
bool CAssetsCache::AddRestrictedVerifier(const std::string& assetName, const std::string& verifier)
{
    // Insert the reissue information into the reissue map
    CAssetCacheRestrictedVerifiers newVerifier(assetName, verifier);

    if (setNewRestrictedVerifierToRemove.count(newVerifier))
        setNewRestrictedVerifierToRemove.erase(newVerifier);

    setNewRestrictedVerifierToAdd.insert(newVerifier);

    return true;
}

//! Changes Memory Only
bool CAssetsCache::RemoveRestrictedVerifier(const std::string& assetName, const std::string& verifier, const bool fUndoingReissue)
{
    CAssetCacheRestrictedVerifiers newVerifier(assetName, verifier);
    newVerifier.fUndoingRessiue = fUndoingReissue;

    if (setNewRestrictedVerifierToAdd.count(newVerifier))
        setNewRestrictedVerifierToAdd.erase(newVerifier);

    setNewRestrictedVerifierToRemove.insert(newVerifier);

    return true;
}

bool CAssetsCache::DumpCacheToDatabase()
{
    try {
        bool dirty = false;
        std::string message;

        // Remove new assets from the database
        for (auto newAsset : setNewAssetsToRemove) {
            passetsCache->Erase(newAsset.asset.strName);
            if (!passetsdb->EraseAssetData(newAsset.asset.strName)) {
                dirty = true;
                message = "_Failed Erasing New Asset Data from database";
            }

            if (dirty) {
                return error("%s : %s", __func__, message);
            }

            if (!prestricteddb->EraseVerifier(newAsset.asset.strName)) {
                dirty = true;
                message = "_Failed Erasing verifier of new asset removal data from database";
            }

            if (fAssetIndex) {
                if (!passetsdb->EraseAssetAddressQuantity(newAsset.asset.strName, newAsset.address)) {
                    dirty = true;
                    message = "_Failed Erasing Address Balance from database";
                }

                if (!passetsdb->EraseAddressAssetQuantity(newAsset.address, newAsset.asset.strName)) {
                    dirty = true;
                    message = "_Failed Erasing New Asset Address Balance from AddressAsset database";
                }
            }

            if (dirty) {
                return error("%s : %s", __func__, message);
            }
        }

        // Add the new assets to the database
        for (auto newAsset : setNewAssetsToAdd) {
            passetsCache->Put(newAsset.asset.strName, CDatabasedAssetData(newAsset.asset, newAsset.blockHeight, newAsset.blockHash));
            if (!passetsdb->WriteAssetData(newAsset.asset, newAsset.blockHeight, newAsset.blockHash)) {
                dirty = true;
                message = "_Failed Writing New Asset Data to database";
            }

            if (dirty) {
                return error("%s : %s", __func__, message);
            }

            if (fAssetIndex) {
                if (!passetsdb->WriteAssetAddressQuantity(newAsset.asset.strName, newAsset.address,
                                                          newAsset.asset.nAmount)) {
                    dirty = true;
                    message = "_Failed Writing Address Balance to database";
                }

                if (!passetsdb->WriteAddressAssetQuantity(newAsset.address, newAsset.asset.strName,
                                                          newAsset.asset.nAmount)) {
                    dirty = true;
                    message = "_Failed Writing Address Balance to database";
                }
            }

            if (dirty) {
                return error("%s : %s", __func__, message);
            }
        }

        if (fAssetIndex) {
            // Remove the new owners from database
            for (auto ownerAsset : setNewOwnerAssetsToRemove) {
                if (!passetsdb->EraseAssetAddressQuantity(ownerAsset.assetName, ownerAsset.address)) {
                    dirty = true;
                    message = "_Failed Erasing Owner Address Balance from database";
                }

                if (!passetsdb->EraseAddressAssetQuantity(ownerAsset.address, ownerAsset.assetName)) {
                    dirty = true;
                    message = "_Failed Erasing New Owner Address Balance from AddressAsset database";
                }

                if (dirty) {
                    return error("%s : %s", __func__, message);
                }
            }

            // Add the new owners to database
            for (auto ownerAsset : setNewOwnerAssetsToAdd) {
                auto pair = std::make_pair(ownerAsset.assetName, ownerAsset.address);
                if (mapAssetsAddressAmount.count(pair) && mapAssetsAddressAmount.at(pair) > 0) {
                    if (!passetsdb->WriteAssetAddressQuantity(ownerAsset.assetName, ownerAsset.address,
                                                              mapAssetsAddressAmount.at(pair))) {
                        dirty = true;
                        message = "_Failed Writing Owner Address Balance to database";
                    }

                    if (!passetsdb->WriteAddressAssetQuantity(ownerAsset.address, ownerAsset.assetName,
                                                              mapAssetsAddressAmount.at(pair))) {
                        dirty = true;
                        message = "_Failed Writing Address Balance to database";
                    }

                    if (dirty) {
                        return error("%s : %s", __func__, message);
                    }
                }
            }

            // Undo the transfering by updating the balances in the database

            for (auto undoTransfer : setNewTransferAssetsToRemove) {
                auto pair = std::make_pair(undoTransfer.transfer.strName, undoTransfer.address);
                if (mapAssetsAddressAmount.count(pair)) {
                    if (mapAssetsAddressAmount.at(pair) == 0) {
                        if (!passetsdb->EraseAssetAddressQuantity(undoTransfer.transfer.strName,
                                                                  undoTransfer.address)) {
                            dirty = true;
                            message = "_Failed Erasing Address Quantity from database";
                        }

                        if (!passetsdb->EraseAddressAssetQuantity(undoTransfer.address,
                                                                  undoTransfer.transfer.strName)) {
                            dirty = true;
                            message = "_Failed Erasing UndoTransfer Address Balance from AddressAsset database";
                        }

                        if (dirty) {
                            return error("%s : %s", __func__, message);
                        }
                    } else {
                        if (!passetsdb->WriteAssetAddressQuantity(undoTransfer.transfer.strName,
                                                                  undoTransfer.address,
                                                                  mapAssetsAddressAmount.at(pair))) {
                            dirty = true;
                            message = "_Failed Writing updated Address Quantity to database when undoing transfers";
                        }

                        if (!passetsdb->WriteAddressAssetQuantity(undoTransfer.address,
                                                                  undoTransfer.transfer.strName,
                                                                  mapAssetsAddressAmount.at(pair))) {
                            dirty = true;
                            message = "_Failed Writing Address Balance to database";
                        }

                        if (dirty) {
                            return error("%s : %s", __func__, message);
                        }
                    }
                }
            }


            // Save the new transfers by updating the quantity in the database
            for (auto newTransfer : setNewTransferAssetsToAdd) {
                auto pair = std::make_pair(newTransfer.transfer.strName, newTransfer.address);
                // During init and reindex it disconnects and verifies blocks, can create a state where vNewTransfer will contain transfers that have already been spent. So if they aren't in the map, we can skip them.
                if (mapAssetsAddressAmount.count(pair)) {
                    if (!passetsdb->WriteAssetAddressQuantity(newTransfer.transfer.strName, newTransfer.address,
                                                              mapAssetsAddressAmount.at(pair))) {
                        dirty = true;
                        message = "_Failed Writing new address quantity to database";
                    }

                    if (!passetsdb->WriteAddressAssetQuantity(newTransfer.address, newTransfer.transfer.strName,
                                                              mapAssetsAddressAmount.at(pair))) {
                        dirty = true;
                        message = "_Failed Writing Address Balance to database";
                    }

                    if (dirty) {
                        return error("%s : %s", __func__, message);
                    }
                }
            }
        }

        for (auto newReissue : setNewReissueToAdd) {
            auto reissue_name = newReissue.reissue.strName;
            auto pair = make_pair(reissue_name, newReissue.address);
            if (mapReissuedAssetData.count(reissue_name)) {
                if(!passetsdb->WriteAssetData(mapReissuedAssetData.at(reissue_name), newReissue.blockHeight, newReissue.blockHash)) {
                    dirty = true;
                    message = "_Failed Writing reissue asset data to database";
                }

                if (dirty) {
                    return error("%s : %s", __func__, message);
                }

                passetsCache->Erase(reissue_name);

                if (fAssetIndex) {

                    if (mapAssetsAddressAmount.count(pair) && mapAssetsAddressAmount.at(pair) > 0) {
                        if (!passetsdb->WriteAssetAddressQuantity(pair.first, pair.second,
                                                                  mapAssetsAddressAmount.at(pair))) {
                            dirty = true;
                            message = "_Failed Writing reissue asset quantity to the address quantity database";
                        }

                        if (!passetsdb->WriteAddressAssetQuantity(pair.second, pair.first,
                                                                  mapAssetsAddressAmount.at(pair))) {
                            dirty = true;
                            message = "_Failed Writing Address Balance to database";
                        }

                        if (dirty) {
                            return error("%s, %s", __func__, message);
                        }
                    }
                }
            }
        }

        for (auto undoReissue : setNewReissueToRemove) {
            // In the case the the issue and reissue are both being removed
            // we can skip this call because the removal of the issue should remove all data pertaining the to asset
            // Fixes the issue where the reissue data will write over the removed asset meta data that was removed above
            CNewAsset asset(undoReissue.reissue.strName, 0);
            CAssetCacheNewAsset testNewAssetCache(asset, "", 0 , uint256());
            if (setNewAssetsToRemove.count(testNewAssetCache)) {
                continue;
            }

            auto reissue_name = undoReissue.reissue.strName;
            if (mapReissuedAssetData.count(reissue_name)) {
                if(!passetsdb->WriteAssetData(mapReissuedAssetData.at(reissue_name), undoReissue.blockHeight, undoReissue.blockHash)) {
                    dirty = true;
                    message = "_Failed Writing undo reissue asset data to database";
                }

                if (fAssetIndex) {
                    auto pair = make_pair(undoReissue.reissue.strName, undoReissue.address);
                    if (mapAssetsAddressAmount.count(pair)) {
                        if (mapAssetsAddressAmount.at(pair) == 0) {
                            if (!passetsdb->EraseAssetAddressQuantity(reissue_name, undoReissue.address)) {
                                dirty = true;
                                message = "_Failed Erasing Address Balance from database";
                            }

                            if (!passetsdb->EraseAddressAssetQuantity(undoReissue.address, reissue_name)) {
                                dirty = true;
                                message = "_Failed Erasing UndoReissue Balance from AddressAsset database";
                            }
                        } else {
                            if (!passetsdb->WriteAssetAddressQuantity(reissue_name, undoReissue.address,
                                                                      mapAssetsAddressAmount.at(pair))) {
                                dirty = true;
                                message = "_Failed Writing the undo of reissue of asset from database";
                            }

                            if (!passetsdb->WriteAddressAssetQuantity(undoReissue.address, reissue_name,
                                                                      mapAssetsAddressAmount.at(pair))) {
                                dirty = true;
                                message = "_Failed Writing Address Balance to database";
                            }
                        }
                    }
                }

                if (dirty) {
                    return error("%s : %s", __func__, message);
                }

                passetsCache->Erase(reissue_name);
            }
        }

        // Add new verifier strings for restricted assets
        for (auto newVerifier : setNewRestrictedVerifierToAdd) {
            auto assetName = newVerifier.assetName;
            if (!prestricteddb->WriteVerifier(assetName, newVerifier.verifier)) {
                dirty = true;
                message = "_Failed Writing restricted verifier to database";
            }

            if (dirty) {
                return error("%s : %s", __func__, message);
            }

            passetsVerifierCache->Erase(assetName);
        }

        // Undo verifier string for restricted assets
        for (auto undoVerifiers : setNewRestrictedVerifierToRemove) {
            auto assetName = undoVerifiers.assetName;

            // If we are undoing a reissue, we need to save back the old verifier string to database
            if (undoVerifiers.fUndoingRessiue) {
                if (!prestricteddb->WriteVerifier(assetName, undoVerifiers.verifier)) {
                    dirty = true;
                    message = "_Failed Writing undo restricted verifer to database";
                }
            } else {
                if (!prestricteddb->EraseVerifier(assetName)) {
                    dirty = true;
                    message = "_Failed Writing undo restricted verifer to database";
                }
            }

            if (dirty) {
                return error("%s : %s", __func__, message);
            }

            passetsVerifierCache->Erase(assetName);
        }

        // Add the new qualifier commands to the database
        for (auto newQualifierAddress : setNewQualifierAddressToAdd) {
            if (newQualifierAddress.type == QualifierType::REMOVE_QUALIFIER) {
                passetsQualifierCache->Erase(newQualifierAddress.GetHash().GetHex());
                if (!prestricteddb->EraseAddressQualifier(newQualifierAddress.address, newQualifierAddress.assetName)) {
                    dirty = true;
                    message = "_Failed Erasing address qualifier from database";
                }
                if (fAssetIndex && !dirty) {
                    if (!prestricteddb->EraseQualifierAddress(newQualifierAddress.address,
                                                              newQualifierAddress.assetName)) {
                        dirty = true;
                        message = "_Failed Erasing qualifier address from database";
                    }
                }
            } else if (newQualifierAddress.type == QualifierType::ADD_QUALIFIER) {
                passetsQualifierCache->Put(newQualifierAddress.GetHash().GetHex(), 1);
                if (!prestricteddb->WriteAddressQualifier(newQualifierAddress.address, newQualifierAddress.assetName))
                {
                    dirty = true;
                    message = "_Failed Writing address qualifier to database";
                }
                if (fAssetIndex & !dirty) {
                    if (!prestricteddb->WriteQualifierAddress(newQualifierAddress.address, newQualifierAddress.assetName))
                    {
                        dirty = true;
                        message = "_Failed Writing qualifier address to database";
                    }
                }
            }

            if (dirty) {
                return error("%s : %s", __func__, message);
            }
        }

        // Undo the qualifier commands
        for (auto undoQualifierAddress : setNewQualifierAddressToRemove) {
            if (undoQualifierAddress.type == QualifierType::REMOVE_QUALIFIER) { // If we are undoing a removal, we write the data to database
                passetsQualifierCache->Put(undoQualifierAddress.GetHash().GetHex(), 1);
                if (!prestricteddb->WriteAddressQualifier(undoQualifierAddress.address, undoQualifierAddress.assetName)) {
                    dirty = true;
                    message = "_Failed undoing a removal of a address qualifier  from database";
                }
                if (fAssetIndex & !dirty) {
                    if (!prestricteddb->WriteQualifierAddress(undoQualifierAddress.address, undoQualifierAddress.assetName))
                    {
                        dirty = true;
                        message = "_Failed undoing a removal of a qualifier address from database";
                    }
                }
            } else if (undoQualifierAddress.type == QualifierType::ADD_QUALIFIER) { // If we are undoing an addition, we remove the data from the database
                passetsQualifierCache->Erase(undoQualifierAddress.GetHash().GetHex());
                if (!prestricteddb->EraseAddressQualifier(undoQualifierAddress.address, undoQualifierAddress.assetName))
                {
                    dirty = true;
                    message = "_Failed undoing a addition of a address qualifier to database";
                }
                if (fAssetIndex && !dirty) {
                    if (!prestricteddb->EraseQualifierAddress(undoQualifierAddress.address,
                                                              undoQualifierAddress.assetName)) {
                        dirty = true;
                        message = "_Failed undoing a addition of a qualifier address from database";
                    }
                }
            }

            if (dirty) {
                return error("%s : %s", __func__, message);
            }
        }

        // Add new restricted address commands
        for (auto newRestrictedAddress : setNewRestrictedAddressToAdd) {
            if (newRestrictedAddress.type == RestrictedType::UNFREEZE_ADDRESS) {
                passetsRestrictionCache->Erase(newRestrictedAddress.GetHash().GetHex());
                if (!prestricteddb->EraseRestrictedAddress(newRestrictedAddress.address, newRestrictedAddress.assetName)) {
                    dirty = true;
                    message = "_Failed Erasing restricted address from database";
                }
            } else if (newRestrictedAddress.type == RestrictedType::FREEZE_ADDRESS) {
                passetsRestrictionCache->Put(newRestrictedAddress.GetHash().GetHex(), 1);
                if (!prestricteddb->WriteRestrictedAddress(newRestrictedAddress.address, newRestrictedAddress.assetName))
                {
                    dirty = true;
                    message = "_Failed Writing restricted address to database";
                }
            }

            if (dirty) {
                return error("%s : %s", __func__, message);
            }
        }

        // Undo the qualifier addresses from database
        for (auto undoRestrictedAddress : setNewRestrictedAddressToRemove) {
            if (undoRestrictedAddress.type == RestrictedType::UNFREEZE_ADDRESS) { // If we are undoing an unfreeze, we need to freeze the address
                passetsRestrictionCache->Put(undoRestrictedAddress.GetHash().GetHex(), 1);
                if (!prestricteddb->WriteRestrictedAddress(undoRestrictedAddress.address, undoRestrictedAddress.assetName)) {
                    dirty = true;
                    message = "_Failed undoing a removal of a restricted address from database";
                }
            } else if (undoRestrictedAddress.type == RestrictedType::FREEZE_ADDRESS) { // If we are undoing a freeze, we need to unfreeze the address
                passetsRestrictionCache->Erase(undoRestrictedAddress.GetHash().GetHex());
                if (!prestricteddb->EraseRestrictedAddress(undoRestrictedAddress.address, undoRestrictedAddress.assetName))
                {
                    dirty = true;
                    message = "_Failed undoing a addition of a restricted address to database";
                }
            }

            if (dirty) {
                return error("%s : %s", __func__, message);
            }
        }

        // Add new global restriction commands
        for (auto newGlobalRestriction : setNewRestrictedGlobalToAdd) {
            if (newGlobalRestriction.type == RestrictedType::GLOBAL_UNFREEZE) {
                passetsGlobalRestrictionCache->Erase(newGlobalRestriction.assetName);
                if (!prestricteddb->EraseGlobalRestriction(newGlobalRestriction.assetName)) {
                    dirty = true;
                    message = "_Failed Erasing global restriction from database";
                }
            } else if (newGlobalRestriction.type == RestrictedType::GLOBAL_FREEZE) {
                passetsGlobalRestrictionCache->Put(newGlobalRestriction.assetName, 1);
                if (!prestricteddb->WriteGlobalRestriction(newGlobalRestriction.assetName))
                {
                    dirty = true;
                    message = "_Failed Writing global restriction to database";
                }
            }

            if (dirty) {
                return error("%s : %s", __func__, message);
            }
        }

        // Undo the global restriction commands
        for (auto undoGlobalRestriction : setNewRestrictedGlobalToRemove) {
            if (undoGlobalRestriction.type == RestrictedType::GLOBAL_UNFREEZE) { // If we are undoing an global unfreeze, we need to write a global freeze
                passetsGlobalRestrictionCache->Put(undoGlobalRestriction.assetName, 1);
                if (!prestricteddb->WriteGlobalRestriction(undoGlobalRestriction.assetName)) {
                    dirty = true;
                    message = "_Failed undoing a global unfreeze of a restricted asset from database";
                }
            } else if (undoGlobalRestriction.type == RestrictedType::GLOBAL_FREEZE) { // If we are undoing a global freeze, erase the freeze from the database
                passetsGlobalRestrictionCache->Erase(undoGlobalRestriction.assetName);
                if (!prestricteddb->EraseGlobalRestriction(undoGlobalRestriction.assetName))
                {
                    dirty = true;
                    message = "_Failed undoing a global freeze of a restricted asset to database";
                }
            }

            if (dirty) {
                return error("%s : %s", __func__, message);
            }
        }

        if (fAssetIndex) {
            // Undo the asset spends by updating there balance in the database
            for (auto undoSpend : vUndoAssetAmount) {
                auto pair = std::make_pair(undoSpend.assetName, undoSpend.address);
                if (mapAssetsAddressAmount.count(pair)) {
                    if (!passetsdb->WriteAssetAddressQuantity(undoSpend.assetName, undoSpend.address,
                                                              mapAssetsAddressAmount.at(pair))) {
                        dirty = true;
                        message = "_Failed Writing updated Address Quantity to database when undoing spends";
                    }

                    if (!passetsdb->WriteAddressAssetQuantity(undoSpend.address, undoSpend.assetName,
                                                              mapAssetsAddressAmount.at(pair))) {
                        dirty = true;
                        message = "_Failed Writing Address Balance to database";
                    }

                    if (dirty) {
                        return error("%s : %s", __func__, message);
                    }
                }
            }


            // Save the assets that have been spent by erasing the quantity in the database
            for (auto spentAsset : vSpentAssets) {
                auto pair = make_pair(spentAsset.assetName, spentAsset.address);
                if (mapAssetsAddressAmount.count(pair)) {
                    if (mapAssetsAddressAmount.at(pair) == 0) {
                        if (!passetsdb->EraseAssetAddressQuantity(spentAsset.assetName, spentAsset.address)) {
                            dirty = true;
                            message = "_Failed Erasing a Spent Asset, from database";
                        }

                        if (!passetsdb->EraseAddressAssetQuantity(spentAsset.address, spentAsset.assetName)) {
                            dirty = true;
                            message = "_Failed Erasing a Spent Asset from AddressAsset database";
                        }

                        if (dirty) {
                            return error("%s : %s", __func__, message);
                        }
                    } else {
                        if (!passetsdb->WriteAssetAddressQuantity(spentAsset.assetName, spentAsset.address,
                                                                  mapAssetsAddressAmount.at(pair))) {
                            dirty = true;
                            message = "_Failed Erasing a Spent Asset, from database";
                        }

                        if (!passetsdb->WriteAddressAssetQuantity(spentAsset.address, spentAsset.assetName,
                                                                  mapAssetsAddressAmount.at(pair))) {
                            dirty = true;
                            message = "_Failed Writing Address Balance to database";
                        }

                        if (dirty) {
                            return error("%s : %s", __func__, message);
                        }
                    }
                }
            }
        }

        ClearDirtyCache();

        return true;
    } catch (const std::runtime_error& e) {
        return error("%s : %s ", __func__, std::string("System error while flushing assets: ") + e.what());
    }
}

// This function will put all current cache data into the global passets cache.
//! Do not call this function on the passets pointer
bool CAssetsCache::Flush()
{

    if (!passets)
        return error("%s: Couldn't find passets pointer while trying to flush assets cache", __func__);

    try {
        for (auto &item : setNewAssetsToAdd) {
            if (passets->setNewAssetsToRemove.count(item))
                passets->setNewAssetsToRemove.erase(item);
            passets->setNewAssetsToAdd.insert(item);
        }

        for (auto &item : setNewAssetsToRemove) {
            if (passets->setNewAssetsToAdd.count(item))
                passets->setNewAssetsToAdd.erase(item);
            passets->setNewAssetsToRemove.insert(item);
        }

        for (auto &item : mapAssetsAddressAmount)
            passets->mapAssetsAddressAmount[item.first] = item.second;

        for (auto &item : mapReissuedAssetData)
            passets->mapReissuedAssetData[item.first] = item.second;

        for (auto &item : setNewOwnerAssetsToAdd) {
            if (passets->setNewOwnerAssetsToRemove.count(item))
                passets->setNewOwnerAssetsToRemove.erase(item);
            passets->setNewOwnerAssetsToAdd.insert(item);
        }

        for (auto &item : setNewOwnerAssetsToRemove) {
            if (passets->setNewOwnerAssetsToAdd.count(item))
                passets->setNewOwnerAssetsToAdd.erase(item);
            passets->setNewOwnerAssetsToRemove.insert(item);
        }

        for (auto &item : setNewReissueToAdd) {
            if (passets->setNewReissueToRemove.count(item))
                passets->setNewReissueToRemove.erase(item);
            passets->setNewReissueToAdd.insert(item);
        }

        for (auto &item : setNewReissueToRemove) {
            if (passets->setNewReissueToAdd.count(item))
                passets->setNewReissueToAdd.erase(item);
            passets->setNewReissueToRemove.insert(item);
        }

        for (auto &item : setNewTransferAssetsToAdd) {
            if (passets->setNewTransferAssetsToRemove.count(item))
                passets->setNewTransferAssetsToRemove.erase(item);
            passets->setNewTransferAssetsToAdd.insert(item);
        }

        for (auto &item : setNewTransferAssetsToRemove) {
            if (passets->setNewTransferAssetsToAdd.count(item))
                passets->setNewTransferAssetsToAdd.erase(item);
            passets->setNewTransferAssetsToRemove.insert(item);
        }

        for (auto &item : vSpentAssets) {
            passets->vSpentAssets.emplace_back(item);
        }

        for (auto &item : vUndoAssetAmount) {
            passets->vUndoAssetAmount.emplace_back(item);
        }

        for(auto &item : setNewQualifierAddressToAdd) {
            if (passets->setNewQualifierAddressToRemove.count(item)) {
                passets->setNewQualifierAddressToRemove.erase(item);
            }

            if (passets->setNewQualifierAddressToAdd.count(item)) {
                passets->setNewQualifierAddressToAdd.erase(item);
            }

            passets->setNewQualifierAddressToAdd.insert(item);
        }

        for(auto &item : setNewQualifierAddressToRemove) {
            if (passets->setNewQualifierAddressToAdd.count(item)) {
                passets->setNewQualifierAddressToAdd.erase(item);
            }

            if (passets->setNewQualifierAddressToRemove.count(item)) {
                passets->setNewQualifierAddressToRemove.erase(item);
            }

            passets->setNewQualifierAddressToRemove.insert(item);
        }

        for(auto &item : setNewRestrictedAddressToAdd) {
            if (passets->setNewRestrictedAddressToRemove.count(item)) {
                passets->setNewRestrictedAddressToRemove.erase(item);
            }

            if (passets->setNewRestrictedAddressToAdd.count(item)) {
                passets->setNewRestrictedAddressToAdd.erase(item);
            }

            passets->setNewRestrictedAddressToAdd.insert(item);
        }

        for(auto &item : setNewRestrictedAddressToRemove) {
            if (passets->setNewRestrictedAddressToAdd.count(item)) {
                passets->setNewRestrictedAddressToAdd.erase(item);
            }

            if (passets->setNewRestrictedAddressToRemove.count(item)) {
                passets->setNewRestrictedAddressToRemove.erase(item);
            }

            passets->setNewRestrictedAddressToRemove.insert(item);
        }

        for(auto &item : setNewRestrictedGlobalToAdd) {
            if (passets->setNewRestrictedGlobalToRemove.count(item)) {
                passets->setNewRestrictedGlobalToRemove.erase(item);
            }

            if (passets->setNewRestrictedGlobalToAdd.count(item)) {
                passets->setNewRestrictedGlobalToAdd.erase(item);
            }

            passets->setNewRestrictedGlobalToAdd.insert(item);
        }

        for(auto &item : setNewRestrictedGlobalToRemove) {
            if (passets->setNewRestrictedGlobalToAdd.count(item)) {
                passets->setNewRestrictedGlobalToAdd.erase(item);
            }

            if (passets->setNewRestrictedGlobalToRemove.count(item)) {
                passets->setNewRestrictedGlobalToRemove.erase(item);
            }

            passets->setNewRestrictedGlobalToRemove.insert(item);
        }

        for (auto &item : setNewRestrictedVerifierToAdd) {
            if (passets->setNewRestrictedVerifierToRemove.count(item)) {
                passets->setNewRestrictedVerifierToRemove.erase(item);
            }

            if (passets->setNewRestrictedVerifierToAdd.count(item)) {
                passets->setNewRestrictedVerifierToAdd.erase(item);
            }

            passets->setNewRestrictedVerifierToAdd.insert(item);
        }

        for (auto &item : setNewRestrictedVerifierToRemove) {
            if (passets->setNewRestrictedVerifierToAdd.count(item)) {
                passets->setNewRestrictedVerifierToAdd.erase(item);
            }

            if (passets->setNewRestrictedVerifierToRemove.count(item)) {
                passets->setNewRestrictedVerifierToRemove.erase(item);
            }

            passets->setNewRestrictedVerifierToRemove.insert(item);
        }

        for (auto &item : mapRootQualifierAddressesAdd) {
            for (auto asset : item.second) {
                passets->mapRootQualifierAddressesAdd[item.first].insert(asset);
            }
        }

        for (auto &item : mapRootQualifierAddressesRemove) {
            for (auto asset : item.second) {
                passets->mapRootQualifierAddressesAdd[item.first].insert(asset);
            }
        }

        return true;

    } catch (const std::runtime_error& e) {
        return error("%s : %s ", __func__, std::string("System error while flushing assets: ") + e.what());
    }
}

//! Get the amount of memory the cache is using
size_t CAssetsCache::DynamicMemoryUsage() const
{
    // TODO make sure this is accurate
    return memusage::DynamicUsage(mapAssetsAddressAmount) + memusage::DynamicUsage(mapReissuedAssetData);
}

//! Get an estimated size of the cache in bytes that will be needed inorder to save to database
size_t CAssetsCache::GetCacheSize() const
{
    // COutPoint: 32 bytes
    // CNewAsset: Max 80 bytes
    // CAssetTransfer: Asset Name, CAmount ( 40 bytes)
    // CReissueAsset: Max 80 bytes
    // CAmount: 8 bytes
    // Asset Name: Max 32 bytes
    // Address: 40 bytes
    // Block hash: 32 bytes
    // CTxOut: CAmount + CScript (105 + 8 = 113 bytes)

    size_t size = 0;

    size += (32 + 40 + 8) * vUndoAssetAmount.size(); // Asset Name, Address, CAmount

    size += (40 + 40 + 32) * setNewTransferAssetsToRemove.size(); // CAssetTrasnfer, Address, COutPoint
    size += (40 + 40 + 32) * setNewTransferAssetsToAdd.size(); // CAssetTrasnfer, Address, COutPoint

    size += 72 * setNewOwnerAssetsToAdd.size(); // Asset Name, Address
    size += 72 * setNewOwnerAssetsToRemove.size(); // Asset Name, Address

    size += (32 + 40 + 8) * vSpentAssets.size(); // Asset Name, Address, CAmount

    size += (80 + 40 + 32 + sizeof(int)) * setNewAssetsToAdd.size(); // CNewAsset, Address, Block hash, int
    size += (80 + 40 + 32 + sizeof(int)) * setNewAssetsToRemove.size(); // CNewAsset, Address, Block hash, int

    size += (80 + 40 + 32 + 32 + sizeof(int)) * setNewReissueToAdd.size(); // CReissueAsset, Address, COutPoint, Block hash, int
    size += (80 + 40 + 32 + 32 + sizeof(int)) * setNewReissueToRemove.size(); // CReissueAsset, Address, COutPoint, Block hash, int

    // TODO add the qualfier, and restricted sets into this calculation

    return size;
}

//! Get an estimated size of the cache in bytes that will be needed inorder to save to database
size_t CAssetsCache::GetCacheSizeV2() const
{
    // COutPoint: 32 bytes
    // CNewAsset: Max 80 bytes
    // CAssetTransfer: Asset Name, CAmount ( 40 bytes)
    // CReissueAsset: Max 80 bytes
    // CAmount: 8 bytes
    // Asset Name: Max 32 bytes
    // Address: 40 bytes
    // Block hash: 32 bytes
    // CTxOut: CAmount + CScript (105 + 8 = 113 bytes)

    size_t size = 0;
    size += memusage::DynamicUsage(vUndoAssetAmount);
    size += memusage::DynamicUsage(setNewTransferAssetsToRemove);
    size += memusage::DynamicUsage(setNewTransferAssetsToAdd);
    size += memusage::DynamicUsage(setNewOwnerAssetsToAdd);
    size += memusage::DynamicUsage(setNewOwnerAssetsToRemove);
    size += memusage::DynamicUsage(vSpentAssets);
    size += memusage::DynamicUsage(setNewAssetsToAdd);
    size += memusage::DynamicUsage(setNewAssetsToRemove);
    size += memusage::DynamicUsage(setNewReissueToAdd);
    size += memusage::DynamicUsage(setNewReissueToRemove);

    return size;
}

bool CheckIssueBurnTx(const CTxOut& txOut, const AssetType& type, const int numberIssued)
{
    if (type == AssetType::REISSUE || type == AssetType::VOTE || type == AssetType::OWNER || type == AssetType::INVALID)
        return false;

    CAmount burnAmount = 0;
    std::string burnAddress = "";

    // Get the burn address and amount for the type of asset
    burnAmount = GetBurnAmount(type);
    burnAddress = GetBurnAddress(type);

    // If issuing multiple (unique) assets need to burn for each
    burnAmount *= numberIssued;

    // Check if script satisfies the burn amount
    if (!(txOut.nValue == burnAmount))
        return false;

    // Extract the destination
    CTxDestination destination;
    if (!ExtractDestination(txOut.scriptPubKey, destination))
        return false;

    // Verify destination is valid
    if (!IsValidDestination(destination))
        return false;

    // Check destination address is the burn address
    auto strDestination = EncodeDestination(destination);
    if (!(strDestination == burnAddress))
        return false;

    return true;
}

bool CheckIssueBurnTx(const CTxOut& txOut, const AssetType& type)
{
    return CheckIssueBurnTx(txOut, type, 1);
}

bool CheckReissueBurnTx(const CTxOut& txOut)
{
    // Check the first transaction and verify that the correct AVN Amount
    if (txOut.nValue != GetReissueAssetBurnAmount())
        return false;

    // Extract the destination
    CTxDestination destination;
    if (!ExtractDestination(txOut.scriptPubKey, destination))
        return false;

    // Verify destination is valid
    if (!IsValidDestination(destination))
        return false;

    // Check destination address is the correct burn address
    if (EncodeDestination(destination) != "RXReissueAssetXXXXXXXXXXXXXXVEFAWu")
        return false;

    return true;
}

bool CheckIssueDataTx(const CTxOut& txOut)
{
    // Verify 'avnq' is in the transaction
    CScript scriptPubKey = txOut.scriptPubKey;

    int nStartingIndex = 0;
    return IsScriptNewAsset(scriptPubKey, nStartingIndex);
}

bool CheckReissueDataTx(const CTxOut& txOut)
{
    // Verify 'avnr' is in the transaction
    CScript scriptPubKey = txOut.scriptPubKey;

    return IsScriptReissueAsset(scriptPubKey);
}

bool CheckOwnerDataTx(const CTxOut& txOut)
{
    // Verify 'avnq' is in the transaction
    CScript scriptPubKey = txOut.scriptPubKey;

    return IsScriptOwnerAsset(scriptPubKey);
}

bool CheckTransferOwnerTx(const CTxOut& txOut)
{
    // Verify 'avnq' is in the transaction
    CScript scriptPubKey = txOut.scriptPubKey;

    return IsScriptTransferAsset(scriptPubKey);
}

bool IsScriptNewAsset(const CScript& scriptPubKey)
{
    int index = 0;
    return IsScriptNewAsset(scriptPubKey, index);
}

bool IsScriptNewAsset(const CScript& scriptPubKey, int& nStartingIndex)
{
    int nType = 0;
    bool fIsOwner =false;
    if (scriptPubKey.IsAssetScript(nType, fIsOwner, nStartingIndex)) {
        return nType == TX_NEW_ASSET && !fIsOwner;
    }
    return false;
}

bool IsScriptNewUniqueAsset(const CScript& scriptPubKey)
{
    int index = 0;
    return IsScriptNewUniqueAsset(scriptPubKey, index);
}

bool IsScriptNewUniqueAsset(const CScript &scriptPubKey, int &nStartingIndex)
{
    int nType = 0;
    bool fIsOwner = false;
    if (!scriptPubKey.IsAssetScript(nType, fIsOwner, nStartingIndex))
        return false;

    CNewAsset asset;
    std::string address;
    if (!AssetFromScript(scriptPubKey, asset, address))
        return false;

    AssetType assetType;
    if (!IsAssetNameValid(asset.strName, assetType))
        return false;

    return AssetType::UNIQUE == assetType;
}

bool IsScriptNewMsgChannelAsset(const CScript& scriptPubKey)
{
    int index = 0;
    return IsScriptNewMsgChannelAsset(scriptPubKey, index);
}

bool IsScriptNewMsgChannelAsset(const CScript &scriptPubKey, int &nStartingIndex)
{
    int nType = 0;
    bool fIsOwner = false;
    if (!scriptPubKey.IsAssetScript(nType, fIsOwner, nStartingIndex))
        return false;

    CNewAsset asset;
    std::string address;
    if (!AssetFromScript(scriptPubKey, asset, address))
        return false;

    AssetType assetType;
    if (!IsAssetNameValid(asset.strName, assetType))
        return false;

    return AssetType::MSGCHANNEL == assetType;
}

bool IsScriptOwnerAsset(const CScript& scriptPubKey)
{

    int index = 0;
    return IsScriptOwnerAsset(scriptPubKey, index);
}

bool IsScriptOwnerAsset(const CScript& scriptPubKey, int& nStartingIndex)
{
    int nType = 0;
    bool fIsOwner =false;
    if (scriptPubKey.IsAssetScript(nType, fIsOwner, nStartingIndex)) {
        return nType == TX_NEW_ASSET && fIsOwner;
    }

    return false;
}

bool IsScriptReissueAsset(const CScript& scriptPubKey)
{
    int index = 0;
    return IsScriptReissueAsset(scriptPubKey, index);
}

bool IsScriptReissueAsset(const CScript& scriptPubKey, int& nStartingIndex)
{
    int nType = 0;
    bool fIsOwner =false;
    if (scriptPubKey.IsAssetScript(nType, fIsOwner, nStartingIndex)) {
        return nType == TX_REISSUE_ASSET;
    }

    return false;
}

bool IsScriptTransferAsset(const CScript& scriptPubKey)
{
    int index = 0;
    return IsScriptTransferAsset(scriptPubKey, index);
}

bool IsScriptTransferAsset(const CScript& scriptPubKey, int& nStartingIndex)
{
    int nType = 0;
    bool fIsOwner = false;
    if (scriptPubKey.IsAssetScript(nType, fIsOwner, nStartingIndex)) {
        return nType == TX_TRANSFER_ASSET;
    }

    return false;
}

bool IsScriptNewQualifierAsset(const CScript& scriptPubKey)
{
    int index = 0;
    return IsScriptNewQualifierAsset(scriptPubKey, index);
}

bool IsScriptNewQualifierAsset(const CScript &scriptPubKey, int &nStartingIndex)
{
    int nType = 0;
    bool fIsOwner = false;
    if (!scriptPubKey.IsAssetScript(nType, fIsOwner, nStartingIndex))
        return false;

    CNewAsset asset;
    std::string address;
    if (!AssetFromScript(scriptPubKey, asset, address))
        return false;

    AssetType assetType;
    if (!IsAssetNameValid(asset.strName, assetType))
        return false;

    return AssetType::QUALIFIER == assetType || AssetType::SUB_QUALIFIER == assetType;
}

bool IsScriptNewRestrictedAsset(const CScript& scriptPubKey)
{
    int index = 0;
    return IsScriptNewRestrictedAsset(scriptPubKey, index);
}

bool IsScriptNewRestrictedAsset(const CScript &scriptPubKey, int &nStartingIndex)
{
    int nType = 0;
    bool fIsOwner = false;
    if (!scriptPubKey.IsAssetScript(nType, fIsOwner, nStartingIndex))
        return false;

    CNewAsset asset;
    std::string address;
    if (!AssetFromScript(scriptPubKey, asset, address))
        return false;

    AssetType assetType;
    if (!IsAssetNameValid(asset.strName, assetType))
        return false;

    return AssetType::RESTRICTED == assetType;
}


//! Returns a boolean on if the asset exists
bool CAssetsCache::CheckIfAssetExists(const std::string& name, bool fForceDuplicateCheck)
{
    // Create objects that will be used to check the dirty cache
    CNewAsset asset;
    asset.strName = name;
    CAssetCacheNewAsset cachedAsset(asset, "", 0, uint256());

    // Check the dirty caches first and see if it was recently added or removed
    if (setNewAssetsToRemove.count(cachedAsset)) {
        return false;
    }

    // Check the dirty caches first and see if it was recently added or removed
    if (passets->setNewAssetsToRemove.count(cachedAsset)) {
        return false;
    }

    if (setNewAssetsToAdd.count(cachedAsset)) {
        if (fForceDuplicateCheck) {
            return true;
        }
        else {
            LogPrintf("%s : Found asset %s in setNewAssetsToAdd but force duplicate check wasn't true\n", __func__, name);
        }
    }

    if (passets->setNewAssetsToAdd.count(cachedAsset)) {
        if (fForceDuplicateCheck) {
            return true;
        }
        else {
            LogPrintf("%s : Found asset %s in setNewAssetsToAdd but force duplicate check wasn't true\n", __func__, name);
        }
    }

    // Check the cache, if it doesn't exist in the cache. Try and read it from database
    if (passetsCache) {
        if (passetsCache->Exists(name)) {
            if (fForceDuplicateCheck) {
                return true;
            }
            else {
                LogPrintf("%s : Found asset %s in passetsCache but force duplicate check wasn't true\n", __func__, name);
            }
        } else {
            if (passetsdb) {
                CNewAsset readAsset;
                int nHeight;
                uint256 hash;
                if (passetsdb->ReadAssetData(name, readAsset, nHeight, hash)) {
                    passetsCache->Put(readAsset.strName, CDatabasedAssetData(readAsset, nHeight, hash));
                    if (fForceDuplicateCheck) {
                        return true;
                    }
                    else {
                        LogPrintf("%s : Found asset %s in passetsdb but force duplicate check wasn't true\n", __func__, name);
                    }
                }
            }
        }
    }
    return false;
}

bool CAssetsCache::GetAssetMetaDataIfExists(const std::string &name, CNewAsset &asset)
{
    int height;
    uint256 hash;
    return GetAssetMetaDataIfExists(name, asset, height, hash);
}

bool CAssetsCache::GetAssetMetaDataIfExists(const std::string &name, CNewAsset &asset, int& nHeight, uint256& blockHash)
{
    // Check the map that contains the reissued asset data. If it is in this map, it hasn't been saved to disk yet
    if (mapReissuedAssetData.count(name)) {
        asset = mapReissuedAssetData.at(name);
        return true;
    }

    // Check the map that contains the reissued asset data. If it is in this map, it hasn't been saved to disk yet
    if (passets->mapReissuedAssetData.count(name)) {
        asset = passets->mapReissuedAssetData.at(name);
        return true;
    }

    // Create objects that will be used to check the dirty cache
    CNewAsset tempAsset;
    tempAsset.strName = name;
    CAssetCacheNewAsset cachedAsset(tempAsset, "", 0, uint256());

    // Check the dirty caches first and see if it was recently added or removed
    if (setNewAssetsToRemove.count(cachedAsset)) {
        LogPrintf("%s : Found in new assets to Remove - Returning False\n", __func__);
        return false;
    }

    // Check the dirty caches first and see if it was recently added or removed
    if (passets->setNewAssetsToRemove.count(cachedAsset)) {
        LogPrintf("%s : Found in new assets to Remove - Returning False\n", __func__);
        return false;
    }

    auto setIterator = setNewAssetsToAdd.find(cachedAsset);
    if (setIterator != setNewAssetsToAdd.end()) {
        asset = setIterator->asset;
        nHeight = setIterator->blockHeight;
        blockHash = setIterator->blockHash;
        return true;
    }

    setIterator = passets->setNewAssetsToAdd.find(cachedAsset);
    if (setIterator != passets->setNewAssetsToAdd.end()) {
        asset = setIterator->asset;
        nHeight = setIterator->blockHeight;
        blockHash = setIterator->blockHash;
        return true;
    }

    // Check the cache, if it doesn't exist in the cache. Try and read it from database
    if (passetsCache) {
        if (passetsCache->Exists(name)) {
            CDatabasedAssetData data;
            data = passetsCache->Get(name);
            asset = data.asset;
            nHeight = data.nHeight;
            blockHash = data.blockHash;
            return true;
        }
    }

    if (passetsdb && passetsCache) {
        CNewAsset readAsset;
        int height;
        uint256 hash;
        if (passetsdb->ReadAssetData(name, readAsset, height, hash)) {
            asset = readAsset;
            nHeight = height;
            blockHash = hash;
            passetsCache->Put(readAsset.strName, CDatabasedAssetData(readAsset, height, hash));
            return true;
        }
    }

    LogPrintf("%s : Didn't find asset '%s' anywhere (local dirty, global dirty, LRU cache%s, LevelDB%s). Returning False\n",
              __func__, name,
              passetsCache ? "" : " [NULL]",
              passetsdb ? "" : " [NULL]");
    return false;
}

bool GetAssetInfoFromScript(const CScript& scriptPubKey, std::string& strName, CAmount& nAmount)
{
    CAssetOutputEntry data;
    if(!GetAssetData(scriptPubKey, data))
        return false;

    strName = data.assetName;
    nAmount = data.nAmount;

    return true;
}

bool GetAssetInfoFromCoin(const Coin& coin, std::string& strName, CAmount& nAmount)
{
    return GetAssetInfoFromScript(coin.out.scriptPubKey, strName, nAmount);
}

bool GetAssetData(const CScript& script, CAssetOutputEntry& data)
{
    // Placeholder strings that will get set if you successfully get the transfer or asset from the script
    std::string address = "";
    std::string assetName = "";

    int nType = 0;
    bool fIsOwner = false;
    if (!script.IsAssetScript(nType, fIsOwner)) {
        return false;
    }

    int type = nType;

    // Get the New Asset or Transfer Asset from the scriptPubKey
    if (type == TX_NEW_ASSET && !fIsOwner) {
        CNewAsset asset;
        if (AssetFromScript(script, asset, address)) {
            data.type = TX_NEW_ASSET;
            data.nAmount = asset.nAmount;
            data.destination = DecodeDestination(address);
            data.assetName = asset.strName;
            return true;
        } else if (MsgChannelAssetFromScript(script, asset, address)) {
            data.type = TX_NEW_ASSET;
            data.nAmount = asset.nAmount;
            data.destination = DecodeDestination(address);
            data.assetName = asset.strName;
        } else if (QualifierAssetFromScript(script, asset, address)) {
            data.type = TX_NEW_ASSET;
            data.nAmount = asset.nAmount;
            data.destination = DecodeDestination(address);
            data.assetName = asset.strName;
        } else if (RestrictedAssetFromScript(script, asset, address)) {
            data.type = TX_NEW_ASSET;
            data.nAmount = asset.nAmount;
            data.destination = DecodeDestination(address);
            data.assetName = asset.strName;
        }
    } else if (type == TX_TRANSFER_ASSET) {
        CAssetTransfer transfer;
        if (TransferAssetFromScript(script, transfer, address)) {
            data.type = TX_TRANSFER_ASSET;
            data.nAmount = transfer.nAmount;
            data.destination = DecodeDestination(address);
            data.assetName = transfer.strName;
            data.message = transfer.message;
            data.expireTime = transfer.nExpireTime;
            return true;
        } else {
            LogPrintf("Failed to get transfer from script\n");
        }
    } else if (type == TX_NEW_ASSET && fIsOwner) {
        if (OwnerAssetFromScript(script, assetName, address)) {
            data.type = TX_NEW_ASSET;
            data.nAmount = OWNER_ASSET_AMOUNT;
            data.destination = DecodeDestination(address);
            data.assetName = assetName;
            return true;
        }
    } else if (type == TX_REISSUE_ASSET) {
        CReissueAsset reissue;
        if (ReissueAssetFromScript(script, reissue, address)) {
            data.type = TX_REISSUE_ASSET;
            data.nAmount = reissue.nAmount;
            data.destination = DecodeDestination(address);
            data.assetName = reissue.strName;
            return true;
        }
    }

    return false;
}

CAmount GetIssueAssetBurnAmount()
{
    return 500 * COIN;
}

CAmount GetReissueAssetBurnAmount()
{
    return 100 * COIN;
}

CAmount GetIssueSubAssetBurnAmount()
{
    return 100 * COIN;
}

CAmount GetIssueUniqueAssetBurnAmount()
{
    return 5 * COIN;
}

CAmount GetIssueMsgChannelAssetBurnAmount()
{
    return 100 * COIN;
}

CAmount GetIssueQualifierAssetBurnAmount()
{
    return 1000 * COIN;
}

CAmount GetIssueSubQualifierAssetBurnAmount()
{
    return 100 * COIN;
}

CAmount GetIssueRestrictedAssetBurnAmount()
{
    return 1500 * COIN;
}

CAmount GetAddNullQualifierTagBurnAmount()
{
    return COIN / 10;
}

CAmount GetBurnAmount(const int nType)
{
    return GetBurnAmount((AssetType(nType)));
}

CAmount GetBurnAmount(const AssetType type)
{
    switch (type) {
        case AssetType::ROOT:
            return GetIssueAssetBurnAmount();
        case AssetType::SUB:
            return GetIssueSubAssetBurnAmount();
        case AssetType::MSGCHANNEL:
            return GetIssueMsgChannelAssetBurnAmount();
        case AssetType::OWNER:
            return 0;
        case AssetType::UNIQUE:
            return GetIssueUniqueAssetBurnAmount();
        case AssetType::VOTE:
            return 0;
        case AssetType::REISSUE:
            return GetReissueAssetBurnAmount();
        case AssetType::QUALIFIER:
            return GetIssueQualifierAssetBurnAmount();
        case AssetType::SUB_QUALIFIER:
            return GetIssueSubQualifierAssetBurnAmount();
        case AssetType::RESTRICTED:
            return GetIssueRestrictedAssetBurnAmount();
        case AssetType::NULL_ADD_QUALIFIER:
            return GetAddNullQualifierTagBurnAmount();
        default:
            return 0;
    }
}

std::string GetBurnAddress(const int nType)
{
    return GetBurnAddress((AssetType(nType)));
}

std::string GetBurnAddress(const AssetType type)
{
    // Testnet and regtest share the same burn address set (Ravencoin-compatible, version byte 0x6f)
    bool fTestnet = (Params().GetChainType() == ChainType::TESTNET ||
                     Params().GetChainType() == ChainType::REGTEST);

    switch (type) {
        case AssetType::ROOT:
            return fTestnet ? "n1issueAssetXXXXXXXXXXXXXXXXWdnemQ"
                            : "RXissueAssetXXXXXXXXXXXXXXXXXhhZGt";
        case AssetType::SUB:
            return fTestnet ? "n1issueSubAssetXXXXXXXXXXXXXbNiH6v"
                            : "RXissueSubAssetXXXXXXXXXXXXXWcwhwL";
        case AssetType::MSGCHANNEL:
            return fTestnet ? "n1issueMsgChanneLAssetXXXXXXW2ptUU"
                            : "RXissueMsgChanneLAssetXXXXXXSjHvAY";
        case AssetType::OWNER:
            return "";
        case AssetType::UNIQUE:
            return fTestnet ? "n1issueUniqueAssetXXXXXXXXXXS4695i"
                            : "RXissueUniqueAssetXXXXXXXXXXWEAe58";
        case AssetType::VOTE:
            return "";
        case AssetType::REISSUE:
            return fTestnet ? "n1ReissueAssetXXXXXXXXXXXXXXWG9NLd"
                            : "RXReissueAssetXXXXXXXXXXXXXXVEFAWu";
        case AssetType::QUALIFIER:
            return fTestnet ? "n1issueQuaLifierXXXXXXXXXXXXUysLTj"
                            : "RXissueQuaLifierXXXXXXXXXXXXUgEDbC";
        case AssetType::SUB_QUALIFIER:
            return fTestnet ? "n1issueSubQuaLifierXXXXXXXXXYffPLh"
                            : "RXissueSubQuaLifierXXXXXXXXXVTzvv5";
        case AssetType::RESTRICTED:
            return fTestnet ? "n1issueRestrictedXXXXXXXXXXXXZVT9V"
                            : "RXissueRestrictedXXXXXXXXXXXXzJZ1q";
        case AssetType::NULL_ADD_QUALIFIER:
            return fTestnet ? "n1addTagBurnXXXXXXXXXXXXXXXXWQj6Et"
                            : "RXaddTagBurnXXXXXXXXXXXXXXXXZQm5ya";
        default:
            return "";
    }
}

bool IsBurnAddress(const std::string& address)
{
    if (address.empty()) return false;

    // Global burn address (not tied to any specific AssetType)
    bool fTestnet = (Params().GetChainType() == ChainType::TESTNET ||
                     Params().GetChainType() == ChainType::REGTEST);
    if (address == (fTestnet ? "n1BurnXXXXXXXXXXXXXXXXXXXXXXU1qejP"
                             : "RXBurnXXXXXXXXXXXXXXXXXXXXXXWUo9FV"))
        return true;

    for (int i = 0; i <= static_cast<int>(AssetType::NULL_ADD_QUALIFIER); i++) {
        std::string burnAddr = GetBurnAddress(static_cast<AssetType>(i));
        if (!burnAddr.empty() && address == burnAddr)
            return true;
    }
    return false;
}

//! This will get the amount that an address for a certain asset contains from the database if they cache doesn't already have it
bool GetBestAssetAddressAmount(CAssetsCache& cache, const std::string& assetName, const std::string& address)
{
    if (fAssetIndex) {
        auto pair = make_pair(assetName, address);

        // If the caches map has the pair, return true because the map already contains the best dirty amount
        if (cache.mapAssetsAddressAmount.count(pair))
            return true;

        // If the caches map has the pair, return true because the map already contains the best dirty amount
        if (passets->mapAssetsAddressAmount.count(pair)) {
            cache.mapAssetsAddressAmount[pair] = passets->mapAssetsAddressAmount.at(pair);
            return true;
        }

        // If the database contains the assets address amount, insert it into the database and return true
        CAmount nDBAmount;
        if (passetsdb->ReadAssetAddressQuantity(pair.first, pair.second, nDBAmount)) {
            cache.mapAssetsAddressAmount.insert(make_pair(pair, nDBAmount));
            return true;
        }
    }

    // The amount wasn't found return false
    return false;
}

// 46 char base58 --> 34 char KAW compatible
std::string DecodeAssetData(std::string encoded)
{
    // ANS
    if (CAvianNameSystemID::IsValidID(encoded)) {
        return encoded;
    }

    // IPFS
    else if (encoded.size() == 46) {
        std::vector<unsigned char> b;
        (void)DecodeBase58(encoded, b, 64);
        return std::string(b.begin(), b.end());
    }

    // TXID
    else if (encoded.size() == 64 && IsHex(encoded)) {
        std::vector<unsigned char> vec = ParseHex(encoded);
        return std::string(vec.begin(), vec.end());
    }

    return "";

};

std::string EncodeAssetData(std::string decoded)
{
    // ANS
    if (CAvianNameSystemID::IsValidID(decoded)) {
        return decoded;
    }

    // IPFS
    else if (decoded.size() == 34) {
        return EncodeIPFS(decoded);
    }

    // TXID
    else if (decoded.size() == 32){
        return HexStr(decoded);
    }

    return "";
}

// 46 char base58 --> 34 char KAW compatible
std::string DecodeIPFS(std::string encoded)
{
    std::vector<unsigned char> b;
    (void)DecodeBase58(encoded, b, 64);
    return std::string(b.begin(), b.end());
};

// 34 char KAW compatible --> 46 char base58
std::string EncodeIPFS(std::string decoded)
{
    std::vector<char> charData(decoded.begin(), decoded.end());
    std::vector<unsigned char> unsignedCharData;
    for (char c : charData)
        unsignedCharData.push_back(static_cast<unsigned char>(c));
    return EncodeBase58(unsignedCharData);
};

// Return true if the amount is valid with the units passed in
bool CheckAmountWithUnits(const CAmount& nAmount, const int8_t nUnits)
{
    return nAmount % int64_t(pow(10, (MAX_UNIT - nUnits))) == 0;
}

bool CheckEncoded(const std::string& hash, std::string& strError) {
    std::string encodedStr = EncodeAssetData(hash);

    // ANS
    if (IsAvianNameSystemDeployed() && CAvianNameSystemID::IsValidID(encodedStr)) {
        return true;
    }

    // IPFS
    if (encodedStr.substr(0, 2) == "Qm" && encodedStr.size() == 46) {
        return true;
    }

    if (AreMessagesDeployed()) {
        if (IsHex(encodedStr) && encodedStr.length() == 64) {
            return true;
        }
    }

    strError = _("Invalid parameter: ipfs_hash/ans_id is not valid or txid hash is not the right length");

    return false;
}

void GetTxOutAssetTypes(const std::vector<CTxOut>& vout, int& issues, int& reissues, int& transfers, int& owners)
{
    for (auto out: vout) {
        int type;
        bool fIsOwner;
        if (out.scriptPubKey.IsAssetScript(type, fIsOwner)) {
            if (type == TX_NEW_ASSET && !fIsOwner)
                issues++;
            else if (type == TX_NEW_ASSET && fIsOwner)
                owners++;
            else if (type == TX_TRANSFER_ASSET)
                transfers++;
            else if (type == TX_REISSUE_ASSET)
                reissues++;
        }
    }
}

bool ParseAssetScript(CScript scriptPubKey, uint160 &hashBytes, std::string &assetName, CAmount &assetAmount) {
    int nType;
    bool fIsOwner;
    int _nStartingPoint;
    std::string _strAddress;
    bool isAsset = false;
    if (scriptPubKey.IsAssetScript(nType, fIsOwner, _nStartingPoint)) {
        if (nType == TX_NEW_ASSET) {
            if (fIsOwner) {
                if (OwnerAssetFromScript(scriptPubKey, assetName, _strAddress)) {
                    assetAmount = OWNER_ASSET_AMOUNT;
                    isAsset = true;
                } else {
                    LogPrintf("%s : Couldn't get new owner asset from script: %s", __func__, HexStr(scriptPubKey));
                }
            } else {
                CNewAsset asset;
                if (AssetFromScript(scriptPubKey, asset, _strAddress)) {
                    assetName = asset.strName;
                    assetAmount = asset.nAmount;
                    isAsset = true;
                } else {
                    LogPrintf("%s : Couldn't get new asset from script: %s", __func__, HexStr(scriptPubKey));
                }
            }
        } else if (nType == TX_REISSUE_ASSET) {
            CReissueAsset asset;
            if (ReissueAssetFromScript(scriptPubKey, asset, _strAddress)) {
                assetName = asset.strName;
                assetAmount = asset.nAmount;
                isAsset = true;
            } else {
                LogPrintf("%s : Couldn't get reissue asset from script: %s", __func__, HexStr(scriptPubKey));
            }
        } else if (nType == TX_TRANSFER_ASSET) {
            CAssetTransfer asset;
            if (TransferAssetFromScript(scriptPubKey, asset, _strAddress)) {
                assetName = asset.strName;
                assetAmount = asset.nAmount;
                isAsset = true;
            } else {
                LogPrintf("%s : Couldn't get transfer asset from script: %s", __func__, HexStr(scriptPubKey));
            }
        } else {
            LogPrintf("%s : Unsupported asset type: %s", __func__, nType);
        }
    } else {
//        LogPrintf("%s : Found no asset in script: %s", __func__, HexStr(scriptPubKey));
    }
    if (isAsset) {
//        LogPrintf("%s : Found assets in script at address %s : %s (%s)", __func__, _strAddress, assetName, assetAmount);
        hashBytes = uint160(std::vector <unsigned char>(scriptPubKey.begin()+3, scriptPubKey.begin()+23));
        return true;
    }
    return false;
}

CNullAssetTxData::CNullAssetTxData(const std::string &strAssetname, const int8_t &nFlag)
{
    SetNull();
    this->asset_name = strAssetname;
    this->flag = nFlag;
}

bool CNullAssetTxData::IsValid(std::string &strError, CAssetsCache &assetCache, bool fForceCheckPrimaryAssetExists) const
{
    AssetType type;
    if (!IsAssetNameValid(asset_name, type)) {
        strError = _("Asset name is not valid");
        return false;
    }

    if (type != AssetType::QUALIFIER && type != AssetType::SUB_QUALIFIER && type != AssetType::RESTRICTED) {
        strError = _("Asset must be a qualifier, sub qualifier, or a restricted asset");
        return false;
    }

    if (flag != 0 && flag != 1) {
        strError = _("Flag must be 1 or 0");
        return false;
    }

    if (fForceCheckPrimaryAssetExists) {
        if (!assetCache.CheckIfAssetExists(asset_name)) {
            strError = _("Asset doesn't exist: ") + asset_name;
            return false;
        }
    }

    return true;
}

void CNullAssetTxData::ConstructTransaction(CScript &script) const
{
    DataStream ssAssetTxData{};
    ssAssetTxData << *this;

    std::vector<unsigned char> vchMessage;
    auto data = UCharCast(ssAssetTxData.data());
    vchMessage.insert(vchMessage.end(), data, data + ssAssetTxData.size());
    script << ToByteVector(vchMessage);
}

void CNullAssetTxData::ConstructGlobalRestrictionTransaction(CScript &script) const
{
    DataStream ssAssetTxData{};
    ssAssetTxData << *this;

    std::vector<unsigned char> vchMessage;
    auto data = UCharCast(ssAssetTxData.data());
    vchMessage.insert(vchMessage.end(), data, data + ssAssetTxData.size());
    script << OP_AVN_ASSET << OP_RESERVED << OP_RESERVED << ToByteVector(vchMessage);
}

CNullAssetTxVerifierString::CNullAssetTxVerifierString(const std::string &verifier)
{
    SetNull();
    this->verifier_string = verifier;
}

void CNullAssetTxVerifierString::ConstructTransaction(CScript &script) const
{
    DataStream ssAssetTxData{};
    ssAssetTxData << *this;

    std::vector<unsigned char> vchMessage;
    auto data = UCharCast(ssAssetTxData.data());
    vchMessage.insert(vchMessage.end(), data, data + ssAssetTxData.size());
    script << OP_AVN_ASSET << OP_RESERVED << ToByteVector(vchMessage);
}

bool CAssetsCache::GetAssetVerifierStringIfExists(const std::string &name, CNullAssetTxVerifierString& verifierString, bool fSkipTempCache)
{

    /** There are circumstances where a blocks transactions could be changing an assets verifier string, While at the
     * same time a transaction is added to the same block that is trying to transfer the assets who verifier string is
     * changing.
     * Depending on the ordering of these two transactions. The verifier string used to verify the validity of the
     * transaction could be different.
     * To fix this all restricted asset transfer validation checks will use only the latest connect block tips caches
     * and databases to validate it. This allows for asset transfers and verify string change transactions to be added in the same block
     * without failing validation
    **/

    // Create objects that will be used to check the dirty cache
    CAssetCacheRestrictedVerifiers tempCacheVerifier {name, ""};

    auto setIterator = setNewRestrictedVerifierToRemove.find(tempCacheVerifier);
    // Check the dirty caches first and see if it was recently added or removed
    if (!fSkipTempCache && setIterator != setNewRestrictedVerifierToRemove.end()) {
        if (setIterator->fUndoingRessiue) {
            verifierString.verifier_string = setIterator->verifier;
            return true;
        }
        return false;
    }

    setIterator = passets->setNewRestrictedVerifierToRemove.find(tempCacheVerifier);
    // Check the dirty caches first and see if it was recently added or removed
    if (setIterator != passets->setNewRestrictedVerifierToRemove.end()) {
        if (setIterator->fUndoingRessiue) {
            verifierString.verifier_string = setIterator->verifier;
            return true;
        }
        return false;
    }

    setIterator = setNewRestrictedVerifierToAdd.find(tempCacheVerifier);
    if (!fSkipTempCache && setIterator != setNewRestrictedVerifierToAdd.end()) {
        verifierString.verifier_string = setIterator->verifier;
        return true;
    }

    setIterator = passets->setNewRestrictedVerifierToAdd.find(tempCacheVerifier);
    if (setIterator != passets->setNewRestrictedVerifierToAdd.end()) {
        verifierString.verifier_string = setIterator->verifier;
        return true;
    }

    // Check the cache, if it doesn't exist in the cache. Try and read it from database
    if (passetsVerifierCache) {
        if (passetsVerifierCache->Exists(name)) {
            verifierString = passetsVerifierCache->Get(name);
            return true;
        }
    }

    if (prestricteddb) {
        std::string verifier;
        if (prestricteddb->ReadVerifier(name, verifier)) {
            verifierString.verifier_string = verifier;
            if (passetsVerifierCache)
                passetsVerifierCache->Put(name, verifierString);
            return true;
        }
    }

    return false;
}

bool CAssetsCache::CheckForAddressQualifier(const std::string &qualifier_name, const std::string& address, bool fSkipTempCache)
{
    /** There are circumstances where a blocks transactions could be removing or adding a qualifier to an address,
     * While at the same time a transaction is added to the same block that is trying to transfer to the same address.
     * Depending on the ordering of these two transactions. The qualifier database used to verify the validity of the
     * transactions could be different.
     * To fix this all restricted asset transfer validation checks will use only the latest connect block tips caches
     * and databases to validate it. This allows for asset transfers and address qualifier transactions to be added in the same block
     * without failing validation
    **/

    // Create cache object that will be used to check the dirty caches
    CAssetCacheQualifierAddress cachedQualifierAddress(qualifier_name, address, QualifierType::ADD_QUALIFIER);

    // Check the dirty caches first and see if it was recently added or removed
    auto setIterator = setNewQualifierAddressToRemove.find(cachedQualifierAddress);
    if (!fSkipTempCache &&setIterator != setNewQualifierAddressToRemove.end()) {
        // Undoing a remove qualifier command, means that we are adding the qualifier to the address
        return setIterator->type == QualifierType::REMOVE_QUALIFIER;
    }


    setIterator = passets->setNewQualifierAddressToRemove.find(cachedQualifierAddress);
    if (setIterator != passets->setNewQualifierAddressToRemove.end()) {
        // Undoing a remove qualifier command, means that we are adding the qualifier to the address
        return setIterator->type == QualifierType::REMOVE_QUALIFIER;
    }

    setIterator = setNewQualifierAddressToAdd.find(cachedQualifierAddress);
    if (!fSkipTempCache && setIterator != setNewQualifierAddressToAdd.end()) {
        // Return true if we are adding the qualifier, and false if we are removing it
        return setIterator->type == QualifierType::ADD_QUALIFIER;
    }


    setIterator = passets->setNewQualifierAddressToAdd.find(cachedQualifierAddress);
    if (setIterator != passets->setNewQualifierAddressToAdd.end()) {
        if (setIterator->type == QualifierType::ADD_QUALIFIER) {
            return true;
        } else {
            // BUG FIX:
            // This scenario can occur if a tag #TAG is removed from an address in a block, then in a later block
            // #TAG/#SECOND is added to the address.
            // If a database event hasn't occurred yet the in memory caches will find that #TAG should be removed from the
            // address and would normally fail this check. Now we can check for the exact condition where a subqualifier
            // was added later.

            auto tempChecker = CAssetCacheRootQualifierChecker(qualifier_name, address);
            if (passets->mapRootQualifierAddressesAdd.count(tempChecker)) {
                if (passets->mapRootQualifierAddressesAdd.at(tempChecker).size()) {
                    return true;
                }
            }

            return false;
        }
    }

    auto tempChecker = CAssetCacheRootQualifierChecker(qualifier_name, address);
    if (!fSkipTempCache && mapRootQualifierAddressesAdd.count(tempChecker)){
        if (mapRootQualifierAddressesAdd.at(tempChecker).size()) {
            return true;
        }
    }

    if (passets->mapRootQualifierAddressesAdd.count(tempChecker)) {
        if (passets->mapRootQualifierAddressesAdd.at(tempChecker).size()) {
            return true;
        }
    }

    // Check the cache, if it doesn't exist in the cache. Try and read it from database
    if (passetsQualifierCache) {
        if (passetsQualifierCache->Exists(cachedQualifierAddress.GetHash().GetHex())) {
            return true;
        }
    }

    if (prestricteddb) {
        // Check for exact qualifier, and add to cache if it exists
        if (prestricteddb->ReadAddressQualifier(address, qualifier_name)) {
            passetsQualifierCache->Put(cachedQualifierAddress.GetHash().GetHex(), 1);
            return true;
        }

        // Look for sub qualifiers
        if (prestricteddb->CheckForAddressRootQualifier(address, qualifier_name)){
            return true;
        }
    }

    return false;
}


bool CAssetsCache::CheckForAddressRestriction(const std::string &restricted_name, const std::string& address, bool fSkipTempCache)
{
    /** There are circumstances where a blocks transactions could be removing or adding a restriction to an address,
     * While at the same time a transaction is added to the same block that is trying to transfer from that address.
     * Depending on the ordering of these two transactions. The address restriction database used to verify the validity of the
     * transactions could be different.
     * To fix this all restricted asset transfer validation checks will use only the latest connect block tips caches
     * and databases to validate it. This allows for asset transfers and address restriction transactions to be added in the same block
     * without failing validation
    **/

    // Create cache object that will be used to check the dirty caches (type, doesn't matter in this search)
    CAssetCacheRestrictedAddress cachedRestrictedAddress(restricted_name, address, RestrictedType::FREEZE_ADDRESS);

    // Check the dirty caches first and see if it was recently added or removed
    auto setIterator = setNewRestrictedAddressToRemove.find(cachedRestrictedAddress);
    if (!fSkipTempCache && setIterator != setNewRestrictedAddressToRemove.end()) {
        // Undoing a unfreeze, means that we are adding back a freeze
        return setIterator->type == RestrictedType::UNFREEZE_ADDRESS;
    }

    setIterator = passets->setNewRestrictedAddressToRemove.find(cachedRestrictedAddress);
    if (setIterator != passets->setNewRestrictedAddressToRemove.end()) {
        // Undoing a unfreeze, means that we are adding back a freeze
        return setIterator->type == RestrictedType::UNFREEZE_ADDRESS;
    }

    setIterator = setNewRestrictedAddressToAdd.find(cachedRestrictedAddress);
    if (!fSkipTempCache && setIterator != setNewRestrictedAddressToAdd.end()) {
        // Return true if we are freezing the address
        return setIterator->type == RestrictedType::FREEZE_ADDRESS;
    }

    setIterator = passets->setNewRestrictedAddressToAdd.find(cachedRestrictedAddress);
    if (setIterator != passets->setNewRestrictedAddressToAdd.end()) {
        // Return true if we are freezing the address
        return setIterator->type == RestrictedType::FREEZE_ADDRESS;
    }

    // Check the cache, if it doesn't exist in the cache. Try and read it from database
    if (passetsRestrictionCache) {
        if (passetsRestrictionCache->Exists(cachedRestrictedAddress.GetHash().GetHex())) {
            return true;
        }
    }

    if (prestricteddb) {
        if (prestricteddb->ReadRestrictedAddress(address, restricted_name)) {
            if (passetsRestrictionCache) {
                passetsRestrictionCache->Put(cachedRestrictedAddress.GetHash().GetHex(), 1);
            }
            return true;
        }
    }

    return false;
}

bool CAssetsCache::CheckForGlobalRestriction(const std::string &restricted_name, bool fSkipTempCache)
{
    /** There are circumstances where a blocks transactions could be freezing all asset transfers. While at
     * the same time a transaction is added to the same block that is trying to transfer the same asset that is being
     * frozen.
     * Depending on the ordering of these two transactions. The global restriction database used to verify the validity of the
     * transactions could be different.
     * To fix this all restricted asset transfer validation checks will use only the latest connect block tips caches
     * and databases to validate it. This allows for asset transfers and global restriction transactions to be added in the same block
     * without failing validation
    **/

    // Create cache object that will be used to check the dirty caches (type, doesn't matter in this search)
    CAssetCacheRestrictedGlobal cachedRestrictedGlobal(restricted_name, RestrictedType::GLOBAL_FREEZE);

    // Check the dirty caches first and see if it was recently added or removed
    auto setIterator = setNewRestrictedGlobalToRemove.find(cachedRestrictedGlobal);
    if (!fSkipTempCache && setIterator != setNewRestrictedGlobalToRemove.end()) {
        // Undoing a removal of a global unfreeze, means that is will become frozen
        return setIterator->type == RestrictedType::GLOBAL_UNFREEZE;
    }

    setIterator = passets->setNewRestrictedGlobalToRemove.find(cachedRestrictedGlobal);
    if (setIterator != passets->setNewRestrictedGlobalToRemove.end()) {
        // Undoing a removal of a global unfreeze, means that is will become frozen
        return setIterator->type == RestrictedType::GLOBAL_UNFREEZE;
    }

    setIterator = setNewRestrictedGlobalToAdd.find(cachedRestrictedGlobal);
    if (!fSkipTempCache && setIterator != setNewRestrictedGlobalToAdd.end()) {
        // Return true if we are adding a freeze command
        return setIterator->type == RestrictedType::GLOBAL_FREEZE;
    }

    setIterator = passets->setNewRestrictedGlobalToAdd.find(cachedRestrictedGlobal);
    if (setIterator != passets->setNewRestrictedGlobalToAdd.end()) {
        // Return true if we are adding a freeze command
        return setIterator->type == RestrictedType::GLOBAL_FREEZE;
    }

    // Check the cache, if it doesn't exist in the cache. Try and read it from database
    if (passetsGlobalRestrictionCache) {
        if (passetsGlobalRestrictionCache->Exists(cachedRestrictedGlobal.assetName)) {
            return true;
        }
    }

    if (prestricteddb) {
        if (prestricteddb->ReadGlobalRestriction(restricted_name)) {
            if (passetsGlobalRestrictionCache)
                passetsGlobalRestrictionCache->Put(cachedRestrictedGlobal.assetName, 1);
            return true;
        }
    }

    return false;
}

void ExtractVerifierStringQualifiers(const std::string& verifier, std::set<std::string>& qualifiers)
{
    std::string s(verifier);

    std::regex regexSearch = std::regex(R"([A-Z0-9_.]+)");
    std::smatch match;

    while (std::regex_search(s,match,regexSearch)) {
        for (auto str : match)
            qualifiers.insert(str);
        s = match.suffix().str();
    }
}

std::string GetStrippedVerifierString(const std::string& verifier)
{
    // Remove all white spaces from the verifier string
    std::string str_without_whitespaces = LibBoolEE::removeWhitespaces(verifier);

    // Remove all '#' from the verifier string
    std::string str_without_qualifier_tags = LibBoolEE::removeCharacter(str_without_whitespaces, QUALIFIER_CHAR);

    return str_without_qualifier_tags;
}

bool CheckVerifierString(const std::string& verifier, std::set<std::string>& setFoundQualifiers, std::string& strError, ErrorReport* errorReport)
{
    // If verifier string is true, always return true
    if (verifier == "true") {
        return true;
    }

    // If verifier string is empty, return false
    if (verifier.empty()) {
        strError = _("Verifier string can not be empty. To default to true, use \"true\"");
        if (errorReport) {
            errorReport->type = ErrorReport::ErrorType::EmptyString;
            errorReport->strDevData = "bad-txns-null-verifier-empty";
        }
        return false;
    }

    // Remove all white spaces, and # from the string as this is how it will be stored in database, and in the script
    std::string strippedVerifier = GetStrippedVerifierString(verifier);

    // Check the stripped size to make sure it isn't over 80
    if (strippedVerifier.length() > 80){
        strError = _("Verifier string has length greater than 80 after whitespaces and '#' are removed");
        if (errorReport) {
            errorReport->type = ErrorReport::ErrorType::LengthToLarge;
            errorReport->strDevData = "bad-txns-null-verifier-length-greater-than-max-length";
            errorReport->vecUserData.emplace_back(strippedVerifier);
        }
        return false;
    }

    // Extract the qualifiers from the verifier string
    ExtractVerifierStringQualifiers(strippedVerifier, setFoundQualifiers);

    // Create an object that stores if an address contains a qualifier
    LibBoolEE::Vals vals;

    // If the check address is empty

    // set all qualifiers in the verifier to true
    for (auto qualifier : setFoundQualifiers) {

        std::string edited_qualifier;

        // Qualifer string was stripped above, so we need to add back the #
        edited_qualifier = QUALIFIER_CHAR + qualifier;

        if (!IsQualifierNameValid(edited_qualifier)) {
            strError = "bad-txns-null-verifier-invalid-asset-name-" + qualifier;
            if (errorReport) {
                errorReport->type = ErrorReport::ErrorType::InvalidQualifierName;
                errorReport->vecUserData.emplace_back(edited_qualifier);
                errorReport->strDevData = "bad-txns-null-verifier-invalid-asset-name-" + qualifier;
            }
            return false;
        }

        vals.insert(std::make_pair(qualifier, true));
    }

    try {
        LibBoolEE::resolve(verifier, vals, errorReport);
        return true;
    } catch (const std::runtime_error& run_error) {
        if (errorReport) {
            if (errorReport->type == ErrorReport::ErrorType::NotSetError) {
                errorReport->type = ErrorReport::ErrorType::InvalidSyntax;
                errorReport->vecUserData.emplace_back(run_error.what());
                errorReport->strDevData = "bad-txns-null-verifier-failed-syntax-check";
            }
        }
        strError = "bad-txns-null-verifier-failed-syntax-check";
        return error("%s : Verifier string failed to resolve. Please check string syntax - exception: %s\n", __func__, run_error.what());
    }
}

bool VerifyNullAssetDataFlag(const int& flag, std::string& strError)
{
    // Check the flag
    if (flag != 0 && flag != 1) {
        strError = "bad-txns-null-data-flag-must-be-0-or-1";
        return false;
    }

    return true;
}

bool VerifyQualifierChange(CAssetsCache& cache, const CNullAssetTxData& data, const std::string& address, std::string& strError)
{
    // Check the flag
    if (!VerifyNullAssetDataFlag(data.flag, strError))
        return false;

    // Check to make sure we only allow changes to the current status
    bool fHasQualifier = cache.CheckForAddressQualifier(data.asset_name, address, true);
    QualifierType type = data.flag ? QualifierType::ADD_QUALIFIER : QualifierType::REMOVE_QUALIFIER;
    if (type == QualifierType::ADD_QUALIFIER) {
        if (fHasQualifier) {
            strError = "bad-txns-null-data-add-qualifier-when-already-assigned";
            return false;
        }
    } else if (type == QualifierType::REMOVE_QUALIFIER) {
        if (!fHasQualifier) {
            strError = "bad-txns-null-data-removing-qualifier-when-not-assigned";
            return false;
        }
    }

    return true;
}

bool VerifyRestrictedAddressChange(CAssetsCache& cache, const CNullAssetTxData& data, const std::string& address, std::string& strError)
{
    // Check the flag
    if (!VerifyNullAssetDataFlag(data.flag, strError))
        return false;

    // Get the current status of the asset and the given address
    bool fIsFrozen = cache.CheckForAddressRestriction(data.asset_name, address, true);

    // Assign the type based on the data
    RestrictedType type = data.flag ? RestrictedType::FREEZE_ADDRESS : RestrictedType::UNFREEZE_ADDRESS;

    if (type == RestrictedType::FREEZE_ADDRESS) {
        if (fIsFrozen) {
            strError = "bad-txns-null-data-freeze-address-when-already-frozen";
            return false;
        }
    } else if (type == RestrictedType::UNFREEZE_ADDRESS) {
        if (!fIsFrozen) {
            strError = "bad-txns-null-data-unfreeze-address-when-not-frozen";
            return false;
        }
    }

    return true;
}

bool VerifyGlobalRestrictedChange(CAssetsCache& cache, const CNullAssetTxData& data, std::string& strError)
{
    // Check the flag
    if (!VerifyNullAssetDataFlag(data.flag, strError))
        return false;

    // Get the current status of the asset globally
    bool fIsGloballyFrozen = cache.CheckForGlobalRestriction(data.asset_name, true);

    // Assign the type based on the data
    RestrictedType type = data.flag ? RestrictedType::GLOBAL_FREEZE : RestrictedType::GLOBAL_UNFREEZE;

    if (type == RestrictedType::GLOBAL_FREEZE) {
        if (fIsGloballyFrozen) {
            strError = "bad-txns-null-data-global-freeze-when-already-frozen";
            return false;
        }
    } else if (type == RestrictedType::GLOBAL_UNFREEZE) {
        if (!fIsGloballyFrozen) {
            strError = "bad-txns-null-data-global-unfreeze-when-not-frozen";
            return false;
        }
    }

    return true;
}

////////////////


bool CheckVerifierAssetTxOut(const CTxOut& txout, std::string& strError)
{
    CNullAssetTxVerifierString verifier;
    if (!AssetNullVerifierDataFromScript(txout.scriptPubKey, verifier)) {
        strError = "bad-txns-null-verifier-data-serialization";
        return false;
    }

    // All restricted verifiers should have white spaces stripped from the data before it is added to a script
    if ((int)verifier.verifier_string.find_first_of(' ') != -1) {
        strError = "bad-txns-null-verifier-data-contained-whitespaces";
        return false;
    }

    // All restricted verifiers should have # stripped from that data before it is added to a script
    if ((int)verifier.verifier_string.find_first_of('#') != -1) {
        strError = "bad-txns-null-verifier-data-contained-qualifier-character-#";
        return false;
    }

    std::set<std::string> setFoundQualifiers;
    if (!CheckVerifierString(verifier.verifier_string, setFoundQualifiers, strError))
        return false;

    return true;
}
///////////////
bool ContextualCheckNullAssetTxOut(const CTxOut& txout, CAssetsCache* assetCache, std::string& strError, std::vector<std::pair<std::string, CNullAssetTxData>>* myNullAssetData)
{
    // Get the data from the script
    CNullAssetTxData data;
    std::string address;
    if (!AssetNullDataFromScript(txout.scriptPubKey, data, address)) {
        strError = "bad-txns-null-asset-data-serialization";
        return false;
    }

    // Validate the tx data against the cache, and database
    if (assetCache) {
        if (IsAssetNameAQualifier(data.asset_name)) {
            if (!VerifyQualifierChange(*assetCache, data, address, strError)) {
                return false;
            }

        } else if (IsAssetNameAnRestricted(data.asset_name)) {
            if (!VerifyRestrictedAddressChange(*assetCache, data, address, strError))
                return false;
        } else {
            strError = "bad-txns-null-asset-data-on-non-restricted-or-qualifier-asset";
            return false;
        }
    }

    return true;
}

bool ContextualCheckGlobalAssetTxOut(const CTxOut& txout, CAssetsCache* assetCache, std::string& strError)
{
    // Get the data from the script
    CNullAssetTxData data;
    if (!GlobalAssetNullDataFromScript(txout.scriptPubKey, data)) {
        strError = "bad-txns-null-global-asset-data-serialization";
        return false;
    }

    // Validate the tx data against the cache, and database
    if (assetCache) {
        if (!VerifyGlobalRestrictedChange(*assetCache, data, strError))
            return false;
    }
    return true;
}

bool ContextualCheckVerifierAssetTxOut(const CTxOut& txout, CAssetsCache* assetCache, std::string& strError)
{
    CNullAssetTxVerifierString verifier;
    if (!AssetNullVerifierDataFromScript(txout.scriptPubKey, verifier)) {
        strError = "bad-txns-null-verifier-data-serialization";
        return false;
    }

    if (assetCache) {
        std::string strError = "";
        std::string address = "";
        std::string strVerifier = verifier.verifier_string;
        if (!ContextualCheckVerifierString(assetCache, strVerifier, address, strError))
            return false;
    }

    return true;
}

bool ContextualCheckVerifierString(CAssetsCache* cache, const std::string& verifier, const std::string& check_address, std::string& strError, ErrorReport* errorReport)
{
    // If verifier is set to true, return true
    if (verifier == "true")
        return true;

    // Check against the non contextual changes first
    std::set<std::string> setFoundQualifiers;
    if (!CheckVerifierString(verifier, setFoundQualifiers, strError, errorReport))
        return false;

    // Loop through each qualifier and make sure that the asset exists
    for(auto qualifier : setFoundQualifiers) {
        std::string search = QUALIFIER_CHAR + qualifier;
        if (!cache->CheckIfAssetExists(search, true)) {
            if (errorReport) {
                errorReport->type = ErrorReport::ErrorType::AssetDoesntExist;
                errorReport->vecUserData.emplace_back(search);
                errorReport->strDevData = "bad-txns-null-verifier-contains-non-issued-qualifier";
            }
            strError = "bad-txns-null-verifier-contains-non-issued-qualifier";
            return false;
        }
    }

    // If we got this far, and the check_address is empty. The CheckVerifyString method already did the syntax checks
    // No need to do any more checks, as it will fail because the check_address is empty
    if (check_address.empty())
        return true;

    // Create an object that stores if an address contains a qualifier
    LibBoolEE::Vals vals;

    // Add the qualifiers into the vals object
    for (auto qualifier : setFoundQualifiers) {
        std::string search = QUALIFIER_CHAR + qualifier;

        // Check to see if the address contains the qualifier
        bool has_qualifier = cache->CheckForAddressQualifier(search, check_address, true);

        // Add the true or false value into the vals
        vals.insert(std::make_pair(qualifier, has_qualifier));
    }

    try {
        bool ret = LibBoolEE::resolve(verifier, vals, errorReport);
        if (!ret) {
            if (errorReport) {
                if (errorReport->type == ErrorReport::ErrorType::NotSetError) {
                    errorReport->type = ErrorReport::ErrorType::FailedToVerifyAgainstAddress;
                    errorReport->vecUserData.emplace_back(check_address);
                    errorReport->strDevData = "bad-txns-null-verifier-address-failed-verification";
                }
            }

            error("%s : The address %s failed to verify against: %s. Is null %d", __func__, check_address, verifier, errorReport ? 0 : 1);
            strError = "bad-txns-null-verifier-address-failed-verification";
        }
        return ret;

    } catch (const std::runtime_error& run_error) {

        if (errorReport) {
            if (errorReport->type == ErrorReport::ErrorType::NotSetError) {
                errorReport->type = ErrorReport::ErrorType::InvalidSyntax;
            }

            errorReport->vecUserData.emplace_back(run_error.what());
            errorReport->strDevData = "bad-txns-null-verifier-failed-contexual-syntax-check";
        }

        strError = "bad-txns-null-verifier-failed-contexual-syntax-check";
        return error("%s : Verifier string failed to resolve. Please check string syntax - exception: %s\n", __func__, run_error.what());
    }
}

bool ContextualCheckTransferAsset(CAssetsCache* assetCache, const CAssetTransfer& transfer, const std::string& address, std::string& strError)
{
    strError = "";
    AssetType assetType;
    if (!IsAssetNameValid(transfer.strName, assetType)) {
        strError = "Invalid parameter: asset_name must only consist of valid characters and have a size between 3 and 30 characters. See help for more details.";
        return false;
    }

    if (transfer.nAmount <= 0) {
        strError = "Invalid parameter: asset amount can't be equal to or less than zero.";
        return false;
    }

    if (AreMessagesDeployed()) {
        // This is for the current testnet6 only.
        if (transfer.nAmount <= 0) {
            strError = "Invalid parameter: asset amount can't be equal to or less than zero.";
            return false;
        }

        if (transfer.message.empty() && transfer.nExpireTime > 0) {
            strError = "Invalid parameter: asset transfer expiration time requires a message to be attached to the transfer";
            return false;
        }

        if (transfer.nExpireTime < 0) {
            strError = "Invalid parameter: expiration time must be a positive value";
            return false;
        }

        if (transfer.message.size() && !CheckEncoded(transfer.message, strError)) {
            return false;
        }
    }

    // If the transfer is a message channel asset. Check to make sure that it is UNIQUE_ASSET_AMOUNT
    if (assetType == AssetType::MSGCHANNEL) {
        if (!AreMessagesDeployed()) {
            strError = "bad-txns-transfer-msgchannel-before-messaging-is-active";
            return false;
        }
    }

    if (assetType == AssetType::RESTRICTED) {
        if (!AreRestrictedAssetsDeployed()) {
            strError = "bad-txns-transfer-restricted-before-it-is-active";
            return false;
        }

        if (assetCache) {
            if (assetCache->CheckForGlobalRestriction(transfer.strName, true)) {
                strError = "bad-txns-transfer-restricted-asset-that-is-globally-restricted";
                return false;
            }
        }


        std::string strError = "";
        if (!transfer.ContextualCheckAgainstVerifyString(assetCache, address, strError)) {
            error("%s : %s", __func__, strError);
            return false;
        }
    }

    // If the transfer is a qualifier channel asset.
    if (assetType == AssetType::QUALIFIER || assetType == AssetType::SUB_QUALIFIER) {
        if (!AreRestrictedAssetsDeployed()) {
            strError = "bad-txns-transfer-qualifier-before-it-is-active";
            return false;
        }
    }
    return true;
}

bool CheckNewAsset(const CNewAsset& asset, std::string& strError)
{
    strError = "";

    AssetType assetType;
    if (!IsAssetNameValid(std::string(asset.strName), assetType)) {
        strError = _("Invalid parameter: asset_name must only consist of valid characters and have a size between 3 and 30 characters. See help for more details.");
        return false;
    }

    if (assetType == AssetType::UNIQUE || assetType == AssetType::MSGCHANNEL) {
        if (asset.units != UNIQUE_ASSET_UNITS) {
            strError = _("Invalid parameter: units must be ") + std::to_string(UNIQUE_ASSET_UNITS);
            return false;
        }
        if (asset.nAmount != UNIQUE_ASSET_AMOUNT) {
            strError = _("Invalid parameter: amount must be ") + std::to_string(UNIQUE_ASSET_AMOUNT);
            return false;
        }
        if (asset.nReissuable != 0) {
            strError = _("Invalid parameter: reissuable must be 0");
            return false;
        }
    }

    if (assetType == AssetType::QUALIFIER || assetType == AssetType::SUB_QUALIFIER) {
        if (asset.units != QUALIFIER_ASSET_UNITS) {
            strError = _("Invalid parameter: units must be ") + std::to_string(QUALIFIER_ASSET_UNITS);
            return false;
        }
        if (asset.nAmount < QUALIFIER_ASSET_MIN_AMOUNT || asset.nAmount > QUALIFIER_ASSET_MAX_AMOUNT) {
            strError = _("Invalid parameter: amount must be between ") + std::to_string(QUALIFIER_ASSET_MIN_AMOUNT) + " - " + std::to_string(QUALIFIER_ASSET_MAX_AMOUNT);
            return false;
        }
        if (asset.nReissuable != 0) {
            strError = _("Invalid parameter: reissuable must be 0");
            return false;
        }
    }

    if (IsAssetNameAnOwner(std::string(asset.strName))) {
        strError = _("Invalid parameters: asset_name can't have a '!' at the end of it. See help for more details.");
        return false;
    }

    if (asset.nAmount <= 0) {
        strError = _("Invalid parameter: asset amount can't be equal to or less than zero.");
        return false;
    }

    if (asset.nAmount > MAX_MONEY) {
        strError = _("Invalid parameter: asset amount greater than max money: ") + std::to_string(MAX_MONEY / COIN);
        return false;
    }

    if (asset.units < 0 || asset.units > 8) {
        strError = _("Invalid parameter: units must be between 0-8.");
        return false;
    }

    if (!CheckAmountWithUnits(asset.nAmount, asset.units)) {
        strError = _("Invalid parameter: amount must be divisible by the smaller unit assigned to the asset");
        return false;
    }

    if (asset.nReissuable != 0 && asset.nReissuable != 1) {
        strError = _("Invalid parameter: reissuable must be 0 or 1");
        return false;
    }

    if (asset.nHasIPFS != 0 && asset.nHasIPFS != 1) {
        strError = _("Invalid parameter: has_ipfs must be 0 or 1.");
        return false;
    }

    if (asset.nHasANS != 0 && asset.nHasANS != 1) {
        strError = _("Invalid parameter: has_ans must be 0 or 1.");
        return false;
    }

    if (asset.nHasANS) {
        if (asset.nReissuable != 1) {
            strError = _("Invalid parameter: ANS assets must be reissuable (reissuable must be 1)");
            return false;
        }
        if (asset.nAmount != COIN) {
            strError = _("Invalid parameter: ANS assets must have a quantity of exactly 1");
            return false;
        }
        if (asset.units != 0) {
            strError = _("Invalid parameter: ANS assets must have 0 decimal units");
            return false;
        }
    }

    return true;
}

/** AIP-0010: Returns true if the asset name is eligible to carry ANS data.
 *  Eligible names: ROOT .AVN assets (end in ".AVN") or sub-assets of .AVN names
 *  (contain ".AVN/" or ".AVN#" — i.e. the root ends in .AVN).
 */
static bool IsANSEligibleName(const std::string& name)
{
    const std::string& domain = CAvianNameSystemID::domain; // ".AVN"
    if (name.size() <= domain.size()) return false;
    // Check for ROOT .AVN ending (e.g. BOB.AVN)
    if (name.substr(name.size() - domain.size()) == domain) return true;
    // Check for direct sub-asset of a .AVN name (e.g. BOB.AVN/BTC)
    // Must be exactly one level deep — no second '/' after the separator
    size_t pos = name.find(domain);
    if (pos != std::string::npos) {
        size_t endPos = pos + domain.size();
        if (endPos < name.size() && name[endPos] == '/') {
            std::string ticker = name.substr(endPos + 1);
            // Only a direct sub-asset (no further nesting) is ANS-eligible
            if (!ticker.empty() && ticker.find('/') == std::string::npos)
                return true;
        }
    }
    return false;
}

bool ContextualCheckNewAsset(CAssetsCache* assetCache, const CNewAsset& asset, std::string& strError, const CTxMemPool* mempool)
{
    if (!AreAssetsDeployed()) {
        strError = "bad-txns-new-asset-when-assets-is-not-active";
        return false;
    }

    if (!CheckNewAsset(asset, strError))
        return false;

    // Check our current cache to see if the asset has been created yet
    if (assetCache->CheckIfAssetExists(asset.strName, true)) {
        strError = std::string(_("Invalid parameter: asset_name '")) + asset.strName + std::string(_("' has already been used"));
        return false;
    }

    // Check the mempool for a pending transaction that already creates this asset
    if (mempool) {
        LOCK(mempool->cs);
        AssertLockHeld(mempool->cs);
        if (mempool->mapAssetToHash.count(asset.strName)) {
            strError = std::string(_("Invalid parameter: asset_name '")) + asset.strName + std::string(_("' is already being created in the mempool"));
            return false;
        }
    }

    // Check the ipfs hash as it changes when messaging goes active
    if (asset.nHasIPFS && asset.strIPFSHash.size() != 34) {
        if (!AreMessagesDeployed()) {
            strError = _("Invalid parameter: ipfs_hash must be 46 characters. Txid must be valid 64 character hash");
            return false;
        } else {
            if (asset.strIPFSHash.size() != 32) {
                strError = _("Invalid parameter: ipfs_hash must be 46 characters. Txid must be valid 64 character hash");
                return false;
            }
        }
    }

    // ANS not allowed when they are not deployed
    if (asset.nHasANS && !IsAvianNameSystemDeployed()) {
        strError = _("Invalid parameter: ANS IDs not allowed when they are not deployed.");
        return false;
    }

    // Check asset name for ANS - must end in .AVN or be a sub-asset of a .AVN name (AIP-0010)
    if (asset.nHasANS) {
        if (!IsANSEligibleName(asset.strName)) {
            strError = std::string(_("Invalid parameter: asset name needs to end in '")) + CAvianNameSystemID::domain + std::string(_("' (or be a sub-asset of one) since ANS data is attached."));
            return false;
        }
    }

    if (asset.nHasIPFS) {
        if (!CheckEncoded(asset.strIPFSHash, strError))
            return false;
    }

    if (asset.nHasANS) {
        if (!CheckEncoded(asset.strANSID, strError))
            return false;
    }

    return true;
}

bool CheckReissueAsset(const CReissueAsset& asset, std::string& strError)
{
    strError = "";

    if (asset.nAmount < 0 || asset.nAmount >= MAX_MONEY) {
        strError = _("Unable to reissue asset: amount must be 0 or larger");
        return false;
    }

    if (asset.nUnits > MAX_UNIT || asset.nUnits < -1) {
        strError = _("Unable to reissue asset: unit must be between 8 and -1");
        return false;
    }

    /// -------- TESTNET ONLY ---------- ///
    // Testnet has a couple blocks that have invalid nReissue values before constriants were created
    bool fSkip = false;
    if (Params().GetChainType() == ChainType::TESTNET) {
        if (asset.strName == "GAMINGWEB" && asset.nReissuable == 109) {
            fSkip = true;
        } else if (asset.strName == "UINT8" && asset.nReissuable == -47) {
            fSkip = true;
        }
    }
    /// -------- TESTNET ONLY ---------- ///

    if (!fSkip && asset.nReissuable != 0 && asset.nReissuable != 1) {
        strError = _("Unable to reissue asset: reissuable must be 0 or 1");
        return false;
    }

    AssetType type;
    IsAssetNameValid(asset.strName, type);

    if (type == AssetType::RESTRICTED) {
        // TODO Add checks for restricted asset if we can come up with any
    }

    return true;
}

bool ContextualCheckReissueAsset(CAssetsCache* assetCache, const CReissueAsset& reissue_asset, std::string& strError, const CTransaction& tx)
{
    // We are using this just to get the strAddress
    CReissueAsset reissue;
    std::string strAddress;
    if (!ReissueAssetFromTransaction(tx, reissue, strAddress)) {
        strError = "bad-txns-reissue-asset-contextual-check";
        return false;
    }

    // run non contextual checks
    if (!CheckReissueAsset(reissue_asset, strError))
        return false;

    // Check previous asset data with the reissuesd data
    CNewAsset prev_asset;
    if (!assetCache->GetAssetMetaDataIfExists(reissue_asset.strName, prev_asset)) {
        strError = _("Unable to reissue asset: asset_name '") + reissue_asset.strName + _("' doesn't exist in the database");
        return false;
    }

    if (!prev_asset.nReissuable) {
        // Check to make sure the asset can be reissued
        strError = _("Unable to reissue asset: reissuable is set to false");
        return false;
    }

    if (prev_asset.nAmount + reissue_asset.nAmount > MAX_MONEY) {
        strError = _("Unable to reissue asset: asset_name '") + reissue_asset.strName +
                   _("' the amount trying to reissue is to large");
        return false;
    }

    if (!CheckAmountWithUnits(reissue_asset.nAmount, prev_asset.units)) {
        strError = _("Unable to reissue asset: amount must be divisible by the smaller unit assigned to the asset");
        return false;
    }

    if (reissue_asset.nUnits < prev_asset.units && reissue_asset.nUnits != -1) {
        strError = _("Unable to reissue asset: unit must be larger than current unit selection");
        return false;
    }

    // Check the ipfs hash
    if (reissue_asset.strIPFSHash != "" && reissue_asset.strIPFSHash.size() != 34 && (AreMessagesDeployed() && reissue_asset.strIPFSHash.size() != 32)) {
        strError = _("Invalid parameter: ipfs_hash must be 34 bytes, Txid must be 32 bytes");
        return false;
    }

    if (reissue_asset.strIPFSHash != "") {
        if (!CheckEncoded(reissue_asset.strIPFSHash, strError))
            return false;
    }

    // ANS not allowed when they are not deployed
    if (reissue_asset.strANSID != "" && !IsAvianNameSystemDeployed()) {
        strError = _("Invalid parameter: ANS IDs not allowed when they are not deployed.");
        return false;
    }

    // Check asset name for ANS - must end in .AVN or be a sub-asset of a .AVN name (AIP-0010)
    if (reissue_asset.strANSID != "") {
        if (!IsANSEligibleName(reissue_asset.strName)) {
            strError = std::string(_("Invalid parameter: asset name needs to end in '")) + CAvianNameSystemID::domain + std::string(_("' (or be a sub-asset of one) since ANS data is attached."));
            return false;
        }
    }

    if (reissue_asset.strANSID != "") {
        if (!CheckEncoded(reissue_asset.strANSID, strError))
            return false;
    }

    if (IsAssetNameAnRestricted(reissue_asset.strName)) {
        CNullAssetTxVerifierString new_verifier;
        bool fNotFound = false;

        // Try and get the verifier string if it was changed
        if (!GetVerifierStringFromTx(tx, new_verifier, strError, fNotFound)) {
            // If it return false for any other reason besides not being found, fail the transaction check
            if (!fNotFound) {
                return false;
            }
        }

        if (reissue_asset.nAmount > 0) {
            // If it wasn't found, get the current verifier and validate against it
            if (fNotFound) {
                CNullAssetTxVerifierString current_verifier;
                if (assetCache->GetAssetVerifierStringIfExists(reissue_asset.strName, current_verifier)) {
                    if (!ContextualCheckVerifierString(assetCache, current_verifier.verifier_string, strAddress, strError))
                        return false;
                } else {
                    // This should happen, but if it does. The wallet needs to shutdown,
                    // TODO, remove this after restricted assets have been tested in testnet for some time, and this hasn't happened yet. It this has happened. Investigation is required by the dev team
                    error("%s : failed to get verifier string from a restricted asset, this shouldn't happen, database is out of sync. Reindex required. Please report this is to development team asset name: %s, txhash : %s",__func__, reissue_asset.strName, tx.GetHash().GetHex());
                    strError = "failed to get verifier string from a restricted asset, database is out of sync. Reindex required. Please report this is to development team";
                    return false;
                }
            } else {
                if (!ContextualCheckVerifierString(assetCache, new_verifier.verifier_string, strAddress, strError))
                    return false;
            }
        }
    }


    return true;
}

bool ContextualCheckReissueAsset(CAssetsCache* assetCache, const CReissueAsset& reissue_asset, std::string& strError)
{
    // run non contextual checks
    if (!CheckReissueAsset(reissue_asset, strError))
        return false;

    // Check previous asset data with the reissuesd data
    if (assetCache) {
        CNewAsset prev_asset;
        if (!assetCache->GetAssetMetaDataIfExists(reissue_asset.strName, prev_asset)) {
            strError = _("Unable to reissue asset: asset_name '") + reissue_asset.strName +
                       _("' doesn't exist in the database");
            return false;
        }

        if (!prev_asset.nReissuable) {
            // Check to make sure the asset can be reissued
            strError = _("Unable to reissue asset: reissuable is set to false");
            return false;
        }

        if (prev_asset.nAmount + reissue_asset.nAmount > MAX_MONEY) {
            strError = _("Unable to reissue asset: asset_name '") + reissue_asset.strName +
                       _("' the amount trying to reissue is to large");
            return false;
        }

        if (!CheckAmountWithUnits(reissue_asset.nAmount, prev_asset.units)) {
            strError = _("Unable to reissue asset: amount must be divisible by the smaller unit assigned to the asset");
            return false;
        }

        if (reissue_asset.nUnits < prev_asset.units && reissue_asset.nUnits != -1) {
            strError = _("Unable to reissue asset: unit must be larger than current unit selection");
            return false;
        }
    }

    // Check the ipfs hash
    if (reissue_asset.strIPFSHash != "" && reissue_asset.strIPFSHash.size() != 34 && (AreMessagesDeployed() && reissue_asset.strIPFSHash.size() != 32)) {
        strError = _("Invalid parameter: ipfs_hash must be 34 bytes, Txid must be 32 bytes");
        return false;
    }

    if (reissue_asset.strIPFSHash != "") {
        if (!CheckEncoded(reissue_asset.strIPFSHash, strError))
            return false;
    }

    // ANS not allowed when they are not deployed
    if (reissue_asset.strANSID != "" && !IsAvianNameSystemDeployed()) {
        strError = _("Invalid parameter: ANS IDs not allowed when they are not deployed.");
        return false;
    }

    // Check asset name for ANS - must end in .AVN or be a sub-asset of a .AVN name (AIP-0010)
    if (reissue_asset.strANSID != "") {
        if (!IsANSEligibleName(reissue_asset.strName)) {
            strError = std::string(_("Invalid parameter: asset name needs to end in '")) + CAvianNameSystemID::domain + std::string(_("' (or be a sub-asset of one) since ANS data is attached."));
            return false;
        }
    }

    if (reissue_asset.strANSID != "") {
        if (!CheckEncoded(reissue_asset.strANSID, strError))
            return false;
    }

    return true;
}

bool ContextualCheckUniqueAssetTx(CAssetsCache* assetCache, std::string& strError, const CTransaction& tx)
{
    for (auto out : tx.vout)
    {
        if (IsScriptNewUniqueAsset(out.scriptPubKey))
        {
            CNewAsset asset;
            std::string strAddress;
            if (!AssetFromScript(out.scriptPubKey, asset, strAddress)) {
                strError = "bad-txns-issue-unique-serialization-failed";
                return false;
            }

            if (!ContextualCheckUniqueAsset(assetCache, asset, strError))
                return false;
        }
    }

    return true;
}

bool ContextualCheckUniqueAsset(CAssetsCache* assetCache, const CNewAsset& unique_asset, std::string& strError)
{
    if (!ContextualCheckNewAsset(assetCache, unique_asset, strError))
        return false;

    return true;
}

std::string GetUserErrorString(const ErrorReport& report)
{
    switch (report.type) {
        case ErrorReport::ErrorType::NotSetError: return _("Error not set");
        case ErrorReport::ErrorType::InvalidQualifierName: return _("Invalid Qualifier Name: ") + report.vecUserData[0];
        case ErrorReport::ErrorType::EmptyString: return _("Verifier string is empty");
        case ErrorReport::ErrorType::LengthToLarge: return _("Length is to large. Please use a smaller length");
        case ErrorReport::ErrorType::InvalidSubExpressionFormula: return _("Invalid expressions in verifier string: ") + report.vecUserData[0];
        case ErrorReport::ErrorType::InvalidSyntax: return _("Invalid syntax: ") + report.vecUserData[0];
        case ErrorReport::ErrorType::AssetDoesntExist: return _("Asset doesn't exist: ") + report.vecUserData[0];
        case ErrorReport::ErrorType::FailedToVerifyAgainstAddress: return _("This address doesn't contain the correct tags to pass the verifier string check: ") + report.vecUserData[0];
        case ErrorReport::ErrorType::EmptySubExpression: return _("The verifier string has two operators without a tag between them");
        case ErrorReport::ErrorType::UnknownOperator: return _("The symbol: '") + report.vecUserData[0] + _("' is not a valid character in the expression: ") + report.vecUserData[1];
        case ErrorReport::ErrorType::ParenthesisParity: return _("Every '(' must have a corresponding ')' in the expression: ") + report.vecUserData[0];
        case ErrorReport::ErrorType::VariableNotFound: return _("Variable is not allow in the expression: '") + report.vecUserData[0] + "'";;
        default:
            return _("Error not set");
    }
}

UniValue UnitValueFromAmount(const CAmount& amount, int8_t units)
{
    // Asset amounts are stored as CAmount in 1e-8 base units (like AVN).
    // The asset's `units` field restricts the displayed/allowed decimal places.
    // For example, an asset issued with amount 10 and units=0 is stored as
    // 10 * COIN internally, but should be displayed as "10".
    const bool sign{amount < 0};
    const int64_t n_abs{sign ? -amount : amount};

    // Clamp units to the supported range.
    if (units < 0) units = 0;
    if (units > MAX_UNIT) units = MAX_UNIT;

    const int64_t quotient{n_abs / COIN};
    const int64_t remainder_full{n_abs % COIN};

    if (units == 0) {
        return UniValue(UniValue::VNUM, strprintf("%s%d", sign ? "-" : "", quotient));
    }

    int64_t remainder_divisor{1};
    for (int i = 0; i < (MAX_UNIT - units); ++i) remainder_divisor *= 10;
    const int64_t remainder{remainder_full / remainder_divisor};

    return UniValue(UniValue::VNUM, strprintf("%s%d.%0*d", sign ? "-" : "", quotient, units, remainder));
}

UniValue AssetUnitValueFromAmount(const CAmount& amount, const std::string& assetName)
{
    uint8_t units = MAX_UNIT;
    if (IsAssetNameAnOwner(assetName)) {
        units = OWNER_UNITS;
    } else if (passets) {
        CNewAsset assetData;
        if (passets->GetAssetMetaDataIfExists(assetName, assetData)) {
            units = assetData.units;
        }
    }
    return UnitValueFromAmount(amount, units);
}
