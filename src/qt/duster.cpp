// Copyright (c) 2022 The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "duster.h"
#include "ui_duster.h"

#include "addressbookpage.h"
#include "addresstablemodel.h"
#include "avianamountfield.h"
#include "avianunits.h"
#include "guiutil.h"
#include "init.h"
#include "optionsmodel.h"
#include "platformstyle.h"
#include "util.h"
#include "validation.h"
#include "walletmodel.h"

#include "policy/feerate.h"
#include "wallet/coincontrol.h"

#include <inttypes.h>

#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QFileDialog>
#include <QHeaderView>
#include <QMainWindow>
#include <QMenu>
#include <QMessageBox>
#include <QProgressDialog>
#include <QSettings>
#include <QSize>
#include <QTextStream>
#include <QThread>
#include <QUrl>
#include <QtConcurrent/QtConcurrentRun>
#include <boost/filesystem.hpp>
#include <string>

DusterDialog::DusterDialog(const PlatformStyle* _platformStyle, QWidget* parent) : QDialog(parent),
                                                                                   ui(new Ui::DusterDialog),
                                                                                   platformStyle(_platformStyle),
                                                                                   coinControl(new CCoinControl())
{
    // Setup the UI
    ui->setupUi(this);
    ui->dustAddress->setReadOnly(true);

    // Set default values for amount fields
    ui->minInputAmount->setValue(1000000);       // 0.01 AVN in satoshis
    ui->maxInputAmount->setValue(2500000000);    // 25 AVN in satoshis
    ui->maxBatchAmount->setValue(1000000000000); // 10,000 AVN in satoshis
    // maxBatches default is 0 (unlimited) - set in UI form

    // Use the table and info label from the UI file
    blocksTable = ui->blocksTable;
    infoLabel = ui->infoLabel;

    // Configure the table properties
    createBlockList();

    // Connect UI elements that are now defined in the .ui file
    connect(ui->refreshButton, SIGNAL(clicked()), this, SLOT(updateBlockList()));
    connect(ui->consolidateButton, SIGNAL(clicked()), this, SLOT(compactBlocks()));

    ui->addressBookButton->setIcon(platformStyle->SingleColorIcon(":/icons/address-book"));

    // Load settings - mimicking Python script limits
    minimumBlockAmount = 3; // MIN_TXOUTS_TO_CONSOLIDATE = 3
    blockDivisor = 500;     // max_num_tx = 500 (default from Python script)
}

DusterDialog::~DusterDialog()
{
    // Clean up coin control instance member
    delete coinControl;
    delete ui;
}

void DusterDialog::setModel(WalletModel* model)
{
    this->model = model;
}

void DusterDialog::createBlockList()
{
    blocksTable->setColumnCount(9);

    QStringList headers;
    headers << tr("Address") << tr("Amount") << tr("Confirmations") << tr("Date") << tr("Details")
            << tr("Label") << tr("Amount64") << tr("Vout") << tr("Size");
    blocksTable->setHorizontalHeaderLabels(headers);

    blocksTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    blocksTable->setSelectionMode(QAbstractItemView::NoSelection);
    blocksTable->setShowGrid(false);
    blocksTable->setAlternatingRowColors(true);

    // Hide internal columns
    blocksTable->hideColumn(COLUMN_LABEL);
    blocksTable->hideColumn(COLUMN_AMOUNT_INT64);
    blocksTable->hideColumn(COLUMN_VOUT_INDEX);
    blocksTable->hideColumn(COLUMN_INPUT_SIZE);

    // Set column widths for visible columns
    blocksTable->horizontalHeader()->setStretchLastSection(true);
    blocksTable->horizontalHeader()->resizeSection(COLUMN_ADDRESS, 240);
    blocksTable->horizontalHeader()->resizeSection(COLUMN_AMOUNT, 120);
    blocksTable->horizontalHeader()->resizeSection(COLUMN_CONFIRMATIONS, 100);
    blocksTable->horizontalHeader()->resizeSection(COLUMN_DATE, 150);
}

void DusterDialog::updateBlockList()
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

    for (const std::pair<QString, std::vector<COutput>>& coins : mapCoins) {
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
        for (const COutput& out : coins.second) {
            // Apply amount filters
            CAmount utxoValue = out.tx->tx->vout[out.i].nValue;
            CAmount minAmount = ui->minInputAmount->value();
            CAmount maxAmount = ui->maxInputAmount->value();

            if (utxoValue < minAmount || utxoValue > maxAmount) {
                continue; // Skip UTXOs outside the filter range
            }

            // Create cell objects and associate values
            QTableWidgetItem* amountItem = new QTableWidgetItem();
            amountItem->setFlags(amountItem->flags() ^ Qt::ItemIsEditable);
            QTableWidgetItem* dateItem = new QTableWidgetItem();
            dateItem->setFlags(dateItem->flags() ^ Qt::ItemIsEditable);
            QTableWidgetItem* labelItem = new QTableWidgetItem();
            labelItem->setFlags(labelItem->flags() ^ Qt::ItemIsEditable);
            QTableWidgetItem* addressItem = new QTableWidgetItem();
            addressItem->setFlags(addressItem->flags() ^ Qt::ItemIsEditable);
            QTableWidgetItem* confirmationItem = new QTableWidgetItem();
            confirmationItem->setFlags(confirmationItem->flags() ^ Qt::ItemIsEditable);
            QTableWidgetItem* transactionItem = new QTableWidgetItem();
            QTableWidgetItem* amountInt64Item = new QTableWidgetItem();
            QTableWidgetItem* voutIndex = new QTableWidgetItem();
            QTableWidgetItem* inputSize = new QTableWidgetItem();

            int nInputSize = 148; // 180 if uncompressed public key
            nSum += out.tx->tx->vout[out.i].nValue;
            nChildren++;

            // Address
            CTxDestination outputAddress;
            QString sAddress = "";
            if (ExtractDestination(out.tx->tx->vout[out.i].scriptPubKey, outputAddress)) {
                sAddress = CAvianAddress(outputAddress).ToString().c_str();
                addressItem->setText(sAddress);
                CPubKey pubkey;
                CKeyID* keyid = boost::get<CKeyID>(&outputAddress);
                if (keyid && model->getPubKey(*keyid, pubkey) && !pubkey.IsCompressed())
                    nInputSize = 180;
            }

            // Label
            if (!(sAddress == sWalletAddress)) {
                labelItem->setToolTip(tr("change from %1 (%2)").arg(sWalletLabel).arg(sWalletAddress));
                labelItem->setText(tr("(change)"));
            } else {
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

    // Sort view to default
    sortView(COLUMN_AMOUNT_INT64, Qt::AscendingOrder);
    blocksTable->setEnabled(true);

    // Add count
    if (blocksTable->rowCount() <= minimumBlockAmount) {
        infoLabel->setText(tr("The wallet is clean."));
    } else {
        infoLabel->setText("<b>" + tr("Found ") + QString::number(blocksTable->rowCount()) + tr(" blocks to compact.") + "</b>");
    }
}

void DusterDialog::on_addressBookButton_clicked()
{
    if (!model)
        return;

    AddressBookPage dlg(platformStyle, AddressBookPage::ForSelection, AddressBookPage::ReceivingTab, this);
    dlg.setModel(model->getAddressTableModel());

    if (dlg.exec()) {
        ui->dustAddress->setText(dlg.getReturnValue());
    }
}

void DusterDialog::compactBlocks()
{
    // Safety check: ensure we have a model
    if (!model) {
        QMessageBox::warning(this, tr("UTXO Consolidation"), tr("No wallet model available."), QMessageBox::Ok, QMessageBox::Ok);
        return;
    }

    // Safety check: ensure we have a destination address
    if (ui->dustAddress->text().isEmpty()) {
        QMessageBox::warning(this, tr("UTXO Consolidation"), tr("Please select a destination address first."), QMessageBox::Ok, QMessageBox::Ok);
        return;
    }

    // Check if blockchain is synchronized - consolidation requires accurate UTXO state
    if (IsInitialBlockDownload()) {
        GUIUtil::SyncWarningMessage syncWarning(this);
        bool sendTransaction = syncWarning.showTransactionSyncWarningMessage();
        if (!sendTransaction)
            return;
    }

    // Check number of blocks
    if (blocksTable->rowCount() <= minimumBlockAmount) {
        QMessageBox::information(this, tr("UTXO Consolidation"), tr("The wallet is already optimized."), QMessageBox::Ok, QMessageBox::Ok);
        return;
    }

    QString strMessage = tr("UTXOs will now be consolidated. If your wallet is encrypted, enter the passphrase only once. <b>Are you sure you want to do it now</b> ?");
    QMessageBox::StandardButton retval = QMessageBox::question(
        this, tr("UTXO Consolidation"), strMessage,
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Yes);

    if (retval == QMessageBox::Cancel)
        return;

    // Unlock the wallet for consolidation only once
    WalletModel::UnlockContext ctx(model->requestUnlock());
    if (!ctx.isValid()) {
        QMessageBox::warning(this, tr("Send Coins"),
            tr("Cannot unlock wallet at this time, please try again later."),
            QMessageBox::Ok, QMessageBox::Ok);
        return;
    }

    // Create a progress dialog for user feedback
    QProgressDialog progressDialog(tr("Consolidating UTXOs..."), tr("Cancel"), 0, 100, this);
    progressDialog.setWindowModality(Qt::WindowModal);
    progressDialog.setSizeGripEnabled(false); // Disable size grip to prevent resizing
    progressDialog.setLabelText(tr("Scanning wallet for UTXOs..."));
    progressDialog.setValue(0);
    progressDialog.setMinimumWidth(380); // Set reasonable minimum width
    progressDialog.setMaximumWidth(400); // Limit maximum width to prevent growing
    progressDialog.setFixedHeight(130);  // Fixed height
    progressDialog.show();
    QApplication::processEvents(); // Update dialog immediately after showing

    // Get fresh UTXO list from model (on main thread for safety)
    std::map<QString, std::vector<COutput>> mapCoins;
    model->listCoins(mapCoins);

    // Count total UTXOs that match the filter criteria
    int totalUTXOFiltered = 0;
    CAmount minAmount = ui->minInputAmount->value();
    CAmount maxAmount = ui->maxInputAmount->value();

    for (const auto& coins : mapCoins) {
        for (const COutput& out : coins.second) {
            if (!out.tx || out.i >= out.tx->tx->vout.size()) {
                continue;
            }
            CAmount utxoValue = out.tx->tx->vout[out.i].nValue;
            if (utxoValue >= minAmount && utxoValue <= maxAmount) {
                totalUTXOFiltered++;
            }
        }
    }

    // Keep wallet operations on main thread for thread safety
    // Use processEvents() to keep UI responsive during block validation
    int batchCount = 0;
    int totalProcessed = 0;
    QString finalMessage;

    while (true) {
        // Check if user cancelled
        if (progressDialog.wasCanceled()) {
            finalMessage = tr("Consolidation was cancelled after processing %1 batches.").arg(batchCount);
            break;
        }

        // Allow UI events and block validation to process
        QApplication::processEvents();

        // Check if we've reached the maximum batch limit
        int maxBatches = ui->maxBatches->value();
        if (maxBatches > 0 && batchCount >= maxBatches) {
            finalMessage = tr("Reached maximum batch limit of %1. Processed %2 batches.").arg(maxBatches).arg(batchCount);
            break;
        }

        // Get fresh UTXO list from model (on main thread for safety)
        std::map<QString, std::vector<COutput>> mapCoins;
        model->listCoins(mapCoins);

        // Check if we're done
        if (totalUTXOFiltered <= minimumBlockAmount) {
            finalMessage = tr("Consolidation completed! Processed %1 batches with %2 total UTXOs.").arg(batchCount).arg(totalProcessed);
            break;
        }

        // Update progress
        int maxUtxosPerBatch = ui->maxUtxosPerBatch->value();
        int estimatedBatches = qMax(1, (totalUTXOFiltered + maxUtxosPerBatch - 1) / maxUtxosPerBatch);
        if (maxBatches > 0) {
            estimatedBatches = qMin(estimatedBatches, maxBatches);
        }
        int progress = qMin(99, (batchCount * 100) / qMax(1, estimatedBatches));
        progressDialog.setRange(0, 100);
        progressDialog.setValue(progress);
        progressDialog.setLabelText(tr("Processing batch %1 of ~%2... (%3 UTXOs remaining)").arg(batchCount + 1).arg(estimatedBatches).arg(totalUTXOFiltered));

        // Collect UTXOs for this batch
        coinControl->SetNull();

        CFeeRate minFeeRate(1000); // 1000 satoshis per kilobyte = 1 sat/byte
        coinControl->m_feerate = minFeeRate;
        coinControl->fOverrideFeeRate = true;

        QList<SendCoinsRecipient> recipients;
        qint64 selectionSum = 0;
        int utxosInBatch = 0;
        CAmount minBatchAmount = ui->minInputAmount->value();
        CAmount maxBatchAmount = ui->maxBatchAmount->value();

        // Collect UTXOs from the coin map
        for (const auto& coins : mapCoins) {
            for (const COutput& out : coins.second) {
                if (utxosInBatch >= ui->maxUtxosPerBatch->value()) {
                    break;
                }

                if (!out.tx || out.i >= out.tx->tx->vout.size()) {
                    continue;
                }

                CAmount utxoValue = out.tx->tx->vout[out.i].nValue;
                CAmount minAmount = ui->minInputAmount->value();
                CAmount maxAmount = ui->maxInputAmount->value();

                if (utxoValue < minAmount || utxoValue > maxAmount) {
                    continue;
                }

                // Stop when we reach max batch amount from UI
                if (selectionSum + utxoValue > maxBatchAmount) {
                    break;
                }
                COutPoint outpt(out.tx->GetHash(), out.i);
                coinControl->Select(outpt);
                selectionSum += utxoValue;
                utxosInBatch++;
            }
            if (utxosInBatch >= ui->maxUtxosPerBatch->value()) {
                break;
            }
        }

        if (utxosInBatch < 3 || selectionSum <= minBatchAmount) {
            finalMessage = tr("Consolidation completed! Processed %1 batches with %2 total UTXOs.").arg(batchCount).arg(totalProcessed);
            break;
        }

        // Check against user-configured maximum batch amount
        if (selectionSum > maxBatchAmount) {
            finalMessage = tr("Consolidation completed! Processed %1 batches with %2 total UTXOs.").arg(batchCount).arg(totalProcessed);
            break;
        }

        // Create the consolidation transaction
        SendCoinsRecipient rcp;
        rcp.amount = selectionSum;
        rcp.fSubtractFeeFromAmount = true;
        rcp.address = ui->dustAddress->text();

        // Preserve existing label or create new one
        QString existingLabel = model->getAddressTableModel()->labelForAddress(rcp.address);
        if (existingLabel.isEmpty()) {
            rcp.label = "[CONSOLIDATION]";
        } else {
            rcp.label = existingLabel;
        }

        if (selectionSum <= 100000LL) {
            continue;
        }

        // Validate address format
        if (rcp.address.isEmpty()) {
            finalMessage = tr("Destination address is empty.");
            break;
        }

        recipients.append(rcp);

        // Send the transaction (on main thread for wallet safety)
        WalletModelTransaction* tx = nullptr;
        WalletModel::SendCoinsReturn sendstatus;
        try {
            tx = new WalletModelTransaction(recipients);

            // Prepare transaction with coin control
            WalletModel::SendCoinsReturn prepareStatus = model->prepareTransaction(*tx, *coinControl);
            if (prepareStatus.status != WalletModel::OK) {
                sendstatus = prepareStatus;
            } else {
                sendstatus = model->sendCoins(*tx);
            }
        } catch (const std::exception& e) {
            if (tx) {
                delete tx;
            }
            finalMessage = tr("Exception occurred during transaction creation: %1").arg(e.what());
            break;
        } catch (...) {
            if (tx) {
                delete tx;
            }
            finalMessage = tr("Unknown exception occurred during transaction creation.");
            break;
        }

        if (tx) {
            delete tx;
        }

        // Check result
        if (sendstatus.status != WalletModel::OK) {
            QString errorMsg = tr("Transaction failed: ");
            QString debugInfo = tr("\nBatch: %1, UTXOs: %2, Amount: %3")
                                    .arg(batchCount + 1)
                                    .arg(utxosInBatch)
                                    .arg(selectionSum);

            switch (sendstatus.status) {
            case WalletModel::InvalidAddress:
                errorMsg += tr("Invalid address");
                break;
            case WalletModel::InvalidAmount:
                errorMsg += tr("Invalid amount");
                break;
            case WalletModel::AmountExceedsBalance:
                errorMsg += tr("Amount exceeds balance");
                break;
            case WalletModel::AmountWithFeeExceedsBalance:
                errorMsg += tr("Amount with fee exceeds balance");
                break;
            case WalletModel::DuplicateAddress:
                errorMsg += tr("Duplicate address");
                break;
            case WalletModel::TransactionCreationFailed:
                errorMsg += tr("Transaction creation failed (wallet may be locked)");
                break;
            case WalletModel::TransactionCommitFailed:
                errorMsg += tr("Transaction commit failed");
                break;
            case WalletModel::AbsurdFee:
                errorMsg += tr("Absurd fee");
                break;
            case WalletModel::PaymentRequestExpired:
                errorMsg += tr("Payment request expired");
                break;
            default:
                errorMsg += tr("Unknown error (code: %1)").arg((int)sendstatus.status);
                break;
            }

            finalMessage = errorMsg;
            break;
        }

        batchCount++;
        totalProcessed += utxosInBatch;

        // Update progress after successful batch
        progressDialog.setValue(batchCount);

        // Small delay to prevent overwhelming the system
        QThread::msleep(100);
    }

    // Ensure progress reaches 100% at the end
    progressDialog.setValue(progressDialog.maximum());

    // Close progress dialog
    progressDialog.close();

    // Show result message
    if (finalMessage.isEmpty()) {
        QMessageBox::information(this, tr("UTXO Consolidation"),
            tr("Consolidation completed! Processed %1 batches with %2 total UTXOs.").arg(batchCount).arg(totalProcessed),
            QMessageBox::Ok, QMessageBox::Ok);
    } else {
        QMessageBox::warning(this, tr("UTXO Consolidation"), finalMessage,
            QMessageBox::Ok, QMessageBox::Ok);
    }

    updateBlockList();
}

QString DusterDialog::strPad(QString s, int nPadLength, QString sPadding)
{
    while (s.length() < nPadLength) {
        s = sPadding + s;
    }
    return s;
}

void DusterDialog::sortView(int column, Qt::SortOrder order)
{
    sortColumn = column;
    sortOrder = order;
    blocksTable->sortByColumn(column, order);
    blocksTable->horizontalHeader()->setSortIndicator((sortColumn == COLUMN_AMOUNT_INT64 ? 0 : sortColumn), sortOrder);
}


void DusterDialog::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
}

void DusterDialog::showEvent(QShowEvent* event)
{
    updateBlockList();
}
