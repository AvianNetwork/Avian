// Copyright (c) 2018-2019 The Raven Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <assets/messages.h>
#include <assets/myassetsdb.h>
#include <assets/assets.h>
#include <key_io.h>
#include <logging.h>

// Extern declarations for globals defined in init.cpp
extern CMessageDB* pmessagedb;
extern CMessageChannelDB* pmessagechanneldb;
extern CLRUCache<std::string, CMessage>* pMessagesCache;
extern CLRUCache<std::string, int8_t>* pMessageSubscribedChannelsCache;
extern CLRUCache<std::string, int8_t>* pMessagesSeenAddressCache;

std::set<COutPoint> setDirtyMessagesRemove;
std::map<COutPoint, CMessage> mapDirtyMessagesAdd;
std::map<COutPoint, CMessage> mapDirtyMessagesOrphaned;

std::set<std::string> setDirtyChannelsAdd;
std::set<std::string> setDirtyChannelsRemove;
std::set<std::string> setSubscribedChannelsAskedForFalse;

std::set<std::string> setDirtySeenAddressAdd;
std::set<std::string> setAddressAskedForFalse;

Mutex cs_messaging;


int8_t IntFromMessageStatus(MessageStatus status)
{
    return (int8_t)status;
}

MessageStatus MessageStatusFromInt(int8_t nStatus)
{
    return (MessageStatus)nStatus;
}

std::string MessageStatusToString(MessageStatus status)
{
    switch (status) {
        case MessageStatus::READ: return "READ";
        case MessageStatus::UNREAD: return "UNREAD";
        case MessageStatus::ORPHAN: return "ORPHAN";
        case MessageStatus::EXPIRED: return "EXPIRED";
        case MessageStatus::SPAM: return "SPAM";
        case MessageStatus::HIDDEN: return "HIDDEN";
        default: return "ERROR";
    }
}

CMessage::CMessage() {
    SetNull();
}

CMessage::CMessage(const COutPoint& out, const std::string& strName, const std::string& ipfsHash, const int64_t& nExpiredTime, const int64_t& time)
{
    SetNull();
    this->out = out;
    this->strName = strName;
    this->ipfsHash = ipfsHash;
    this->nExpiredTime = nExpiredTime;
    this->time = time;
    status = MessageStatus::UNREAD;
}

bool IsChannelSubscribed(const std::string &name)
{
    if (!pMessageSubscribedChannelsCache || !pmessagechanneldb)
        return false;

    // Check Dirty Cache for newly added channel additions
    if (setDirtyChannelsAdd.count(name))
        return true;

    // Check Dirty Cache for newly removed channels
    if (setDirtyChannelsRemove.count(name))
        return false;

    // Check the Channel Cache and see if it is in the Cache
    if (pMessageSubscribedChannelsCache->Exists(name))
        return true;

    // Check if we have already searched for this before
    if (setSubscribedChannelsAskedForFalse.count(name))
        return false;

    // Check to see if the message database contains the asset
    if (pmessagechanneldb->ReadMyMessageChannel(name)) {
        pMessageSubscribedChannelsCache->Put(name, 1);
        return true;
    }

    // Help prevent spam and unneeded database reads
    setSubscribedChannelsAskedForFalse.insert(name);

    return false;
}

bool GetMessage(const COutPoint& out, CMessage& message)
{
    if (!pmessagedb || !pMessagesCache)
        return false;

    // Check the dirty add cache
    if (mapDirtyMessagesAdd.count(out)) {
        message = mapDirtyMessagesAdd.at(out);
        return true;
    }

    // Check the dirty remove cache
    if (setDirtyMessagesRemove.count(out))
        return false;

    // Check database cache
    if (pMessagesCache->Exists(out.ToString())) {
        message = pMessagesCache->Get(out.ToString());
        return true;
    }

    // Check the database
    if (pmessagedb->ReadMessage(out, message)) {
        pMessagesCache->Put(out.ToString(), message);
        return true;
    }

    return false;
}

void AddChannel(const std::string &name)
{
    // Add channel to dirty cache to add
    setDirtyChannelsAdd.insert(name);

    // If the channel name is in the dirty remove cache. Remove it so it doesn't get deleted on flush
    setDirtyChannelsRemove.erase(name);
    setSubscribedChannelsAskedForFalse.erase(name);
}

void RemoveChannel(const std::string &name)
{
    // Add channel to dirty cache to remove
    setDirtyChannelsRemove.insert(name);

    // If the channel name is in the dirty add cache. Remove it so it doesn't get added on flush
    setDirtyChannelsAdd.erase(name);
}

void AddMessage(const CMessage& message)
{
    // Add message to dirty map cache to add
    mapDirtyMessagesAdd.insert(std::make_pair(message.out, message));

    // Remove message Out from dirty set Cache to remove
    setDirtyMessagesRemove.erase(message.out);
    mapDirtyMessagesOrphaned.erase(message.out);
}

void RemoveMessage(const CMessage& message)
{
    RemoveMessage(message.out);
}

void RemoveMessage(const COutPoint &out)
{
    // Add message out to dirty set Cache to remove
    setDirtyMessagesRemove.insert(out);

    // Remove message from map Dirty Message to add
    mapDirtyMessagesAdd.erase(out);
    mapDirtyMessagesOrphaned.erase(out);
}

void OrphanMessage(const COutPoint &out)
{
    CMessage message;
    if (GetMessage(out, message))
        OrphanMessage(message);
}

void OrphanMessage(const CMessage& message)
{
    mapDirtyMessagesOrphaned[message.out] = message;

    // Remove from other dirty caches
    mapDirtyMessagesAdd.erase(message.out);
}

bool IsAddressSeen(const std::string &address)
{
    if (!pmessagechanneldb || !pMessagesSeenAddressCache)
        return false;

    if (setDirtySeenAddressAdd.count(address)) // Check dirty set
        return true;

    if (pMessagesSeenAddressCache->Exists(address)) {
        return true;
    }

    if (setAddressAskedForFalse.count(address)) {
        return false;
    }

    if (pmessagechanneldb->ReadUsedAddress(address)) {
        pMessagesSeenAddressCache->Put(address, 1);
        return true;
    }

    setAddressAskedForFalse.insert(address);

    return false;
}

void AddAddressSeen(const std::string &address)
{
    setDirtySeenAddressAdd.insert(address);
    setSubscribedChannelsAskedForFalse.erase(address);
}

size_t GetMessageDirtyCacheSize()
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
    // CMessage: Max 123 Bytes


    size_t size = 0;
    // Messages Caches
    size += 32 * setDirtyMessagesRemove.size(); // COutPoint;
    size += (32 + 123) * mapDirtyMessagesAdd.size(); // COutPoint -> CMessage
    size += (32 + 123) * mapDirtyMessagesOrphaned.size(); // COutPoint -> CMessage


    // Message Channel Caches
    size += 32 * setDirtyChannelsAdd.size();
    size += 32 * setDirtyChannelsRemove.size();
    size += 32 * setSubscribedChannelsAskedForFalse.size();

    // Address Seen Caches
    size += 32 * setDirtySeenAddressAdd.size();
    size += 32 * setAddressAskedForFalse.size();

    return size;
}


std::string CZMQMessage::createJsonString()
{
    std::string str = "";
    str += "{";
    str += "\"blockheight\": " + std::to_string(this->blockHeight) + ", ";
    str += "\"assetname\": \"" + this->assetName + "\", ";
    str += "\"ipfshash\": \"" + EncodeAssetData(this->ipfsHash) + "\", ";
    str += "\"expiretime\": " + std::to_string(this->nExpireTime);
    str += "}";

    return str;
}
