# Copyright (c) 2026 ALENOC <https://github.com/ALENOC> (Ravencoin RIP-25)
# Copyright (c) 2024-present The Avian Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
#
# RIP-25: liboqs depends package for ML-DSA-44 post-quantum signatures.
# Ported from Ravencoin RIP-25 <https://github.com/RavenProject/Ravencoin/pull/1281>

package=liboqs
$(package)_version=0.16.0
$(package)_download_path=https://github.com/open-quantum-safe/liboqs/archive/refs/tags/
$(package)_file_name=$($(package)_version).tar.gz
$(package)_sha256_hash=162d5b510518ee5f285f82fa1f16402a885176e818bf1b1a4c3c91c9a2f01eae
$(package)_dependencies=

define $(package)_set_vars
  $(package)_config_opts=-DOQS_BUILD_ONLY_LIB=ON
  $(package)_config_opts+=-DOQS_MINIMAL_BUILD="SIG_ml_dsa_44"
  $(package)_config_opts+=-DOQS_USE_OPENSSL=OFF
  $(package)_config_opts+=-DBUILD_SHARED_LIBS=OFF
  $(package)_config_opts+=-DOQS_DIST_BUILD=ON
  $(package)_config_opts+=-DOQS_PERMIT_UNSUPPORTED_ARCHITECTURE=ON
  $(package)_config_opts+=-DCMAKE_SYSTEM_PROCESSOR=$(host_arch)
  $(package)_config_opts_darwin+=-DOQS_DIST_BUILD=OFF
  $(package)_config_opts_darwin+=-DOQS_USE_AVX2_INSTRUCTIONS=OFF
  $(package)_config_opts_darwin+=-DOQS_USE_AVX_INSTRUCTIONS=OFF
  $(package)_config_opts_darwin+=-DOQS_USE_SSE2_INSTRUCTIONS=OFF
endef

define $(package)_config_cmds
  $($(package)_cmake) -S . -B .
endef

define $(package)_build_cmds
  $(MAKE)
endef

define $(package)_stage_cmds
  $(MAKE) DESTDIR=$($(package)_staging_dir) install
endef

define $(package)_postprocess_cmds
  rm -rf share
endef
