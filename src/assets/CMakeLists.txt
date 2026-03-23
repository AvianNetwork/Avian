# Copyright (c) 2024-present The Avian Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

add_library(bitcoin_assets STATIC EXCLUDE_FROM_ALL
  ans.cpp
  assetdb.cpp
  assets.cpp
  assetsnapshotdb.cpp
  assettypes.cpp
  LibBoolEE.cpp
  messages.cpp
  myassetsdb.cpp
  restricteddb.cpp
  rewards.cpp
  snapshotrequestdb.cpp
)
target_link_libraries(bitcoin_assets
  PRIVATE
    core_interface
    bitcoin_common
    bitcoin_util
    univalue
    leveldb
    Boost::headers
)
