#!/usr/bin/env bash
#
# Copyright (c) 2019-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

# RIP-25: a WITH_LIBOQS=ON fuzz build so the real ML-DSA-44 verifier/decoder is
# fuzzed, not the stub. Run nightly (off the per-PR critical path) via
# .github/workflows/fuzz-liboqs-nightly.yml, and focused on the mldsa44 target,
# which constructs a witness-v2 spend and shapes the witness toward the ML-DSA
# sizes so control actually reaches mldsa::Verify. The per-PR fuzz job stays
# WITH_LIBOQS=OFF and covers the parsing/consensus-structure paths of all targets.

export LC_ALL=C.UTF-8

export CI_IMAGE_NAME_TAG="mirror.gcr.io/ubuntu:24.04"
export CONTAINER_NAME=ci_native_fuzz_liboqs
export APT_LLVM_V="21"
export PACKAGES="clang-${APT_LLVM_V} llvm-${APT_LLVM_V} libclang-rt-${APT_LLVM_V}-dev libevent-dev libboost-dev libsqlite3-dev libcapnp-dev capnproto"
export NO_DEPENDS=1
export RUN_UNIT_TESTS=false
export RUN_FUNCTIONAL_TESTS=false
export RUN_FUZZ_TESTS=true
# Grow (and persist, via the workflow) a corpus instead of a throwaway smoke:
# --generate loads the corpus in ${BASE_BUILD_DIR}/fuzz_corpus, fuzzes, and saves
# new coverage-increasing inputs back so each nightly run deepens it.
export FUZZ_GENERATE=${FUZZ_GENERATE:-true}
# Run only the ML-DSA-44 target: it is the one that reaches the real verifier, and
# the liboqs build exists solely to exercise that path.
export FUZZ_TARGETS=${FUZZ_TARGETS:-mldsa44}
export GOAL="all"
export CI_CONTAINER_CAP="--cap-add SYS_PTRACE"  # If run with (ASan + LSan), the container needs access to ptrace (https://github.com/google/sanitizers/issues/764)
# Build liboqs 0.16.0 in-container (03_test_script.sh, BUILD_LIBOQS) so WITH_LIBOQS=ON
# can find and statically link it. Version pinned to depends/packages/liboqs.mk.
export BUILD_LIBOQS=true
export LIBOQS_VERSION=0.16.0
export AVIAN_CONFIG="\
 -DBUILD_FOR_FUZZING=ON \
 -DWITH_LIBOQS=ON \
 -DSANITIZERS=fuzzer,address,undefined,float-divide-by-zero,integer \
 -DCMAKE_C_COMPILER=clang \
 -DCMAKE_CXX_COMPILER=clang++ \
 -DCMAKE_C_FLAGS='-ftrivial-auto-var-init=pattern' \
 -DCMAKE_CXX_FLAGS='-ftrivial-auto-var-init=pattern' \
"
