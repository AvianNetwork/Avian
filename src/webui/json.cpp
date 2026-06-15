// Copyright (c) 2025-present The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <webui/webui_internal.h>

#include <univalue.h>

#include <string>

std::string JsonError(const std::string& msg)
{
    UniValue obj(UniValue::VOBJ);
    obj.pushKV("error", msg);
    return obj.write();
}
