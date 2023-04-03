// Copyright (c) 2022 The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef AVN_DUSTER_H
#define AVN_DUSTER_H

#include <QDialog>
#include <QWidget>
#include <QComboBox>
#include <QTableWidget>
#include <QDir>
#include <QPushButton>
#include <QLabel>
#include <QMainWindow>
#include <boost/filesystem.hpp>

namespace Ui {
  class DustingGui;
}

class WalletModel;
class CCoinControl;
class PlatformStyle;

class DustingGui : public QDialog
{
	Q_OBJECT

public:
    static CCoinControl *coinControl;

	DustingGui(const PlatformStyle*, QWidget *parent = 0);
	void setModel(WalletModel *model);

private Q_SLOTS:
	void updateBlockList();
	void compactBlocks();
    void on_addressBookButton_clicked();

protected:
	void resizeEvent(QResizeEvent *event);
	void showEvent(QShowEvent * event);

private:
	Ui::DustingGui *ui;
    const PlatformStyle *platformStyle;
	QTableWidget *blocksTable;
	WalletModel *model;
	int sortColumn;
	Qt::SortOrder sortOrder;
	QLabel *infoLabel;
	QPushButton *refreshButton;
	QPushButton *dustButton;
	int blockDivisor;
	int minimumBlockAmount;
	int minimumOutAmount;
    enum
    {
        COLUMN_AMOUNT,
        COLUMN_DATE,
        COLUMN_LABEL,
        COLUMN_ADDRESS,
        COLUMN_CONFIRMATIONS,
        COLUMN_TXHASH,
        COLUMN_AMOUNT_INT64,
        COLUMN_VOUT_INDEX,
		COLUMN_INPUT_SIZE
    };

	void createBlockList();
	void sortView(int column, Qt::SortOrder order);
	QString strPad(QString s, int nPadLength, QString sPadding);
};

#endif // AVN_DUSTER_H
