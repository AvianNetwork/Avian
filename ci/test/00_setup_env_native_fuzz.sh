#!/usr/bin/env bash
#
# Copyright (c) 2019-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

export LC_ALL=C.UTF-8

export CI_IMAGE_NAME_TAG="mirror.gcr.io/ubuntu:24.04"
export CONTAINER_NAME=ci_native_fuzz
export APT_LLVM_V="21"
export PACKAGES="clang-${APT_LLVM_V} llvm-${APT_LLVM_V} libclang-rt-${APT_LLVM_V}-dev libevent-dev libboost-dev libsqlite3-dev libcapnp-dev capnproto"
export NO_DEPENDS=1
export RUN_UNIT_TESTS=false
export RUN_FUNCTIONAL_TESTS=false
export RUN_FUZZ_TESTS=true
# Avian: do not replay Bitcoin Core's qa-assets corpus (its inputs are shaped for
# Bitcoin's magic/chainparams/serialization, so they exercise the wrong paths here).
# Instead fuzz every target generatively for FUZZ_EMPTY_MIN_TIME seconds - a build +
# short generative smoke that catches harness build rot and immediate crashes on
# Avian-correct inputs. A dedicated Avian corpus + long runs is a separate, later effort.
export FUZZ_SKIP_CORPUS=${FUZZ_SKIP_CORPUS:-true}
export FUZZ_EMPTY_MIN_TIME=${FUZZ_EMPTY_MIN_TIME:-2}
export GOAL="all"
export CI_CONTAINER_CAP="--cap-add SYS_PTRACE"  # If run with (ASan + LSan), the container needs access to ptrace (https://github.com/google/sanitizers/issues/764)
export AVIAN_CONFIG="\
 -DBUILD_FOR_FUZZING=ON \
 -DSANITIZERS=fuzzer,address,undefined,float-divide-by-zero,integer \
 -DCMAKE_C_COMPILER=clang \
 -DCMAKE_CXX_COMPILER=clang++ \
 -DCMAKE_C_FLAGS='-ftrivial-auto-var-init=pattern' \
 -DCMAKE_CXX_FLAGS='-ftrivial-auto-var-init=pattern' \
"
