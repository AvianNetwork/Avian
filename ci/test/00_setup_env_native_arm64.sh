#!/usr/bin/env bash
#
# Copyright (c) 2019-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

# RIP-25: unit tests on 64-bit ARM Linux with the real ML-DSA-44 code. liboqs
# picks its verifier per CPU, so ARM nodes run NEON code that the x86-64 jobs
# never reach. mldsa44_conformance_tests checks that verifier against the
# portable C one on the Wycheproof and ACVP vectors. Runs natively on an ARM
# runner; 00_setup_env_arm.sh is the unrelated 32-bit cross build.

export LC_ALL=C.UTF-8

export CONTAINER_NAME=ci_native_arm64
export CI_IMAGE_NAME_TAG="mirror.gcr.io/ubuntu:24.04"
export NO_DEPENDS=1
export PACKAGES="libboost-dev libevent-dev libsqlite3-dev"
export RUN_UNIT_TESTS=true
export RUN_FUNCTIONAL_TESTS=false
export GOAL="all"
export BUILD_LIBOQS=true
export LIBOQS_VERSION=0.16.0
export LIBOQS_REQUIRE_BACKEND=AARCH64
# No GUI and no IPC: the x86-64 job builds those, and neither changes which
# verifier runs.
export AVIAN_CONFIG="-DBUILD_GUI=OFF -DENABLE_IPC=OFF -DBUILD_TESTS=ON -DWITH_LIBOQS=ON"
