// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <wallet/load.h>

#include <common/args.h>
#include <interfaces/chain.h>
#include <scheduler.h>
#include <util/check.h>
#include <util/fs.h>
#include <util/string.h>
#include <util/translation.h>
#include <wallet/context.h>
#include <wallet/spend.h>
#include <wallet/wallet.h>
#include <wallet/walletdb.h>

#include <univalue.h>

#include <system_error>

using util::Join;

namespace wallet {
bool VerifyWallets(WalletContext& context)
{
    interfaces::Chain& chain = *context.chain;
    ArgsManager& args = *Assert(context.args);

    if (args.IsArgSet("-walletdir")) {
        const fs::path wallet_dir{args.GetPathArg("-walletdir")};
        std::error_code error;
        // The canonical path cleans the path, preventing >1 Berkeley environment instances for the same directory
        // It also lets the fs::exists and fs::is_directory checks below pass on windows, since they return false
        // if a path has trailing slashes, and it strips trailing slashes.
        fs::path canonical_wallet_dir = fs::canonical(wallet_dir, error);
        if (error || !fs::exists(canonical_wallet_dir)) {
            chain.initError(strprintf(_("Specified -walletdir \"%s\" does not exist"), fs::PathToString(wallet_dir)));
            return false;
        } else if (!fs::is_directory(canonical_wallet_dir)) {
            chain.initError(strprintf(_("Specified -walletdir \"%s\" is not a directory"), fs::PathToString(wallet_dir)));
            return false;
        // The canonical path transforms relative paths into absolute ones, so we check the non-canonical version
        } else if (!wallet_dir.is_absolute()) {
            chain.initError(strprintf(_("Specified -walletdir \"%s\" is a relative path"), fs::PathToString(wallet_dir)));
            return false;
        }
        args.ForceSetArg("-walletdir", fs::PathToString(canonical_wallet_dir));
    }

    LogInfo("Using wallet directory %s", fs::PathToString(GetWalletDir()));

    chain.initMessage(_("Verifying wallet(s)…"));

    // For backwards compatibility if an unnamed top level wallet exists in the
    // wallets directory, include it in the default list of wallets to load.
    if (!args.IsArgSet("wallet")) {
        DatabaseOptions options;
        DatabaseStatus status;
        ReadDatabaseArgs(args, options);
        bilingual_str error_string;
        options.require_existing = true;
        options.verify = false;
        if (MakeWalletDatabase("", options, status, error_string)) {
            common::SettingsValue wallets(common::SettingsValue::VARR);
            wallets.push_back(""); // Default wallet name is ""
            // Pass write=false because no need to write file and probably
            // better not to. If unnamed wallet needs to be added next startup
            // and the setting is empty, this code will just run again.
            chain.overwriteRwSetting("wallet", std::move(wallets), interfaces::SettingsAction::SKIP_WRITE);
        }
    }

    // Keep track of each wallet absolute path to detect duplicates.
    std::set<fs::path> wallet_paths;

    for (const auto& wallet : chain.getSettingsList("wallet")) {
        if (!wallet.isStr()) {
            chain.initError(_("Invalid value detected for '-wallet' or '-nowallet'. "
                              "'-wallet' requires a string value, while '-nowallet' accepts only '1' to disable all wallets"));
            return false;
        }
        const auto& wallet_file = wallet.get_str();
        // Strip redundant "wallets/" prefix if GetWalletDir() already points
        // to the wallets subdirectory. This handles stale settings entries that
        // were saved relative to the data dir root.
        std::string resolved_wallet_file = wallet_file;
        fs::path wallet_dir = GetWalletDir();
        if (wallet_dir.filename() == "wallets" &&
            (resolved_wallet_file.substr(0, 8) == "wallets/" || resolved_wallet_file.substr(0, 8) == "wallets\\")) {
            resolved_wallet_file = resolved_wallet_file.substr(8);
        }
        const fs::path path = fsbridge::AbsPathJoin(wallet_dir, fs::PathFromString(resolved_wallet_file));

        if (!wallet_paths.insert(path).second) {
            // Only warn about duplicates if the name wasn't resolved from a
            // stale prefix; silently skip resolved duplicates.
            if (resolved_wallet_file == wallet_file) {
                chain.initWarning(strprintf(_("Ignoring duplicate -wallet %s."), resolved_wallet_file));
            }
            continue;
        }

        DatabaseOptions options;
        DatabaseStatus status;
        ReadDatabaseArgs(args, options);
        options.require_existing = true;
        options.verify = true;
        bilingual_str error_string;
        if (!MakeWalletDatabase(resolved_wallet_file, options, status, error_string)) {
            if (status == DatabaseStatus::FAILED_NOT_FOUND) {
                chain.initWarning(Untranslated(strprintf("Skipping -wallet path that doesn't exist. %s", error_string.original)));
            } else if (status == DatabaseStatus::FAILED_LEGACY_DISABLED) {
                // Skipping legacy wallets as they will not be loaded.
                // This will be properly communicated to the user during the loading process.
                continue;
            } else {
                chain.initError(error_string);
                return false;
            }
        }
    }

    return true;
}

bool LoadWallets(WalletContext& context)
{
    interfaces::Chain& chain = *context.chain;
    try {
        std::set<fs::path> wallet_paths;
        for (const auto& wallet : chain.getSettingsList("wallet")) {
            if (!wallet.isStr()) {
                chain.initError(_("Invalid value detected for '-wallet' or '-nowallet'. "
                                  "'-wallet' requires a string value, while '-nowallet' accepts only '1' to disable all wallets"));
                return false;
            }
            const auto& name = wallet.get_str();
            // Strip redundant "wallets/" prefix if GetWalletDir() already points
            // to the wallets subdirectory (see VerifyWallets for details).
            std::string resolved_name = name;
            fs::path wallet_dir = GetWalletDir();
            if (wallet_dir.filename() == "wallets" &&
                (resolved_name.substr(0, 8) == "wallets/" || resolved_name.substr(0, 8) == "wallets\\")) {
                resolved_name = resolved_name.substr(8);
            }
            if (!wallet_paths.insert(fs::PathFromString(resolved_name)).second) {
                continue;
            }
            DatabaseOptions options;
            DatabaseStatus status;
            ReadDatabaseArgs(*context.args, options);
            options.require_existing = true;
            options.verify = false; // No need to verify, assuming verified earlier in VerifyWallets()
            bilingual_str error;
            std::vector<bilingual_str> warnings;
            std::unique_ptr<WalletDatabase> database = MakeWalletDatabase(resolved_name, options, status, error);
            if (!database) {
                if (status == DatabaseStatus::FAILED_NOT_FOUND) continue;
                if (status == DatabaseStatus::FAILED_LEGACY_DISABLED) {
                    // Inform user that legacy wallet is not loaded and suggest upgrade options
                    chain.initWarning(error);
                    continue;
                }
            }
            chain.initMessage(_("Loading wallet…"));
            std::shared_ptr<CWallet> pwallet = database ? CWallet::Create(context, resolved_name, std::move(database), options.create_flags, error, warnings) : nullptr;
            if (!warnings.empty()) chain.initWarning(Join(warnings, Untranslated("\n")));
            if (!pwallet) {
                chain.initError(error);
                return false;
            }

            NotifyWalletLoaded(context, pwallet);
            AddWallet(context, pwallet);
        }
        return true;
    } catch (const std::runtime_error& e) {
        chain.initError(Untranslated(e.what()));
        return false;
    }
}

void StartWallets(WalletContext& context)
{
    for (const std::shared_ptr<CWallet>& pwallet : GetWallets(context)) {
        pwallet->postInitProcess();
    }

    context.scheduler->scheduleEvery([&context] { MaybeResendWalletTxs(context); }, 1min);
}

void UnloadWallets(WalletContext& context)
{
    auto wallets = GetWallets(context);
    while (!wallets.empty()) {
        auto wallet = wallets.back();
        wallets.pop_back();
        std::vector<bilingual_str> warnings;
        RemoveWallet(context, wallet, /* load_on_start= */ std::nullopt, warnings);
        WaitForDeleteWallet(std::move(wallet));
    }
}
} // namespace wallet
