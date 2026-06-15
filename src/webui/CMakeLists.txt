# Copyright (c) 2025-present The Avian Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

add_library(bitcoin_webui STATIC EXCLUDE_FROM_ALL
  auth.cpp
  assets_api.cpp
  json.cpp
  node_api.cpp
  static.cpp
  wallet_api.cpp
  webui.cpp
)
target_compile_definitions(bitcoin_webui
  INTERFACE
    ENABLE_WEBUI=1
)
target_link_libraries(bitcoin_webui
  PRIVATE
    core_interface
    univalue
    Boost::headers
    $<TARGET_NAME_IF_EXISTS:bitcoin_wallet>
)
