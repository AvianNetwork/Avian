// Copyright (c) 2011-2014 The Bitcoin Core developers
// Copyright (c) 2017-2019 The Raven Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef AVIAN_QT_AVIANADDRESSVALIDATOR_H
#define AVIAN_QT_AVIANADDRESSVALIDATOR_H

#include <QValidator>

/** Base58 entry widget validator, checks for valid characters and
 * removes some whitespace.
 */
class AvianAddressEntryValidator : public QValidator
{
    Q_OBJECT

public:
    explicit AvianAddressEntryValidator(QObject *parent);

    State validate(QString &input, int &pos) const;
};

/** Avian address widget validator, checks for a valid avian address.
 */
class AvianAddressCheckValidator : public QValidator
{
    Q_OBJECT

public:
    explicit AvianAddressCheckValidator(QObject *parent);

    State validate(QString &input, int &pos) const;
};

#endif // AVIAN_QT_AVIANADDRESSVALIDATOR_H
