// Copyright (c) 2024 The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef AVIAN_RPC_UTIL_H
#define AVIAN_RPC_UTIL_H

#include "script/standard.h"
#include "uint256.h"
#include <string>
#include <vector>

class JSONRPCRequest;
class UniValue;

namespace RPC
{

class Params
{
private:
    const JSONRPCRequest& request;

public:
    explicit Params(const JSONRPCRequest& req);

    size_t Size() const;
    bool Has(size_t idx) const;
    bool IsNull(size_t idx) const;

    int GetInt(size_t idx, int default_value = 0) const;
    bool GetBool(size_t idx, bool default_value = false) const;
    double GetDouble(size_t idx, double default_value = 0.0) const;
    int64_t GetInt64(size_t idx, int64_t default_value = 0) const;
    std::string GetString(size_t idx, const std::string& default_value = "") const;

    UniValue GetArray(size_t idx) const;
    UniValue GetObj(size_t idx) const;

    uint256 GetHash(size_t idx) const;
    uint256 GetHash(size_t idx, const std::string& param_name) const;

    CTxDestination GetAddress(size_t idx) const;
    std::string GetAddressString(size_t idx) const;

    int GetIntBounded(size_t idx, int min, int max) const;
    int64_t GetInt64Bounded(size_t idx, int64_t min, int64_t max) const;

    void CheckCount(size_t min) const;
    void CheckCount(size_t min, size_t max) const;
};

struct Arg {
    std::string name;
    std::string type;
    bool required;
    std::string description;
    std::string default_value;

    Arg(const std::string& n, const std::string& t, bool req, const std::string& desc)
        : name(n), type(t), required(req), description(desc), default_value("") {}

    Arg(const std::string& n, const std::string& t, bool req, const std::string& desc, const std::string& def)
        : name(n), type(t), required(req), description(desc), default_value(def) {}
};

struct Result {
    std::string type;
    std::string description;

    Result(const std::string& t, const std::string& desc) : type(t), description(desc) {}
};

class HelpBuilder
{
private:
    std::string name;
    std::vector<Arg> args;
    std::string description;
    Result result;
    std::vector<std::pair<std::string, std::string>> examples;

public:
    explicit HelpBuilder(const std::string& cmd_name, const Result& res);

    HelpBuilder& Description(const std::string& desc);
    HelpBuilder& Arg(const Arg& arg);
    HelpBuilder& Arg(const std::string& name, const std::string& type, bool required, const std::string& desc);
    HelpBuilder& Arg(const std::string& name, const std::string& type, const std::string& desc);
    HelpBuilder& Example(const std::string& label, const std::string& cmd);

    std::string Build() const;
};

} // namespace RPC

#endif
