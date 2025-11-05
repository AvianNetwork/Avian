// Copyright (c) 2022 The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef AVN_DUSTER_H
#define AVN_DUSTER_H

#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QLabel>
#include <QMainWindow>
#include <QPushButton>
#include <QTableWidget>
#include <QWidget>
#include <boost/filesystem.hpp>

namespace Ui
{
class DusterDialog;
}

class WalletModel;
class CCoinControl;
class PlatformStyle;

class DusterDialog : public QDialog
{
    Q_OBJECT

public:
    static CCoinControl* coinControl;

    DusterDialog(const PlatformStyle*, QWidget* parent = 0);
    ~DusterDialog();
    void setModel(WalletModel* model);

private Q_SLOTS:
    void updateBlockList();
    void compactBlocks();
    void on_addressBookButton_clicked();

protected:
    void resizeEvent(QResizeEvent* event);
    void showEvent(QShowEvent* event);

private:
    Ui::DusterDialog* ui;
    const PlatformStyle* platformStyle;
    QTableWidget* blocksTable;
    WalletModel* model;
    int sortColumn;
    Qt::SortOrder sortOrder;
    QLabel* infoLabel;
    QPushButton* refreshButton;
    QPushButton* dustButton;
    int blockDivisor;
    int minimumBlockAmount;
    enum {
        COLUMN_ADDRESS,       // 0 - Address
        COLUMN_AMOUNT,        // 1 - Amount
        COLUMN_CONFIRMATIONS, // 2 - Confirmations
        COLUMN_DATE,          // 3 - Date
        COLUMN_TXHASH,        // 4 - Details (Transaction Hash)
        COLUMN_LABEL,         // 5 - Label (hidden column for internal use)
        COLUMN_AMOUNT_INT64,  // 6 - Amount as int64 (hidden column for sorting)
        COLUMN_VOUT_INDEX,    // 7 - Vout index (hidden column)
        COLUMN_INPUT_SIZE     // 8 - Input size (hidden column)
    };

    void createBlockList();
    void sortView(int column, Qt::SortOrder order);
    QString strPad(QString s, int nPadLength, QString sPadding);
};

#endif // AVN_DUSTER_H
