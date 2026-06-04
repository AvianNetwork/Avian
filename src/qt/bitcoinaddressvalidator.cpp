// Copyright (c) 2011-2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/bitcoinaddressvalidator.h>

#include <key_io.h>

/* Base58 characters are:
     "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"

  This is:
  - All numbers except for '0'
  - All upper-case letters except for 'I' and 'O'
  - All lower-case letters except for 'l'
*/

BitcoinAddressEntryValidator::BitcoinAddressEntryValidator(QObject *parent) :
    QValidator(parent)
{
}

QValidator::State BitcoinAddressEntryValidator::validate(QString &input, int &pos) const
{
    Q_UNUSED(pos);

    // Empty address is "intermediate" input
    if (input.isEmpty())
        return QValidator::Intermediate;

    // Correction
    for (int idx = 0; idx < input.size();)
    {
        bool removeChar = false;
        QChar ch = input.at(idx);
        // Corrections made are very conservative on purpose, to avoid
        // users unexpectedly getting away with typos that would normally
        // be detected, and thus sending to the wrong address.
        switch(ch.unicode())
        {
        // Qt categorizes these as "Other_Format" not "Separator_Space"
        case 0x200B: // ZERO WIDTH SPACE
        case 0xFEFF: // ZERO WIDTH NO-BREAK SPACE
            removeChar = true;
            break;
        default:
            break;
        }

        // Remove whitespace
        if (ch.isSpace())
            removeChar = true;

        // To next character
        if (removeChar)
            input.remove(idx, 1);
        else
            ++idx;
    }

    // Validation
    QValidator::State state = QValidator::Acceptable;
    for (int idx = 0; idx < input.size(); ++idx)
    {
        int ch = input.at(idx).unicode();

        if (((ch >= '0' && ch<='9') ||
            (ch >= 'a' && ch<='z') ||
            (ch >= 'A' && ch<='Z')) &&
            ch != 'I' && ch != 'O') // Valid Base58 and Bech32 characters
        {
            // Alphanumeric and valid in base58/bech32
        }
        else if (ch == 'I' || ch == 'O' || ch == '.')
        {
            // Allowed for ANS name entry (e.g. BOB.AVN)
        }
        else
        {
            state = QValidator::Invalid;
            break;
        }
    }

    if (state == QValidator::Invalid)
        return state;

    // A complete ANS name (e.g. "BOB.AVN") must be Acceptable so that
    // QLineEdit emits editingFinished on focus loss (Qt skips the signal
    // when the entry validator returns only Intermediate).
    if (input.toUpper().endsWith(".AVN") && input.length() > 4)
        return QValidator::Acceptable;

    // Inputs containing ANS-only characters but not yet a complete .AVN name
    // are in-progress — degrade to Intermediate so base58/bech32 addresses
    // in the same character set are not prematurely accepted.
    for (int idx = 0; idx < input.size(); ++idx) {
        int ch = input.at(idx).unicode();
        if (ch == 'I' || ch == 'O' || ch == '.') {
            state = QValidator::Intermediate;
            break;
        }
    }

    return state;
}

BitcoinAddressCheckValidator::BitcoinAddressCheckValidator(QObject *parent) :
    QValidator(parent)
{
}

QValidator::State BitcoinAddressCheckValidator::validate(QString &input, int &pos) const
{
    Q_UNUSED(pos);
    // Validate the passed Bitcoin address
    if (IsValidDestinationString(input.toStdString())) {
        return QValidator::Acceptable;
    }

    return QValidator::Invalid;
}
