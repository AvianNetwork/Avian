// Copyright (c) 2022 The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "duster.h"
#include "ui_duster.h"
#include "util.h"

#include "wallet/coincontrol.h"
#include "coincontroldialog.h"

#include "init.h"
#include "avianunits.h"
#include "walletmodel.h"
#include "addresstablemodel.h"
#include "optionsmodel.h"
#include "addressbookpage.h"

#include <inttypes.h>

#include <QClipboard>
#include <QMessageBox>
#include <QMenu>
#include <QFileDialog>
#include <QProgressDialog>
#include <QTextStream>
#include <QDesktopServices>
#include <QUrl>
#include <QSettings>
#include <QSize>
#include <boost/filesystem.hpp>
#include <string>
#include <QMainWindow>
#include <QDateTime>
#include <QHeaderView>

#include <QDebug>

CCoinControl* DustingGui::coinControl = new CCoinControl();

DustingGui::DustingGui(const PlatformStyle *_platformStyle, QWidget *parent) : 
    QDialog(parent), 
    ui(new Ui::DustingGui),
    platformStyle(_platformStyle)
{
	// Setup the UI
	ui->setupUi(this);
	ui->mainLayout->setSizeConstraint(QLayout::SetNoConstraint);
	ui->dustAddress->setReadOnly(true);

	// Create and add block list
	createBlockList();
	ui->mainLayout->addWidget(blocksTable, 3, 0, 1, 5);

	// General info label
	infoLabel = new QLabel();
	ui->mainLayout->addWidget(infoLabel, 4, 0, 1, 5);

	// Create and add refresh button
	QPushButton *refreshButton = new QPushButton(tr("&Refresh"));
	connect(refreshButton, SIGNAL(clicked()), this, SLOT(updateBlockList()));
	ui->mainLayout->addWidget(refreshButton, 5, 0);

	// Create and add dust button
	QPushButton *dustButton = new QPushButton(tr("&Dust now"));
	connect(dustButton, SIGNAL(clicked()), this, SLOT(compactBlocks()));
	ui->mainLayout->addWidget(dustButton, 5, 4);

	// Load settings
	minimumBlockAmount = 16;
	minimumOutAmount = 100000000;
	blockDivisor = 80;

//	ui->mainLayout->addWidget(fileLabel, 0, 0);
//	ui->mainLayout->addWidget(fileComboBox, 0, 1, 1, 2);
//	ui->mainLayout->addWidget(textLabel, 1, 0);
//	ui->mainLayout->addWidget(textComboBox, 1, 1, 1, 2);
//	ui->mainLayout->addWidget(directoryLabel, 2, 0);
//	ui->mainLayout->addWidget(directoryComboBox, 2, 1);
//	ui->mainLayout->addWidget(browseButton, 2, 2);
//	ui->mainLayout->addWidget(filesFoundLabel, 4, 0, 1, 2);
//	ui->mainLayout->addWidget(findButton, 4, 2);
//	ui->mainLayout->addWidget(resetButton, 5, 2);

//	updateBlockList();
}

void DustingGui::setModel(WalletModel *model)
{
    this->model = model;
}

void DustingGui::createBlockList()
{
	blocksTable = new QTableWidget(0, 9);
	blocksTable->setSelectionBehavior(QAbstractItemView::SelectRows);

	QStringList labels;
	labels << tr("Amount") << tr("Date") << tr("Label") << tr("Received with") << tr("Confirmations") << tr("_transaction_id") << tr("_amount_int_64") << tr("_vout_index") << ("_input_size");
	blocksTable->setHorizontalHeaderLabels(labels);

	blocksTable->setColumnWidth(COLUMN_AMOUNT, 120);
	blocksTable->setColumnWidth(COLUMN_DATE, 110);
	blocksTable->setColumnWidth(COLUMN_LABEL, 110);
	blocksTable->setColumnWidth(COLUMN_ADDRESS, 330);
	blocksTable->setColumnWidth(COLUMN_CONFIRMATIONS, 130);
	blocksTable->setColumnHidden(COLUMN_TXHASH, true);					// Store transacton hash in this column, but do not show it.
	blocksTable->setColumnHidden(COLUMN_AMOUNT_INT64, true);			// Store int 64 amount in this column, but do not show it.
	blocksTable->setColumnHidden(COLUMN_VOUT_INDEX, true);				// Store vout index.
	blocksTable->setColumnHidden(COLUMN_INPUT_SIZE, true);				// Store input size.

	blocksTable->verticalHeader()->hide();
	blocksTable->setShowGrid(true);
	blocksTable->setSelectionMode(QAbstractItemView::MultiSelection);

//	connect(filesTable, SIGNAL(cellActivated(int,int)),
//		this, SLOT(openFileOfItem(int,int)));
}

void DustingGui::updateBlockList()
{
	// Prepare to refresh
    blocksTable->setRowCount(0);
    blocksTable->setEnabled(false);
    blocksTable->setAlternatingRowColors(true);

    int nDisplayUnit = AvianUnits::AVN;
    if (model && model->getOptionsModel())
		nDisplayUnit = model->getOptionsModel()->getDisplayUnit();

	// Loop own coins    
	std::map<QString, std::vector<COutput>> mapCoins;
	model->listCoins(mapCoins);

    for (const std::pair<QString, std::vector<COutput>>& coins : mapCoins)
    {
        QString sWalletAddress = coins.first;
        QString sWalletLabel = "";
        if (model->getAddressTableModel()) {
            sWalletLabel = model->getAddressTableModel()->labelForAddress(sWalletAddress);
		}
        if (sWalletLabel.length() == 0) {
            sWalletLabel = tr("(no label)");
		}

        int64_t nSum = 0;
        int nChildren = 0;
        for (const COutput& out : coins.second)
        {
			if(out.nDepth >= 100) {
				// Create cell objects and associate values
				QTableWidgetItem *amountItem = new QTableWidgetItem();
				amountItem->setFlags(amountItem->flags() ^ Qt::ItemIsEditable);
				QTableWidgetItem *dateItem = new QTableWidgetItem();
				dateItem->setFlags(dateItem->flags() ^ Qt::ItemIsEditable);
				QTableWidgetItem *labelItem = new QTableWidgetItem();
				labelItem->setFlags(labelItem->flags() ^ Qt::ItemIsEditable);
				QTableWidgetItem *addressItem = new QTableWidgetItem();
				addressItem->setFlags(addressItem->flags() ^ Qt::ItemIsEditable);
				QTableWidgetItem *confirmationItem = new QTableWidgetItem();
				confirmationItem->setFlags(confirmationItem->flags() ^ Qt::ItemIsEditable);
				QTableWidgetItem *transactionItem = new QTableWidgetItem();
				QTableWidgetItem *amountInt64Item = new QTableWidgetItem();
				QTableWidgetItem *voutIndex = new QTableWidgetItem();
				QTableWidgetItem *inputSize = new QTableWidgetItem();

				int nInputSize = 148; // 180 if uncompressed public key
				nSum += out.tx->tx->vout[out.i].nValue;
				nChildren++;
								
				// Address
				CTxDestination outputAddress;
				QString sAddress = "";
				if(ExtractDestination(out.tx->tx->vout[out.i].scriptPubKey, outputAddress))
				{
					sAddress = CAvianAddress(outputAddress).ToString().c_str();
					addressItem->setText(sAddress);
					CPubKey pubkey;
					CKeyID *keyid = boost::get< CKeyID >(&outputAddress);
					if (keyid && model->getPubKey(*keyid, pubkey) && !pubkey.IsCompressed())
						nInputSize = 180;
				}

				// Label
				if (!(sAddress == sWalletAddress)) // Change
				{
					// Tooltip from where the change comes from
					labelItem->setToolTip(tr("change from %1 (%2)").arg(sWalletLabel).arg(sWalletAddress));
					labelItem->setText(tr("(change)"));
				}
				else
				{
					QString sLabel = "";
					if (model->getAddressTableModel())
						sLabel = model->getAddressTableModel()->labelForAddress(sAddress);
					if (sLabel.length() == 0)
						sLabel = tr("(no label)");
					labelItem->setText(sLabel);
					if (ui->dustAddress->text() == "") {
						ui->dustAddress->setText(sAddress);
					}
				}

				// Amount
				amountItem->setText(AvianUnits::format(nDisplayUnit, out.tx->tx->vout[out.i].nValue));
				amountInt64Item->setText(strPad(QString::number(out.tx->tx->vout[out.i].nValue), 18, "0")); // Padding so that sorting works correctly.

				// Date
				dateItem->setText(QDateTime::fromTime_t(out.tx->GetTxTime()).toUTC().toString("yy-MM-dd hh:mm"));
				
				// Confirmations
				confirmationItem->setText(strPad(QString::number(out.nDepth), 8, " "));
				
				// Transaction hash
				uint256 txhash = out.tx->GetHash();
				transactionItem->setText(txhash.GetHex().c_str());

				// vout index
				voutIndex->setText(QString::number(out.i));

				// Input size
				inputSize->setText(QString::number(nInputSize));

				// Add row
				int row = blocksTable->rowCount();
				blocksTable->insertRow(row);
				blocksTable->setItem(row, COLUMN_AMOUNT, amountItem);
				blocksTable->setItem(row, COLUMN_DATE, dateItem);
				blocksTable->setItem(row, COLUMN_LABEL, labelItem);
				blocksTable->setItem(row, COLUMN_ADDRESS, addressItem);
				blocksTable->setItem(row, COLUMN_CONFIRMATIONS, confirmationItem);
				blocksTable->setItem(row, COLUMN_TXHASH, transactionItem);
				blocksTable->setItem(row, COLUMN_AMOUNT_INT64, amountInt64Item);
				blocksTable->setItem(row, COLUMN_VOUT_INDEX, voutIndex);
				blocksTable->setItem(row, COLUMN_INPUT_SIZE, inputSize);
			}
		}
	}
    
    // Sort view to default
    sortView(COLUMN_AMOUNT_INT64, Qt::AscendingOrder);
    blocksTable->setEnabled(true);

	// Add count
	if (blocksTable->rowCount() <= minimumBlockAmount) {
		infoLabel->setText(tr("The wallet is clean."));
	}
	else {
		infoLabel->setText("<b>" + tr("Found ") + QString::number(blocksTable->rowCount()) + tr(" blocks to compact.") + "</b>");
	}
}

void DustingGui::on_addressBookButton_clicked()
{
    if (!model)
        return;

    AddressBookPage dlg(platformStyle, AddressBookPage::ForSelection, AddressBookPage::ReceivingTab, this);
    dlg.setModel(model->getAddressTableModel());

    if(dlg.exec()) {
		ui->dustAddress->setText(dlg.getReturnValue());
    }
}

void DustingGui::compactBlocks()
{
	// Check number of blocks
	if (blocksTable->rowCount() <= minimumBlockAmount)
	{
		QMessageBox::information(this, tr("Wallet Dusting"), tr("The wallet is already optimized."), QMessageBox::Ok, QMessageBox::Ok);
		return;
	}

	// Warn the user
	QString strMessage = tr("The wallet will now be dusted. If your wallet is encrypted, enter the passphrase only once. <b>Are you sure you want to do it now</b> ?");
	QMessageBox::StandardButton retval = QMessageBox::question(
	      this, tr("Wallet Dusting"), strMessage,
	      QMessageBox::Yes|QMessageBox::Cancel, QMessageBox::Yes);

	if (retval == QMessageBox::Cancel)
	    return;

	// Refresh the selection after having put it offline
	updateBlockList();
	int nOps = ((blocksTable->rowCount() - minimumBlockAmount) / blockDivisor) + 1;
	int nOdds = (blocksTable->rowCount() - minimumBlockAmount) % blockDivisor;
	if (nOdds == 1) {
		nOdds = blockDivisor + 1;
	}
	else if (nOdds == 2) {
		nOdds = blockDivisor + 2;
	}
	else if (nOdds == 0) {
		nOdds = blockDivisor;
	}
	if (nOdds >= (blocksTable->rowCount() - minimumBlockAmount)) {
		// Optimize the last piece to the target length
		nOdds = blocksTable->rowCount() - minimumBlockAmount + 2;
	}

	// Unlock the wallet for dusting only once
	WalletModel::UnlockContext ctx(model->requestUnlock());
	if (!ctx.isValid())
	{
		QMessageBox::warning(this, tr("Send Coins"),
	        tr("Cannot unlock wallet at this time, please try again later."),
	        QMessageBox::Ok, QMessageBox::Ok);
		return;
	}

	// Select the first batch of items
    QList<SendCoinsRecipient> recipients;
	qint64 selectionSum;
	WalletModel::SendCoinsReturn sendstatus;
	while (nOps > 0) {
		// Reset previous selection
		coinControl->SetNull();
		recipients.clear();
		selectionSum = 0;
		for (int i = 0; i < nOdds; i++)
		{
			// Prepare this selection
			QTableWidgetItem *itemAmount = blocksTable->item(i, COLUMN_AMOUNT_INT64);
			QTableWidgetItem *itemTx = blocksTable->item(i, COLUMN_TXHASH);
			QTableWidgetItem *itemVout = blocksTable->item(i, COLUMN_VOUT_INDEX);
			COutPoint outpt(uint256(itemTx->text().toStdString()), itemVout->text().toUInt());
			coinControl->Select(outpt);
			selectionSum += itemAmount->text().toULong();

			// Select the row to show something on screen
			blocksTable->selectRow(i);
			QApplication::instance()->processEvents();
			MilliSleep(5);
		}
		MilliSleep(500);
		for (int i = 0; i < nOdds; i++)
		{
			blocksTable->removeRow(i);
			QApplication::instance()->processEvents();
		}
		blocksTable->clearSelection();
		QApplication::instance()->processEvents();

		// Append this selection
		// append this selection
		SendCoinsRecipient rcp;
		rcp.amount = selectionSum;
		rcp.amount -= 10000;				// this is safe value to not incurr in "not enough for fee" errors, in any case it will be credited back as "change"
		rcp.label = "[DUSTING]";
		rcp.address = ui->dustAddress->text();
		recipients.append(rcp);

		// Show the send coin interface
		WalletModelTransaction* tx = new WalletModelTransaction(recipients);

		CCoinControl ctrl;
		if (model->getOptionsModel()->getCoinControlFeatures())
			ctrl = *CoinControlDialog::coinControl;

		WalletModel::SendCoinsReturn prepareStatus;
		prepareStatus = model->prepareTransaction(*tx, ctrl);

		switch(prepareStatus.status)
		{
		case WalletModel::InvalidAddress:
			QMessageBox::critical(this, tr("Send Coins"), 
				tr("The recipient address is not valid, please recheck."), 
				QMessageBox::Ok, QMessageBox::Ok);
			break;
		case WalletModel::InvalidAmount:
			QMessageBox::critical(this, tr("Send Coins"), 
				tr("The amount to pay must be larger than 0"), 
				QMessageBox::Ok, QMessageBox::Ok);
			break;
		case WalletModel::AmountExceedsBalance:
			QMessageBox::critical(this, tr("Send Coins"), 
				tr("The amount exceeds your balance."), 
				QMessageBox::Ok, QMessageBox::Ok);
			break;
		case WalletModel::AmountWithFeeExceedsBalance:
			QMessageBox::critical(this, tr("Send Coins"), 
				tr("The total exceeds your balance when the transaction fee is included"), 
				QMessageBox::Ok, QMessageBox::Ok);
			break;
		case WalletModel::DuplicateAddress:
			QMessageBox::critical(this, tr("Send Coins"), 
				tr("Duplicate address found, can only send to each address once per send operation."), 
				QMessageBox::Ok, QMessageBox::Ok);
			break;
		case WalletModel::TransactionCreationFailed:
			QMessageBox::critical(this, tr("Send Coins"), 
				tr("Transaction creation failed!"), 
				QMessageBox::Ok, QMessageBox::Ok);
			break;
		case WalletModel::OK:
			break;
		}
		
		sendstatus = model->sendCoins(*tx);

		// Check the return value
		bool breakCycle = true;
		switch(sendstatus.status)
		{
		case WalletModel::InvalidAddress:
		    QMessageBox::warning(this, tr("Send Coins"),
		        tr("The recipient address is not valid, please recheck."),
		        QMessageBox::Ok, QMessageBox::Ok);
		    break;
		case WalletModel::InvalidAmount:
		    QMessageBox::warning(this, tr("Send Coins"),
		        tr("The amount to pay must be larger than 0."),
		        QMessageBox::Ok, QMessageBox::Ok);
		    break;
		case WalletModel::AmountExceedsBalance:
		    QMessageBox::warning(this, tr("Send Coins"),
		        tr("The amount exceeds your balance."),
		        QMessageBox::Ok, QMessageBox::Ok);
		    break;
		case WalletModel::AmountWithFeeExceedsBalance:
		    QMessageBox::warning(this, tr("Send Coins"),
		        tr("The total exceeds your balance when the the transaction fee is included."),
		        QMessageBox::Ok, QMessageBox::Ok);
		    break;
		case WalletModel::DuplicateAddress:
		    QMessageBox::warning(this, tr("Send Coins"),
		        tr("Duplicate address found, can only send to each address once per send operation."),
		        QMessageBox::Ok, QMessageBox::Ok);
		    break;
		case WalletModel::TransactionCreationFailed:
		    QMessageBox::warning(this, tr("Send Coins"),
		        tr("Error: Transaction creation failed."),
		        QMessageBox::Ok, QMessageBox::Ok);
		    break;
		case WalletModel::TransactionCommitFailed:
		    QMessageBox::warning(this, tr("Send Coins"),
		        tr("Error: The transaction was rejected. This might happen if some of the coins in your wallet were already spent, such as if you used a copy of wallet.dat and coins were spent in the copy but not marked as spent here."),
		        QMessageBox::Ok, QMessageBox::Ok);
		    break;
		case WalletModel::AbsurdFee:
		    QMessageBox::warning(this, tr("Send Coins"),
		        tr("Error: The transaction was rejected due to an absurd fee, please use a lower fee."),
		        QMessageBox::Ok, QMessageBox::Ok);
		    break;
		case WalletModel::PaymentRequestExpired:
		    QMessageBox::warning(this, tr("Send Coins"),
		        tr("Error: The transaction was rejected because the payment request expired."),
		        QMessageBox::Ok, QMessageBox::Ok);
		    break;
		case WalletModel::Aborted: // User aborted, nothing to do
			break;
		case WalletModel::OK:
			breakCycle = false;
		    break;
		}

		// Something went wrong, just quit.
		if (breakCycle) {
			updateBlockList();
			return;
		}

		// Refresh the list and recalculate values to always include the change that is left over as first item.
		updateBlockList();
		nOps = (blocksTable->rowCount() - minimumBlockAmount) / blockDivisor;
		nOdds = (blocksTable->rowCount() - minimumBlockAmount) % blockDivisor;
		if (nOdds == 1) {
			nOdds = blockDivisor + 1;
		}
		else if (nOdds == 2) {
			nOdds = blockDivisor + 2;
		}
		else if (nOdds == 0) {
			nOdds = blockDivisor;
		}
		if (nOdds >= (blocksTable->rowCount() - minimumBlockAmount)) {
			nOdds = blocksTable->rowCount() - minimumBlockAmount + 1;		// optimize the last piece to the target length
		}
	}
}

QString DustingGui::strPad(QString s, int nPadLength, QString sPadding)
{
	while (s.length() < nPadLength) {
		s = sPadding + s;
	}
    return s;
}

void DustingGui::sortView(int column, Qt::SortOrder order)
{
    sortColumn = column;
    sortOrder = order;
    blocksTable->sortByColumn(column, order);
    blocksTable->horizontalHeader()->setSortIndicator((sortColumn == COLUMN_AMOUNT_INT64 ? 0 : sortColumn), sortOrder);
}


void DustingGui::resizeEvent(QResizeEvent* event)
{
// resize the inner table when necessary
//	int ww = width();
//	filesTable->setColumnWidth(1, ww - 278);
}

void DustingGui::showEvent(QShowEvent* event)
{
	updateBlockList();
}
