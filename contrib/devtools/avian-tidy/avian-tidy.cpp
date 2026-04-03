// Copyright (c) 2023 Bitcoin Developers
// Copyright (c) 2026 The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "nontrivial-threadlocal.h"

#include <clang-tidy/ClangTidyModule.h>
#include <clang-tidy/ClangTidyModuleRegistry.h>

class AvianModule final : public clang::tidy::ClangTidyModule
{
public:
    void addCheckFactories(clang::tidy::ClangTidyCheckFactories& CheckFactories) override
    {
        CheckFactories.registerCheck<avian::NonTrivialThreadLocal>("avian-nontrivial-threadlocal");
    }
};

static clang::tidy::ClangTidyModuleRegistry::Add<AvianModule>
    X("avian-module", "Adds avian checks.");

volatile int AvianModuleAnchorSource = 0;
