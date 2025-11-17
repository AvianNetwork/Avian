// Copyright (c) 2011-2016 The Bitcoin Core developers
// Copyright (c) 2017-2019 The Raven Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "sendcoinsdialog.h"
#include "ui_sendcoinsdialog.h"

#include "addresstablemodel.h"
#include "avianunits.h"
#include "clientmodel.h"
#include "coincontroldialog.h"
#include "guiconstants.h"
#include "guiutil.h"
#include "optionsmodel.h"
#include "platformstyle.h"
#include "sendcoinsentry.h"
#include "walletmodel.h"

#include "base58.h"
#include "chainparams.h"
#include "policy/fees.h"
#include "psbt.h"
#include "txmempool.h"
#include "ui_interface.h"
#include "utilstrencodings.h"
#include "validation.h" // mempool and minRelayTxFee
#include "wallet/coincontrol.h"
#include "wallet/fees.h"
#include "wallet/psbtwallet.h"

#include <QClipboard>
#include <QFile>
#include <QFileDialog>
#include <QFontMetrics>
#include <QGraphicsDropShadowEffect>
#include <QScrollBar>
#include <QSettings>
#include <QTimer>

SendCoinsDialog::SendCoinsDialog(const PlatformStyle* _platformStyle, QWidget* parent) : QDialog(parent),
                                                                                         ui(new Ui::SendCoinsDialog),
                                                                                         clientModel(0),
                                                                                         model(0),
                                                                                         fNewRecipientAllowed(true),
                                                                                         fFeeMinimized(true),
                                                                                         platformStyle(_platformStyle)
{
    ui->setupUi(this);

    // Coin control interactions
    connect(ui->pushButtonCoinControl, &QPushButton::clicked,
        this, &SendCoinsDialog::coinControlButtonClicked);
    connect(ui->checkBoxCoinControlChange, &QCheckBox::stateChanged,
        this, &SendCoinsDialog::coinControlChangeChecked);
    connect(ui->lineEditCoinControlChange, &QLineEdit::textChanged,
        this, &SendCoinsDialog::coinControlChangeEdited);

    setupCoinControl(platformStyle);
    setupScrollView(platformStyle);
    setupFeeControl(platformStyle);

    // Initialize dialog with a blank recipient entry so the form is visible immediately.
    addEntry();
}
void SendCoinsDialog::setClientModel(ClientModel* _clientModel)
{
    this->clientModel = _clientModel;

    if (_clientModel) {
        connect(_clientModel, SIGNAL(numBlocksChanged(int, QDateTime, double, bool)), this, SLOT(updateSmartFeeLabel()));
    }
}

void SendCoinsDialog::setModel(WalletModel* _model)
{
    this->model = _model;

    if (_model && _model->getOptionsModel()) {
        for (int i = 0; i < ui->entries->count(); ++i) {
            SendCoinsEntry* entry = qobject_cast<SendCoinsEntry*>(ui->entries->itemAt(i)->widget());
            if (entry) {
                entry->setModel(_model);
            }
        }

        setBalance(_model->getBalance(), _model->getUnconfirmedBalance(), _model->getImmatureBalance(),
            _model->getWatchBalance(), _model->getWatchUnconfirmedBalance(), _model->getWatchImmatureBalance());
        connect(_model, SIGNAL(balanceChanged(CAmount, CAmount, CAmount, CAmount, CAmount, CAmount)), this, SLOT(setBalance(CAmount, CAmount, CAmount, CAmount, CAmount, CAmount)));
        connect(_model->getOptionsModel(), SIGNAL(displayUnitChanged(int)), this, SLOT(updateDisplayUnit()));
        updateDisplayUnit();

        // Coin Control
        connect(_model->getOptionsModel(), SIGNAL(displayUnitChanged(int)), this, SLOT(coinControlUpdateLabels()));
        connect(_model->getOptionsModel(), SIGNAL(coinControlFeaturesChanged(bool)), this, SLOT(coinControlFeatureChanged(bool)));
        ui->frameCoinControl->setVisible(_model->getOptionsModel()->getCoinControlFeatures());
        coinControlUpdateLabels();

        // fee section
        for (const int& n : confTargets) {
            ui->confTargetSelector->addItem(tr("%1 (%2 blocks)").arg(GUIUtil::formatNiceTimeOffset(n * Params().GetConsensus().nPowTargetSpacing)).arg(n));
        }
        connect(ui->confTargetSelector, SIGNAL(currentIndexChanged(int)), this, SLOT(updateSmartFeeLabel()));
        connect(ui->confTargetSelector, SIGNAL(currentIndexChanged(int)), this, SLOT(coinControlUpdateLabels()));

#if (QT_VERSION >= QT_VERSION_CHECK(5, 15, 0))
        connect(ui->groupFee, &QButtonGroup::idClicked, this, &SendCoinsDialog::updateFeeSectionControls);
        connect(ui->groupFee, &QButtonGroup::idClicked, this, &SendCoinsDialog::coinControlUpdateLabels);
#else
        connect(ui->groupFee, SIGNAL(buttonClicked(int)), this, SLOT(updateFeeSectionControls()));
        connect(ui->groupFee, SIGNAL(buttonClicked(int)), this, SLOT(coinControlUpdateLabels()));
#endif
        connect(ui->customFee, SIGNAL(valueChanged()), this, SLOT(coinControlUpdateLabels()));
        connect(ui->checkBoxMinimumFee, SIGNAL(stateChanged(int)), this, SLOT(setMinimumFee()));
        connect(ui->checkBoxMinimumFee, SIGNAL(stateChanged(int)), this, SLOT(updateFeeSectionControls()));
        connect(ui->checkBoxMinimumFee, SIGNAL(stateChanged(int)), this, SLOT(coinControlUpdateLabels()));
        //        connect(ui->optInRBF, SIGNAL(stateChanged(int)), this, SLOT(updateSmartFeeLabel()));
        //        connect(ui->optInRBF, SIGNAL(stateChanged(int)), this, SLOT(coinControlUpdateLabels()));
        ui->customFee->setSingleStep(GetRequiredFee(1000));
        updateFeeSectionControls();
        updateMinFeeLabel();
        updateSmartFeeLabel();

        // set default rbf checkbox state
        //        ui->optInRBF->setCheckState(model->getDefaultWalletRbf() ? Qt::Checked : Qt::Unchecked);
        ui->optInRBF->hide();

        // set the smartfee-sliders default value (wallets default conf.target or last stored value)
        QSettings settings;
        if (settings.value("nSmartFeeSliderPosition").toInt() != 0) {
            // migrate nSmartFeeSliderPosition to nConfTarget
            // nConfTarget is available since 0.15 (replaced nSmartFeeSliderPosition)
            int nConfirmTarget = 25 - settings.value("nSmartFeeSliderPosition").toInt(); // 25 == old slider range
            settings.setValue("nConfTarget", nConfirmTarget);
            settings.remove("nSmartFeeSliderPosition");
        }
        if (settings.value("nConfTarget").toInt() == 0)
            ui->confTargetSelector->setCurrentIndex(getIndexForConfTarget(model->getDefaultConfirmTarget()));
        else
            ui->confTargetSelector->setCurrentIndex(getIndexForConfTarget(settings.value("nConfTarget").toInt()));
    }
}

SendCoinsDialog::~SendCoinsDialog()
{
    QSettings settings;
    settings.setValue("fFeeSectionMinimized", fFeeMinimized);
    settings.setValue("nFeeRadio", ui->groupFee->checkedId());
    settings.setValue("nConfTarget", getConfTargetForIndex(ui->confTargetSelector->currentIndex()));
    settings.setValue("nTransactionFee", (qint64)ui->customFee->value());
    settings.setValue("fPayOnlyMinFee", ui->checkBoxMinimumFee->isChecked());

    delete ui;
}

void SendCoinsDialog::setupCoinControl(const PlatformStyle* platformStyle)
{
    /** Create the shadow effects on the frames */
    ui->frameCoinControl->setGraphicsEffect(GUIUtil::getShadowEffect());
}

void SendCoinsDialog::setupScrollView(const PlatformStyle* platformStyle)
{
    /** Update the scrollview*/
    // ui->scrollArea->setStyleSheet(QString(".QScrollArea{background-color: %1; border: none}").arg(platformStyle->WidgetBackGroundColor().name()));
    // ui->scrollArea->setAutoFillBackground(true);
    ui->scrollArea->setGraphicsEffect(GUIUtil::getShadowEffect());

    // Add some spacing so we can see the whole card
    ui->entries->setContentsMargins(10, 10, 20, 20);
    // ui->scrollAreaWidgetContents->setStyleSheet(QString(".QWidget{ background-color: %1;}").arg(platformStyle->WidgetBackGroundColor().name()));
}

void SendCoinsDialog::setupFeeControl(const PlatformStyle* platformStyle)
{
    /** Create the shadow effects on the frames */
    ui->frameFee->setStyleSheet(QString(".QFrame#frameFee { border-top: 2px solid %1;padding-top: 20px;}").arg(platformStyle->Avian_2B737F().name()));
    // ui->frameFee->setGraphicsEffect(GUIUtil::getShadowEffect());
}

void SendCoinsDialog::on_sendButton_clicked()
{
    if (!model || !model->getOptionsModel())
        return;

    QList<SendCoinsRecipient> recipients;
    bool valid = true;

    for (int i = 0; i < ui->entries->count(); ++i) {
        SendCoinsEntry* entry = qobject_cast<SendCoinsEntry*>(ui->entries->itemAt(i)->widget());
        if (entry) {
            if (entry->validate()) {
                recipients.append(entry->getValue());
            } else {
                valid = false;
            }
        }
    }

    if (!valid || recipients.isEmpty()) {
        return;
    }

    fNewRecipientAllowed = false;
    WalletModel::UnlockContext ctx(model->requestUnlock());
    if (!ctx.isValid()) {
        // Unlock wallet was cancelled
        fNewRecipientAllowed = true;
        return;
    }

    // prepare transaction for getting txFee earlier
    WalletModelTransaction currentTransaction(recipients);
    WalletModel::SendCoinsReturn prepareStatus;

    // Always use a CCoinControl instance, use the CoinControlDialog instance if CoinControl has been enabled
    CCoinControl ctrl;
    if (model->getOptionsModel()->getCoinControlFeatures())
        ctrl = *CoinControlDialog::coinControl;

    updateCoinControlState(ctrl);

    if (IsInitialBlockDownload()) {
        GUIUtil::SyncWarningMessage syncWarning(this);
        bool sendTransaction = syncWarning.showTransactionSyncWarningMessage();
        if (!sendTransaction)
            return;
    }

    prepareStatus = model->prepareTransaction(currentTransaction, ctrl);

    // process prepareStatus and on error generate message shown to user
    processSendCoinsReturn(prepareStatus,
        AvianUnits::formatWithUnit(model->getOptionsModel()->getDisplayUnit(), currentTransaction.getTransactionFee()));

    if (prepareStatus.status != WalletModel::OK) {
        fNewRecipientAllowed = true;
        return;
    }

    CAmount txFee = currentTransaction.getTransactionFee();

    // Format confirmation message
    QStringList formatted;
    for (const SendCoinsRecipient& rcp : currentTransaction.getRecipients()) {
        // generate bold amount string
        QString amount = "<b>" + AvianUnits::formatHtmlWithUnit(model->getOptionsModel()->getDisplayUnit(), rcp.amount);
        amount.append("</b>");
        // generate monospace address string
        QString address = "<span style='font-family: monospace;'>" + rcp.address;
        address.append("</span>");

        QString recipientElement;

        if (!rcp.paymentRequest.IsInitialized()) // normal payment
        {
            if (rcp.label.length() > 0) // label with address
            {
                recipientElement = tr("%1 to %2").arg(amount, GUIUtil::HtmlEscape(rcp.label));
                recipientElement.append(QString(" (%1)").arg(address));
            } else // just address
            {
                recipientElement = tr("%1 to %2").arg(amount, address);
            }
        } else if (!rcp.authenticatedMerchant.isEmpty()) // authenticated payment request
        {
            recipientElement = tr("%1 to %2").arg(amount, GUIUtil::HtmlEscape(rcp.authenticatedMerchant));
        } else // unauthenticated payment request
        {
            recipientElement = tr("%1 to %2").arg(amount, address);
        }

        formatted.append(recipientElement);
    }

    QString questionString = tr("Are you sure you want to send?");
    questionString.append("<br /><br />%1");

    if (txFee > 0) {
        // append fee string if a fee is required
        questionString.append("<hr /><span style='color:#aa0000;'>");
        questionString.append(AvianUnits::formatHtmlWithUnit(model->getOptionsModel()->getDisplayUnit(), txFee));
        questionString.append("</span> ");
        questionString.append(tr("added as transaction fee"));

        // append transaction size
        questionString.append(" (" + QString::number((double)currentTransaction.getTransactionSize() / 1000) + " kB)");
    }

    // add total amount in all subdivision units
    questionString.append("<hr />");
    CAmount totalAmount = currentTransaction.getTotalTransactionAmount() + txFee;
    QStringList alternativeUnits;
    for (AvianUnits::Unit u : AvianUnits::availableUnits()) {
        if (u != model->getOptionsModel()->getDisplayUnit())
            alternativeUnits.append(AvianUnits::formatHtmlWithUnit(u, totalAmount));
    }
    questionString.append(tr("Total Amount %1")
            .arg(AvianUnits::formatHtmlWithUnit(model->getOptionsModel()->getDisplayUnit(), totalAmount)));
    questionString.append(QString("<span style='font-size:10pt;font-weight:normal;'><br />(=%2)</span>")
            .arg(alternativeUnits.join(" " + tr("or") + "<br />")));

    //    if (ui->optInRBF->isChecked())
    //    {
    //        questionString.append("<hr /><span>");
    //        questionString.append(tr("This transaction signals replaceability (optin-RBF)."));
    //        questionString.append("</span>");
    //    }

    SendConfirmationDialog confirmationDialog(tr("Confirm send coins"),
        questionString.arg(formatted.join("<br />")), SEND_CONFIRM_DELAY, this);
    // Use the return value of exec() directly so custom codes (e.g. 42 for PSBT) are preserved.
    int retval = confirmationDialog.exec();

    // Check if "Create Unsigned PSBT" (QMessageBox::Save) button was clicked
    if (retval == QMessageBox::Save) {
        // "Create Unsigned PSBT" clicked - create PSBT without signing
        QString errorMsg;
        try {
            auto tx = currentTransaction.getTransaction();
            if (!tx) {
                errorMsg = tr("Transaction pointer is null");
            } else {
                CMutableTransaction mtx = CMutableTransaction{*tx};

                // BIP-174 requires the TX inside a PSBT to have empty scriptSig/scriptWitness fields.
                for (auto& txin : mtx.vin) {
                    txin.scriptSig.clear();
                    txin.scriptWitness.SetNull();
                }

                PartiallySignedTransaction psbtx(mtx);

                for (auto& input : psbtx.inputs) {
                    input.final_script_sig.clear();
                    input.final_script_witness.clear();
                }

                // Populate UTXO metadata, redeem scripts, and HD keypaths (when available) via wallet helper.
                CWallet* pwallet = model ? model->getWallet() : nullptr;
                if (pwallet) {
                    for (unsigned int i = 0; i < mtx.vin.size(); ++i) {
                        const COutPoint& prevout = mtx.vin[i].prevout;
                        FillPSBTInputWalletData(*pwallet, prevout, psbtx, i);
                    }

                    std::string utxoError;
                    if (!EnsurePSBTInputUTXOs(pwallet, psbtx, utxoError)) {
                        errorMsg = tr("Failed to fetch input data for PSBT: %1").arg(QString::fromStdString(utxoError));
                        throw std::runtime_error(utxoError);
                    }
                }

                presentPSBT(psbtx);
                fNewRecipientAllowed = true;
                return;
            }
        } catch (const std::bad_alloc& e) {
            errorMsg = tr("Memory allocation failed: %1").arg(QString::fromStdString(e.what()));
        } catch (const std::exception& e) {
            errorMsg = tr("Failed to create PSBT: %1").arg(QString::fromStdString(e.what()));
        } catch (...) {
            errorMsg = tr("Unknown error occurred while creating PSBT");
        }

        if (!errorMsg.isEmpty()) {
            QMessageBox::warning(this, tr("Error Creating PSBT"), errorMsg);
        }

        fNewRecipientAllowed = true;
        return;
    }

    if (retval != QMessageBox::Yes) {
        fNewRecipientAllowed = true;
        return;
    }

    // now send the prepared transaction
    WalletModel::SendCoinsReturn sendStatus = model->sendCoins(currentTransaction);
    // process sendStatus and on error generate message shown to user
    processSendCoinsReturn(sendStatus);

    if (sendStatus.status == WalletModel::OK) {
        accept();
        CoinControlDialog::coinControl->UnSelectAll();
        coinControlUpdateLabels();
    }
    fNewRecipientAllowed = true;
}

void SendCoinsDialog::on_clearButton_clicked()
{
    clear();
}

void SendCoinsDialog::on_addButton_clicked()
{
    addEntry();
}

void SendCoinsDialog::clear()
{
    // Remove entries until only one left
    while (ui->entries->count()) {
        ui->entries->takeAt(0)->widget()->deleteLater();
    }
    addEntry();

    updateTabsAndLabels();
}

void SendCoinsDialog::reject()
{
    clear();
}

void SendCoinsDialog::accept()
{
    clear();
}

SendCoinsEntry* SendCoinsDialog::addEntry()
{
    SendCoinsEntry* entry = new SendCoinsEntry(platformStyle, this);
    entry->setModel(model);
    ui->entries->addWidget(entry);
    connect(entry, SIGNAL(removeEntry(SendCoinsEntry*)), this, SLOT(removeEntry(SendCoinsEntry*)));
    connect(entry, SIGNAL(payAmountChanged()), this, SLOT(coinControlUpdateLabels()));
    connect(entry, SIGNAL(subtractFeeFromAmountChanged()), this, SLOT(coinControlUpdateLabels()));

    // Focus the field, so that entry can start immediately
    entry->clear();
    entry->setFocus();
    ui->scrollAreaWidgetContents->resize(ui->scrollAreaWidgetContents->sizeHint());
    qApp->processEvents();
    QScrollBar* bar = ui->scrollArea->verticalScrollBar();
    if (bar)
        bar->setSliderPosition(bar->maximum());

    updateTabsAndLabels();
    return entry;
}

void SendCoinsDialog::updateTabsAndLabels()
{
    setupTabChain(0);
    coinControlUpdateLabels();
}

void SendCoinsDialog::removeEntry(SendCoinsEntry* entry)
{
    entry->hide();

    // If the last entry is about to be removed add an empty one
    if (ui->entries->count() == 1)
        addEntry();

    entry->deleteLater();

    updateTabsAndLabels();
}

QWidget* SendCoinsDialog::setupTabChain(QWidget* prev)
{
    for (int i = 0; i < ui->entries->count(); ++i) {
        SendCoinsEntry* entry = qobject_cast<SendCoinsEntry*>(ui->entries->itemAt(i)->widget());
        if (entry) {
            prev = entry->setupTabChain(prev);
        }
    }
    QWidget::setTabOrder(prev, ui->sendButton);
    QWidget::setTabOrder(ui->sendButton, ui->clearButton);
    QWidget::setTabOrder(ui->clearButton, ui->addButton);
    return ui->addButton;
}

void SendCoinsDialog::setAddress(const QString& address)
{
    SendCoinsEntry* entry = 0;
    // Replace the first entry if it is still unused
    if (ui->entries->count() == 1) {
        SendCoinsEntry* first = qobject_cast<SendCoinsEntry*>(ui->entries->itemAt(0)->widget());
        if (first->isClear()) {
            entry = first;
        }
    }
    if (!entry) {
        entry = addEntry();
    }

    entry->setAddress(address);
}

void SendCoinsDialog::pasteEntry(const SendCoinsRecipient& rv)
{
    if (!fNewRecipientAllowed)
        return;

    SendCoinsEntry* entry = 0;
    // Replace the first entry if it is still unused
    if (ui->entries->count() == 1) {
        SendCoinsEntry* first = qobject_cast<SendCoinsEntry*>(ui->entries->itemAt(0)->widget());
        if (first->isClear()) {
            entry = first;
        }
    }
    if (!entry) {
        entry = addEntry();
    }

    entry->setValue(rv);
    updateTabsAndLabels();
}

bool SendCoinsDialog::handlePaymentRequest(const SendCoinsRecipient& rv)
{
    // Just paste the entry, all pre-checks
    // are done in paymentserver.cpp.
    pasteEntry(rv);
    return true;
}

void SendCoinsDialog::setBalance(const CAmount& balance, const CAmount& unconfirmedBalance, const CAmount& immatureBalance, const CAmount& watchBalance, const CAmount& watchUnconfirmedBalance, const CAmount& watchImmatureBalance)
{
    Q_UNUSED(unconfirmedBalance);
    Q_UNUSED(immatureBalance);
    Q_UNUSED(watchBalance);
    Q_UNUSED(watchUnconfirmedBalance);
    Q_UNUSED(watchImmatureBalance);

    if (model && model->getOptionsModel()) {
        ui->labelBalance->setText(AvianUnits::formatWithUnit(model->getOptionsModel()->getDisplayUnit(), balance));
    }
}

void SendCoinsDialog::updateDisplayUnit()
{
    setBalance(model->getBalance(), 0, 0, 0, 0, 0);
    ui->customFee->setDisplayUnit(model->getOptionsModel()->getDisplayUnit());
    updateMinFeeLabel();
    updateSmartFeeLabel();
}

void SendCoinsDialog::processSendCoinsReturn(const WalletModel::SendCoinsReturn& sendCoinsReturn, const QString& msgArg)
{
    QPair<QString, CClientUIInterface::MessageBoxFlags> msgParams;
    // Default to a warning message, override if error message is needed
    msgParams.second = CClientUIInterface::MSG_WARNING;

    // This comment is specific to SendCoinsDialog usage of WalletModel::SendCoinsReturn.
    // WalletModel::TransactionCommitFailed is used only in WalletModel::sendCoins()
    // all others are used only in WalletModel::prepareTransaction()
    switch (sendCoinsReturn.status) {
    case WalletModel::InvalidAddress:
        msgParams.first = tr("The recipient address is not valid. Please recheck.");
        break;
    case WalletModel::InvalidAmount:
        msgParams.first = tr("The amount to pay must be larger than 0.");
        break;
    case WalletModel::AmountExceedsBalance:
        msgParams.first = tr("The amount exceeds your balance.");
        break;
    case WalletModel::AmountWithFeeExceedsBalance:
        msgParams.first = tr("The total exceeds your balance when the %1 transaction fee is included.").arg(msgArg);
        break;
    case WalletModel::DuplicateAddress:
        msgParams.first = tr("Duplicate address found: addresses should only be used once each.");
        break;
    case WalletModel::TransactionCreationFailed:
        msgParams.first = tr("Transaction creation failed!");
        msgParams.second = CClientUIInterface::MSG_ERROR;
        break;
    case WalletModel::TransactionCommitFailed:
        msgParams.first = tr("The transaction was rejected with the following reason: %1").arg(sendCoinsReturn.reasonCommitFailed);
        msgParams.second = CClientUIInterface::MSG_ERROR;
        break;
    case WalletModel::AbsurdFee:
        msgParams.first = tr("A fee higher than %1 is considered an absurdly high fee.").arg(AvianUnits::formatWithUnit(model->getOptionsModel()->getDisplayUnit(), maxTxFee));
        break;
    case WalletModel::PaymentRequestExpired:
        msgParams.first = tr("Payment request expired.");
        msgParams.second = CClientUIInterface::MSG_ERROR;
        break;
    // included to prevent a compiler warning.
    case WalletModel::OK:
    default:
        return;
    }

    Q_EMIT message(tr("Send Coins"), msgParams.first, msgParams.second);
}

void SendCoinsDialog::minimizeFeeSection(bool fMinimize)
{
    ui->labelFeeMinimized->setVisible(fMinimize);
    ui->buttonChooseFee->setVisible(fMinimize);
    ui->buttonMinimizeFee->setVisible(!fMinimize);
    ui->frameFeeSelection->setVisible(!fMinimize);
    ui->horizontalLayoutSmartFee->setContentsMargins(0, (fMinimize ? 0 : 6), 0, 0);
    fFeeMinimized = fMinimize;
}

void SendCoinsDialog::on_buttonChooseFee_clicked()
{
    minimizeFeeSection(false);
}

void SendCoinsDialog::on_buttonMinimizeFee_clicked()
{
    updateFeeMinimizedLabel();
    minimizeFeeSection(true);
}

void SendCoinsDialog::setMinimumFee()
{
    ui->customFee->setValue(GetRequiredFee(1000));
}

void SendCoinsDialog::updateFeeSectionControls()
{
    ui->confTargetSelector->setEnabled(ui->radioSmartFee->isChecked());
    ui->labelSmartFee->setEnabled(ui->radioSmartFee->isChecked());
    ui->labelSmartFee2->setEnabled(ui->radioSmartFee->isChecked());
    ui->labelSmartFee3->setEnabled(ui->radioSmartFee->isChecked());
    ui->labelFeeEstimation->setEnabled(ui->radioSmartFee->isChecked());
    ui->checkBoxMinimumFee->setEnabled(ui->radioCustomFee->isChecked());
    ui->labelMinFeeWarning->setEnabled(ui->radioCustomFee->isChecked());
    ui->labelCustomPerKilobyte->setEnabled(ui->radioCustomFee->isChecked() && !ui->checkBoxMinimumFee->isChecked());
    ui->customFee->setEnabled(ui->radioCustomFee->isChecked() && !ui->checkBoxMinimumFee->isChecked());
}

void SendCoinsDialog::updateFeeMinimizedLabel()
{
    if (!model || !model->getOptionsModel())
        return;

    if (ui->radioSmartFee->isChecked())
        ui->labelFeeMinimized->setText(ui->labelSmartFee->text());
    else {
        ui->labelFeeMinimized->setText(AvianUnits::formatWithUnit(model->getOptionsModel()->getDisplayUnit(), ui->customFee->value()) + "/kB");
    }
}

void SendCoinsDialog::updateMinFeeLabel()
{
    if (model && model->getOptionsModel())
        ui->checkBoxMinimumFee->setText(tr("Pay only the required fee of %1").arg(AvianUnits::formatWithUnit(model->getOptionsModel()->getDisplayUnit(), GetRequiredFee(1000)) + "/kB"));
}

void SendCoinsDialog::updateCoinControlState(CCoinControl& ctrl)
{
    if (ui->radioCustomFee->isChecked()) {
        ctrl.m_feerate = CFeeRate(ui->customFee->value());
    } else {
        ctrl.m_feerate.reset();
    }
    // Avoid using global defaults when sending money from the GUI
    // Either custom fee will be used or if not selected, the confirmation target from dropdown box
    ctrl.m_confirm_target = getConfTargetForIndex(ui->confTargetSelector->currentIndex());
    //    ctrl.signalRbf = ui->optInRBF->isChecked();
}

void SendCoinsDialog::updateSmartFeeLabel()
{
    if (!model || !model->getOptionsModel())
        return;
    CCoinControl coin_control;
    updateCoinControlState(coin_control);
    coin_control.m_feerate.reset(); // Explicitly use only fee estimation rate for smart fee labels
    FeeCalculation feeCalc;
    CFeeRate feeRate = CFeeRate(GetMinimumFee(1000, coin_control, ::mempool, ::feeEstimator, &feeCalc));

    ui->labelSmartFee->setText(AvianUnits::formatWithUnit(model->getOptionsModel()->getDisplayUnit(), feeRate.GetFeePerK()) + "/kB");

    if (feeCalc.reason == FeeReason::FALLBACK) {
        ui->labelSmartFee2->show(); // (Smart fee not initialized yet. This usually takes a few blocks...)
        ui->labelFeeEstimation->setText("");
        ui->fallbackFeeWarningLabel->setVisible(true);
        int lightness = ui->fallbackFeeWarningLabel->palette().color(QPalette::WindowText).lightness();
        QColor warning_colour(255 - (lightness / 5), 176 - (lightness / 3), 48 - (lightness / 14));
        ui->fallbackFeeWarningLabel->setStyleSheet("QLabel { color: " + warning_colour.name() + "; }");
#ifndef QTversionPreFiveEleven
        ui->fallbackFeeWarningLabel->setIndent(QFontMetrics(ui->fallbackFeeWarningLabel->font()).horizontalAdvance("x"));
#else
        ui->fallbackFeeWarningLabel->setIndent(QFontMetrics(ui->fallbackFeeWarningLabel->font()).width("x"));
#endif
    } else {
        ui->labelSmartFee2->hide();
        ui->labelFeeEstimation->setText(tr("Estimated to begin confirmation within %n block(s).", "", feeCalc.returnedTarget));
        ui->fallbackFeeWarningLabel->setVisible(false);
    }

    updateFeeMinimizedLabel();
}

// Coin Control: copy label "Quantity" to clipboard
void SendCoinsDialog::coinControlClipboardQuantity()
{
    GUIUtil::setClipboard(ui->labelCoinControlQuantity->text());
}

// Coin Control: copy label "Amount" to clipboard
void SendCoinsDialog::coinControlClipboardAmount()
{
    GUIUtil::setClipboard(ui->labelCoinControlAmount->text().left(ui->labelCoinControlAmount->text().indexOf(" ")));
}

// Coin Control: copy label "Fee" to clipboard
void SendCoinsDialog::coinControlClipboardFee()
{
    GUIUtil::setClipboard(ui->labelCoinControlFee->text().left(ui->labelCoinControlFee->text().indexOf(" ")).replace(ASYMP_UTF8, ""));
}

// Coin Control: copy label "After fee" to clipboard
void SendCoinsDialog::coinControlClipboardAfterFee()
{
    GUIUtil::setClipboard(ui->labelCoinControlAfterFee->text().left(ui->labelCoinControlAfterFee->text().indexOf(" ")).replace(ASYMP_UTF8, ""));
}

// Coin Control: copy label "Bytes" to clipboard
void SendCoinsDialog::coinControlClipboardBytes()
{
    GUIUtil::setClipboard(ui->labelCoinControlBytes->text().replace(ASYMP_UTF8, ""));
}

// Coin Control: copy label "Dust" to clipboard
void SendCoinsDialog::coinControlClipboardLowOutput()
{
    GUIUtil::setClipboard(ui->labelCoinControlLowOutput->text());
}

// Coin Control: copy label "Change" to clipboard
void SendCoinsDialog::coinControlClipboardChange()
{
    GUIUtil::setClipboard(ui->labelCoinControlChange->text().left(ui->labelCoinControlChange->text().indexOf(" ")).replace(ASYMP_UTF8, ""));
}

// Coin Control: settings menu - coin control enabled/disabled by user
void SendCoinsDialog::coinControlFeatureChanged(bool checked)
{
    ui->frameCoinControl->setVisible(checked);

    if (!checked && model) // coin control features disabled
        CoinControlDialog::coinControl->SetNull();

    coinControlUpdateLabels();
}

// Coin Control: button inputs -> show actual coin control dialog
void SendCoinsDialog::coinControlButtonClicked()
{
    CoinControlDialog dlg(platformStyle);
    dlg.setModel(model);
    dlg.exec();
    coinControlUpdateLabels();
}

// Coin Control: checkbox custom change address
void SendCoinsDialog::coinControlChangeChecked(int state)
{
    if (state == Qt::Unchecked) {
        CoinControlDialog::coinControl->destChange = CNoDestination();
        ui->labelCoinControlChangeLabel->clear();
    } else
        // use this to re-validate an already entered address
        coinControlChangeEdited(ui->lineEditCoinControlChange->text());

    ui->lineEditCoinControlChange->setEnabled((state == Qt::Checked));
}

// Coin Control: custom change address changed
void SendCoinsDialog::coinControlChangeEdited(const QString& text)
{
    if (model && model->getAddressTableModel()) {
        // Default to no change address until verified
        CoinControlDialog::coinControl->destChange = CNoDestination();
        ui->labelCoinControlChangeLabel->setStyleSheet("QLabel{color:red;}");

        const CTxDestination dest = DecodeDestination(text.toStdString());

        if (text.isEmpty()) // Nothing entered
        {
            ui->labelCoinControlChangeLabel->setText("");
        } else if (!IsValidDestination(dest)) // Invalid address
        {
            ui->labelCoinControlChangeLabel->setText(tr("Warning: Invalid Avian address"));
        } else // Valid address
        {
            if (!model->IsSpendable(dest)) {
                ui->labelCoinControlChangeLabel->setText(tr("Warning: Unknown change address"));

                // confirmation dialog
                QMessageBox::StandardButton btnRetVal = QMessageBox::question(this, tr("Confirm custom change address"), tr("The address you selected for change is not part of this wallet. Any or all funds in your wallet may be sent to this address. Are you sure?"),
                    QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);

                if (btnRetVal == QMessageBox::Yes)
                    CoinControlDialog::coinControl->destChange = dest;
                else {
                    ui->lineEditCoinControlChange->setText("");
                    // ui->labelCoinControlChangeLabel->setStyleSheet("QLabel{color:black;}");
                    ui->labelCoinControlChangeLabel->setText("");
                }
            } else // Known change address
            {
                // ui->labelCoinControlChangeLabel->setStyleSheet("QLabel{color:black;}");

                // Query label
                QString associatedLabel = model->getAddressTableModel()->labelForAddress(text);
                if (!associatedLabel.isEmpty())
                    ui->labelCoinControlChangeLabel->setText(associatedLabel);
                else
                    ui->labelCoinControlChangeLabel->setText(tr("(no label)"));

                CoinControlDialog::coinControl->destChange = dest;
            }
        }
    }
}

// Coin Control: update labels
void SendCoinsDialog::coinControlUpdateLabels()
{
    if (!model || !model->getOptionsModel())
        return;

    updateCoinControlState(*CoinControlDialog::coinControl);

    // set pay amounts
    CoinControlDialog::payAmounts.clear();
    CoinControlDialog::fSubtractFeeFromAmount = false;

    for (int i = 0; i < ui->entries->count(); ++i) {
        SendCoinsEntry* entry = qobject_cast<SendCoinsEntry*>(ui->entries->itemAt(i)->widget());
        if (entry && !entry->isHidden()) {
            SendCoinsRecipient rcp = entry->getValue();
            CoinControlDialog::payAmounts.append(rcp.amount);
            if (rcp.fSubtractFeeFromAmount)
                CoinControlDialog::fSubtractFeeFromAmount = true;
        }
    }

    if (CoinControlDialog::coinControl->HasSelected()) {
        // actual coin control calculation
        CoinControlDialog::updateLabels(model, this);

        // show coin control stats
        ui->labelCoinControlAutomaticallySelected->hide();
        ui->widgetCoinControl->show();
    } else {
        // hide coin control stats
        ui->labelCoinControlAutomaticallySelected->show();
        ui->widgetCoinControl->hide();
        ui->labelCoinControlInsuffFunds->hide();
    }
}

void SendCoinsDialog::presentPSBT(const PartiallySignedTransaction& psbt)
{
    try {
        // Serialize PSBT to raw bytes and base64
        CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
        ss << psbt;
        QByteArray raw(reinterpret_cast<const char*>(ss.data()), (int)ss.size());
        QString base64 = QString::fromStdString(EncodeBase64(std::string(ss.begin(), ss.end())));

        // Copy base64 to clipboard for convenience
        QApplication::clipboard()->setText(base64);

        // Short txid for suggested filename
        QString shortId;
        if (!psbt.tx.vin.empty()) {
            shortId = QString::fromStdString(psbt.tx.GetHash().ToString().substr(0, 12));
        } else {
            shortId = QStringLiteral("psbt");
        }

        QMessageBox box(QMessageBox::Information,
            tr("Unsigned Transaction"),
            tr("The PSBT has been copied to the clipboard as base64.\nSelect a format to save (binary recommended)."),
            QMessageBox::NoButton, this);
        QPushButton* saveBinaryBtn = box.addButton(tr("Save Binary .psbt"), QMessageBox::AcceptRole);
        QPushButton* saveBase64Btn = box.addButton(tr("Save Base64 .psbt.txt"), QMessageBox::ActionRole);
        QPushButton* discardBtn = box.addButton(tr("Discard"), QMessageBox::RejectRole);
        box.exec();

        auto saveToFile = [&](const QString& fileName, const QByteArray& contents) {
            QFile f(fileName);
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                f.write(contents);
                f.close();
            } else {
                QMessageBox::warning(this, tr("Save Failed"), tr("Could not open file for writing: %1").arg(fileName));
            }
        };

        if (box.clickedButton() == saveBinaryBtn) {
            QString suggested = QString("unsigned-%1.psbt").arg(shortId);
            QString fileName = QFileDialog::getSaveFileName(this, tr("Save Binary PSBT"), suggested, tr("PSBT Binary (*.psbt);;All Files (*)"));
            if (!fileName.isEmpty()) {
                saveToFile(fileName, raw);
            }
        } else if (box.clickedButton() == saveBase64Btn) {
            QString suggested = QString("unsigned-%1.psbt.txt").arg(shortId);
            QString fileName = QFileDialog::getSaveFileName(this, tr("Save Base64 PSBT"), suggested, tr("Text PSBT (*.psbt.txt);;All Files (*)"));
            if (!fileName.isEmpty()) {
                saveToFile(fileName, base64.toUtf8());
            }
        }
    } catch (const std::exception& e) {
        QMessageBox::warning(this, tr("Error"), tr("Failed to create PSBT: %1").arg(QString::fromStdString(e.what())));
    } catch (...) {
        QMessageBox::warning(this, tr("Error"), tr("Unknown error occurred while creating PSBT"));
    }
}

SendConfirmationDialog::SendConfirmationDialog(const QString& title, const QString& text, int _secDelay, QWidget* parent) : QMessageBox(QMessageBox::Question, title, text, QMessageBox::Yes | QMessageBox::Cancel | QMessageBox::Save, parent), secDelay(_secDelay)
{
    setDefaultButton(QMessageBox::Cancel);
    yesButton = button(QMessageBox::Yes);
    psbtButton = button(QMessageBox::Save);
    // Rename the standard Save button to our specific action text
    psbtButton->setText(tr("Create Unsigned PSBT"));

    // Disable both action buttons until countdown completes
    yesButton->setEnabled(false);
    psbtButton->setEnabled(false);

    updateYesButton();
    connect(&countDownTimer, SIGNAL(timeout()), this, SLOT(countDown()));
    countDownTimer.setSingleShot(false);
}

int SendConfirmationDialog::exec()
{
    // Start countdown timer each time exec is invoked
    updateYesButton();
    countDownTimer.start(1000);
    int result = QMessageBox::exec();
    countDownTimer.stop();
    return result; // Caller differentiates with QMessageBox::Save for PSBT creation
}

void SendConfirmationDialog::countDown()
{
    secDelay--;
    updateYesButton();

    if (secDelay <= 0) {
        countDownTimer.stop();
    }
}

void SendConfirmationDialog::updateYesButton()
{
    if (secDelay > 0) {
        yesButton->setEnabled(false);
        yesButton->setText(tr("Yes") + " (" + QString::number(secDelay) + ")");
        psbtButton->setEnabled(false);
        psbtButton->setText(tr("Create Unsigned PSBT") + " (" + QString::number(secDelay) + ")");
    } else {
        yesButton->setEnabled(true);
        yesButton->setText(tr("Yes"));
        psbtButton->setEnabled(true);
        psbtButton->setText(tr("Create Unsigned PSBT"));
    }
}
