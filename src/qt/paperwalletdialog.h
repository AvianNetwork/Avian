// Copyright (c) 2017-2019 The Raven Core developers
// Copyright (c) 2022 The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_PAPERWALLETDIALOG_H
#define BITCOIN_QT_PAPERWALLETDIALOG_H

#include <QDialog>

class WalletModel;

namespace Ui {
    class PaperWalletDialog;
}

/** "Paper Wallet" dialog box */
class PaperWalletDialog : public QDialog
{
    Q_OBJECT

public:
    explicit PaperWalletDialog(QWidget *parent = nullptr);
    ~PaperWalletDialog();

    void setModel(WalletModel *model);

private:
    Ui::PaperWalletDialog *ui;
    WalletModel *model;
    static const int PAPER_WALLET_READJUST_LIMIT = 20;
    static const int PAPER_WALLET_PAGE_MARGIN = 50;

private Q_SLOTS:
    void on_getNewAddress_clicked();
    void on_printButton_clicked();
};

#endif // BITCOIN_QT_PAPERWALLETDIALOG_H
