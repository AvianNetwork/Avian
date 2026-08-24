#!/usr/bin/env bash
#
# Copyright (c) 2019-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

export LC_ALL=C.UTF-8

export CONTAINER_NAME=ci_native
export CI_IMAGE_NAME_TAG="mirror.gcr.io/ubuntu:24.04"
export NO_DEPENDS=1
export PACKAGES="cmake ninja-build pkgconf libboost-dev libevent-dev libsqlite3-dev libzmq3-dev qt6-base-dev qt6-tools-dev qt6-l10n-tools libqrencode-dev"
export RUN_UNIT_TESTS=true
# The functional (Python) test suite is not yet localized to Avian; keep it off for now.
export RUN_FUNCTIONAL_TESTS=false
export GOAL="install"
# RIP-25 (feat/rip-25-keygen): build liboqs from source and build WITH_LIBOQS=ON so the
# ML-DSA-44 code and its tests (pqkey_tests, mldsa44_sighash_tests) are actually exercised.
# 03_test_script.sh builds liboqs when BUILD_LIBOQS=true; the version is pinned to match
# depends/packages/liboqs.mk. WITH_LIBOQS defaults ON and is a hard error if liboqs is
# missing, so the install must precede cmake.
export BUILD_LIBOQS=true
export LIBOQS_VERSION=0.16.0
# BUILD_TESTS defaults OFF, so it must be enabled explicitly or test_avian is never built
# and ctest finds no tests to run. BUILD_GUI_TESTS is forced OFF: the Qt GUI test suite
# (test_avian-qt) is not yet localized to Avian; the GUI itself is still built (BUILD_GUI=ON).
export AVIAN_CONFIG="-DBUILD_GUI=ON -DBUILD_GUI_TESTS=OFF -DWITH_ZMQ=ON -DENABLE_IPC=OFF -DWERROR=ON -DBUILD_TESTS=ON -DWITH_LIBOQS=ON"
