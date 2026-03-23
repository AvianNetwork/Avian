#!/usr/bin/env bash
#
# Copyright (c) 2019-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

export LC_ALL=C.UTF-8

export CONTAINER_NAME=ci_native_build_only
export CI_IMAGE_NAME_TAG="mirror.gcr.io/ubuntu:24.04"
export NO_DEPENDS=1
export PACKAGES="cmake ninja-build pkgconf libboost-dev libevent-dev libsqlite3-dev libzmq3-dev qt6-base-dev qt6-tools-dev qt6-l10n-tools libqrencode-dev"
export RUN_UNIT_TESTS=false
export RUN_FUNCTIONAL_TESTS=false
export GOAL="install"
export AVIAN_CONFIG="-DBUILD_GUI=ON -DWITH_ZMQ=ON -DENABLE_IPC=OFF -DWERROR=ON"
