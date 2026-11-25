#!/usr/bin/env bash
#
# Copyright (c) 2019-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

# RIP-25: unit tests on Apple Silicon with the real ML-DSA-44 code, so the NEON
# verifier is also checked under the macOS toolchain and its CPU detection. See
# 00_setup_env_native_arm64.sh for what the tests compare.
#
# This is liboqs's default build, not the one macOS releases ship. depends
# builds liboqs for darwin with OQS_DIST_BUILD=OFF, and liboqs then skips CPU
# feature detection when cross-compiling, so release binaries get the portable C
# verifier only. That verifier is the reference half of every comparison here.

export LC_ALL=C.UTF-8

export CONTAINER_NAME="ci_mac_native_arm64"  # macos does not use a container, but the env var is needed for logging
export CMAKE_GENERATOR="Ninja"
export CI_OS_NAME="macos"
export NO_DEPENDS=1
export OSX_SDK=""
export RUN_UNIT_TESTS=true
export RUN_FUNCTIONAL_TESTS=false
export GOAL="all"
# Apple clang warns where the Linux compilers do not (an unused constant in
# txpackage_tests.cpp, for one). This job is about test results, not warnings.
export NO_WERROR=1
# policyestimator_tests prints about 100 MB of debug log. The ctest on this
# runner reads a test's output more slowly the more it has already read, so the
# test blocks on its own output and takes over 40 minutes instead of 6 seconds,
# stalling every test that runs beside it.
export CTEST_EXCLUDE_REGEX='^policyestimator_tests$'
export BUILD_LIBOQS=true
export LIBOQS_VERSION=0.16.0
# The runner's /usr/local is not ours to write to.
export LIBOQS_PREFIX="${BASE_SCRATCH_DIR}/liboqs"
export LIBOQS_REQUIRE_BACKEND=AARCH64
# Homebrew's CMake is newer than the Linux images' and reports directory-scope
# commands in the tree (include_directories and the like) as developer errors.
export AVIAN_CONFIG="-DBUILD_GUI=OFF -DENABLE_IPC=OFF -DBUILD_TESTS=ON -DWITH_LIBOQS=ON -Wno-error=dev"
