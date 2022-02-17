// Copyright (c) 2022 The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef AVIAN_WRAPPING_H
#define AVIAN_WRAPPING_H

#include "walletmodel.h"
#include "guiutil.h"

#include <QDialog>
#include <QThread>

class PlatformStyle;

namespace Ui {
    class WrapPage;
}

class WrapPage: public QDialog
{
    Q_OBJECT

public:
    explicit WrapPage(const PlatformStyle *platformStyle, QWidget *parent = 0);
    ~WrapPage();

    void setModel(WalletModel *model);

private Q_SLOTS:
    void wrapped_clicked();
    void setBalance(const CAmount& balance, const CAmount& unconfirmedBalance, const CAmount& immatureBalance,
                    const CAmount& watchOnlyBalance, const CAmount& watchUnconfBalance, const CAmount& watchImmatureBalance);

private:
    Ui::WrapPage *ui;
    WalletModel *model;
};

#endif // AVIAN_WRAPPING_H
