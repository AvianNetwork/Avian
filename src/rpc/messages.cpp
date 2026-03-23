// Copyright (c) 2017-2019 The Raven Core developers
// Copyright (c) 2020-2024 The Avian developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <rpc/server.h>
#include <rpc/util.h>

#include <assets/assets.h>
#include <assets/assetdb.h>
#include <assets/messages.h>
#include <assets/myassetsdb.h>
#include <util/time.h>
#include <validation.h>

#include <univalue.h>

extern CMessageDB* pmessagedb;
extern CMessageChannelDB* pmessagechanneldb;
extern CLRUCache<std::string, CMessage>* pMessagesCache;
extern CLRUCache<std::string, int8_t>* pMessageSubscribedChannelsCache;
extern bool fMessaging;
extern CMyRestrictedDB* pmyrestricteddb;

static RPCHelpMan viewallmessages()
{
    return RPCHelpMan{
        "viewallmessages",
        "View all messages that the wallet contains.\n",
        {},
        RPCResult{
            RPCResult::Type::ARR, "", "",
            {
                {RPCResult::Type::OBJ, "", "",
                {
                    {RPCResult::Type::STR, "Asset Name", "the name of the asset the message was sent on"},
                    {RPCResult::Type::STR, "Message", "the IPFS hash of the message"},
                    {RPCResult::Type::STR, "Time", "the time of the message"},
                    {RPCResult::Type::NUM, "Block Height", "the block height the message was included in"},
                    {RPCResult::Type::STR, "Status", "message status (READ, UNREAD, ORPHAN, EXPIRED, SPAM, HIDDEN, ERROR)"},
                    {RPCResult::Type::STR, "Expire Time", /*optional=*/true, "expiration time if set"},
                }},
            }
        },
        RPCExamples{
            HelpExampleCli("viewallmessages", "")
          + HelpExampleRpc("viewallmessages", "")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            if (!fMessaging) {
                throw JSONRPCError(RPC_MISC_ERROR, "Messaging is disabled. To enable, run without -disablemessaging.");
            }

            if (!pMessagesCache || !pmessagedb) {
                throw JSONRPCError(RPC_INTERNAL_ERROR, "Message database is not available.");
            }

            std::set<CMessage> setMessages;
            pmessagedb->LoadMessages(setMessages);

            // Apply dirty cache state
            for (const auto& [out, message] : mapDirtyMessagesOrphaned) {
                CMessage orphanMsg = message;
                orphanMsg.status = MessageStatus::ORPHAN;
                setMessages.erase(orphanMsg);
                setMessages.insert(orphanMsg);
            }

            for (const auto& out : setDirtyMessagesRemove) {
                CMessage message;
                message.out = out;
                setMessages.erase(message);
            }

            for (const auto& [out, message] : mapDirtyMessagesAdd) {
                setMessages.erase(message);
                setMessages.insert(message);
            }

            UniValue messages(UniValue::VARR);
            for (const auto& message : setMessages) {
                UniValue obj(UniValue::VOBJ);
                obj.pushKV("Asset Name", message.strName);
                obj.pushKV("Message", EncodeAssetData(message.ipfsHash));
                obj.pushKV("Time", FormatISO8601DateTime(message.time));
                obj.pushKV("Block Height", message.nBlockHeight);
                obj.pushKV("Status", MessageStatusToString(message.status));
                if (message.nExpiredTime) {
                    obj.pushKV("Expire Time", FormatISO8601DateTime(message.nExpiredTime));
                }
                messages.push_back(obj);
            }

            return messages;
        },
    };
}

static RPCHelpMan viewallmessagechannels()
{
    return RPCHelpMan{
        "viewallmessagechannels",
        "View all message channels the wallet is subscribed to.\n",
        {},
        RPCResult{
            RPCResult::Type::ARR, "", "",
            {
                {RPCResult::Type::STR, "", "channel name"},
            }
        },
        RPCExamples{
            HelpExampleCli("viewallmessagechannels", "")
          + HelpExampleRpc("viewallmessagechannels", "")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            if (!fMessaging) {
                throw JSONRPCError(RPC_MISC_ERROR, "Messaging is disabled. To enable, run without -disablemessaging.");
            }

            if (!pMessageSubscribedChannelsCache || !pmessagechanneldb) {
                throw JSONRPCError(RPC_INTERNAL_ERROR, "Message channel database is not available.");
            }

            std::set<std::string> setChannels;
            pmessagechanneldb->LoadMyMessageChannels(setChannels);

            for (const auto& name : setDirtyChannelsRemove) {
                setChannels.erase(name);
            }

            for (const auto& name : setDirtyChannelsAdd) {
                setChannels.insert(name);
            }

            UniValue channels(UniValue::VARR);
            for (const auto& name : setChannels) {
                channels.push_back(name);
            }

            return channels;
        },
    };
}

static RPCHelpMan subscribetochannel()
{
    return RPCHelpMan{
        "subscribetochannel",
        "Subscribe to a certain message channel.\n",
        {
            {"channel_name", RPCArg::Type::STR, RPCArg::Optional::NO, "the channel name to subscribe to, must end with '!' or have '~' in the name"},
        },
        RPCResult{
            RPCResult::Type::STR, "", "confirmation message"
        },
        RPCExamples{
            HelpExampleCli("subscribetochannel", "\"ASSET_NAME!\"")
          + HelpExampleRpc("subscribetochannel", "\"ASSET_NAME!\"")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            if (!fMessaging) {
                throw JSONRPCError(RPC_MISC_ERROR, "Messaging is disabled. To enable, run without -disablemessaging.");
            }

            if (!pMessageSubscribedChannelsCache || !pmessagechanneldb) {
                throw JSONRPCError(RPC_INTERNAL_ERROR, "Message channel database is not available.");
            }

            std::string channel_name = request.params[0].get_str();

            AssetType type;
            if (!IsAssetNameValid(channel_name, type))
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Channel name is not valid.");

            // If root or sub asset, add owner tag
            if (type == AssetType::ROOT || type == AssetType::SUB) {
                channel_name += "!";
                if (!IsAssetNameValid(channel_name, type))
                    throw JSONRPCError(RPC_INVALID_PARAMETER, "Channel name is not valid.");
            }

            if (type != AssetType::OWNER && type != AssetType::MSGCHANNEL)
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Channel must be an owner asset or message channel asset, e.g. OWNER! or MSG_CHANNEL~123.");

            AddChannel(channel_name);

            return "Subscribed to channel: " + channel_name;
        },
    };
}

static RPCHelpMan unsubscribefromchannel()
{
    return RPCHelpMan{
        "unsubscribefromchannel",
        "Unsubscribe from a certain message channel.\n",
        {
            {"channel_name", RPCArg::Type::STR, RPCArg::Optional::NO, "the channel name to unsubscribe from, must end with '!' or have '~' in the name"},
        },
        RPCResult{
            RPCResult::Type::STR, "", "confirmation message"
        },
        RPCExamples{
            HelpExampleCli("unsubscribefromchannel", "\"ASSET_NAME!\"")
          + HelpExampleRpc("unsubscribefromchannel", "\"ASSET_NAME!\"")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            if (!fMessaging) {
                throw JSONRPCError(RPC_MISC_ERROR, "Messaging is disabled. To enable, run without -disablemessaging.");
            }

            if (!pMessageSubscribedChannelsCache || !pmessagechanneldb) {
                throw JSONRPCError(RPC_INTERNAL_ERROR, "Message channel database is not available.");
            }

            std::string channel_name = request.params[0].get_str();

            AssetType type;
            if (!IsAssetNameValid(channel_name, type))
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Channel name is not valid.");

            if (type == AssetType::ROOT || type == AssetType::SUB) {
                channel_name += "!";
                if (!IsAssetNameValid(channel_name, type))
                    throw JSONRPCError(RPC_INVALID_PARAMETER, "Channel name is not valid.");
            }

            if (type != AssetType::OWNER && type != AssetType::MSGCHANNEL)
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Channel must be an owner asset or message channel asset, e.g. OWNER! or MSG_CHANNEL~123.");

            RemoveChannel(channel_name);

            return "Unsubscribed from channel: " + channel_name;
        },
    };
}

static RPCHelpMan clearmessages()
{
    return RPCHelpMan{
        "clearmessages",
        "Delete current database of messages.\n",
        {},
        RPCResult{
            RPCResult::Type::STR, "", "confirmation with count of deleted messages"
        },
        RPCExamples{
            HelpExampleCli("clearmessages", "")
          + HelpExampleRpc("clearmessages", "")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            if (!fMessaging) {
                throw JSONRPCError(RPC_MISC_ERROR, "Messaging is disabled. To enable, run without -disablemessaging.");
            }

            if (!pMessagesCache || !pmessagedb) {
                throw JSONRPCError(RPC_INTERNAL_ERROR, "Message database is not available.");
            }

            int count = 0;
            count += mapDirtyMessagesAdd.size();

            pMessagesCache->Clear();
            setDirtyMessagesRemove.clear();
            mapDirtyMessagesAdd.clear();
            mapDirtyMessagesOrphaned.clear();
            pmessagedb->EraseAllMessages(count);

            return "Erased " + std::to_string(count) + " messages from the database and cache";
        },
    };
}

static RPCHelpMan viewmytaggedaddresses()
{
    return RPCHelpMan{
        "viewmytaggedaddresses",
        "View all addresses this wallet owns that have been tagged.\n",
        {},
        RPCResult{
            RPCResult::Type::ARR, "", "",
            {
                {RPCResult::Type::OBJ, "", "",
                {
                    {RPCResult::Type::STR, "Address", "the address that was tagged"},
                    {RPCResult::Type::STR, "Tag Name", "the asset tag name"},
                    {RPCResult::Type::STR, "Assigned", /*optional=*/true, "UTC datetime of assignment"},
                    {RPCResult::Type::STR, "Removed", /*optional=*/true, "UTC datetime of removal"},
                }},
            }
        },
        RPCExamples{
            HelpExampleCli("viewmytaggedaddresses", "")
          + HelpExampleRpc("viewmytaggedaddresses", "")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            if (!pmyrestricteddb)
                throw JSONRPCError(RPC_INTERNAL_ERROR, "Restricted database is not available.");

            std::vector<std::tuple<std::string, std::string, bool, uint32_t>> myTaggedAddresses;
            pmyrestricteddb->LoadMyTaggedAddresses(myTaggedAddresses);

            UniValue myTags(UniValue::VARR);
            for (const auto& item : myTaggedAddresses) {
                UniValue obj(UniValue::VOBJ);
                obj.pushKV("Address", std::get<0>(item));
                obj.pushKV("Tag Name", std::get<1>(item));
                if (std::get<2>(item))
                    obj.pushKV("Assigned", FormatISO8601DateTime(std::get<3>(item)));
                else
                    obj.pushKV("Removed", FormatISO8601DateTime(std::get<3>(item)));
                myTags.push_back(obj);
            }

            return myTags;
        },
    };
}

static RPCHelpMan viewmyrestrictedaddresses()
{
    return RPCHelpMan{
        "viewmyrestrictedaddresses",
        "View all addresses this wallet owns that have been restricted.\n",
        {},
        RPCResult{
            RPCResult::Type::ARR, "", "",
            {
                {RPCResult::Type::OBJ, "", "",
                {
                    {RPCResult::Type::STR, "Address", "the restricted address"},
                    {RPCResult::Type::STR, "Asset Name", "the restricted asset name"},
                    {RPCResult::Type::STR, "Restricted", /*optional=*/true, "UTC datetime of restriction"},
                    {RPCResult::Type::STR, "Derestricted", /*optional=*/true, "UTC datetime of derestriction"},
                }},
            }
        },
        RPCExamples{
            HelpExampleCli("viewmyrestrictedaddresses", "")
          + HelpExampleRpc("viewmyrestrictedaddresses", "")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            if (!pmyrestricteddb)
                throw JSONRPCError(RPC_INTERNAL_ERROR, "Restricted database is not available.");

            std::vector<std::tuple<std::string, std::string, bool, uint32_t>> myRestrictedAddresses;
            pmyrestricteddb->LoadMyRestrictedAddresses(myRestrictedAddresses);

            UniValue myRestricted(UniValue::VARR);
            for (const auto& item : myRestrictedAddresses) {
                UniValue obj(UniValue::VOBJ);
                obj.pushKV("Address", std::get<0>(item));
                obj.pushKV("Asset Name", std::get<1>(item));
                if (std::get<2>(item))
                    obj.pushKV("Restricted", FormatISO8601DateTime(std::get<3>(item)));
                else
                    obj.pushKV("Derestricted", FormatISO8601DateTime(std::get<3>(item)));
                myRestricted.push_back(obj);
            }

            return myRestricted;
        },
    };
}

void RegisterMessageRPCCommands(CRPCTable& t)
{
    static const CRPCCommand commands[]{
        {"messages", &viewallmessages},
        {"messages", &viewallmessagechannels},
        {"messages", &subscribetochannel},
        {"messages", &unsubscribefromchannel},
        {"messages", &clearmessages},
        {"restricted", &viewmytaggedaddresses},
        {"restricted", &viewmyrestrictedaddresses},
    };
    for (const auto& c : commands) {
        t.appendCommand(c.name, &c);
    }
}
