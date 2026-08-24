// Copyright (c) 2011-2016 The Bitcoin Core developers
// Copyright (c) 2017-2019 The Raven Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/restrictedassetsdialog.h>
#include <qt/forms/ui_restrictedassetsdialog.h>

#include <qt/bitcoinunits.h>
#include <qt/clientmodel.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <qt/walletmodel.h>
#include <qt/assettablemodel.h>
#include <qt/assetfilterproxy.h>

#include <assets/assets.h>
#include <assets/assettypes.h>
#include <addresstype.h>
#include <key_io.h>
#include <validation.h>
#include <qt/guiconstants.h>
#include <qt/restrictedassignqualifier.h>
#include <qt/forms/ui_restrictedassignqualifier.h>
#include <qt/restrictedfreezeaddress.h>
#include <qt/forms/ui_restrictedfreezeaddress.h>
#include <qt/sendcoinsdialog.h>
#include <qt/myrestrictedassettablemodel.h>
#include <wallet/asset_tx.h>
#include <wallet/wallet.h>

#include <QGraphicsDropShadowEffect>
#include <QDateTime>
#include <QFontMetrics>
#include <QMessageBox>
#include <QScrollBar>
#include <QSettings>
#include <QTextDocument>
#include <QTimer>
#include <QSortFilterProxyModel>

#include <policy/policy.h>
#include <core_io.h>
#include <wallet/coincontrol.h>
#include <assets/myassetsdb.h>

extern CMyRestrictedDB* pmyrestricteddb;

RestrictedAssetsDialog::RestrictedAssetsDialog(const PlatformStyle *_platformStyle, QWidget *parent) :
        QDialog(parent),
        ui(new Ui::RestrictedAssetsDialog),
        clientModel(0),
        model(0),
        platformStyle(_platformStyle)
{
    ui->setupUi(this);
    setWindowTitle("Manage Restricted Assets");
    setupStyling(_platformStyle);
}

void RestrictedAssetsDialog::setClientModel(ClientModel *_clientModel)
{
    this->clientModel = _clientModel;
}

void RestrictedAssetsDialog::setModel(WalletModel *_model)
{
    this->model = _model;

    if(_model && _model->getOptionsModel()) {
        setBalance(_model->getCachedBalance());
        connect(_model, &WalletModel::balanceChanged, this, &RestrictedAssetsDialog::setBalance);
        connect(_model->getOptionsModel(), &OptionsModel::displayUnitChanged, this, &RestrictedAssetsDialog::updateDisplayUnit);
        updateDisplayUnit();

        assetFilterProxy = new AssetFilterProxy(this);
        assetFilterProxy->setSourceModel(_model->getAssetTableModel());
        assetFilterProxy->setDynamicSortFilter(true);
        assetFilterProxy->setAssetNamePrefix("$");
        assetFilterProxy->setSortCaseSensitivity(Qt::CaseInsensitive);
        assetFilterProxy->setFilterCaseSensitivity(Qt::CaseInsensitive);

        myRestrictedAssetsFilterProxy = new QSortFilterProxyModel(this);
        myRestrictedAssetsFilterProxy->setSourceModel(_model->getMyRestrictedAssetsTableModel());
        myRestrictedAssetsFilterProxy->setDynamicSortFilter(true);
        myRestrictedAssetsFilterProxy->setSortCaseSensitivity(Qt::CaseInsensitive);
        myRestrictedAssetsFilterProxy->setFilterCaseSensitivity(Qt::CaseInsensitive);

        myRestrictedAssetsFilterProxy->setSortRole(Qt::EditRole);

        ui->myAddressList->setModel(myRestrictedAssetsFilterProxy);
        ui->myAddressList->horizontalHeader()->setStretchLastSection(true);
        ui->myAddressList->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        ui->myAddressList->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        ui->myAddressList->setAlternatingRowColors(true);
        ui->myAddressList->setSortingEnabled(true);
        ui->myAddressList->verticalHeader()->hide();

        ui->listAssets->setModel(assetFilterProxy);
        ui->listAssets->horizontalHeader()->setStretchLastSection(true);
        ui->listAssets->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        ui->listAssets->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        ui->listAssets->setAlternatingRowColors(true);
        ui->listAssets->verticalHeader()->hide();

        AssignQualifier *assignQualifier = new AssignQualifier(platformStyle, this);
        assignQualifier->setWalletModel(_model);
        assignQualifier->setObjectName("tab_assign_qualifier");
        connect(assignQualifier->getUI()->buttonSubmit, SIGNAL(clicked()), this, SLOT(assignQualifierClicked()));
        connect(assignQualifier->getUI()->buttonCheck,  SIGNAL(clicked()), this, SLOT(checkQualifierClicked()));
        ui->tabWidget->addTab(assignQualifier, "Assign/Remove Qualifier");

        FreezeAddress *freezeAddress = new FreezeAddress(platformStyle, this);
        freezeAddress->setWalletModel(_model);
        freezeAddress->setObjectName("tab_freeze_address");
        connect(freezeAddress->getUI()->buttonSubmit, SIGNAL(clicked()), this, SLOT(freezeAddressClicked()));
        connect(freezeAddress->getUI()->buttonCheck,  SIGNAL(clicked()), this, SLOT(checkFreezeClicked()));
        ui->tabWidget->addTab(freezeAddress, "Restrict Addresses/Global");
    }
}

RestrictedAssetsDialog::~RestrictedAssetsDialog()
{
    delete ui;
}

void RestrictedAssetsDialog::setupStyling(const PlatformStyle *platformStyle)
{
    /** Create the shadow effects on the frames */
    ui->frameAssetBalance->setGraphicsEffect(GUIUtil::getShadowEffect());
    ui->frameAddressList->setGraphicsEffect(GUIUtil::getShadowEffect());
    ui->tabFrame->setGraphicsEffect(GUIUtil::getShadowEffect());
}

QWidget *RestrictedAssetsDialog::setupTabChain(QWidget *prev)
{
    return prev;
}

void RestrictedAssetsDialog::setBalance(const interfaces::WalletBalances& balances)
{
    if(model && model->getOptionsModel())
    {
        ui->labelBalance->setText(BitcoinUnits::formatWithUnit(model->getOptionsModel()->getDisplayUnit(), balances.balance));
    }
}

void RestrictedAssetsDialog::updateDisplayUnit()
{
    setBalance(model->getCachedBalance());
}

void RestrictedAssetsDialog::freezeAddressClicked()
{
    WalletModel::UnlockContext ctx(model->requestUnlock());
    if(!ctx.isValid())
    {
        return;
    }

    wallet::CWallet* pwallet = model->wallet().wallet();
    if (!pwallet) {
        QMessageBox::critical(this, tr("Error"), tr("Wallet not available."));
        return;
    }

    // Get the freeze tab widget
    FreezeAddress *freezeTab = findChild<FreezeAddress*>("tab_freeze_address");
    if (!freezeTab) return;
    Ui::FreezeAddress *fui = freezeTab->getUI();

    // Get the restricted asset name from combo box
    QString assetName = fui->assetComboBox->currentText();
    if (assetName.isEmpty()) {
        QMessageBox::warning(this, tr("Error"), tr("Please select a restricted asset."));
        return;
    }
    std::string asset_name = assetName.toStdString();
    if (asset_name[0] != RESTRICTED_CHAR)
        asset_name = std::string(1, RESTRICTED_CHAR) + asset_name;

    // Determine the operation from radio buttons
    bool isGlobal = fui->radioButtonGlobalFreeze->isChecked() || fui->radioButtonGlobalUnfreeze->isChecked();
    bool isFreeze = fui->radioButtonFreezeAddress->isChecked() || fui->radioButtonGlobalFreeze->isChecked();
    int8_t flag = isFreeze ? 1 : 0;

    if (!fui->radioButtonFreezeAddress->isChecked() && !fui->radioButtonUnfreezeAddress->isChecked() &&
        !fui->radioButtonGlobalFreeze->isChecked() && !fui->radioButtonGlobalUnfreeze->isChecked()) {
        QMessageBox::warning(this, tr("Error"), tr("Please select a freeze/unfreeze option."));
        return;
    }

    // Get optional asset data
    std::string asset_data = fui->lineEditAssetData->text().trimmed().toStdString();

    // Get change address
    std::string change_address;
    if (fui->checkBoxChangeAddress->isChecked() && !fui->lineEditChangeAddress->text().isEmpty()) {
        change_address = fui->lineEditChangeAddress->text().trimmed().toStdString();
        CTxDestination dest = DecodeDestination(change_address);
        if (std::get_if<PKHash>(&dest) == nullptr) {
            QMessageBox::warning(this, tr("Error"), tr("Change address must use legacy (P2PKH) format."));
            return;
        }
    }

    // Build the transaction
    LOCK(pwallet->cs_wallet);

    if (change_address.empty()) {
        auto op_dest = pwallet->GetNewDestination(OutputType::LEGACY, "");
        if (!op_dest) {
            QMessageBox::critical(this, tr("Error"), tr("Failed to generate change address."));
            return;
        }
        change_address = EncodeDestination(*op_dest);
    }

    std::string ownerName = RestrictedNameToOwnerName(asset_name);
    std::vector<std::pair<CAssetTransfer, std::string>> vTransfers;
    CAssetTransfer assetTransfer(ownerName, OWNER_ASSET_AMOUNT, DecodeAssetData(asset_data), 0);
    vTransfers.emplace_back(std::make_pair(assetTransfer, change_address));

    wallet::CCoinControl ctrl;
    ctrl.destChange = DecodeDestination(change_address);

    CTransactionRef tx;
    CAmount nFeeRequired;
    std::pair<int, std::string> error;

    if (isGlobal) {
        // Global freeze/unfreeze
        std::vector<CNullAssetTxData> nullGlobalRestrictionData;
        CNullAssetTxData nullData(asset_name, flag);
        nullGlobalRestrictionData.push_back(nullData);

        if (!wallet::CreateTransferAssetTransaction(*pwallet, ctrl, vTransfers, "", error, tx, nFeeRequired, nullptr, &nullGlobalRestrictionData)) {
            QMessageBox::critical(this, tr("Error"), QString::fromStdString(error.second));
            return;
        }
    } else {
        // Per-address freeze/unfreeze
        std::string address = fui->lineEditAddress->text().trimmed().toStdString();
        if (address.empty()) {
            QMessageBox::warning(this, tr("Error"), tr("Please enter an address to freeze/unfreeze."));
            return;
        }
        CTxDestination addr_dest = DecodeDestination(address);
        if (std::get_if<PKHash>(&addr_dest) == nullptr) {
            QMessageBox::warning(this, tr("Error"), tr("Address must use legacy (P2PKH) format. SegWit and bech32 addresses are not supported."));
            return;
        }

        std::vector<std::pair<CNullAssetTxData, std::string>> nullAssetTxData;
        CNullAssetTxData nullData(asset_name, flag);
        nullAssetTxData.emplace_back(std::make_pair(nullData, address));

        if (!wallet::CreateTransferAssetTransaction(*pwallet, ctrl, vTransfers, "", error, tx, nFeeRequired, &nullAssetTxData)) {
            QMessageBox::critical(this, tr("Error"), QString::fromStdString(error.second));
            return;
        }
    }

    // Confirm before broadcasting
    {
        bool isGlobalOp = fui->radioButtonGlobalFreeze->isChecked() || fui->radioButtonGlobalUnfreeze->isChecked();
        QString opDesc = isFreeze ? tr("freeze") : tr("unfreeze");
        QString scopeDesc = isGlobalOp ? tr("ALL trading for %1").arg(assetName) :
                                         tr("address %1 for %2").arg(fui->lineEditAddress->text().trimmed()).arg(assetName);
        QString feeStr = BitcoinUnits::formatWithUnit(model->getOptionsModel()->getDisplayUnit(), nFeeRequired);
        auto reply = QMessageBox::question(this, tr("Confirm Restriction"),
            tr("You are about to %1 %2.\n\nEstimated fee: %3\n\nProceed?").arg(opDesc).arg(scopeDesc).arg(feeStr),
            QMessageBox::Yes | QMessageBox::No);
        if (reply != QMessageBox::Yes)
            return;
    }

    std::string txid;
    if (!wallet::SendAssetTransaction(*pwallet, tx, error, txid)) {
        QMessageBox::critical(this, tr("Error"), QString::fromStdString(error.second));
        return;
    }

    // Record per-address restrictions in pmyrestricteddb so the Address List panel shows them.
    // Global freezes have no specific address and are not recorded here.
    if (!isGlobal && pmyrestricteddb) {
        std::string addr = fui->lineEditAddress->text().trimmed().toStdString();
        pmyrestricteddb->WriteRestrictedAddress(addr, asset_name, isFreeze, 0);
        model->getMyRestrictedAssetsTableModel()->updateMyRestrictedAssets(
            QString::fromStdString(addr),
            assetName,
            isFreeze ? 1 : 0,
            QDateTime::currentSecsSinceEpoch());
    }

    QMessageBox::information(this, tr("Success"),
        tr("Transaction sent successfully.\nTxID: %1").arg(QString::fromStdString(txid)));
}

void RestrictedAssetsDialog::assignQualifierClicked()
{
    WalletModel::UnlockContext ctx(model->requestUnlock());
    if(!ctx.isValid())
    {
        return;
    }

    wallet::CWallet* pwallet = model->wallet().wallet();
    if (!pwallet) {
        QMessageBox::critical(this, tr("Error"), tr("Wallet not available."));
        return;
    }

    // Get the qualifier tab widget
    AssignQualifier *qualifierTab = findChild<AssignQualifier*>("tab_assign_qualifier");
    if (!qualifierTab) return;
    Ui::AssignQualifier *qui = qualifierTab->getUI();

    // Get qualifier name from combo box
    QString qualifierName = qui->assetComboBox->currentText();
    if (qualifierName.isEmpty()) {
        QMessageBox::warning(this, tr("Error"), tr("Please select a qualifier asset."));
        return;
    }
    std::string tag_name = qualifierName.toStdString();
    if (tag_name[0] != QUALIFIER_CHAR)
        tag_name = std::string(1, QUALIFIER_CHAR) + tag_name;

    // Determine assign (1) or remove (0) from the type combo box
    int assignTypeIndex = qui->assignTypeComboBox->currentIndex();
    int8_t flag = (assignTypeIndex == 0) ? 1 : 0;

    // Get the target address
    std::string to_address = qui->lineEditAddress->text().trimmed().toStdString();
    if (to_address.empty()) {
        QMessageBox::warning(this, tr("Error"), tr("Please enter an address."));
        return;
    }
    CTxDestination to_dest = DecodeDestination(to_address);
    if (std::get_if<PKHash>(&to_dest) == nullptr) {
        QMessageBox::warning(this, tr("Error"), tr("Address must use legacy (P2PKH) format. SegWit and bech32 addresses are not supported."));
        return;
    }

    // Get optional asset data
    std::string asset_data = qui->lineEditAssetData->text().trimmed().toStdString();

    // Get change address
    std::string change_address;
    if (qui->checkBoxChangeAddress->isChecked() && !qui->lineEditChangeAddress->text().isEmpty()) {
        change_address = qui->lineEditChangeAddress->text().trimmed().toStdString();
        CTxDestination dest = DecodeDestination(change_address);
        if (std::get_if<PKHash>(&dest) == nullptr) {
            QMessageBox::warning(this, tr("Error"), tr("Change address must use legacy (P2PKH) format."));
            return;
        }
    }

    // Build the transaction
    LOCK(pwallet->cs_wallet);

    if (change_address.empty()) {
        auto op_dest = pwallet->GetNewDestination(OutputType::LEGACY, "");
        if (!op_dest) {
            QMessageBox::critical(this, tr("Error"), tr("Failed to generate change address."));
            return;
        }
        change_address = EncodeDestination(*op_dest);
    }

    // Transfer qualifier token to self (change address) to prove ownership
    std::vector<std::pair<CAssetTransfer, std::string>> vTransfers;
    CAssetTransfer assetTransfer(tag_name, QUALIFIER_ASSET_MIN_AMOUNT, DecodeAssetData(asset_data), 0);
    vTransfers.emplace_back(std::make_pair(assetTransfer, change_address));

    // Attach null asset tx data to tag/untag the address
    std::vector<std::pair<CNullAssetTxData, std::string>> nullAssetTxData;
    CNullAssetTxData nullData(tag_name, flag);
    nullAssetTxData.emplace_back(std::make_pair(nullData, to_address));

    wallet::CCoinControl ctrl;
    ctrl.destChange = DecodeDestination(change_address);

    CTransactionRef tx;
    CAmount nFeeRequired;
    std::pair<int, std::string> error;

    if (!wallet::CreateTransferAssetTransaction(*pwallet, ctrl, vTransfers, "", error, tx, nFeeRequired, &nullAssetTxData)) {
        QMessageBox::critical(this, tr("Error"), QString::fromStdString(error.second));
        return;
    }

    // Confirm before broadcasting
    {
        QString action = (flag == 1) ? tr("assign qualifier %1 to") : tr("remove qualifier %1 from");
        QString feeStr = BitcoinUnits::formatWithUnit(model->getOptionsModel()->getDisplayUnit(), nFeeRequired);
        auto reply = QMessageBox::question(this, tr("Confirm Qualifier Change"),
            tr("You are about to %1 address %2.\n\nEstimated fee: %3\n\nProceed?")
                .arg(action.arg(qualifierName))
                .arg(qui->lineEditAddress->text().trimmed())
                .arg(feeStr),
            QMessageBox::Yes | QMessageBox::No);
        if (reply != QMessageBox::Yes)
            return;
    }

    std::string txid;
    if (!wallet::SendAssetTransaction(*pwallet, tx, error, txid)) {
        QMessageBox::critical(this, tr("Error"), QString::fromStdString(error.second));
        return;
    }

    // Record the qualifier assignment in pmyrestricteddb so the Address List panel shows it.
    if (pmyrestricteddb) {
        pmyrestricteddb->WriteTaggedAddress(to_address, tag_name, flag == 1, 0);
        model->getMyRestrictedAssetsTableModel()->updateMyRestrictedAssets(
            QString::fromStdString(to_address),
            qualifierName,
            flag,
            QDateTime::currentSecsSinceEpoch());
    }

    QMessageBox::information(this, tr("Success"),
        tr("Transaction sent successfully.\nTxID: %1").arg(QString::fromStdString(txid)));
}

void RestrictedAssetsDialog::checkFreezeClicked()
{
    FreezeAddress *freezeTab = findChild<FreezeAddress*>("tab_freeze_address");
    if (!freezeTab) return;
    Ui::FreezeAddress *fui = freezeTab->getUI();

    QString assetName = fui->assetComboBox->currentText();
    if (assetName.isEmpty()) {
        QMessageBox::warning(this, tr("Check"), tr("Please select a restricted asset."));
        return;
    }
    std::string asset_name = assetName.toStdString();
    if (asset_name[0] != RESTRICTED_CHAR)
        asset_name = std::string(1, RESTRICTED_CHAR) + asset_name;

    LOCK(cs_main);
    if (!passets) {
        QMessageBox::warning(this, tr("Check"), tr("Asset cache not available."));
        return;
    }

    bool isGlobal = fui->radioButtonGlobalFreeze->isChecked() || fui->radioButtonGlobalUnfreeze->isChecked();
    if (isGlobal) {
        bool frozen = passets->CheckForGlobalRestriction(asset_name);
        QMessageBox::information(this, tr("Global Restriction Status"),
            frozen ? tr("%1 is currently globally frozen (all trading restricted).").arg(assetName)
                   : tr("%1 is not globally frozen.").arg(assetName));
    } else {
        QString address = fui->lineEditAddress->text().trimmed();
        if (address.isEmpty()) {
            QMessageBox::warning(this, tr("Check"), tr("Please enter an address to check."));
            return;
        }
        bool frozen = passets->CheckForAddressRestriction(asset_name, address.toStdString(), true);
        QMessageBox::information(this, tr("Address Restriction Status"),
            frozen ? tr("Address %1 is currently frozen for %2.").arg(address).arg(assetName)
                   : tr("Address %1 is not frozen for %2.").arg(address).arg(assetName));
    }
}

void RestrictedAssetsDialog::checkQualifierClicked()
{
    AssignQualifier *qualifierTab = findChild<AssignQualifier*>("tab_assign_qualifier");
    if (!qualifierTab) return;
    Ui::AssignQualifier *qui = qualifierTab->getUI();

    QString qualifierName = qui->assetComboBox->currentText();
    if (qualifierName.isEmpty()) {
        QMessageBox::warning(this, tr("Check"), tr("Please select a qualifier asset."));
        return;
    }
    std::string tag_name = qualifierName.toStdString();
    if (tag_name[0] != QUALIFIER_CHAR)
        tag_name = std::string(1, QUALIFIER_CHAR) + tag_name;

    QString address = qui->lineEditAddress->text().trimmed();
    if (address.isEmpty()) {
        QMessageBox::warning(this, tr("Check"), tr("Please enter an address to check."));
        return;
    }

    LOCK(cs_main);
    if (!passets) {
        QMessageBox::warning(this, tr("Check"), tr("Asset cache not available."));
        return;
    }

    bool hasQualifier = passets->CheckForAddressQualifier(tag_name, address.toStdString(), true);
    QMessageBox::information(this, tr("Qualifier Status"),
        hasQualifier ? tr("Address %1 has qualifier %2.").arg(address).arg(qualifierName)
                     : tr("Address %1 does not have qualifier %2.").arg(address).arg(qualifierName));
}
