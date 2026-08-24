#!/usr/bin/env python3
#
# Copyright (c) 2024-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit.

SHARED_EXCLUDED_SUBTREES = ["src/leveldb/",
                 "src/crc32c/",
                 "src/secp256k1/",
                 "src/minisketch/",
                 "src/ipc/libmultiprocess/",
                 # Avian: vendored X16R / MinotaurX proof-of-work code, kept verbatim
                 # from upstream (contains original French-origin comments, etc.).
                 "src/algo/",
                ]
