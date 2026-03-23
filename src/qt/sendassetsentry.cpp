// Copyright (c) 2011-2016 The Bitcoin Core developers
// Copyright (c) 2017-2020 The Raven Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#include <qt/sendassetsentry.h>
#include "ui_sendassetsentry.h"

#include <qt/addressbookpage.h>
#include <qt/addresstablemodel.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <qt/walletmodel.h>
#include <qt/assetcontroldialog.h>
#include <qt/assettablemodel.h>
#include <qt/guiconstants.h>

#include <wallet/coincontrol.h>
#include <wallet/wallet.h>
#include <wallet/spend.h>
#include <assets/assets.h>

#include <QGraphicsDropShadowEffect>
#include <QApplication>
#include <QClipboard>
#include <validation.h>
#include <core_io.h>
#include <QStringListModel>
#include <QSortFilterProxyModel>
#include <QCompleter>

SendAssetsEntry::SendAssetsEntry(const PlatformStyle *_platformStyle, const QStringList myAssetsNames, QWidget *parent) :
    QStackedWidget(parent),
    ui(new Ui::SendAssetsEntry),
    model(0),
    platformStyle(_platformStyle)
{
    ui->setupUi(this);

    ui->addressBookButton->setIcon(platformStyle->SingleColorIcon(":/icons/address-book"));
    ui->pasteButton->setIcon(platformStyle->SingleColorIcon(":/icons/editpaste"));
    ui->deleteButton->setIcon(platformStyle->SingleColorIcon(":/icons/remove"));
    ui->deleteButton_is->setIcon(platformStyle->SingleColorIcon(":/icons/remove"));
    ui->deleteButton_s->setIcon(platformStyle->SingleColorIcon(":/icons/remove"));

    setCurrentWidget(ui->SendCoins);

    ui->addAsLabel->setPlaceholderText(tr("Enter a label for this address to add it to your address book"));

    // normal avian address field
    GUIUtil::setupAddressWidget(ui->payTo, this);
    // just a label for displaying avian address(es)
    ui->payTo_is->setFont(GUIUtil::fixedPitchFont());

    // Connect signals
    connect(ui->payAssetAmount, SIGNAL(valueChanged()), this, SIGNAL(payAmountChanged()));
    connect(ui->deleteButton, SIGNAL(clicked()), this, SLOT(deleteClicked()));
    connect(ui->deleteButton_is, SIGNAL(clicked()), this, SLOT(deleteClicked()));
    connect(ui->deleteButton_s, SIGNAL(clicked()), this, SLOT(deleteClicked()));
    connect(ui->assetSelectionBox, SIGNAL(activated(int)), this, SLOT(onAssetSelected(int)));
    connect(ui->administratorCheckbox, SIGNAL(clicked()), this, SLOT(onSendOwnershipChanged()));

    ui->administratorCheckbox->setToolTip(tr("Select to view administrator assets to transfer"));

    /** Setup the asset list combobox */
    stringModel = new QStringListModel;
    stringModel->insertRow(stringModel->rowCount());
    stringModel->setData(stringModel->index(stringModel->rowCount() - 1, 0), "", Qt::DisplayRole);

    for (auto name : myAssetsNames)
    {
        stringModel->insertRow(stringModel->rowCount());
        stringModel->setData(stringModel->index(stringModel->rowCount() - 1, 0), name, Qt::DisplayRole);
    }

    proxy = new QSortFilterProxyModel;
    proxy->setSourceModel(stringModel);
    proxy->setFilterCaseSensitivity(Qt::CaseInsensitive);

    ui->assetSelectionBox->setModel(proxy);
    ui->assetSelectionBox->setEditable(true);

    completer = new QCompleter(proxy,this);
    completer->setCompletionMode(QCompleter::PopupCompletion);
    completer->setCaseSensitivity(Qt::CaseInsensitive);
    ui->assetSelectionBox->setCompleter(completer);

    ui->assetSelectionBox->lineEdit()->setPlaceholderText(tr("Select an asset to transfer"));
    ui->assetSelectionBox->setMinimumWidth(32);

    /** Setup the amount box */
    ui->ownershipWarningMessage->hide();

    fShowAdministratorList = false;

    this->setGraphicsEffect(GUIUtil::getShadowEffect());

    ui->payAssetAmount->setUnit(MAX_UNIT);
    ui->payAssetAmount->setDisabled(false);
    ui->memoBox->installEventFilter(this);
}

SendAssetsEntry::~SendAssetsEntry()
{
    delete ui;
}

void SendAssetsEntry::on_pasteButton_clicked()
{
    // Paste text from clipboard into recipient field
    ui->payTo->setText(QApplication::clipboard()->text());
}

void SendAssetsEntry::on_addressBookButton_clicked()
{
    if(!model)
        return;
    AddressBookPage dlg(platformStyle, AddressBookPage::ForSelection, AddressBookPage::SendingTab, this);
    dlg.setModel(model->getAddressTableModel());
    if(dlg.exec())
    {
        ui->payTo->setText(dlg.getReturnValue());
        ui->payAssetAmount->setFocus();
    }
}

void SendAssetsEntry::on_payTo_textChanged(const QString &address)
{
    updateLabel(address);
}

void SendAssetsEntry::setModel(WalletModel *_model)
{
    this->model = _model;
    clear();
}

void SendAssetsEntry::clear()
{
    // clear UI elements for normal payment
    ui->payTo->clear();
    ui->addAsLabel->clear();
    ui->messageTextLabel->clear();
    ui->messageTextLabel->hide();
    ui->messageLabel->hide();
    // clear UI elements for unauthenticated payment request
    ui->memoTextLabel_is->clear();
    // clear UI elements for authenticated payment request
    ui->payTo_s->clear();
    ui->memoTextLabel_s->clear();

    ui->payAssetAmount->clear();

    // Reset the selected asset
    ui->assetSelectionBox->setCurrentIndex(0);
}

void SendAssetsEntry::deleteClicked()
{
    Q_EMIT removeEntry(this);
}

bool SendAssetsEntry::validate()
{
    if (!model)
        return false;

    // Check input validity
    bool retval = true;

    // Skip checks for payment request
    if (recipient.paymentRequest.IsInitialized())
        return retval;

    if (!model->validateAddress(ui->payTo->text()))
    {
        ui->payTo->setValid(false);
        retval = false;
    }

    if (ui->assetSelectionBox->currentIndex() == 0) {
        ui->assetSelectionBox->lineEdit()->setStyleSheet(STYLE_INVALID);
        retval = false;
    }

    if (!ui->payAssetAmount->validate())
    {
        retval = false;
    }

    if (ui->payAssetAmount->value(0) <= 0)
    {
        ui->payAssetAmount->setValid(false);
        retval = false;
    }

    if (!ui->memoBox->text().isEmpty()) {
        if (!AreMessagesDeployed()) {
            ui->messageTextLabel->show();
            ui->messageTextLabel->setText(tr("Memos can only be added once messaging is voted in"));
            ui->memoBox->setStyleSheet(STYLE_INVALID);
            retval = false;
        }

        size_t size = ui->memoBox->text().size();

        if (size != 46) {
            if (!AreMessagesDeployed()) {
                ui->memoBox->setStyleSheet(STYLE_INVALID);
                retval = false;
            } else {
                if (size != 64) {
                    ui->memoBox->setStyleSheet(STYLE_INVALID);
                    retval = false;
                }
            }
        }

        std::string error = "";
        if(!CheckEncoded(DecodeAssetData(ui->memoBox->text().toStdString()), error)) {
            ui->memoBox->setStyleSheet(STYLE_INVALID);
            retval = false;
        }
    }

    std::string assetName = ui->assetSelectionBox->currentText().toStdString();
    if (IsAssetNameAnRestricted(assetName)) {
        if (passets) {
            if (passets->CheckForGlobalRestriction(assetName)) {
                ui->assetSelectionBox->lineEdit()->setStyleSheet(STYLE_INVALID);
                ui->messageTextLabel->show();
                ui->messageTextLabel->setText(tr("This restricted asset has been frozen globally. No transfers can be sent on the network."));
                retval = false;
            }

            CNullAssetTxVerifierString verifier;
            if (passets->GetAssetVerifierStringIfExists(assetName, verifier)) {
                std::string strError = "";
                ErrorReport report;
                if (!ContextualCheckVerifierString(passets, verifier.verifier_string,ui->payTo->text().toStdString(), strError, &report)) {
                    ui->payTo->setValid(false);
                    ui->messageTextLabel->show();
                    ui->messageTextLabel->setText(QString::fromStdString(GetUserErrorString(report)));
                    retval = false;
                }
            }
        }
    }

    return retval;
}

SendAssetsRecipient SendAssetsEntry::getValue()
{
    // Payment request
    if (recipient.paymentRequest.IsInitialized())
        return recipient;

    // Normal payment
    recipient.assetName = ui->assetSelectionBox->currentText();
    recipient.address = ui->payTo->text();
    recipient.label = ui->addAsLabel->text();
    recipient.amount = ui->payAssetAmount->value();
    recipient.message = ui->memoBox->text();

    return recipient;
}

QWidget *SendAssetsEntry::setupTabChain(QWidget *prev)
{
    QWidget::setTabOrder(prev, ui->payTo);
    QWidget::setTabOrder(ui->payTo, ui->addAsLabel);
    QWidget::setTabOrder(ui->addressBookButton, ui->pasteButton);
    QWidget::setTabOrder(ui->pasteButton, ui->deleteButton);
    QWidget::setTabOrder(ui->deleteButton, ui->payAssetAmount);
    QWidget::setTabOrder(ui->payAssetAmount, ui->memoBox);
    return ui->memoBox;
}

void SendAssetsEntry::setValue(const SendAssetsRecipient &value)
{
    recipient = value;

    if (recipient.assetName != "") {
        int index = ui->assetSelectionBox->findText(recipient.assetName);
        ui->assetSelectionBox->setCurrentIndex(index);
        onAssetSelected(index);
    }
}

void SendAssetsEntry::setAddress(const QString &address)
{
    ui->payTo->setText(address);
    ui->payAssetAmount->setFocus();
}

bool SendAssetsEntry::isClear()
{
    return ui->payTo->text().isEmpty() && ui->payTo_is->text().isEmpty() && ui->payTo_s->text().isEmpty();
}

void SendAssetsEntry::setFocus()
{
    ui->payTo->setFocus();
}

void SendAssetsEntry::setFocusAssetListBox()
{
    ui->assetSelectionBox->setFocus();
}

bool SendAssetsEntry::updateLabel(const QString &address)
{
    if(!model)
        return false;

    // Fill in label from address book, if address has an associated label
    QString associatedLabel = model->getAddressTableModel()->labelForAddress(address);
    if(!associatedLabel.isEmpty())
    {
        ui->addAsLabel->setText(associatedLabel);
        return true;
    }

    return false;
}

void SendAssetsEntry::onAssetSelected(int index)
{
    ui->assetSelectionBox->lineEdit()->setStyleSheet("background: transparent;border:none;");
    QString name = ui->assetSelectionBox->currentText();
    if (index == 0) {
        ui->assetAmountLabel->clear();
        ui->payAssetAmount->clear();
        ui->payAssetAmount->setDisabled(true);
        return;
    }

    // Check to see if the asset selected is an ownership asset
    bool fIsOwnerAsset = false;
    if (IsAssetNameAnOwner(name.toStdString())) {
        fIsOwnerAsset = true;
        name = name.split("!").first();
    }

    // Check to see if the asset selected is a messenger asset
    bool fIsMessengerAsset = false;
    if (IsAssetNameAnMsgChannel(name.toStdString())) {
        fIsMessengerAsset = true;
    }

    LOCK(cs_main);
    auto currentActiveAssetCache = GetCurrentAssetCache();
    CNewAsset asset;

    // Get the asset metadata if it exists
    if (!currentActiveAssetCache->GetAssetMetaDataIfExists(name.toStdString(), asset)) {
        clear();
        ui->messageLabel->show();
        ui->messageTextLabel->show();
        ui->messageTextLabel->setText(tr("Failed to get asset metadata for: ") + name + "." + tr(" The transaction in which the asset was issued must be mined into a block before you can transfer it"));
        ui->assetAmountLabel->clear();
        return;
    }

    // Look up asset balance from the cached AssetTableModel
    QString balanceStr = tr("(unknown)");
    if (model && model->getAssetTableModel()) {
        AssetTableModel* assetModel = model->getAssetTableModel();
        QString lookupName = ui->assetSelectionBox->currentText();
        for (int i = 0; i < assetModel->rowCount(QModelIndex()); i++) {
            QModelIndex idx = assetModel->index(i, AssetTableModel::Name, QModelIndex());
            if (idx.data(AssetTableModel::AssetNameRole).toString() == lookupName) {
                balanceStr = idx.data(AssetTableModel::FormattedAmountRole).toString();
                break;
            }
        }
    }
    ui->assetAmountLabel->setText(tr("Wallet Balance") + ": <b>" + balanceStr + "</b> " + name);

    ui->messageLabel->hide();
    ui->messageTextLabel->hide();

    // If it is not an ownership asset unlock the amount
    if (!fIsOwnerAsset) {
        ui->payAssetAmount->setUnit(asset.units);
        ui->payAssetAmount->setSingleStep(1);
        ui->payAssetAmount->setDisabled(false);
        ui->payAssetAmount->setValue(0);
    }
    // If it is messenger channel set amount to 1 and keep locked.
    if (fIsMessengerAsset) {
        ui->payAssetAmount->setUnit(asset.units);
        ui->payAssetAmount->setDisabled(true);
        ui->payAssetAmount->setValue(1);
    }
}

void SendAssetsEntry::onSendOwnershipChanged()
{
    switchAdministratorList(true);
}

void SendAssetsEntry::CheckOwnerBox() {
    fUsingAssetControl = true;
    switchAdministratorList();
}

void SendAssetsEntry::IsAssetControl(bool fIsAssetControl, bool fIsOwner)
{
    if (fIsOwner) {
        CheckOwnerBox();
    }
    if (fIsAssetControl) {
        ui->administratorCheckbox->setDisabled(true);
        fUsingAssetControl = true;
    }
}

void SendAssetsEntry::setCurrentIndex(int index)
{
    if (index < ui->assetSelectionBox->count()) {
        ui->assetSelectionBox->setCurrentIndex(index);
        ui->assetSelectionBox->activated(index);
    }
}

void SendAssetsEntry::refreshAssetList()
{
    switchAdministratorList(false);
}

void SendAssetsEntry::switchAdministratorList(bool fSwitchStatus)
{
    if(!model)
        return;

    if (fSwitchStatus)
        fShowAdministratorList = !fShowAdministratorList;

    if (fShowAdministratorList) {
        ui->administratorCheckbox->setChecked(true);
        if (!AssetControlDialog::assetControl()->HasAssetSelected()) {
            QStringList list;
            list << "";
            if (model) {
                wallet::CWallet* pwallet = model->wallet().wallet();
                if (pwallet) {
                    LOCK(pwallet->cs_wallet);
                    wallet::CoinFilterParams params;
                    params.min_amount = 0;
                    wallet::CoinsResult available = wallet::AvailableCoinsWithAssets(*pwallet, nullptr, std::nullopt, params);
                    for (const auto& [assetName, assetOutputs] : available.mapAssetCoins) {
                        if (IsAssetNameAnOwner(assetName))
                            list << QString::fromStdString(assetName);
                    }
                }
            }
            stringModel->setStringList(list);
            ui->assetSelectionBox->lineEdit()->setPlaceholderText(tr("Select an administrator asset to transfer"));
            ui->assetSelectionBox->setFocus();
        } else {
            ui->payTo->setFocus();
        }

        ui->payAssetAmount->setUnit(MIN_UNIT);
        ui->payAssetAmount->setValue(1);
        ui->payAssetAmount->setDisabled(true);

        ui->assetAmountLabel->clear();

        ui->ownershipWarningMessage->setText(tr("Warning: Transferring administrator asset"));
        ui->ownershipWarningMessage->setStyleSheet("color: red");
        ui->ownershipWarningMessage->show();
    } else {
        ui->administratorCheckbox->setChecked(false);
        if (!AssetControlDialog::assetControl()->HasAssetSelected()) {
            QStringList list;
            list << "";
            if (model) {
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
            stringModel->setStringList(list);
            ui->assetSelectionBox->lineEdit()->setPlaceholderText(tr("Select an asset to transfer"));
            ui->assetAmountLabel->clear();
            ui->assetSelectionBox->setFocus();
        } else {
            ui->payTo->setFocus();
        }
        ui->ownershipWarningMessage->hide();
    }
}

bool SendAssetsEntry::eventFilter(QObject *object, QEvent *event)
{
    if (object == ui->memoBox && event->type() == QEvent::FocusIn)
    {
    // Clear invalid flag on focus
        ui->memoBox->setStyleSheet("");
    }
    return QWidget::eventFilter(object, event);
}
