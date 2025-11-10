// Copyright (c) 2009-2022 The Bitcoin Core developers
// Copyright (c) 2025 The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "psbt.h"
#include "streams.h"
#include "utilstrencodings.h"

std::string PartiallySignedTransaction::GetHex() const
{
    CDataStream sstream(SER_NETWORK, PROTOCOL_VERSION);
    sstream << *this;
    return HexStr(sstream.begin(), sstream.end());
}

PartiallySignedTransaction PartiallySignedTransaction::FromHex(const std::string& hex)
{
    std::vector<unsigned char> data = ParseHex(hex);
    CDataStream stream(data, SER_NETWORK, PROTOCOL_VERSION);
    PartiallySignedTransaction psbt;
    stream >> psbt;
    return psbt;
}
