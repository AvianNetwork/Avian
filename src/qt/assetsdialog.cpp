// Copyright (c) 2011-2016 The Bitcoin Core developers
// Copyright (c) 2017-2019 The Raven Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/assetsdialog.h>
#include <qt/sendcoinsdialog.h>
#include "ui_assetsdialog.h"

#include <qt/addresstablemodel.h>
#include <qt/bitcoinunits.h>
#include <qt/clientmodel.h>
#include <interfaces/node.h>
#include <qt/assetcontroldialog.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <qt/sendassetsentry.h>
#include <qt/walletmodel.h>
#include <qt/assettablemodel.h>

#include <assets/assets.h>
#include <core_io.h>
#include <key_io.h>
#include <kernel/chainparams.h>
#include <wallet/coincontrol.h>
#include <wallet/wallet.h>
#include <wallet/spend.h>
#include <wallet/asset_tx.h>
#include <psbt.h>
#include <policy/fees.h>
#include <validation.h>
#include <node/interface_ui.h>
#include <qt/createassetdialog.h>
#include <qt/reissueassetdialog.h>
#include <qt/guiconstants.h>
#include <util/strencodings.h>

#include <QGraphicsDropShadowEffect>
#include <QFontMetrics>
#include <QMessageBox>
#include <QScrollBar>
#include <QSettings>
#include <QTextDocument>
#include <QTimer>

#include <fstream>

// Local conf target helpers (rule 12)
static const int confTargets[] = {2, 4, 6, 12, 24, 48};
static int getConfTargetForIndex(int index) {
    if (index+1 > static_cast<int>(sizeof(confTargets)/sizeof(confTargets[0])))
        return confTargets[sizeof(confTargets)/sizeof(confTargets[0]) - 1];
    if (index < 0) return confTargets[0];
    return confTargets[index];
}
static int getIndexForConfTarget(int target) {
    for (unsigned int i = 0; i < sizeof(confTargets)/sizeof(confTargets[0]); i++)
        if (confTargets[i] >= target) return i;
    return sizeof(confTargets)/sizeof(confTargets[0]) - 1;
}

AssetsDialog::AssetsDialog(const PlatformStyle *_platformStyle, QWidget *parent) :
        QDialog(parent),
        ui(new Ui::AssetsDialog),
        clientModel(0),
        model(0),
        fNewRecipientAllowed(true),
        fFeeMinimized(true),
        platformStyle(_platformStyle)
{
    ui->setupUi(this);

    if (!_platformStyle->getImagesOnButtons()) {
        ui->addButton->setIcon(QIcon());
        ui->clearButton->setIcon(QIcon());
        ui->sendButton->setIcon(QIcon());
    } else {
        ui->addButton->setIcon(_platformStyle->SingleColorIcon(":/icons/add"));
        ui->clearButton->setIcon(_platformStyle->SingleColorIcon(":/icons/remove"));
        ui->sendButton->setIcon(_platformStyle->SingleColorIcon(":/icons/send"));
    }

    GUIUtil::setupAddressWidget(ui->lineEditAssetControlChange, this);

    addEntry();

    connect(ui->addButton, SIGNAL(clicked()), this, SLOT(addEntry()));
    connect(ui->clearButton, SIGNAL(clicked()), this, SLOT(clear()));

    // Coin Control
    connect(ui->pushButtonAssetControl, SIGNAL(clicked()), this, SLOT(assetControlButtonClicked()));
    connect(ui->checkBoxAssetControlChange, SIGNAL(stateChanged(int)), this, SLOT(assetControlChangeChecked(int)));
    connect(ui->lineEditAssetControlChange, SIGNAL(textEdited(const QString &)), this, SLOT(assetControlChangeEdited(const QString &)));

    // Coin Control: clipboard actions
    QAction *clipboardQuantityAction = new QAction(tr("Copy quantity"), this);
    QAction *clipboardAmountAction = new QAction(tr("Copy amount"), this);
    QAction *clipboardFeeAction = new QAction(tr("Copy fee"), this);
    QAction *clipboardAfterFeeAction = new QAction(tr("Copy after fee"), this);
    QAction *clipboardBytesAction = new QAction(tr("Copy bytes"), this);
    QAction *clipboardLowOutputAction = new QAction(tr("Copy dust"), this);
    QAction *clipboardChangeAction = new QAction(tr("Copy change"), this);
    connect(clipboardQuantityAction, SIGNAL(triggered()), this, SLOT(assetControlClipboardQuantity()));
    connect(clipboardAmountAction, SIGNAL(triggered()), this, SLOT(assetControlClipboardAmount()));
    connect(clipboardFeeAction, SIGNAL(triggered()), this, SLOT(assetControlClipboardFee()));
    connect(clipboardAfterFeeAction, SIGNAL(triggered()), this, SLOT(assetControlClipboardAfterFee()));
    connect(clipboardBytesAction, SIGNAL(triggered()), this, SLOT(assetControlClipboardBytes()));
    connect(clipboardLowOutputAction, SIGNAL(triggered()), this, SLOT(assetControlClipboardLowOutput()));
    connect(clipboardChangeAction, SIGNAL(triggered()), this, SLOT(assetControlClipboardChange()));
    ui->labelAssetControlQuantity->addAction(clipboardQuantityAction);
    ui->labelAssetControlAmount->addAction(clipboardAmountAction);
    ui->labelAssetControlFee->addAction(clipboardFeeAction);
    ui->labelAssetControlAfterFee->addAction(clipboardAfterFeeAction);
    ui->labelAssetControlBytes->addAction(clipboardBytesAction);
    ui->labelAssetControlLowOutput->addAction(clipboardLowOutputAction);
    ui->labelAssetControlChange->addAction(clipboardChangeAction);

    // init transaction fee section
    QSettings settings;
    if (!settings.contains("fFeeSectionMinimized"))
        settings.setValue("fFeeSectionMinimized", true);
    if (!settings.contains("nFeeRadio") && settings.contains("nTransactionFee") && settings.value("nTransactionFee").toLongLong() > 0) // compatibility
        settings.setValue("nFeeRadio", 1); // custom
    if (!settings.contains("nFeeRadio"))
        settings.setValue("nFeeRadio", 0); // recommended
    if (!settings.contains("nSmartFeeSliderPosition"))
        settings.setValue("nSmartFeeSliderPosition", 0);
    if (!settings.contains("nTransactionFee"))
        settings.setValue("nTransactionFee", (qint64)wallet::DEFAULT_PAY_TX_FEE);
    if (!settings.contains("fPayOnlyMinFee"))
        settings.setValue("fPayOnlyMinFee", false);
    ui->groupFee->setId(ui->radioSmartFee, 0);
    ui->groupFee->setId(ui->radioCustomFee, 1);
    ui->groupFee->button((int)std::max(0, std::min(1, settings.value("nFeeRadio").toInt())))->setChecked(true);
    ui->customFee->setValue(settings.value("nTransactionFee").toLongLong());
    ui->checkBoxMinimumFee->setChecked(settings.value("fPayOnlyMinFee").toBool());
    minimizeFeeSection(settings.value("fFeeSectionMinimized").toBool());

    /** AVN START */
    setupAssetControlFrame(platformStyle);
    setupScrollView(platformStyle);
    setupFeeControl(platformStyle);
    /** AVN END */
}

void AssetsDialog::setClientModel(ClientModel *_clientModel)
{
    this->clientModel = _clientModel;

    if (_clientModel) {
        connect(_clientModel, SIGNAL(numBlocksChanged(int,QDateTime,double,bool)), this, SLOT(updateSmartFeeLabel()));
    }
}

void AssetsDialog::setModel(WalletModel *_model)
{
    this->model = _model;

    if(_model && _model->getOptionsModel())
    {
        for(int i = 0; i < ui->entries->count(); ++i)
        {
            SendAssetsEntry *entry = qobject_cast<SendAssetsEntry*>(ui->entries->itemAt(i)->widget());
            if(entry)
            {
                entry->setModel(_model);
            }
        }

        // Rule 8: use getCachedBalance() instead of individual getBalance()/etc.
        setBalance(_model->getCachedBalance());
        // Rule 7: new-style connect for balanceChanged
        connect(_model, &WalletModel::balanceChanged, this, &AssetsDialog::setBalance);
        connect(_model->getOptionsModel(), &OptionsModel::displayUnitChanged, this, &AssetsDialog::updateDisplayUnit);
        updateDisplayUnit();

        // Coin Control
        connect(_model->getOptionsModel(), &OptionsModel::displayUnitChanged, this, &AssetsDialog::assetControlUpdateLabels);
        connect(_model->getOptionsModel(), &OptionsModel::coinControlFeaturesChanged, this, &AssetsDialog::assetControlFeatureChanged);

        // Note: customFeeFeaturesChanged signal was removed; fee frame visibility is
        // unconditionally enabled below, so no connection is needed here.


        ui->frameAssetControl->setVisible(false);
        ui->frameAssetControl->setVisible(_model->getOptionsModel()->getCoinControlFeatures());
        ui->frameFee->setVisible(true);
        assetControlUpdateLabels();

        // fee section
        for (const int &n : confTargets) {
            ui->confTargetSelector->addItem(tr("%1 (%2 blocks)").arg(GUIUtil::formatNiceTimeOffset(n * Params().GetConsensus().nPowTargetSpacing)).arg(n));
        }
        connect(ui->confTargetSelector, SIGNAL(currentIndexChanged(int)), this, SLOT(updateSmartFeeLabel()));
        connect(ui->confTargetSelector, SIGNAL(currentIndexChanged(int)), this, SLOT(assetControlUpdateLabels()));
        // Rule 25: always use Qt5/6 code path
        connect(ui->groupFee, &QButtonGroup::idClicked, this, &AssetsDialog::updateFeeSectionControls);
        connect(ui->customFee, SIGNAL(valueChanged()), this, SLOT(assetControlUpdateLabels()));
        connect(ui->checkBoxMinimumFee, SIGNAL(stateChanged(int)), this, SLOT(setMinimumFee()));
        connect(ui->checkBoxMinimumFee, SIGNAL(stateChanged(int)), this, SLOT(updateFeeSectionControls()));
        connect(ui->checkBoxMinimumFee, SIGNAL(stateChanged(int)), this, SLOT(assetControlUpdateLabels()));
        CAmount requiredFee = model->wallet().getRequiredFee(1000);
        ui->customFee->setSingleStep(requiredFee);
        updateFeeSectionControls();
        updateMinFeeLabel();
        updateSmartFeeLabel();

        // set default rbf checkbox state
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
        // Rule 24: model->getDefaultConfirmTarget() does not exist, use 6
        if (settings.value("nConfTarget").toInt() == 0)
            ui->confTargetSelector->setCurrentIndex(getIndexForConfTarget(6));
        else
            ui->confTargetSelector->setCurrentIndex(getIndexForConfTarget(settings.value("nConfTarget").toInt()));
    }
}

AssetsDialog::~AssetsDialog()
{
    QSettings settings;
    settings.setValue("fFeeSectionMinimized", fFeeMinimized);
    settings.setValue("nFeeRadio", ui->groupFee->checkedId());
    settings.setValue("nConfTarget", getConfTargetForIndex(ui->confTargetSelector->currentIndex()));
    settings.setValue("nTransactionFee", (qint64)ui->customFee->value());
    settings.setValue("fPayOnlyMinFee", ui->checkBoxMinimumFee->isChecked());

    delete ui;
}

void AssetsDialog::setupAssetControlFrame(const PlatformStyle *platformStyle)
{
    /** Create the shadow effects on the frames */
    ui->frameAssetControl->setGraphicsEffect(GUIUtil::getShadowEffect());
}

void AssetsDialog::setupScrollView(const PlatformStyle *platformStyle)
{
    /** Update the scrollview*/
    ui->scrollArea->setGraphicsEffect(GUIUtil::getShadowEffect());

    // Add some spacing so we can see the whole card
    ui->entries->setContentsMargins(10,10,20,0);
}

void AssetsDialog::setupFeeControl(const PlatformStyle *platformStyle)
{
    /** Create the shadow effects on the frames */
    ui->frameFee->setStyleSheet(QString(".QFrame#frameFee { border-top: 2px solid %1;padding-top: 20px;}").arg(platformStyle->Avian_2B737F().name()));
}

void AssetsDialog::on_sendButton_clicked()
{
    if(!model || !model->getOptionsModel())
        return;

    QList<SendAssetsRecipient> recipients;
    bool valid = true;

    for(int i = 0; i < ui->entries->count(); ++i)
    {
        SendAssetsEntry *entry = qobject_cast<SendAssetsEntry*>(ui->entries->itemAt(i)->widget());
        if(entry)
        {
            if(entry->validate())
            {
                recipients.append(entry->getValue());
            }
            else
            {
                valid = false;
            }
        }
    }

    if(!valid || recipients.isEmpty())
    {
        return;
    }

    fNewRecipientAllowed = false;
    WalletModel::UnlockContext ctx(model->requestUnlock());
    if(!ctx.isValid())
    {
        // Unlock wallet was cancelled
        fNewRecipientAllowed = true;
        return;
    }

    std::vector< std::pair<CAssetTransfer, std::string> >vTransfers;

    for (auto recipient : recipients) {
        vTransfers.emplace_back(std::make_pair(CAssetTransfer(recipient.assetName.toStdString(), recipient.amount, DecodeAssetData(recipient.message.toStdString()), 0), recipient.address.toStdString()));
    }

    // Always use a CCoinControl instance, use the AssetControlDialog instance if CoinControl has been enabled
    // Rule 2: wallet::CCoinControl
    wallet::CCoinControl ctrl;
    if (model->getOptionsModel()->getCoinControlFeatures())
        ctrl = *AssetControlDialog::assetControl();

    updateAssetControlState(ctrl);

    if (clientModel && clientModel->node().isInitialBlockDownload()) {
        GUIUtil::SyncWarningMessage syncWarning(this);
        if (!syncWarning.showTransactionSyncWarningMessage())
            return;
    }

    // Create the asset transfer transaction via the wallet
    CTransactionRef txRef;
    CAmount nFeeRequired = 0;
    std::pair<int, std::string> error;

    wallet::CWallet* pwallet = model->wallet().wallet();
    if (!pwallet) {
        QMessageBox::critical(this, tr("Error"), tr("Wallet not available."));
        fNewRecipientAllowed = true;
        return;
    }

    std::string changeAddress = "";
    if (ctrl.HasSelected() && ctrl.destChange.index() != 0) {
        changeAddress = EncodeDestination(ctrl.destChange);
    }

    {
        LOCK(pwallet->cs_wallet);
        if (!wallet::CreateTransferAssetTransaction(*pwallet, ctrl, vTransfers, changeAddress, error, txRef, nFeeRequired)) {
            QMessageBox::critical(this, tr("Error Creating Transaction"),
                tr("Error: ") + QString::fromStdString(error.second));
            fNewRecipientAllowed = true;
            return;
        }
    }

    // Format confirmation message
    QStringList formatted;
    for (SendAssetsRecipient &rcp : recipients)
    {
        // generate bold amount string
        QString amount = "<b>" + QString::fromStdString(ValueFromAmountString(rcp.amount, 8)) + " " + rcp.assetName;
        amount.append("</b>");
        // generate monospace address string
        QString address = "<span style='font-family: monospace;'>" + rcp.address;
        address.append("</span>");

        QString recipientElement;

        if (!rcp.paymentRequest.IsInitialized()) // normal payment
        {
            if(rcp.label.length() > 0) // label with address
            {
                recipientElement = tr("%1 to %2").arg(amount, GUIUtil::HtmlEscape(rcp.label));
                recipientElement.append(QString(" (%1)").arg(address));
            }
            else // just address
            {
                recipientElement = tr("%1 to %2").arg(amount, address);
            }
        }
        else if(!rcp.authenticatedMerchant.isEmpty()) // authenticated payment request
        {
            recipientElement = tr("%1 to %2").arg(amount, GUIUtil::HtmlEscape(rcp.authenticatedMerchant));
        }
        else // unauthenticated payment request
        {
            recipientElement = tr("%1 to %2").arg(amount, address);
        }

        formatted.append(recipientElement);
    }

    QString questionString = tr("Are you sure you want to send?");
    questionString.append("<br /><br />%1");

    if(nFeeRequired > 0)
    {
        // append fee string if a fee is required
        questionString.append("<hr /><span style='color:#aa0000;'>");
        questionString.append(BitcoinUnits::formatHtmlWithUnit(model->getOptionsModel()->getDisplayUnit(), nFeeRequired));
        questionString.append("</span> ");
        questionString.append(tr("added as transaction fee"));
    }

    SendConfirmationDialog confirmationDialog(tr("Confirm send assets"),
                                              questionString.arg(formatted.join("<br />")), "", "", SEND_CONFIRM_DELAY, true, true, this);
    confirmationDialog.exec();
    QMessageBox::StandardButton retval = (QMessageBox::StandardButton)confirmationDialog.result();

    if(retval != QMessageBox::Yes && retval != QMessageBox::Save)
    {
        fNewRecipientAllowed = true;
        return;
    }

    if (retval == QMessageBox::Save) {
        // "Create Unsigned" clicked — export as PSBT
        CMutableTransaction mtx = CMutableTransaction{*txRef};
        // Strip scriptSigs and scriptWitnesses — PSBT format requires unsigned tx
        for (CTxIn& txin : mtx.vin) {
            txin.scriptSig.clear();
            txin.scriptWitness.SetNull();
        }
        PartiallySignedTransaction psbtx(mtx);
        bool complete = false;
        const auto err{model->wallet().fillPSBT(std::nullopt, /*sign=*/false, /*bip32derivs=*/true, /*n_signed=*/nullptr, psbtx, complete)};
        if (err) {
            QMessageBox::critical(this, tr("Error"), tr("Failed to create PSBT"));
            fNewRecipientAllowed = true;
            return;
        }
        presentPSBT(psbtx);
    } else {
        // "Send" clicked — broadcast directly
        LOCK(pwallet->cs_wallet);
        std::string txid;
        if (!wallet::SendAssetTransaction(*pwallet, txRef, error, txid)) {
            QMessageBox::critical(this, tr("Error Sending Transaction"),
                tr("Error: ") + QString::fromStdString(error.second));
        } else {
            Q_EMIT message(tr("Send Confirmed"), tr("Asset transaction sent successfully. TXID: %1").arg(QString::fromStdString(txid)),
                CClientUIInterface::MSG_INFORMATION);
            clear();
        }
    }

    fNewRecipientAllowed = true;
}

void AssetsDialog::presentPSBT(PartiallySignedTransaction& psbtx)
{
    // Serialize the PSBT
    DataStream ssTx{};
    ssTx << psbtx;
    GUIUtil::setClipboard(EncodeBase64(ssTx.str()).c_str());
    QMessageBox msgBox(this);
    msgBox.setText(tr("Unsigned Transaction", "PSBT copied"));
    msgBox.setInformativeText(tr("The PSBT has been copied to the clipboard. You can also save it."));
    msgBox.setStandardButtons(QMessageBox::Save | QMessageBox::Discard);
    msgBox.setDefaultButton(QMessageBox::Discard);
    switch (msgBox.exec()) {
    case QMessageBox::Save: {
        QString selectedFilter;
        QString filename = GUIUtil::getSaveFileName(this,
            tr("Save Transaction Data"), "",
            tr("Partially Signed Transaction (Binary)") + QLatin1String(" (*.psbt)"), &selectedFilter);
        if (!filename.isEmpty()) {
            std::ofstream out{filename.toLocal8Bit().data(), std::ofstream::out | std::ofstream::binary};
            out << ssTx.str();
            out.close();
        }
        break;
    }
    case QMessageBox::Discard:
        break;
    }
}

void AssetsDialog::clear()
{
    // Remove entries until only one left
    while(ui->entries->count())
    {
        ui->entries->takeAt(0)->widget()->deleteLater();
    }
    addEntry();

    updateTabsAndLabels();
}

void AssetsDialog::reject()
{
    clear();
}

void AssetsDialog::accept()
{
    clear();
}

SendAssetsEntry *AssetsDialog::addEntry()
{
    QStringList list;
    bool fIsOwner = false;
    bool fIsAssetControl = false;
    if (AssetControlDialog::assetControl()->HasAssetSelected()) {
        list << QString::fromStdString(AssetControlDialog::assetControl()->strAssetSelected);
        fIsOwner = IsAssetNameAnOwner(AssetControlDialog::assetControl()->strAssetSelected);
        fIsAssetControl = true;
    } else if (model) {
        wallet::CWallet* pwallet = model->wallet().wallet();
        if (pwallet) {
            LOCK(pwallet->cs_wallet);
            wallet::CoinFilterParams params;
            params.min_amount = 0;
            wallet::CoinsResult available = wallet::AvailableCoinsWithAssets(*pwallet, nullptr, std::nullopt, params);
            for (const auto& [assetName, assetOutputs] : available.mapAssetCoins) {
                if (!IsAssetNameAnOwner(assetName))
                    list << QString::fromStdString(assetName);
            }
        }
    }

    SendAssetsEntry *entry = new SendAssetsEntry(platformStyle, list, this);
    entry->setModel(model);
    ui->entries->addWidget(entry);
    connect(entry, SIGNAL(removeEntry(SendAssetsEntry*)), this, SLOT(removeEntry(SendAssetsEntry*)));
    connect(entry, SIGNAL(payAmountChanged()), this, SLOT(assetControlUpdateLabels()));
    connect(entry, SIGNAL(subtractFeeFromAmountChanged()), this, SLOT(assetControlUpdateLabels()));

    // Focus the field, so that entry can start immediately
    entry->clear();
    entry->setFocusAssetListBox();
    ui->scrollAreaWidgetContents->resize(ui->scrollAreaWidgetContents->sizeHint());
    qApp->processEvents();
    QScrollBar* bar = ui->scrollArea->verticalScrollBar();
    if(bar)
        bar->setSliderPosition(bar->maximum());

    entry->IsAssetControl(fIsAssetControl, fIsOwner);

    if (list.size() == 1)
        entry->setCurrentIndex(1);

    updateTabsAndLabels();

    return entry;
}

void AssetsDialog::updateTabsAndLabels()
{
    setupTabChain(0);
    assetControlUpdateLabels();
}

void AssetsDialog::removeEntry(SendAssetsEntry* entry)
{
    entry->hide();

    // If the last entry is about to be removed add an empty one
    if (ui->entries->count() == 1)
        addEntry();

    entry->deleteLater();

    updateTabsAndLabels();
}

QWidget *AssetsDialog::setupTabChain(QWidget *prev)
{
    for(int i = 0; i < ui->entries->count(); ++i)
    {
        SendAssetsEntry *entry = qobject_cast<SendAssetsEntry*>(ui->entries->itemAt(i)->widget());
        if(entry)
        {
            prev = entry->setupTabChain(prev);
        }
    }
    QWidget::setTabOrder(prev, ui->sendButton);
    QWidget::setTabOrder(ui->sendButton, ui->clearButton);
    QWidget::setTabOrder(ui->clearButton, ui->addButton);
    return ui->addButton;
}

void AssetsDialog::setAddress(const QString &address)
{
    SendAssetsEntry *entry = 0;
    // Replace the first entry if it is still unused
    if(ui->entries->count() == 1)
    {
        SendAssetsEntry *first = qobject_cast<SendAssetsEntry*>(ui->entries->itemAt(0)->widget());
        if(first->isClear())
        {
            entry = first;
        }
    }
    if(!entry)
    {
        entry = addEntry();
    }

    entry->setAddress(address);
}

void AssetsDialog::pasteEntry(const SendAssetsRecipient &rv)
{
    if(!fNewRecipientAllowed)
        return;

    SendAssetsEntry *entry = 0;
    // Replace the first entry if it is still unused
    if(ui->entries->count() == 1)
    {
        SendAssetsEntry *first = qobject_cast<SendAssetsEntry*>(ui->entries->itemAt(0)->widget());
        if(first->isClear())
        {
            entry = first;
        }
    }
    if(!entry)
    {
        entry = addEntry();
    }

    entry->setValue(rv);
    updateTabsAndLabels();
}

bool AssetsDialog::handlePaymentRequest(const SendAssetsRecipient &rv)
{
    // Just paste the entry, all pre-checks
    // are done in paymentserver.cpp.
    pasteEntry(rv);
    return true;
}

// Rule 6: setBalance signature changed to interfaces::WalletBalances
void AssetsDialog::setBalance(const interfaces::WalletBalances& balances)
{
    if(model && model->getOptionsModel())
    {
        ui->labelBalance->setText(BitcoinUnits::formatWithUnit(model->getOptionsModel()->getDisplayUnit(), balances.balance));
    }
}

void AssetsDialog::updateDisplayUnit()
{
    // Rule 8: use getCachedBalance()
    setBalance(model->getCachedBalance());
    ui->customFee->setDisplayUnit(model->getOptionsModel()->getDisplayUnit());
    updateMinFeeLabel();
    updateSmartFeeLabel();
}

void AssetsDialog::processSendCoinsReturn(const WalletModel::SendCoinsReturn &sendCoinsReturn, const QString &msgArg)
{
    QPair<QString, CClientUIInterface::MessageBoxFlags> msgParams;
    // Default to a warning message, override if error message is needed
    msgParams.second = CClientUIInterface::MSG_WARNING;

    // This comment is specific to SendCoinsDialog usage of WalletModel::SendCoinsReturn.
    // WalletModel::TransactionCommitFailed is used only in WalletModel::sendCoins()
    // all others are used only in WalletModel::prepareTransaction()
    switch(sendCoinsReturn.status)
    {
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
        case WalletModel::AbsurdFee:
            msgParams.first = tr("A fee higher than %1 is considered an absurdly high fee.").arg(BitcoinUnits::formatWithUnit(model->getOptionsModel()->getDisplayUnit(), model->wallet().getDefaultMaxTxFee()));
            break;
            // included to prevent a compiler warning.
        case WalletModel::OK:
        default:
            return;
    }

    Q_EMIT message(tr("Send Coins"), msgParams.first, msgParams.second);
}

void AssetsDialog::minimizeFeeSection(bool fMinimize)
{
    ui->labelFeeMinimized->setVisible(fMinimize);
    ui->buttonChooseFee  ->setVisible(fMinimize);
    ui->buttonMinimizeFee->setVisible(!fMinimize);
    ui->frameFeeSelection->setVisible(!fMinimize);
    ui->horizontalLayoutSmartFee->setContentsMargins(0, (fMinimize ? 0 : 6), 0, 0);
    fFeeMinimized = fMinimize;
}

void AssetsDialog::on_buttonChooseFee_clicked()
{
    minimizeFeeSection(false);
}

void AssetsDialog::on_buttonMinimizeFee_clicked()
{
    updateFeeMinimizedLabel();
    minimizeFeeSection(true);
}

void AssetsDialog::setMinimumFee()
{
    ui->customFee->setValue(model->wallet().getRequiredFee(1000));
}

void AssetsDialog::updateFeeSectionControls()
{
    ui->confTargetSelector      ->setEnabled(ui->radioSmartFee->isChecked());
    ui->labelSmartFee           ->setEnabled(ui->radioSmartFee->isChecked());
    ui->labelSmartFee2          ->setEnabled(ui->radioSmartFee->isChecked());
    ui->labelSmartFee3          ->setEnabled(ui->radioSmartFee->isChecked());
    ui->labelFeeEstimation      ->setEnabled(ui->radioSmartFee->isChecked());
    ui->checkBoxMinimumFee      ->setEnabled(ui->radioCustomFee->isChecked());
    ui->labelMinFeeWarning      ->setEnabled(ui->radioCustomFee->isChecked());
    ui->labelCustomPerKilobyte  ->setEnabled(ui->radioCustomFee->isChecked() && !ui->checkBoxMinimumFee->isChecked());
    ui->customFee               ->setEnabled(ui->radioCustomFee->isChecked() && !ui->checkBoxMinimumFee->isChecked());
}

void AssetsDialog::updateFeeMinimizedLabel()
{
    if(!model || !model->getOptionsModel())
        return;

    if (ui->radioSmartFee->isChecked())
        ui->labelFeeMinimized->setText(ui->labelSmartFee->text());
    else {
        ui->labelFeeMinimized->setText(BitcoinUnits::formatWithUnit(model->getOptionsModel()->getDisplayUnit(), ui->customFee->value()) + "/kB");
    }
}

void AssetsDialog::updateMinFeeLabel()
{
    if (model && model->getOptionsModel())
        ui->checkBoxMinimumFee->setText(tr("Pay only the required fee of %1").arg(
                BitcoinUnits::formatWithUnit(model->getOptionsModel()->getDisplayUnit(), model->wallet().getRequiredFee(1000)) + "/kB")
        );
}

// Rule 21: signature matches header: wallet::CCoinControl&
void AssetsDialog::updateAssetControlState(wallet::CCoinControl& ctrl)
{
    if (ui->radioCustomFee->isChecked()) {
        ctrl.m_feerate = CFeeRate(ui->customFee->value());
    } else {
        ctrl.m_feerate.reset();
    }
    // Avoid using global defaults when sending money from the GUI
    // Either custom fee will be used or if not selected, the confirmation target from dropdown box
    ctrl.m_confirm_target = getConfTargetForIndex(ui->confTargetSelector->currentIndex());
}

void AssetsDialog::updateSmartFeeLabel()
{
    if(!model || !model->getOptionsModel())
        return;
    wallet::CCoinControl coin_control;
    updateAssetControlState(coin_control);
    coin_control.m_feerate.reset(); // Explicitly use only fee estimation rate for smart fee labels

    int returned_target;
    FeeReason reason;
    CFeeRate feeRate = CFeeRate(model->wallet().getMinimumFee(1000, coin_control, &returned_target, &reason));

    ui->labelSmartFee->setText(BitcoinUnits::formatWithUnit(model->getOptionsModel()->getDisplayUnit(), feeRate.GetFeePerK()) + "/kB");

    if (reason == FeeReason::FALLBACK) {
        ui->labelSmartFee2->show();
        ui->labelFeeEstimation->setText("");
        ui->fallbackFeeWarningLabel->setVisible(true);
        int lightness = ui->fallbackFeeWarningLabel->palette().color(QPalette::WindowText).lightness();
        QColor warning_colour(255 - (lightness / 5), 176 - (lightness / 3), 48 - (lightness / 14));
        ui->fallbackFeeWarningLabel->setStyleSheet("QLabel { color: " + warning_colour.name() + "; }");
        ui->fallbackFeeWarningLabel->setIndent(GUIUtil::TextWidth(QFontMetrics(ui->fallbackFeeWarningLabel->font()), "x"));
    } else {
        ui->labelSmartFee2->hide();
        ui->labelFeeEstimation->setText(tr("Estimated to begin confirmation within %n block(s).", "", returned_target));
        ui->fallbackFeeWarningLabel->setVisible(false);
    }

    updateFeeMinimizedLabel();
}

// Coin Control: copy label "Quantity" to clipboard
void AssetsDialog::assetControlClipboardQuantity()
{
    GUIUtil::setClipboard(ui->labelAssetControlQuantity->text());
}

// Coin Control: copy label "Amount" to clipboard
void AssetsDialog::assetControlClipboardAmount()
{
    GUIUtil::setClipboard(ui->labelAssetControlAmount->text().left(ui->labelAssetControlAmount->text().indexOf(" ")));
}

// Coin Control: copy label "Fee" to clipboard
void AssetsDialog::assetControlClipboardFee()
{
    GUIUtil::setClipboard(ui->labelAssetControlFee->text().left(ui->labelAssetControlFee->text().indexOf(" ")).replace(ASYMP_UTF8, ""));
}

// Coin Control: copy label "After fee" to clipboard
void AssetsDialog::assetControlClipboardAfterFee()
{
    GUIUtil::setClipboard(ui->labelAssetControlAfterFee->text().left(ui->labelAssetControlAfterFee->text().indexOf(" ")).replace(ASYMP_UTF8, ""));
}

// Coin Control: copy label "Bytes" to clipboard
void AssetsDialog::assetControlClipboardBytes()
{
    GUIUtil::setClipboard(ui->labelAssetControlBytes->text().replace(ASYMP_UTF8, ""));
}

// Coin Control: copy label "Dust" to clipboard
void AssetsDialog::assetControlClipboardLowOutput()
{
    GUIUtil::setClipboard(ui->labelAssetControlLowOutput->text());
}

// Coin Control: copy label "Change" to clipboard
void AssetsDialog::assetControlClipboardChange()
{
    GUIUtil::setClipboard(ui->labelAssetControlChange->text().left(ui->labelAssetControlChange->text().indexOf(" ")).replace(ASYMP_UTF8, ""));
}

// Coin Control: settings menu - coin control enabled/disabled by user
void AssetsDialog::assetControlFeatureChanged(bool checked)
{
    ui->frameAssetControl->setVisible(checked);

    if (!checked && model) // coin control features disabled
        *AssetControlDialog::assetControl() = wallet::CCoinControl();

    assetControlUpdateLabels();
}

void AssetsDialog::customFeeFeatureChanged(bool checked)
{
    ui->frameFee->setVisible(checked);
}

// Coin Control: button inputs -> show actual coin control dialog
void AssetsDialog::assetControlButtonClicked()
{
    AssetControlDialog dlg(platformStyle);
    dlg.setModel(model);
    dlg.exec();
    assetControlUpdateLabels();
    assetControlUpdateSendCoinsDialog();
}

// Coin Control: checkbox custom change address
void AssetsDialog::assetControlChangeChecked(int state)
{
    if (state == Qt::Unchecked)
    {
        AssetControlDialog::assetControl()->destChange = CNoDestination();
        ui->labelAssetControlChangeLabel->clear();
    }
    else
        // use this to re-validate an already entered address
        assetControlChangeEdited(ui->lineEditAssetControlChange->text());

    ui->lineEditAssetControlChange->setEnabled((state == Qt::Checked));
}

// Coin Control: custom change address changed
void AssetsDialog::assetControlChangeEdited(const QString& text)
{
    if (model && model->getAddressTableModel())
    {
        // Default to no change address until verified
        AssetControlDialog::assetControl()->destChange = CNoDestination();
        ui->labelAssetControlChangeLabel->setStyleSheet("QLabel{color:red;}");

        const CTxDestination dest = DecodeDestination(text.toStdString());

        if (text.isEmpty()) // Nothing entered
        {
            ui->labelAssetControlChangeLabel->setText("");
        }
        else if (!IsValidDestination(dest)) // Invalid address
        {
            ui->labelAssetControlChangeLabel->setText(tr("Warning: Invalid Avian address"));
        }
        else // Valid address
        {
            // Rule 3: model->IsSpendable() does not exist, use wallet interface
            if (!model->wallet().isSpendable(dest)) {
                ui->labelAssetControlChangeLabel->setText(tr("Warning: Unknown change address"));

                // confirmation dialog
                QMessageBox::StandardButton btnRetVal = QMessageBox::question(this, tr("Confirm custom change address"), tr("The address you selected for change is not part of this wallet. Any or all funds in your wallet may be sent to this address. Are you sure?"),
                                                                              QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);

                if(btnRetVal == QMessageBox::Yes)
                    AssetControlDialog::assetControl()->destChange = dest;
                else
                {
                    ui->lineEditAssetControlChange->setText("");
                    ui->labelAssetControlChangeLabel->setStyleSheet("QLabel{color:black;}");
                    ui->labelAssetControlChangeLabel->setText("");
                }
            }
            else // Known change address
            {
                ui->labelAssetControlChangeLabel->setStyleSheet("QLabel{color:black;}");

                // Query label
                QString associatedLabel = model->getAddressTableModel()->labelForAddress(text);
                if (!associatedLabel.isEmpty())
                    ui->labelAssetControlChangeLabel->setText(associatedLabel);
                else
                    ui->labelAssetControlChangeLabel->setText(tr("(no label)"));

                AssetControlDialog::assetControl()->destChange = dest;
            }
        }
    }
}

// Coin Control: update labels
void AssetsDialog::assetControlUpdateLabels()
{
    if (!model || !model->getOptionsModel())
        return;

    updateAssetControlState(*AssetControlDialog::assetControl());

    // set pay amounts
    AssetControlDialog::payAmounts.clear();
    AssetControlDialog::fSubtractFeeFromAmount = false;

    for(int i = 0; i < ui->entries->count(); ++i)
    {
        SendAssetsEntry *entry = qobject_cast<SendAssetsEntry*>(ui->entries->itemAt(i)->widget());
        if(entry && !entry->isHidden())
        {
            SendAssetsRecipient rcp = entry->getValue();
            AssetControlDialog::payAmounts.append(rcp.amount);
        }
    }

    if (AssetControlDialog::assetControl()->HasAssetSelected())
    {
        // actual coin control calculation
        AssetControlDialog::updateLabels(model, this);

        // show coin control stats
        ui->labelAssetControlAutomaticallySelected->hide();
        ui->widgetAssetControl->show();
    }
    else
    {
        // hide coin control stats
        ui->labelAssetControlAutomaticallySelected->show();
        ui->widgetAssetControl->hide();
        ui->labelAssetControlInsuffFunds->hide();
    }
}

/** AVN START */
void AssetsDialog::assetControlUpdateSendCoinsDialog()
{
    for(int i = 0; i < ui->entries->count(); ++i)
    {
        SendAssetsEntry *entry = qobject_cast<SendAssetsEntry*>(ui->entries->itemAt(i)->widget());
        if(entry)
        {
            removeEntry(entry);
        }
    }

    addEntry();

}

void AssetsDialog::processNewTransaction()
{
    for(int i = 0; i < ui->entries->count(); ++i)
    {
        SendAssetsEntry *entry = qobject_cast<SendAssetsEntry*>(ui->entries->itemAt(i)->widget());
        if(entry)
        {
            entry->refreshAssetList();
        }
    }
}

void AssetsDialog::focusAsset(const QModelIndex &idx)
{

    clear();

    SendAssetsEntry *entry = qobject_cast<SendAssetsEntry*>(ui->entries->itemAt(0)->widget());
    if(entry)
    {
        SendAssetsRecipient recipient;
        recipient.assetName = idx.data(AssetTableModel::AssetNameRole).toString();

        entry->setValue(recipient);
        entry->setFocus();
    }
}

void AssetsDialog::focusAssetListBox()
{

    SendAssetsEntry *entry = qobject_cast<SendAssetsEntry*>(ui->entries->itemAt(0)->widget());
    if (entry)
    {
        entry->setFocusAssetListBox();

        if (entry->getValue().assetName != "")
            entry->setFocus();

    }
}

void AssetsDialog::handleFirstSelection()
{
    SendAssetsEntry *entry = qobject_cast<SendAssetsEntry*>(ui->entries->itemAt(0)->widget());
    if (entry) {
        entry->refreshAssetList();
    }
}
/** AVN END */
