// Copyright (c) 2017-2019 The Raven Core developers
// Copyright (c) 2022 The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <assets/assettypes.h>

#include <hash.h>
#include <span.h>

int IntFromAssetType(AssetType type) {
    return (int)type;
}

AssetType AssetTypeFromInt(int nType) {
    return (AssetType)nType;
}

uint256 CAssetCacheQualifierAddress::GetHash() {
    return Hash(MakeUCharSpan(assetName), MakeUCharSpan(address));
}

uint256 CAssetCacheRestrictedAddress::GetHash() {
    return Hash(MakeUCharSpan(assetName), MakeUCharSpan(address));
}

uint256 CAssetCacheRootQualifierChecker::GetHash() {
    return Hash(MakeUCharSpan(rootAssetName), MakeUCharSpan(address));
}
