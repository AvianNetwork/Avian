# Copyright (c) 2023-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

function(generate_setup_nsi)
  set(abs_top_srcdir ${PROJECT_SOURCE_DIR})
  set(abs_top_builddir ${PROJECT_BINARY_DIR})
  set(CLIENT_URL ${PROJECT_HOMEPAGE_URL})
  set(CLIENT_TARNAME "avian")
  set(BITCOIN_WRAPPER_NAME "avian")
  set(BITCOIN_GUI_NAME "avian-qt")
  set(BITCOIN_DAEMON_NAME "aviand")
  set(BITCOIN_CLI_NAME "avian-cli")
  set(EXEEXT ${CMAKE_EXECUTABLE_SUFFIX})
  configure_file(${PROJECT_SOURCE_DIR}/share/setup.nsi.in ${PROJECT_BINARY_DIR}/avian-win64-setup.nsi USE_SOURCE_PERMISSIONS @ONLY)
endfunction()
