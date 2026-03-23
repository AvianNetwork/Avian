// Copyright (c) 2022 The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/duster.h>
#include <qt/forms/ui_duster.h>

#include <qt/addressbookpage.h>
#include <qt/addresstablemodel.h>
#include <qt/bitcoinamountfield.h>
#include <qt/bitcoinunits.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <qt/sendcoinsrecipient.h>
#include <qt/walletmodel.h>
#include <qt/walletmodeltransaction.h>

#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <key_io.h>
#include <wallet/coincontrol.h>

#include <QApplication>
#include <QDateTime>
#include <QHeaderView>
#include <QMessageBox>
#include <QProgressDialog>
#include <QThread>

DusterDialog::DusterDialog(const PlatformStyle* _platformStyle, QWidget* parent) : QDialog(parent),
                                                                                   ui(new Ui::DusterDialog),
                                                                                   platformStyle(_platformStyle),
                                                                                   model(nullptr)
{
    // Setup the UI
    ui->setupUi(this);
    ui->dustAddress->setReadOnly(true);

    // Set default values for amount fields
    ui->minInputAmount->setValue(1000000);       // 0.01 AVN in satoshis
    ui->maxInputAmount->setValue(2500000000);    // 25 AVN in satoshis
    ui->maxBatchAmount->setValue(1000000000000); // 10,000 AVN in satoshis

    // Use the table and info label from the UI file
    blocksTable = ui->blocksTable;
    infoLabel = ui->infoLabel;

    // Configure the table properties
    createBlockList();

    // Connect UI elements
    connect(ui->refreshButton, &QPushButton::clicked, this, &DusterDialog::updateBlockList);
    connect(ui->consolidateButton, &QPushButton::clicked, this, &DusterDialog::compactBlocks);

    ui->addressBookButton->setIcon(platformStyle->SingleColorIcon(":/icons/address-book"));

    // Load settings - mimicking Python script limits
    minimumBlockAmount = 3;
    blockDivisor = 500;
}

DusterDialog::~DusterDialog()
{
    delete ui;
}

void DusterDialog::setModel(WalletModel* _model)
{
    this->model = _model;
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

    if (!model) {
        infoLabel->setText(tr("No wallet model available."));
        blocksTable->setEnabled(true);
        return;
    }

    BitcoinUnit nDisplayUnit = BitcoinUnit::BTC;
    if (model->getOptionsModel())
        nDisplayUnit = model->getOptionsModel()->getDisplayUnit();

    for (const auto& coins : model->wallet().listCoins()) {
        QString sWalletAddress = QString::fromStdString(EncodeDestination(coins.first));
        QString sWalletLabel = "";
        if (model->getAddressTableModel()) {
            sWalletLabel = model->getAddressTableModel()->labelForAddress(sWalletAddress);
        }
        if (sWalletLabel.length() == 0) {
            sWalletLabel = tr("(no label)");
        }

        for (const auto& outpair : coins.second) {
            const COutPoint& output = std::get<0>(outpair);
            const interfaces::WalletTxOut& out = std::get<1>(outpair);

            // Apply amount filters
            CAmount utxoValue = out.txout.nValue;
            CAmount minAmount = ui->minInputAmount->value();
            CAmount maxAmount = ui->maxInputAmount->value();

            if (utxoValue < minAmount || utxoValue > maxAmount) {
                continue;
            }

            // Create cell objects
            QTableWidgetItem* addressItem = new QTableWidgetItem();
            addressItem->setFlags(addressItem->flags() ^ Qt::ItemIsEditable);
            QTableWidgetItem* amountItem = new QTableWidgetItem();
            amountItem->setFlags(amountItem->flags() ^ Qt::ItemIsEditable);
            QTableWidgetItem* confirmationItem = new QTableWidgetItem();
            confirmationItem->setFlags(confirmationItem->flags() ^ Qt::ItemIsEditable);
            QTableWidgetItem* dateItem = new QTableWidgetItem();
            dateItem->setFlags(dateItem->flags() ^ Qt::ItemIsEditable);
            QTableWidgetItem* transactionItem = new QTableWidgetItem();
            QTableWidgetItem* labelItem = new QTableWidgetItem();
            labelItem->setFlags(labelItem->flags() ^ Qt::ItemIsEditable);
            QTableWidgetItem* amountInt64Item = new QTableWidgetItem();
            QTableWidgetItem* voutIndex = new QTableWidgetItem();
            QTableWidgetItem* inputSize = new QTableWidgetItem();

            int nInputSize = 148;

            // Address
            CTxDestination outputAddress;
            QString sAddress = "";
            if (ExtractDestination(out.txout.scriptPubKey, outputAddress)) {
                sAddress = QString::fromStdString(EncodeDestination(outputAddress));
                addressItem->setText(sAddress);
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
            amountItem->setText(BitcoinUnits::format(nDisplayUnit, utxoValue));
            amountInt64Item->setText(strPad(QString::number(utxoValue), 18, "0"));

            // Date
            dateItem->setText(GUIUtil::dateTimeStr(out.time));

            // Confirmations
            confirmationItem->setText(strPad(QString::number(out.depth_in_main_chain), 8, " "));

            // Transaction hash
            transactionItem->setText(QString::fromStdString(output.hash.GetHex()));

            // vout index
            voutIndex->setText(QString::number(output.n));

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

    // Check number of blocks
    if (blocksTable->rowCount() <= minimumBlockAmount) {
        QMessageBox::information(this, tr("UTXO Consolidation"), tr("The wallet is already optimized."), QMessageBox::Ok, QMessageBox::Ok);
        return;
    }

    // Check if blockchain is synchronized
    if (model->node().isInitialBlockDownload()) {
        GUIUtil::SyncWarningMessage syncWarning(this);
        bool sendTransaction = syncWarning.showTransactionSyncWarningMessage();
        if (!sendTransaction)
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

    // Create a progress dialog
    QProgressDialog progressDialog(tr("Consolidating UTXOs..."), tr("Cancel"), 0, 100, this);
    progressDialog.setWindowModality(Qt::WindowModal);
    progressDialog.setSizeGripEnabled(false);
    progressDialog.setLabelText(tr("Scanning wallet for UTXOs..."));
    progressDialog.setValue(0);
    progressDialog.setMinimumWidth(380);
    progressDialog.setMaximumWidth(400);
    progressDialog.setFixedHeight(130);
    progressDialog.show();
    QApplication::processEvents();

    // Count total UTXOs that match the filter criteria
    CAmount minAmount = ui->minInputAmount->value();
    CAmount maxAmount = ui->maxInputAmount->value();
    int totalUTXOFiltered = 0;

    for (const auto& coins : model->wallet().listCoins()) {
        for (const auto& outpair : coins.second) {
            const interfaces::WalletTxOut& out = std::get<1>(outpair);
            CAmount utxoValue = out.txout.nValue;
            if (utxoValue >= minAmount && utxoValue <= maxAmount) {
                totalUTXOFiltered++;
            }
        }
    }

    // Process batches
    int batchCount = 0;
    int totalProcessed = 0;
    QString finalMessage;

    while (true) {
        if (progressDialog.wasCanceled()) {
            finalMessage = tr("Consolidation was cancelled after processing %1 batches.").arg(batchCount);
            break;
        }

        QApplication::processEvents();

        int maxBatches = ui->maxBatches->value();
        if (maxBatches > 0 && batchCount >= maxBatches) {
            finalMessage = tr("Reached maximum batch limit of %1. Processed %2 batches.").arg(maxBatches).arg(batchCount);
            break;
        }

        // Check if we're done
        if (totalUTXOFiltered <= minimumBlockAmount) {
            finalMessage = tr("Consolidation completed! Processed %1 batches with %2 total UTXOs.").arg(batchCount).arg(totalProcessed);
            break;
        }

        // Update progress
        int maxUtxosPerBatch = ui->maxUtxosPerBatch->value();
        int estimatedBatches = qMax(1, (totalUTXOFiltered + maxUtxosPerBatch - 1) / maxUtxosPerBatch);
        if (maxBatches > 0)
            estimatedBatches = qMin(estimatedBatches, maxBatches);
        int progress = qMin(99, (batchCount * 100) / qMax(1, estimatedBatches));
        progressDialog.setRange(0, 100);
        progressDialog.setValue(progress);
        progressDialog.setLabelText(tr("Processing batch %1 of ~%2... (%3 UTXOs remaining)").arg(batchCount + 1).arg(estimatedBatches).arg(totalUTXOFiltered));

        // Collect UTXOs for this batch using coin control
        wallet::CCoinControl coinControl;
        CFeeRate minFeeRate(1000);
        coinControl.m_feerate = minFeeRate;
        coinControl.fOverrideFeeRate = true;

        QList<SendCoinsRecipient> recipients;
        qint64 selectionSum = 0;
        int utxosInBatch = 0;
        CAmount minBatchAmount = ui->minInputAmount->value();
        CAmount maxBatchAmount = ui->maxBatchAmount->value();

        for (const auto& coins : model->wallet().listCoins()) {
            for (const auto& outpair : coins.second) {
                if (utxosInBatch >= ui->maxUtxosPerBatch->value())
                    break;

                const COutPoint& output = std::get<0>(outpair);
                const interfaces::WalletTxOut& out = std::get<1>(outpair);
                CAmount utxoValue = out.txout.nValue;

                if (utxoValue < minAmount || utxoValue > maxAmount)
                    continue;

                if (selectionSum + utxoValue > maxBatchAmount)
                    break;

                coinControl.Select(output);
                selectionSum += utxoValue;
                utxosInBatch++;
            }
            if (utxosInBatch >= ui->maxUtxosPerBatch->value())
                break;
        }

        if (utxosInBatch < 3 || selectionSum <= minBatchAmount) {
            finalMessage = tr("Consolidation completed! Processed %1 batches with %2 total UTXOs.").arg(batchCount).arg(totalProcessed);
            break;
        }

        if (selectionSum > maxBatchAmount) {
            finalMessage = tr("Consolidation completed! Processed %1 batches with %2 total UTXOs.").arg(batchCount).arg(totalProcessed);
            break;
        }

        // Create the consolidation transaction
        SendCoinsRecipient rcp;
        rcp.amount = selectionSum;
        rcp.fSubtractFeeFromAmount = true;
        rcp.address = ui->dustAddress->text();

        QString existingLabel = model->getAddressTableModel()->labelForAddress(rcp.address);
        if (existingLabel.isEmpty()) {
            rcp.label = "[CONSOLIDATION]";
        } else {
            rcp.label = existingLabel;
        }

        if (selectionSum <= 100000LL)
            continue;

        if (rcp.address.isEmpty()) {
            finalMessage = tr("Destination address is empty.");
            break;
        }

        recipients.append(rcp);

        // Send the transaction
        WalletModel::SendCoinsReturn sendstatus;
        try {
            WalletModelTransaction tx(recipients);
            WalletModel::SendCoinsReturn prepareStatus = model->prepareTransaction(tx, coinControl);
            if (prepareStatus.status != WalletModel::OK) {
                sendstatus = prepareStatus;
            } else {
                model->sendCoins(tx);
                sendstatus.status = WalletModel::OK;
            }
        } catch (const std::exception& e) {
            finalMessage = tr("Exception occurred during transaction creation: %1").arg(e.what());
            break;
        } catch (...) {
            finalMessage = tr("Unknown exception occurred during transaction creation.");
            break;
        }

        if (sendstatus.status != WalletModel::OK) {
            QString errorMsg = tr("Transaction failed: ");
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
            case WalletModel::AbsurdFee:
                errorMsg += tr("Absurd fee");
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
        totalUTXOFiltered -= utxosInBatch;

        progressDialog.setValue(batchCount);
        QThread::msleep(100);
    }

    progressDialog.setValue(progressDialog.maximum());
    progressDialog.close();

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
    QDialog::showEvent(event);
    updateBlockList();
}
