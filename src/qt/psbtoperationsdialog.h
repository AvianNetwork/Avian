// Copyright (c) 2024 The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef AVIAN_QT_PSBTOPERATIONSDIALOG_H
#define AVIAN_QT_PSBTOPERATIONSDIALOG_H

#include "psbt.h"

#include <QDialog>

class ClientModel;
class WalletModel;

namespace Ui
{
class PSBTOperationsDialog;
}

class PSBTOperationsDialog : public QDialog
{
    Q_OBJECT

public:
    enum StatusLevel {
        INFO,
        WARN,
        ERR
    };

    explicit PSBTOperationsDialog(QWidget* parent, WalletModel* walletModel = nullptr, ClientModel* clientModel = nullptr);
    ~PSBTOperationsDialog();

    void openWithPSBT(const PartiallySignedTransaction& psbtx);

private Q_SLOTS:
    void signTransaction();
    void broadcastTransaction();
    void copyToClipboard();
    void saveTransaction();

private:
    Ui::PSBTOperationsDialog* m_ui;
    WalletModel* m_wallet_model;
    ClientModel* m_client_model;
    PartiallySignedTransaction m_transaction_data;

    void updateTransactionDisplay();
    QString renderTransaction(const PartiallySignedTransaction& psbtx, const PSBTAnalysis& analysis);
    void showStatus(const QString& msg, StatusLevel level);
    void showTransactionStatus(const PSBTAnalysis& analysis);
    QString suggestedPsbtFilename() const;
    void sanitizeTransaction(PartiallySignedTransaction& psbtx) const;
    PartiallySignedTransaction psbtForExport() const;
};

#endif // AVIAN_QT_PSBTOPERATIONSDIALOG_H
