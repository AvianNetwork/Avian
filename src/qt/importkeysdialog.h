// Copyright (c) 2021 The Ravencoin Lite Core developers 
// Copyright (c) 2021 The Dogecoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef AVIAN_QT_IMPORTKEYS_H
#define AVIAN_QT_IMPORTKEYS_H
class CWallet;
class CBlockIndex;

#include <QDialog>
#include <QThread>

class ImportKeysDialog;
class PlatformStyle;

namespace Ui {
class ImportKeysDialog;
}

/** Preferences dialog. */
class ImportKeysDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ImportKeysDialog(const PlatformStyle *_platformStyle, QWidget *parent = 0);
    ~ImportKeysDialog();

Q_SIGNALS:
    void stopExecutor();
    void rescanWallet(CWallet*, CBlockIndex*);

private:
    Ui::ImportKeysDialog *ui;
    const PlatformStyle *platformStyle;
    QThread thread;
    CWallet *pwalletMain = NULL;

private Q_SLOTS:
    /* set OK button state (enabled / disabled) */
    void setOkButtonState(bool fState);
    void on_resetButton_clicked();
    void on_okButton_clicked();
    void on_cancelButton_clicked();
    void resetDialogValues();

    /* import a private key */
    bool importKey();
};

#endif // AVIAN_QT_IMPORTKEYS_H
