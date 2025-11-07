// Copyright (c) 2024 The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "rpc/params.h"
#include "base58.h"
#include "primitives/transaction.h"
#include "rpc/server.h"
#include "uint256.h"
#include <univalue.h>

namespace RPC
{

Params::Params(const JSONRPCRequest& req) : request(req) {}

size_t Params::Size() const
{
    return request.params.size();
}

bool Params::Has(size_t idx) const
{
    return idx < request.params.size();
}

bool Params::IsNull(size_t idx) const
{
    if (!Has(idx)) return true;
    return request.params[idx].isNull();
}

int Params::GetInt(size_t idx, int default_value) const
{
    if (IsNull(idx)) return default_value;
    return request.params[idx].get_int();
}

bool Params::GetBool(size_t idx, bool default_value) const
{
    if (IsNull(idx)) return default_value;
    return request.params[idx].get_bool();
}

double Params::GetDouble(size_t idx, double default_value) const
{
    if (IsNull(idx)) return default_value;
    return request.params[idx].get_real();
}

int64_t Params::GetInt64(size_t idx, int64_t default_value) const
{
    if (IsNull(idx)) return default_value;
    return request.params[idx].get_int64();
}

std::string Params::GetString(size_t idx, const std::string& default_value) const
{
    if (IsNull(idx)) return default_value;
    return request.params[idx].get_str();
}

UniValue Params::GetArray(size_t idx) const
{
    if (IsNull(idx)) return UniValue(UniValue::VARR);
    if (!request.params[idx].isArray())
        throw JSONRPCError(RPC_TYPE_ERROR, "Parameter " + std::to_string(idx) + " must be an array");
    return request.params[idx];
}

UniValue Params::GetObj(size_t idx) const
{
    if (IsNull(idx)) return UniValue(UniValue::VOBJ);
    if (!request.params[idx].isObject())
        throw JSONRPCError(RPC_TYPE_ERROR, "Parameter " + std::to_string(idx) + " must be an object");
    return request.params[idx];
}

uint256 Params::GetHash(size_t idx) const
{
    return GetHash(idx, "hash");
}

uint256 Params::GetHash(size_t idx, const std::string& param_name) const
{
    std::string str = GetString(idx);
    if (str.empty())
        throw JSONRPCError(RPC_INVALID_PARAMETER, param_name + " cannot be empty");

    uint256 result;
    if (!result.SetHex(str))
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid " + param_name + " format (should be hex)");
    return result;
}

CTxDestination Params::GetAddress(size_t idx) const
{
    std::string str = GetString(idx);
    CTxDestination dest = DecodeDestination(str);
    if (!IsValidDestination(dest))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid address: " + str);
    return dest;
}

std::string Params::GetAddressString(size_t idx) const
{
    return GetString(idx);
}

int Params::GetIntBounded(size_t idx, int min, int max) const
{
    int value = GetInt(idx);
    if (value < min || value > max)
        throw JSONRPCError(RPC_INVALID_PARAMETER,
            "Parameter " + std::to_string(idx) + " must be between " +
                std::to_string(min) + " and " + std::to_string(max));
    return value;
}

int64_t Params::GetInt64Bounded(size_t idx, int64_t min, int64_t max) const
{
    int64_t value = GetInt64(idx);
    if (value < min || value > max)
        throw JSONRPCError(RPC_INVALID_PARAMETER,
            "Parameter " + std::to_string(idx) + " must be between " +
                std::to_string(min) + " and " + std::to_string(max));
    return value;
}

void Params::CheckCount(size_t min) const
{
    if (Size() < min)
        throw JSONRPCError(RPC_INVALID_PARAMETER,
            "Incorrect number of arguments: expected at least " + std::to_string(min) +
                ", got " + std::to_string(Size()));
}

void Params::CheckCount(size_t min, size_t max) const
{
    if (Size() < min || Size() > max)
        throw JSONRPCError(RPC_INVALID_PARAMETER,
            "Incorrect number of arguments: expected between " +
                std::to_string(min) + " and " + std::to_string(max) +
                ", got " + std::to_string(Size()));
}

HelpBuilder::HelpBuilder(const std::string& cmd_name, const Result& res)
    : name(cmd_name), result(res) {}

HelpBuilder& HelpBuilder::Description(const std::string& desc)
{
    description = desc;
    return *this;
}

HelpBuilder& HelpBuilder::Arg(const Arg& arg)
{
    args.push_back(arg);
    return *this;
}

HelpBuilder& HelpBuilder::Arg(const std::string& arg_name, const std::string& type, bool required, const std::string& desc)
{
    args.emplace_back(arg_name, type, required, desc);
    return *this;
}

HelpBuilder& HelpBuilder::Arg(const std::string& arg_name, const std::string& type, const std::string& desc)
{
    args.emplace_back(arg_name, type, false, desc);
    return *this;
}

HelpBuilder& HelpBuilder::Example(const std::string& label, const std::string& cmd)
{
    examples.emplace_back(label, cmd);
    return *this;
}

std::string HelpBuilder::Build() const
{
    std::string result_str;

    result_str += name;
    if (!args.empty()) {
        result_str += " ";
        for (size_t i = 0; i < args.size(); ++i) {
            if (!args[i].required) result_str += "( ";
            result_str += args[i].name;
            if (!args[i].required) result_str += " )";
            if (i < args.size() - 1) result_str += " ";
        }
    }
    result_str += "\n\n";

    result_str += description + "\n\n";

    if (!args.empty()) {
        result_str += "Arguments:\n";
        for (size_t i = 0; i < args.size(); ++i) {
            result_str += std::to_string(i + 1) + ". \"" + args[i].name + "\"";
            result_str += "    (" + args[i].type + ", " + (args[i].required ? "required" : "optional");
            if (!args[i].default_value.empty())
                result_str += ", default=" + args[i].default_value;
            result_str += ")\n";
            result_str += "        " + args[i].description + "\n";
        }
        result_str += "\n";
    }

    result_str += "Result:\n";
    result_str += result.type + "\n";
    result_str += result.description + "\n";

    if (!examples.empty()) {
        result_str += "\nExamples:\n";
        for (const auto& example : examples) {
            result_str += example.first + ":\n";
            result_str += "  " + example.second + "\n";
        }
    }

    return result_str;
}

} // namespace RPC
