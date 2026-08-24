// Copyright (c) 2011-2016 The Bitcoin Core developers
// Copyright (c) 2017-2019 The Raven Core developers
// Copyright (c) 2022 The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/createassetdialog.h>
#include <qt/forms/ui_createassetdialog.h>
#include <qt/platformstyle.h>
#include <qt/walletmodel.h>
#include <qt/addresstablemodel.h>
#include <qt/sendcoinsdialog.h>
#include <qt/coincontroldialog.h>
#include <qt/guiutil.h>
#include <qt/bitcoinunits.h>
#include <qt/clientmodel.h>
#include <qt/optionsmodel.h>
#include <qt/guiconstants.h>

#include <wallet/coincontrol.h>
#include <wallet/asset_tx.h>
#include <policy/fees.h>
#include <psbt.h>
#include <util/strencodings.h>

#include <addresstype.h>
#include <key_io.h>
#include <regex>
#include <string>
#include <validation.h> // mempool and minRelayTxFee
#include <wallet/wallet.h>
#include <wallet/spend.h>
#include <core_io.h>
#include <policy/policy.h>
#include <assets/assets.h>
#include <assets/ans.h>
#include <assets/assetdb.h>
#include <assets/cbor.h>
#include <qt/assettablemodel.h>

#include <fstream>

#include <QGraphicsDropShadowEffect>
#include <QModelIndex>
#include <QDebug>
#include <QMessageBox>
#include <QClipboard>
#include <QSettings>
#include <QStringListModel>
#include <QSortFilterProxyModel>
#include <QCompleter>
#include <QUrl>
#include <QDesktopServices>

static const int confTargets[] = {2, 4, 6, 12, 24, 48};
static int getConfTargetForIndex(int index) {
    int maxI = static_cast<int>(sizeof(confTargets)/sizeof(confTargets[0])) - 1;
    if (index < 0) return confTargets[0];
    if (index > maxI) return confTargets[maxI];
    return confTargets[index];
}
static int getIndexForConfTarget(int target) {
    for (unsigned int i = 0; i < sizeof(confTargets)/sizeof(confTargets[0]); i++)
        if (confTargets[i] >= target) return i;
    return sizeof(confTargets)/sizeof(confTargets[0]) - 1;
}

static wallet::CCoinControl& s_coinControl()
{
    static wallet::CCoinControl instance;
    return instance;
}

CreateAssetDialog::CreateAssetDialog(const PlatformStyle *_platformStyle, QWidget *parent) :
        QDialog(parent, Qt::WindowTitleHint | Qt::CustomizeWindowHint | Qt::WindowCloseButtonHint | Qt::WindowMaximizeButtonHint),
        ui(new Ui::CreateAssetDialog),
        platformStyle(_platformStyle)
{
    ui->setupUi(this);
    setWindowTitle("Create Assets");

    // Address and label are shown only when coin control is enabled (set in setModel).
    // Hide them here so they are never visible before setModel is called.
    ui->addressText->hide();
    ui->addressLabel->hide();

    if (!IsAvianNameSystemDeployed()) {
        ui->ansBox->hide();
        ui->ansType->hide();
        ui->ansText->hide();
    }

    connect(ui->ipfsBox, SIGNAL(clicked()), this, SLOT(ipfsStateChanged()));
    connect(ui->openIpfsButton, SIGNAL(clicked()), this, SLOT(openIpfsBrowser()));
    connect(ui->ansBox, SIGNAL(clicked()), this, SLOT(ansStateChanged()));
    connect(ui->availabilityButton, SIGNAL(clicked()), this, SLOT(checkAvailabilityClicked()));
    connect(ui->nameText, SIGNAL(textChanged(QString)), this, SLOT(onNameChanged(QString)));
    connect(ui->addressText, SIGNAL(textChanged(QString)), this, SLOT(onAddressNameChanged(QString)));
    connect(ui->addressText, &QLineEdit::editingFinished, this, &CreateAssetDialog::onAddressEditingFinished);
    connect(ui->ipfsText, SIGNAL(textChanged(QString)), this, SLOT(onIPFSHashChanged(QString)));
    connect(ui->ansText, SIGNAL(textChanged(QString)), this, SLOT(onANSDataChanged(QString)));
    connect(ui->ansType, SIGNAL(currentIndexChanged(int)), this, SLOT(onANSTypeChanged(int)));
    connect(ui->ansCborAddrEdit, SIGNAL(textChanged(QString)), this, SLOT(onANSDataChanged(QString)));
    connect(ui->ansCborNameEdit, SIGNAL(textChanged(QString)), this, SLOT(onANSDataChanged(QString)));
    connect(ui->ansCborAvatarEdit, SIGNAL(textChanged(QString)), this, SLOT(onANSDataChanged(QString)));
    connect(ui->ansCborBannerEdit, SIGNAL(textChanged(QString)), this, SLOT(onANSDataChanged(QString)));
    connect(ui->ansCborUrlEdit, SIGNAL(textChanged(QString)), this, SLOT(onANSDataChanged(QString)));
    connect(ui->createAssetButton, SIGNAL(clicked()), this, SLOT(onCreateAssetClicked()));
    connect(ui->unitBox, SIGNAL(valueChanged(int)), this, SLOT(onUnitChanged(int)));
    connect(ui->assetType, SIGNAL(activated(int)), this, SLOT(onAssetTypeActivated(int)));
    connect(ui->assetList, SIGNAL(activated(int)), this, SLOT(onAssetListActivated(int)));
    connect(ui->clearButton, SIGNAL(clicked()), this, SLOT(onClearButtonClicked()));
    connect(ui->lineEditVerifierString, SIGNAL(textChanged(QString)), this, SLOT(onVerifierStringChanged(QString)));

    GUIUtil::setupAddressWidget(ui->lineEditCoinControlChange, this);

    // Coin Control
    connect(ui->pushButtonCoinControl, SIGNAL(clicked()), this, SLOT(coinControlButtonClicked()));
    connect(ui->checkBoxCoinControlChange, SIGNAL(stateChanged(int)), this, SLOT(coinControlChangeChecked(int)));
    connect(ui->lineEditCoinControlChange, SIGNAL(textEdited(const QString &)), this, SLOT(coinControlChangeEdited(const QString &)));

    // Coin Control: clipboard actions
    QAction *clipboardQuantityAction = new QAction(tr("Copy quantity"), this);
    QAction *clipboardAmountAction = new QAction(tr("Copy amount"), this);
    QAction *clipboardFeeAction = new QAction(tr("Copy fee"), this);
    QAction *clipboardAfterFeeAction = new QAction(tr("Copy after fee"), this);
    QAction *clipboardBytesAction = new QAction(tr("Copy bytes"), this);
    QAction *clipboardLowOutputAction = new QAction(tr("Copy dust"), this);
    QAction *clipboardChangeAction = new QAction(tr("Copy change"), this);
    connect(clipboardQuantityAction, SIGNAL(triggered()), this, SLOT(coinControlClipboardQuantity()));
    connect(clipboardAmountAction, SIGNAL(triggered()), this, SLOT(coinControlClipboardAmount()));
    connect(clipboardFeeAction, SIGNAL(triggered()), this, SLOT(coinControlClipboardFee()));
    connect(clipboardAfterFeeAction, SIGNAL(triggered()), this, SLOT(coinControlClipboardAfterFee()));
    connect(clipboardBytesAction, SIGNAL(triggered()), this, SLOT(coinControlClipboardBytes()));
    connect(clipboardLowOutputAction, SIGNAL(triggered()), this, SLOT(coinControlClipboardLowOutput()));
    connect(clipboardChangeAction, SIGNAL(triggered()), this, SLOT(coinControlClipboardChange()));
    ui->labelCoinControlQuantity->addAction(clipboardQuantityAction);
    ui->labelCoinControlAmount->addAction(clipboardAmountAction);
    ui->labelCoinControlFee->addAction(clipboardFeeAction);
    ui->labelCoinControlAfterFee->addAction(clipboardAfterFeeAction);
    ui->labelCoinControlBytes->addAction(clipboardBytesAction);
    ui->labelCoinControlLowOutput->addAction(clipboardLowOutputAction);
    ui->labelCoinControlChange->addAction(clipboardChangeAction);

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
        settings.setValue("nTransactionFee", (qint64)CAmount(10000));
    if (!settings.contains("fPayOnlyMinFee"))
        settings.setValue("fPayOnlyMinFee", false);
    ui->groupFee->setId(ui->radioSmartFee, 0);
    ui->groupFee->setId(ui->radioCustomFee, 1);
    ui->groupFee->button((int)std::max(0, std::min(1, settings.value("nFeeRadio").toInt())))->setChecked(true);
    ui->customFee->setValue(settings.value("nTransactionFee").toLongLong());
    ui->checkBoxMinimumFee->setChecked(settings.value("fPayOnlyMinFee").toBool());
    minimizeFeeSection(settings.value("fFeeSectionMinimized").toBool());

    format = "%1<font color=green>%2%3</font>";

    setupCoinControlFrame(platformStyle);
    setupAssetDataView(platformStyle);
    setupFeeControl(platformStyle);

    /** Setup the asset list combobox */
    stringModel = new QStringListModel;

    proxy = new QSortFilterProxyModel;
    proxy->setSourceModel(stringModel);
    proxy->setFilterCaseSensitivity(Qt::CaseInsensitive);

    ui->assetList->setModel(proxy);
    ui->assetList->setEditable(true);
    ui->assetList->lineEdit()->setPlaceholderText("Select an asset");
    ui->assetList->lineEdit()->setStyleSheet("background: transparent;");


    completer = new QCompleter(proxy,this);
    completer->setCompletionMode(QCompleter::PopupCompletion);
    completer->setCaseSensitivity(Qt::CaseInsensitive);
    ui->assetList->setCompleter(completer);

    ui->nameText->installEventFilter(this);
    ui->assetList->installEventFilter(this);
    ui->lineEditVerifierString->installEventFilter(this);
}

void CreateAssetDialog::setClientModel(ClientModel *_clientModel)
{
    this->clientModel = _clientModel;

    if (_clientModel) {
        connect(_clientModel, SIGNAL(numBlocksChanged(int,QDateTime,double,bool)), this, SLOT(updateSmartFeeLabel()));
    }
}

void CreateAssetDialog::setModel(WalletModel *_model)
{
    this->model = _model;

    if(_model && _model->getOptionsModel())
    {
        setBalance(_model->getCachedBalance());
        connect(_model, &WalletModel::balanceChanged, this, &CreateAssetDialog::setBalance);
        connect(_model->getOptionsModel(), &OptionsModel::displayUnitChanged, this, &CreateAssetDialog::updateDisplayUnit);
        updateDisplayUnit();

        // Coin Control
        connect(_model->getOptionsModel(), &OptionsModel::displayUnitChanged, this, &CreateAssetDialog::coinControlUpdateLabels);
        connect(_model->getOptionsModel(), &OptionsModel::coinControlFeaturesChanged, this, &CreateAssetDialog::coinControlFeatureChanged);
        bool fCoinControlEnabled = _model->getOptionsModel()->getCoinControlFeatures();
        ui->frameCoinControl->setVisible(fCoinControlEnabled);
        ui->addressText->setVisible(fCoinControlEnabled);
        ui->addressLabel->setVisible(fCoinControlEnabled);
        coinControlUpdateLabels();

        // Custom Fee Control
        ui->frameFee->setVisible(true);

        // fee section
        for (const int &n : confTargets) {
            ui->confTargetSelector->addItem(tr("%1 (%2 blocks)").arg(GUIUtil::formatNiceTimeOffset(n * Params().GetConsensus().nPowTargetSpacing)).arg(n));
        }
        connect(ui->confTargetSelector, SIGNAL(currentIndexChanged(int)), this, SLOT(updateSmartFeeLabel()));
        connect(ui->confTargetSelector, SIGNAL(currentIndexChanged(int)), this, SLOT(coinControlUpdateLabels()));

        connect(ui->groupFee, &QButtonGroup::idClicked, this, &CreateAssetDialog::updateFeeSectionControls);
        connect(ui->groupFee, &QButtonGroup::idClicked, this, &CreateAssetDialog::coinControlUpdateLabels);
        connect(ui->customFee, SIGNAL(valueChanged()), this, SLOT(coinControlUpdateLabels()));
        connect(ui->checkBoxMinimumFee, SIGNAL(stateChanged(int)), this, SLOT(setMinimumFee()));
        connect(ui->checkBoxMinimumFee, SIGNAL(stateChanged(int)), this, SLOT(updateFeeSectionControls()));
        connect(ui->checkBoxMinimumFee, SIGNAL(stateChanged(int)), this, SLOT(coinControlUpdateLabels()));
//        connect(ui->optInRBF, SIGNAL(stateChanged(int)), this, SLOT(updateSmartFeeLabel()));
//        connect(ui->optInRBF, SIGNAL(stateChanged(int)), this, SLOT(coinControlUpdateLabels()));
        CAmount requiredFee = model->wallet().getRequiredFee(1000);
        ui->customFee->setSingleStep(requiredFee);
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
            ui->confTargetSelector->setCurrentIndex(getIndexForConfTarget(6));
        else
            ui->confTargetSelector->setCurrentIndex(getIndexForConfTarget(settings.value("nConfTarget").toInt()));


        // Setup the default values
        setUpValues();

        restrictedAssetNotSelected();

        adjustSize();
    }
}


CreateAssetDialog::~CreateAssetDialog()
{
    delete ui;
}

bool CreateAssetDialog::eventFilter(QObject *sender, QEvent *event)
{
    if (sender == ui->nameText)
    {
        if(event->type()== QEvent::FocusIn)
        {
            ui->nameText->setStyleSheet("");
        }
    }
    else if (sender == ui->assetList)
    {
        if(event->type()== QEvent::FocusIn)
        {
            ui->assetList->lineEdit()->setStyleSheet("border: none; background: transparent;");
        }
    } else if (sender == ui->lineEditVerifierString)
    {
        if(event->type()== QEvent::FocusIn)
        {
            hideInvalidVerifierStringMessage();
        }
    }
    return QWidget::eventFilter(sender,event);
}

/** Helper Methods */
void CreateAssetDialog::setUpValues()
{
    ui->unitBox->setValue(0);
    ui->reissuableBox->setCheckState(Qt::CheckState::Checked);
    ui->ipfsText->hide();
    ui->ansText->hide();
    ui->ansType->hide();
    ui->ansCborWidget->hide();
    if (!IsAvianNameSystemDeployed()) {
        ui->ansBox->hide();
    } else {
        // Visible but disabled until a .AVN name is typed for ROOT type
        ui->ansBox->setDisabled(true);
    }
    ui->openIpfsButton->hide();
    ui->openIpfsButton->setDisabled(true);
    hideMessage();
    CheckFormState();
    ui->availabilityButton->setDisabled(true);

    ui->unitExampleLabel->setStyleSheet("font-weight: bold");

    // Setup the asset types
    QStringList list;
    list.append(tr("Main Asset") + " (" + BitcoinUnits::formatWithUnit(model->getOptionsModel()->getDisplayUnit(), GetBurnAmount(AssetType::ROOT)) + ")");
    list.append(tr("Sub Asset") + " (" + BitcoinUnits::formatWithUnit(model->getOptionsModel()->getDisplayUnit(), GetBurnAmount(AssetType::SUB)) + ")");
    list.append(tr("Unique Asset") + " (" + BitcoinUnits::formatWithUnit(model->getOptionsModel()->getDisplayUnit(), GetBurnAmount(AssetType::UNIQUE)) + ")");
    list.append(tr("Messaging Channel Asset") + " (" + BitcoinUnits::formatWithUnit(model->getOptionsModel()->getDisplayUnit(), GetBurnAmount(AssetType::MSGCHANNEL)) + ")");
    list.append(tr("Qualifier Asset") + " (" + BitcoinUnits::formatWithUnit(model->getOptionsModel()->getDisplayUnit(), GetBurnAmount(AssetType::QUALIFIER)) + ")");
    list.append(tr("Sub Qualifier Asset") + " (" + BitcoinUnits::formatWithUnit(model->getOptionsModel()->getDisplayUnit(), GetBurnAmount(AssetType::SUB_QUALIFIER)) + ")");
    list.append(tr("Restricted Asset") + " (" + BitcoinUnits::formatWithUnit(model->getOptionsModel()->getDisplayUnit(), GetBurnAmount(AssetType::RESTRICTED)) + ")");
    if (IsAvianNameSystemDeployed()) {
        list.append(tr("ANS Asset") + " (" + BitcoinUnits::formatWithUnit(model->getOptionsModel()->getDisplayUnit(), GetBurnAmount(AssetType::ROOT)) + ")");
    }

    ui->assetType->addItems(list);
    type = IntFromAssetType(AssetType::ROOT);
    ui->assetTypeLabel->setText(tr("Asset Type") + ":");

    // Setup the asset list
    ui->assetList->hide();
    updateAssetList();

    ui->assetFullName->setTextFormat(Qt::RichText);
    ui->assetFullName->setStyleSheet("font-weight: bold");

    //ui->assetType->setStyleSheet("font-weight: bold;");

    // Setup ANS types
    QStringList listTypes;
    for (const auto type : ANSTypes)
        listTypes.append(QString::fromStdString(CAvianNameSystemID::enum_to_string(type).first));

    ui->ansType->addItems(listTypes);
}

void CreateAssetDialog::setupCoinControlFrame(const PlatformStyle *platformStyle)
{
    /** Create the shadow effects on the frames */
    ui->frameCoinControl->setGraphicsEffect(GUIUtil::getShadowEffect());
}

void CreateAssetDialog::setupAssetDataView(const PlatformStyle *platformStyle)
{
    /** Update the scrollview*/
    ui->frameAssetData->setGraphicsEffect(GUIUtil::getShadowEffect());
}

void CreateAssetDialog::setupFeeControl(const PlatformStyle *platformStyle)
{
    /** Create the shadow effects on the frames */
    ui->frameFee->setStyleSheet(QString(".QFrame#frameFee { border-top: 2px solid %1;padding-top: 20px}").arg(platformStyle->Avian_2B737F().name()));
    //ui->frameFee->setGraphicsEffect(GUIUtil::getShadowEffect());
}

void CreateAssetDialog::setBalance(const interfaces::WalletBalances& balances)
{
    if(model && model->getOptionsModel())
    {
        ui->labelBalance->setText(BitcoinUnits::formatWithUnit(model->getOptionsModel()->getDisplayUnit(), balances.balance));
    }
}

void CreateAssetDialog::updateDisplayUnit()
{
    setBalance(model->getCachedBalance());
    ui->customFee->setDisplayUnit(model->getOptionsModel()->getDisplayUnit());
    updateMinFeeLabel();
    updateSmartFeeLabel();
}

void CreateAssetDialog::toggleIPFSText()
{
    if (ui->ipfsBox->isChecked()) {
        ui->ipfsText->show();
        ui->openIpfsButton->show();
    } else {
        ui->openIpfsButton->hide();
        ui->ipfsText->hide();
        ui->ipfsText->clear();
    }
}

void CreateAssetDialog::toggleANSText()
{
    if (ui->ansBox->isChecked()) {
        ui->ansType->show();
        CAvianNameSystemID::Type type = currentANSType();
        if (type == CAvianNameSystemID::PROFILE) {
            ui->ansText->hide();
            ui->ansCborWidget->show();
        } else {
            ui->ansText->show();
            ui->ansCborWidget->hide();
        }
    } else {
        ui->ansType->hide();
        ui->ansText->hide();
        ui->ansCborWidget->hide();
        ui->ansText->clear();
    }
}

CAvianNameSystemID::Type CreateAssetDialog::currentANSType() const
{
    int idx = ui->ansType->currentIndex();
    if (m_ansSubMode) {
        if (idx >= 0 && idx < (int)ANSSubTypes.size())
            return ANSSubTypes[idx];
        return CAvianNameSystemID::XADDR;
    }
    if (idx >= 0 && idx < (int)ANSTypes.size())
        return ANSTypes[idx];
    return CAvianNameSystemID::ADDR;
}

void CreateAssetDialog::setANSSubMode(bool subMode)
{
    if (m_ansSubMode == subMode) return;
    m_ansSubMode = subMode;

    // Repopulate the ANS type combo for the new mode
    ui->ansType->blockSignals(true);
    ui->ansType->clear();
    if (subMode) {
        for (const auto t : ANSSubTypes)
            ui->ansType->addItem(QString::fromStdString(CAvianNameSystemID::enum_to_string(t).first));
    } else {
        for (const auto t : ANSTypes)
            ui->ansType->addItem(QString::fromStdString(CAvianNameSystemID::enum_to_string(t).first));
    }
    ui->ansType->setCurrentIndex(0);
    ui->ansType->blockSignals(false);
}

void CreateAssetDialog::showMessage(QString string)
{
    ui->messageLabel->setStyleSheet("color: red; font-size: 15pt;font-weight: bold;");
    ui->messageLabel->setText(string);
    ui->messageLabel->show();
}

void CreateAssetDialog::showValidMessage(QString string)
{
    ui->messageLabel->setStyleSheet("color: green; font-size: 15pt;font-weight: bold;");
    ui->messageLabel->setText(string);
    ui->messageLabel->show();
}

void CreateAssetDialog::hideMessage()
{
    ui->nameText->setStyleSheet("");
    ui->addressText->setStyleSheet("");
    if (ui->ipfsBox->isChecked())
        ui->ipfsText->setStyleSheet("");
    if (ui->ansBox->isChecked())
        ui->ansText->setStyleSheet("");

    ui->messageLabel->hide();
}

void CreateAssetDialog::showInvalidVerifierStringMessage(QString string)
{
    ui->lineEditVerifierString->setStyleSheet(STYLE_INVALID);
    ui->labelVerifierStringErrorMessage->setStyleSheet("color: red; font-size: 15pt;font-weight: bold;");
    ui->labelVerifierStringErrorMessage->setText(string);
    ui->labelVerifierStringErrorMessage->show();
}

void CreateAssetDialog::hideInvalidVerifierStringMessage()
{
    ui->lineEditVerifierString->setStyleSheet(STYLE_VALID);
    ui->labelVerifierStringErrorMessage->clear();
    ui->labelVerifierStringErrorMessage->hide();
}

void CreateAssetDialog::disableCreateButton()
{
    ui->createAssetButton->setDisabled(true);
}

void CreateAssetDialog::enableCreateButton()
{
    if (checkedAvailablity)
        ui->createAssetButton->setDisabled(false);
}

bool CreateAssetDialog::checkIPFSHash(QString hash)
{
    ui->openIpfsButton->setDisabled(true);

    if (!hash.isEmpty()) {
        std::string error;
        // Do not allow ANS in IPFS
        bool isANS = (hash.toStdString().substr(0, CAvianNameSystemID::prefix.length()) == CAvianNameSystemID::prefix);
        if (!CheckEncoded(DecodeAssetData(hash.toStdString()), error) && !isANS) {
            ui->ipfsText->setStyleSheet("border: 2px solid red");
            showMessage(tr("IPFS must be 46 characters (starting with 'Qm') or Txid must be 64 hex characters"));
            disableCreateButton();
            return false;
        }
        else if (hash.size() != 46 && hash.size() != 64) {
            ui->ipfsText->setStyleSheet("border: 2px solid red");
            showMessage(tr("IPFS Hash must be 46 characters or Txid must be 64 characters"));
            disableCreateButton();
            return false;
        } else if (DecodeAssetData(hash.toStdString()).empty()) {
            showMessage(tr("IPFS/Txid hash is not valid. Please use a valid IPFS/Txid hash"));
            disableCreateButton();
            return false;
        }
    }

    // No problems where found with the hash, reset the border, and hide the messages.
    hideMessage();
    ui->ipfsText->setStyleSheet("");
    ui->openIpfsButton->setDisabled(false);

    return true;
}

void CreateAssetDialog::CheckFormState()
{
    disableCreateButton(); // Disable the button by default
    hideMessage();
    ui->openIpfsButton->setDisabled(true);
    ui->availabilityButton->setDisabled(true);

    const CTxDestination dest = DecodeDestination(ui->addressText->text().toStdString());

    QString name = GetAssetName();

    std::string error;
    AssetType effectiveAssetType = (type == ANS_ASSET_TYPE_INDEX) ? AssetType::ROOT : AssetTypeFromInt(type);
    bool assetNameValid = IsTypeCheckNameValid(effectiveAssetType, name.toStdString(), error);

    if (type != IntFromAssetType(AssetType::ROOT) && type != IntFromAssetType(AssetType::QUALIFIER) && type != IntFromAssetType(AssetType::RESTRICTED) && type != ANS_ASSET_TYPE_INDEX) {
        if (ui->assetList->currentText() == "")
        {
            ui->assetList->lineEdit()->setStyleSheet(STYLE_INVALID);
            ui->availabilityButton->setDisabled(true);
            return;
        }
    }

    if (!assetNameValid && name.size() > 2) {
        ui->nameText->setStyleSheet(STYLE_INVALID);
        showMessage(error.c_str());
        ui->availabilityButton->setDisabled(true);
        return;
    }

    if (!(ui->addressText->text().isEmpty() || IsValidDestination(dest)) && assetNameValid) {
        ui->addressText->setStyleSheet(STYLE_INVALID);
        showMessage(tr("Warning: Invalid Avian address"));
        return;
    }

    if (!ui->addressText->text().isEmpty() && IsValidDestination(dest) && std::get_if<PKHash>(&dest) == nullptr) {
        ui->addressText->setStyleSheet(STYLE_INVALID);
        showMessage(tr("Warning: Address must use legacy (P2PKH) format. SegWit and bech32 addresses are not supported for asset operations."));
        return;
    }

    if (type == IntFromAssetType(AssetType::RESTRICTED)) {

        QString qVerifier = ui->lineEditVerifierString->text();
        std::string strVerifier = qVerifier.toStdString();

        std::string strippedVerifier = GetStrippedVerifierString(strVerifier);

        if (!strVerifier.empty()) {
            // A valid address must be given
            QString qAddress = ui->addressText->text();
            std::string strAddress = qAddress.toStdString();

            if (strAddress.empty()) {
                ui->addressText->setStyleSheet(STYLE_INVALID);
                showMessage(tr("Warning: Restricted Assets Reissuance requires an address"));
                return;
            } else if (!IsValidDestination(dest)) {
                ui->addressText->setStyleSheet(STYLE_INVALID);
                showMessage(tr("Warning: Invalid Avian address"));
                return;
            }

            // Check the verifier string
            std::string strError;
            ErrorReport errorReport;
            errorReport.type = ErrorReport::ErrorType::NotSetError;
            if (!ContextualCheckVerifierString(passets, strippedVerifier, strAddress, strError, &errorReport)) {
                ui->lineEditVerifierString->setStyleSheet(STYLE_INVALID);
                showInvalidVerifierStringMessage(QString::fromStdString(GetUserErrorString(errorReport)));
                return;
            } else {
                hideInvalidVerifierStringMessage();
            }
        }
    }

    if (ui->ipfsBox->isChecked())
        if (!checkIPFSHash(ui->ipfsText->text()))
            return;

    if (ui->ansBox->isChecked()) {
        CAvianNameSystemID::Type ansType = currentANSType();
        std::string typeData;
        bool hasData = false;
        if (ansType == CAvianNameSystemID::PROFILE) {
            ANSProfileData p;
            p.addr   = ui->ansCborAddrEdit->text().toStdString();
            p.name   = ui->ansCborNameEdit->text().toStdString();
            p.avatar = ui->ansCborAvatarEdit->text().toStdString();
            p.banner = ui->ansCborBannerEdit->text().toStdString();
            p.url    = ui->ansCborUrlEdit->text().toStdString();
            hasData = !(p.addr.empty() && p.name.empty() && p.avatar.empty() && p.banner.empty() && p.url.empty());
            if (hasData) typeData = ANS_CBOR::EncodeProfile(p);
        } else {
            hasData = !ui->ansText->text().isEmpty();
            if (hasData) typeData = ui->ansText->text().toStdString();
        }
        if (hasData) {
            std::string error;
            std::string formattedTypeData = CAvianNameSystemID::FormatTypeData(ansType, typeData, error);
            if (error != "") {
                if (ansType != CAvianNameSystemID::PROFILE)
                    ui->ansText->setStyleSheet("border: 2px solid red");
                showMessage(QString::fromStdString(error));
                disableCreateButton();
                return;
            }
            CAvianNameSystemID ans(ansType, formattedTypeData);
            if (!IsAvianNameSystemDeployed()) {
                showMessage(tr("ANS not deployed yet."));
                disableCreateButton();
                return;
            }
            if (!CAvianNameSystemID::IsValidID(ans.to_string())) {
                if (ansType != CAvianNameSystemID::PROFILE)
                    ui->ansText->setStyleSheet("border: 2px solid red");
                showMessage(tr("Invalid ANS data."));
                disableCreateButton();
                return;
            } else {
                ui->ansText->setStyleSheet("");
                hideMessage();
                enableCreateButton();
            }
        }
    }

    if (checkedAvailablity) {
        showValidMessage(tr("Valid Asset"));
        enableCreateButton();
        ui->availabilityButton->setDisabled(true);
    } else {
        disableCreateButton();
        ui->availabilityButton->setDisabled(false);
    }
}

/** SLOTS */
void CreateAssetDialog::ipfsStateChanged()
{
    toggleIPFSText();
}

void CreateAssetDialog::ansStateChanged()
{
    toggleANSText();
}

void CreateAssetDialog::checkAvailabilityClicked()
{
    QString name = GetAssetName();

    LOCK(cs_main);
    auto currentActiveAssetCache = GetCurrentAssetCache();
    if (currentActiveAssetCache) {
        CNewAsset asset;
        if (currentActiveAssetCache->GetAssetMetaDataIfExists(name.toStdString(), asset)) {
            ui->nameText->setStyleSheet(STYLE_INVALID);
            showMessage(tr("Invalid: Asset name already in use"));
            disableCreateButton();
            checkedAvailablity = false;
            return;
        } else {
            qDebug() << "set to true";
            checkedAvailablity = true;
            ui->nameText->setStyleSheet(STYLE_VALID);
        }
    } else {
        checkedAvailablity = false;
        showMessage(tr("Error: Asset Database not in sync"));
        disableCreateButton();
        return;
    }

    CheckFormState();
}

void CreateAssetDialog::openIpfsBrowser()
{
    QString ipfshash = ui->ipfsText->text();
    QString ipfsbrowser = QString("https://ipfs.avn.network/ipfs/");

    // If the ipfs hash isn't there or doesn't start with Qm, disable the action item
    if (ipfshash.size() > 0 && ipfshash.indexOf("Qm") == 0 && ipfsbrowser.indexOf("http") == 0)
    {
        QUrl ipfsurl = QUrl::fromUserInput(ipfsbrowser.replace("%s", ipfshash));

        // Create the box with everything.
        if(QMessageBox::Yes == QMessageBox::question(this,
                                                        tr("Open IPFS content?"),
                                                        tr("Open the following IPFS content in your default browser?\n")
                                                        + ipfsurl.toString()
                                                    ))
        QDesktopServices::openUrl(ipfsurl);
    }
}

void CreateAssetDialog::onNameChanged(QString name)
{
    // Update the displayed name to uppercase if the type only accepts uppercase
    name = type == IntFromAssetType(AssetType::UNIQUE) ? name : name.toUpper();
    UpdateAssetNameToUpper();

    QString assetName = name;

    // Get the identifier for the asset type
    QString identifier = GetSpecialCharacter();

    if (name.size() == 0) {
        hideMessage();
        ui->availabilityButton->setDisabled(true);
        updatePresentedAssetName(name);
        return;
    }

    if (type == IntFromAssetType(AssetType::ROOT)) {
        std::string error;
        auto strName = GetAssetName();
        if (IsTypeCheckNameValid(AssetType::ROOT, strName.toStdString(), error)) {
            hideMessage();
            ui->availabilityButton->setDisabled(false);
            // Enable ANS checkbox only when the name ends in .AVN
            if (IsAvianNameSystemDeployed()) {
                const std::string& domain = CAvianNameSystemID::domain;
                std::string sn = strName.toStdString();
                bool endsWithAVN = sn.size() > domain.size() &&
                                   sn.substr(sn.size() - domain.size()) == domain;
                if (!endsWithAVN && ui->ansBox->isChecked()) {
                    ui->ansBox->setChecked(false);
                    toggleANSText();
                    // Restore free qty/units when name no longer ends in .AVN
                    ui->quantitySpinBox->setMaximum(21000000000);
                    ui->quantitySpinBox->setDisabled(false);
                    ui->unitBox->setDisabled(false);
                }
                ui->ansBox->setDisabled(true); // always locked: checked for .AVN (required), unchecked otherwise
                // Auto-check ansBox and lock qty=1/units=0 when name ends in .AVN
                if (endsWithAVN) {
                    if (!ui->ansBox->isChecked()) {
                        ui->ansBox->setChecked(true);
                        toggleANSText();
                    }
                    ui->quantitySpinBox->setValue(1);
                    ui->quantitySpinBox->setDisabled(true);
                    ui->unitBox->setValue(0);
                    ui->unitBox->setDisabled(true);
                }
            }
        } else {
            ui->nameText->setStyleSheet(STYLE_INVALID);
            showMessage(tr(error.c_str()));
            ui->availabilityButton->setDisabled(true);
            if (IsAvianNameSystemDeployed()) {
                if (ui->ansBox->isChecked()) {
                    ui->ansBox->setChecked(false);
                    toggleANSText();
                }
                ui->ansBox->setDisabled(true);
                // Restore free qty/units when name becomes invalid
                ui->quantitySpinBox->setMaximum(21000000000);
                ui->quantitySpinBox->setDisabled(false);
                ui->unitBox->setDisabled(false);
            }
        }
    } else if (type == ANS_ASSET_TYPE_INDEX) {
        std::string error;
        auto strName = GetAssetName(); // includes the .AVN suffix
        if (IsTypeCheckNameValid(AssetType::ROOT, strName.toStdString(), error)) {
            // UI-only: block reserved base names (e.g. AVN.AVN, RVN.AVN).
            // These are NOT blocked at consensus level to preserve pre-ANS assets on Mainnet.
            const std::string& domain = CAvianNameSystemID::domain;
            std::string sn = strName.toStdString();
            std::string baseName = sn.substr(0, sn.size() - domain.size());
            static const std::regex RESERVED_ANS_BASE("^(RVN|AVN|AVIAN)$");
            if (std::regex_match(baseName, RESERVED_ANS_BASE)) {
                ui->nameText->setStyleSheet(STYLE_INVALID);
                showMessage(tr("Asset name '%1' is reserved.").arg(QString::fromStdString(sn)));
                ui->availabilityButton->setDisabled(true);
            } else {
                hideMessage();
                ui->availabilityButton->setDisabled(false);
            }
        } else {
            ui->nameText->setStyleSheet(STYLE_INVALID);
            showMessage(tr(error.c_str()));
            ui->availabilityButton->setDisabled(true);
        }
    } else if (type == IntFromAssetType(AssetType::SUB) || type == IntFromAssetType(AssetType::UNIQUE) || type == IntFromAssetType(AssetType::MSGCHANNEL)) {
        if (name.size() == 0) {
            hideMessage();
            ui->availabilityButton->setDisabled(true);
        }

        // If an asset isn't selected. Mark the lineedit with invalid style sheet
        if (ui->assetList->currentText() == "")
        {
            ui->assetList->lineEdit()->setStyleSheet(STYLE_INVALID);
            ui->availabilityButton->setDisabled(true);
            return;
        }

        std::string error;
        auto assetType = AssetTypeFromInt(type);
        auto strName = GetAssetName();
        if (IsTypeCheckNameValid(assetType, strName.toStdString(), error)) {
            hideMessage();
            ui->availabilityButton->setDisabled(false);

            // AIP-0010: enable ANS (XADDR mode) for sub-assets of .AVN names
            if (IsAvianNameSystemDeployed() && type == IntFromAssetType(AssetType::SUB)) {
                const std::string& domain = CAvianNameSystemID::domain;
                std::string parentStr = ui->assetList->currentText().toStdString();
                bool parentIsAVN = parentStr.size() > domain.size() &&
                                   parentStr.substr(parentStr.size() - domain.size()) == domain;
                if (parentIsAVN) {
                    setANSSubMode(true);
                    ui->ansBox->setDisabled(false);
                    ui->ansBox->show();
                } else {
                    if (ui->ansBox->isChecked()) {
                        ui->ansBox->setChecked(false);
                        toggleANSText();
                    }
                    setANSSubMode(false);
                    ui->ansBox->setDisabled(true);
                    ui->ansBox->hide();
                }
            }
        } else {
            ui->nameText->setStyleSheet(STYLE_INVALID);
            showMessage(tr(error.c_str()));
            ui->availabilityButton->setDisabled(true);
            if (IsAvianNameSystemDeployed() && type == IntFromAssetType(AssetType::SUB)) {
                if (ui->ansBox->isChecked()) {
                    ui->ansBox->setChecked(false);
                    toggleANSText();
                }
                setANSSubMode(false);
                ui->ansBox->setDisabled(true);
                ui->ansBox->hide();
            }
        }
    } else if (type == IntFromAssetType(AssetType::QUALIFIER) || type == IntFromAssetType(AssetType::SUB_QUALIFIER)) {
        if (name.size() == 0) {
            hideMessage();
            ui->availabilityButton->setDisabled(true);
        }

        if (type == IntFromAssetType(AssetType::SUB_QUALIFIER)) { // If an asset isn't selected. Mark the lineedit with invalid style sheet
            if (ui->assetList->currentText() == "") {
                ui->assetList->lineEdit()->setStyleSheet(STYLE_INVALID);
                ui->availabilityButton->setDisabled(true);
                return;
            }
        }

        std::string error;
        auto assetType = AssetTypeFromInt(type);
        auto strName = GetAssetName();
        if (IsTypeCheckNameValid(assetType, strName.toStdString(), error)) {
            hideMessage();
            ui->availabilityButton->setDisabled(false);
        } else {
            ui->nameText->setStyleSheet(STYLE_INVALID);
            showMessage(tr(error.c_str()));
            ui->availabilityButton->setDisabled(true);
        }

    }

    // Set the assetName
    QString displayName = (type == ANS_ASSET_TYPE_INDEX) ? name + QString::fromStdString(CAvianNameSystemID::domain) : name;
    updatePresentedAssetName(format.arg((type == IntFromAssetType(AssetType::ROOT) || type == ANS_ASSET_TYPE_INDEX) ? "" : ui->assetList->currentText(), identifier, displayName));

    checkedAvailablity = false;
    disableCreateButton();
}

void CreateAssetDialog::onAddressEditingFinished()
{
    const QString text = ui->addressText->text().trimmed();
    const std::string& domain = CAvianNameSystemID::domain;
    const QString domainQ = QString::fromStdString(domain);
    if (!text.toUpper().endsWith(domainQ) || text.length() <= (int)domain.size())
        return;
    if (!passets)
        return;

    LOCK(cs_main);

    const std::string assetName = text.toUpper().toStdString();
    CNewAsset resolvedAsset;
    if (!passets->GetAssetMetaDataIfExists(assetName, resolvedAsset))
        return;

    std::string resolvedAddr;

    if (IsAvianNameSystemDeployed() && resolvedAsset.nHasANS && !resolvedAsset.strANSID.empty()) {
        CAvianNameSystemID ansID(resolvedAsset.strANSID);
        if (ansID.type() == CAvianNameSystemID::ADDR)
            resolvedAddr = ansID.addr();
        else if (ansID.type() == CAvianNameSystemID::PROFILE && !ansID.profile().addr.empty())
            resolvedAddr = ansID.profile().addr;
    }

    // Fallback: use owner token holder address
    if (resolvedAddr.empty() && fAssetIndex && passetsdb) {
        std::string ownerToken = assetName + "!";
        std::vector<std::pair<std::string, CAmount>> ownerAddrs;
        int dbTotal = 0;
        if (passetsdb->AssetAddressDir(ownerAddrs, dbTotal, false, ownerToken, 1, 0) && !ownerAddrs.empty())
            resolvedAddr = ownerAddrs[0].first;
    }

    if (resolvedAddr.empty())
        return;

    ui->addressText->setText(QString::fromStdString(resolvedAddr));
}

void CreateAssetDialog::onAddressNameChanged(QString address)
{
    CheckFormState();
}

void CreateAssetDialog::onVerifierStringChanged(QString verifier)
{
    CheckFormState();
}

void CreateAssetDialog::onIPFSHashChanged(QString hash)
{
    if (checkIPFSHash(hash))
        CheckFormState();
}

void CreateAssetDialog::onANSTypeChanged(int index) {
    CAvianNameSystemID::Type type = currentANSType();
    if (type == CAvianNameSystemID::PROFILE) {
        ui->ansText->hide();
        ui->ansText->clear();
        ui->ansCborWidget->show();
    } else {
        ui->ansText->setPlaceholderText(QString::fromStdString(CAvianNameSystemID::enum_to_string(type).second));
        ui->ansText->clear();
        ui->ansText->show();
        ui->ansCborWidget->hide();
    }
    CheckFormState();
}

void CreateAssetDialog::onANSDataChanged(QString data)
{
    CheckFormState();
}

void CreateAssetDialog::onCreateAssetClicked()
{
    WalletModel::UnlockContext ctx(model->requestUnlock());
    if(!ctx.isValid())
    {
        // Unlock wallet was cancelled
        return;
    }

    QString name = GetAssetName();
    CAmount quantity = ui->quantitySpinBox->value() * COIN;
    int units = ui->unitBox->value();
    bool reissuable = ui->reissuableBox->isChecked();
    bool hasIPFS = ui->ipfsBox->isChecked() && !ui->ipfsText->text().isEmpty();
    CAvianNameSystemID::Type ansType = currentANSType();
    bool hasANS = false;
    if (ui->ansBox->isChecked()) {
        if (ansType == CAvianNameSystemID::PROFILE) {
            hasANS = !ui->ansCborAddrEdit->text().isEmpty() || !ui->ansCborNameEdit->text().isEmpty() ||
                     !ui->ansCborAvatarEdit->text().isEmpty() || !ui->ansCborBannerEdit->text().isEmpty() ||
                     !ui->ansCborUrlEdit->text().isEmpty();
        } else {
            hasANS = !ui->ansText->text().isEmpty();
        }
    }

    std::string ipfsDecoded = "";
    if (hasIPFS) {
        ipfsDecoded = DecodeAssetData(ui->ipfsText->text().toStdString());
    }

    std::string ansDecoded = "";
    if (hasANS) {
        std::string error;
        std::string formattedTypeData;
        std::string typeData;
        if (ansType == CAvianNameSystemID::PROFILE) {
            ANSProfileData p;
            p.addr   = ui->ansCborAddrEdit->text().toStdString();
            p.name   = ui->ansCborNameEdit->text().toStdString();
            p.avatar = ui->ansCborAvatarEdit->text().toStdString();
            p.banner = ui->ansCborBannerEdit->text().toStdString();
            p.url    = ui->ansCborUrlEdit->text().toStdString();
            typeData = ANS_CBOR::EncodeProfile(p);
        } else {
            typeData = ui->ansText->text().toStdString();
        }
        formattedTypeData = CAvianNameSystemID::FormatTypeData(ansType, typeData, error);
        CAvianNameSystemID ansID(ansType, formattedTypeData);
        ansDecoded = ansID.to_string();

        // Warn user — skip warning for reissuable sub-assets (AIP-0010) since updating is expected
        if (!reissuable || !m_ansSubMode) {
            QMessageBox::critical(this, "ANS Warning", tr("Storing data using the Avian Name System will forever stay in the blockchain. You can edit the ANS ID only if the asset is reissueable.") + QString("\n\nANS ID: ") + QString::fromStdString(ansDecoded), QMessageBox::Ok, QMessageBox::Ok);
        }
    }

    CNewAsset asset(name.toStdString(), quantity, units, reissuable ? 1 : 0, hasIPFS ? 1 : 0, ipfsDecoded, hasANS ? 1 : 0, ansDecoded);

    std::string verifierStripped = GetStrippedVerifierString(ui->lineEditVerifierString->text().toStdString());
    bool fRestrictedAssetCreation = false;
    if (type == IntFromAssetType(AssetType::RESTRICTED)) {
        fRestrictedAssetCreation = true;
        if (verifierStripped.empty())
            verifierStripped = "true";
    }

    CTransactionRef txRef;
    std::pair<int, std::string> error;
    CAmount nFeeRequired;

    // Always use a CCoinControl instance, use the CoinControlDialog instance if CoinControl has been enabled
    wallet::CCoinControl ctrl;
    if (model->getOptionsModel()->getCoinControlFeatures())
        ctrl = s_coinControl();

    // If the user has pre-selected inputs via coin control but they are insufficient,
    // reject early before attempting to build the transaction.
    if (ctrl.HasSelected() && ui->labelCoinControlInsuffFunds->isVisible()) {
        QMessageBox::critical(this, tr("Insufficient funds"), tr("The selected inputs do not cover the asset creation burn fee. Please select more inputs or clear coin control."));
        return;
    }

    updateCoinControlState(ctrl);

    QString address;
    if (ui->addressText->text().isEmpty()) {
        address = model->getAddressTableModel()->addRow(AddressTableModel::Receive, "", "", OutputType::LEGACY);
    } else {
        address = ui->addressText->text();
    }

    wallet::CWallet* pwallet = model->wallet().wallet();
    if (!pwallet) {
        showMessage(tr("Wallet not available."));
        return;
    }

    // Create the transaction
    {
        LOCK(pwallet->cs_wallet);
        if (!wallet::CreateAssetTransaction(*pwallet, ctrl, asset, address.toStdString(), error, txRef, nFeeRequired, fRestrictedAssetCreation ? &verifierStripped : nullptr)) {
            showMessage("Invalid: " + QString::fromStdString(error.second));
            return;
        }
    }

    // Format confirmation message
    QStringList formatted;

    // generate bold burn amount string
    QString burnAmount = "<b>" + QString::fromStdString(ValueFromAmountString(GetBurnAmount(getBurnType()), 8)) + " AVN";
    burnAmount.append("</b>");
    // generate monospace burn address string
    QString addressburn = "<span style='font-family: monospace;'>" + QString::fromStdString(GetBurnAddress(getBurnType()));
    addressburn.append("</span>");

    QString recipientElement1;
    recipientElement1 = tr("%1 to %2").arg(burnAmount, addressburn);
    formatted.append(recipientElement1);

    // generate the bold asset amount
    QString assetAmount = "<b>" + QString::fromStdString(ValueFromAmountString(asset.nAmount, asset.units)) + " " + QString::fromStdString(asset.strName);
    assetAmount.append("</b>");

    // generate the monospace address string
    QString assetAddress = "<span style='font-family: monospace;'>" + address;
    assetAddress.append("</span>");

    QString recipientElement2;
    recipientElement2 = tr("%1 to %2").arg(assetAmount, assetAddress);
    formatted.append(recipientElement2);

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

    // add total amount in all subdivision units
    questionString.append("<hr />");
    CAmount totalAmount = GetBurnAmount(getBurnType()) + nFeeRequired;
    QStringList alternativeUnits;
    for (const BitcoinUnit& u : BitcoinUnits::availableUnits())
    {
        if(u != model->getOptionsModel()->getDisplayUnit())
            alternativeUnits.append(BitcoinUnits::formatHtmlWithUnit(u, totalAmount));
    }
    questionString.append(tr("Total Amount %1")
                                .arg(BitcoinUnits::formatHtmlWithUnit(model->getOptionsModel()->getDisplayUnit(), totalAmount)));
    questionString.append(QString("<span style='font-size:10pt;font-weight:normal;'><br />(=%2)</span>")
                                .arg(alternativeUnits.join(" " + tr("or") + "<br />")));

    SendConfirmationDialog confirmationDialog(tr("Confirm send assets"),
                                              questionString.arg(formatted.join("<br />")), "", "", SEND_CONFIRM_DELAY, true, true, this);
    confirmationDialog.exec();
    QMessageBox::StandardButton retval = (QMessageBox::StandardButton)confirmationDialog.result();

    if (retval != QMessageBox::Yes && retval != QMessageBox::Save)
    {
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
            showMessage(tr("Failed to create PSBT"));
            return;
        }
        // Serialize and copy to clipboard, offer save
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
                tr("Save Transaction Data"), QString::fromStdString(asset.strName) + ".psbt",
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
    } else {
        // "Send" clicked — broadcast directly
        std::string txid;
        {
            LOCK(pwallet->cs_wallet);
            if (!wallet::SendAssetTransaction(*pwallet, txRef, error, txid)) {
                showMessage(tr("Invalid: ") + QString::fromStdString(error.second));
            } else {
                QMessageBox msgBox;
                QPushButton *copyButton = msgBox.addButton(tr("Copy"), QMessageBox::ActionRole);
                copyButton->disconnect();
                connect(copyButton, &QPushButton::clicked, this, [=](){
                    QClipboard *p_Clipboard = QApplication::clipboard();
                    p_Clipboard->setText(QString::fromStdString(txid), QClipboard::Mode::Clipboard);

                    QMessageBox copiedBox;
                    copiedBox.setText(tr("Transaction ID Copied"));
                    copiedBox.exec();
                });

                QPushButton *okayButton = msgBox.addButton(QMessageBox::Ok);
                msgBox.setText(tr("Asset transaction sent to network:"));
                msgBox.setInformativeText(QString::fromStdString(txid));
                msgBox.exec();

                if (msgBox.clickedButton() == okayButton) {
                    clear();

                    s_coinControl().UnSelectAll();
                    coinControlUpdateLabels();
                }
            }
        }
    }
}

void CreateAssetDialog::onUnitChanged(int value)
{
    QString text;
    text += "e.g. 1";
    // Add the period
    if (value > 0)
        text += ".";

    // Add the remaining zeros
    for (int i = 0; i < value; i++) {
        text += "0";
    }

    ui->unitExampleLabel->setText(text);
}

void CreateAssetDialog::onChangeAddressChanged(QString changeAddress)
{
    CheckFormState();
}

void CreateAssetDialog::onAssetTypeActivated(int index)
{
    disableCreateButton();
    checkedAvailablity = false;

    int nCurrentType = type;
    // Update the selected type
    type = index;

    bool fANSTypeAsset = type == ANS_ASSET_TYPE_INDEX;
    bool fOrginalTypeAsset = type == IntFromAssetType(AssetType::ROOT) || type == IntFromAssetType(AssetType::SUB) || type == IntFromAssetType(AssetType::UNIQUE) || type == IntFromAssetType(AssetType::MSGCHANNEL);
    bool fRestrictedTypeAsset = type == IntFromAssetType(AssetType::QUALIFIER) || type == IntFromAssetType(AssetType::SUB_QUALIFIER) || type == IntFromAssetType(AssetType::RESTRICTED);

    bool fShowList = type == IntFromAssetType(AssetType::SUB) || type == IntFromAssetType(AssetType::UNIQUE) || type == IntFromAssetType(AssetType::SUB_QUALIFIER) || type == IntFromAssetType(AssetType::RESTRICTED) || type == IntFromAssetType(AssetType::MSGCHANNEL);

    // Make sure the type is only the the supported issue types
    if(!(fOrginalTypeAsset || fRestrictedTypeAsset || fANSTypeAsset)) {
        type = IntFromAssetType(AssetType::ROOT);
        fOrginalTypeAsset = true;
        fANSTypeAsset = false;
    }

    // If we're transitioning FROM ANS Asset to another type, restore the locked controls
    if (nCurrentType == ANS_ASSET_TYPE_INDEX && !fANSTypeAsset) {
        ui->ipfsBox->setDisabled(false);
        if (IsAvianNameSystemDeployed()) {
            ui->ansBox->setChecked(false);
            ui->ansBox->setDisabled(false);
            ui->ansType->setDisabled(false);
        }
        ui->ansType->hide();
        ui->ansCborWidget->hide();
        ui->ansText->hide();
    }

    // Show/hide ansBox based on asset type — only ROOT and ANS types support ANS data
    if (IsAvianNameSystemDeployed() && !fANSTypeAsset) {
        if (type == IntFromAssetType(AssetType::ROOT)) {
            ui->ansBox->show();
            // Set disabled/enabled state based on whether the current name ends in .AVN
            const std::string& domain = CAvianNameSystemID::domain;
            std::string sn = ui->nameText->text().toStdString();
            bool endsWithAVN = sn.size() > domain.size() &&
                               sn.substr(sn.size() - domain.size()) == domain;
            ui->ansBox->setDisabled(true); // always locked: checked for .AVN (required), unchecked otherwise
            if (!endsWithAVN && ui->ansBox->isChecked()) {
                ui->ansBox->setChecked(false);
                toggleANSText();
            }
        } else {
            if (ui->ansBox->isChecked()) {
                ui->ansBox->setChecked(false);
                toggleANSText();
            }
            setANSSubMode(false);
            ui->ansBox->hide();
        }
    }

    // Set the locked/default values for the selected type
    if (fANSTypeAsset) {
        setANSAssetSelected();
    } else if (type == IntFromAssetType(AssetType::UNIQUE) || type == IntFromAssetType(AssetType::MSGCHANNEL)) {
        setUniqueSelected();
    } else if (type == IntFromAssetType(AssetType::QUALIFIER) || type == IntFromAssetType(AssetType::SUB_QUALIFIER)) {
        setQualifierSelected();
    } else {
        clearSelected();
    }

    // Get the identifier for the asset type
    QString identifier = GetSpecialCharacter();

    // Add functionality when switching between restricted and none restricted asset types
    if (nCurrentType != IntFromAssetType(AssetType::RESTRICTED) && type == IntFromAssetType(AssetType::RESTRICTED)) {
        restrictedAssetSelected();
    } else if (nCurrentType == IntFromAssetType(AssetType::RESTRICTED) && type != IntFromAssetType(AssetType::RESTRICTED)) {
        restrictedAssetNotSelected();
    }

    if (type == IntFromAssetType(AssetType::SUB_QUALIFIER)) {
        updateAssetListForSubQualifierIssuance();
    }

    if (fShowList) {
        ui->assetList->show();
    } else {
        ui->assetList->hide();
    }

    UpdateAssetNameMaxSize();

    // Set assetName when it is an original asset type
    if (fOrginalTypeAsset)
        updatePresentedAssetName(format.arg(type == IntFromAssetType(AssetType::ROOT) ? "" : ui->assetList->currentText(), identifier, ui->nameText->text()));

    if (fANSTypeAsset)
        updatePresentedAssetName(format.arg("", identifier, ui->nameText->text() + QString::fromStdString(CAvianNameSystemID::domain)));

    if (fRestrictedTypeAsset) {
        bool fSingleName = type != IntFromAssetType(AssetType::SUB_QUALIFIER);
        updatePresentedAssetName(format.arg(fSingleName ? "" : ui->assetList->currentText(), identifier, ui->nameText->text()));
    }

    if (ui->nameText->text().size()) {
        ui->availabilityButton->setDisabled(false);
    } else {
        ui->availabilityButton->setDisabled(true);
    }

    ui->createAssetButton->setDisabled(true);

    // Update coinControl so it can change the amount that is being spent
    coinControlUpdateLabels();
}

void CreateAssetDialog::onAssetListActivated(int index)
{
    // Get the identifier for the asset type
    QString identifier = GetSpecialCharacter();

    UpdateAssetNameMaxSize();

    // Set assetName
    updatePresentedAssetName(format.arg(type == IntFromAssetType(AssetType::ROOT) || type == IntFromAssetType(AssetType::RESTRICTED) || type == IntFromAssetType(AssetType::QUALIFIER) ? "" : ui->assetList->currentText(), identifier, ui->nameText->text()));

    if (type == IntFromAssetType(AssetType::RESTRICTED)) {
        ui->nameText->setText("$" + ui->assetList->currentText());
        ui->assetFullName->hide();
    }

    if (ui->nameText->text().size())
        ui->availabilityButton->setDisabled(false);
    else
        ui->availabilityButton->setDisabled(true);
    ui->createAssetButton->setDisabled(true);
}

void CreateAssetDialog::updatePresentedAssetName(QString name)
{
    ui->assetFullName->setText(name);
}

QString CreateAssetDialog::GetSpecialCharacter()
{
    if (type == IntFromAssetType(AssetType::SUB) || type == IntFromAssetType(AssetType::SUB_QUALIFIER))
        return "/";
    else if (type == IntFromAssetType(AssetType::UNIQUE))
        return "#";
    else if (type == IntFromAssetType(AssetType::MSGCHANNEL))
        return "~";

    return "";
}

QString CreateAssetDialog::GetAssetName()
{
    if (type == IntFromAssetType(AssetType::ROOT))
        return ui->nameText->text();
    else if (type == IntFromAssetType(AssetType::SUB))
        return ui->assetList->currentText() + "/" + ui->nameText->text();
    else if (type == IntFromAssetType(AssetType::UNIQUE))
        return ui->assetList->currentText() + "#" + ui->nameText->text();
    else if (type == IntFromAssetType(AssetType::MSGCHANNEL))
        return ui->assetList->currentText() + "~" + ui->nameText->text();
    else if (type == IntFromAssetType(AssetType::RESTRICTED))
        return ui->nameText->text();
    else if (type == IntFromAssetType(AssetType::QUALIFIER))
        return ui->nameText->text();
    else if (type == IntFromAssetType(AssetType::SUB_QUALIFIER))
        return ui->assetList->currentText() + "/" + ui->nameText->text();
    else if (type == ANS_ASSET_TYPE_INDEX)
        return ui->nameText->text() + QString::fromStdString(CAvianNameSystemID::domain);
    return "";
}

void CreateAssetDialog::UpdateAssetNameMaxSize()
{
    if (type == IntFromAssetType(AssetType::ROOT) || type == IntFromAssetType(AssetType::QUALIFIER) || type == IntFromAssetType(AssetType::RESTRICTED)) {
        ui->nameText->setMaxLength(30);
    } else if (type == ANS_ASSET_TYPE_INDEX) {
        // Reserve room for the .AVN suffix (4 chars) that is auto-appended
        ui->nameText->setMaxLength(30 - (int)CAvianNameSystemID::domain.size());
    } else if (type == IntFromAssetType(AssetType::SUB) || type == IntFromAssetType(AssetType::UNIQUE) || type == IntFromAssetType(AssetType::SUB_QUALIFIER)) {
        ui->nameText->setMaxLength(30 - (ui->assetList->currentText().size() + 1));
    }
}

void CreateAssetDialog::UpdateAssetNameToUpper()
{
    if (type == IntFromAssetType(AssetType::ROOT) || type == IntFromAssetType(AssetType::SUB) || type == IntFromAssetType(AssetType::RESTRICTED) || type == IntFromAssetType(AssetType::QUALIFIER) || type == IntFromAssetType(AssetType::SUB_QUALIFIER) || type == IntFromAssetType(AssetType::MSGCHANNEL) || type == ANS_ASSET_TYPE_INDEX) {
        ui->nameText->setText(ui->nameText->text().toUpper());
    }
    // The MSGCHANNEL name field holds only the channel tag (after '~'); strip '~' to
    // prevent the user from producing a double-delimiter (e.g. PARENT~~TAG).
    if (type == IntFromAssetType(AssetType::MSGCHANNEL)) {
        QString text = ui->nameText->text();
        QString stripped = text;
        stripped.remove('~');
        if (stripped != text)
            ui->nameText->setText(stripped);
    }
}

void CreateAssetDialog::updateCoinControlState(wallet::CCoinControl& ctrl)
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

void CreateAssetDialog::updateSmartFeeLabel()
{
    if(!model || !model->getOptionsModel())
        return;
    wallet::CCoinControl coin_control;
    updateCoinControlState(coin_control);
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
void CreateAssetDialog::coinControlClipboardQuantity()
{
    GUIUtil::setClipboard(ui->labelCoinControlQuantity->text());
}

// Coin Control: copy label "Amount" to clipboard
void CreateAssetDialog::coinControlClipboardAmount()
{
    GUIUtil::setClipboard(ui->labelCoinControlAmount->text().left(ui->labelCoinControlAmount->text().indexOf(" ")));
}

// Coin Control: copy label "Fee" to clipboard
void CreateAssetDialog::coinControlClipboardFee()
{
    GUIUtil::setClipboard(ui->labelCoinControlFee->text().left(ui->labelCoinControlFee->text().indexOf(" ")).replace(ASYMP_UTF8, ""));
}

// Coin Control: copy label "After fee" to clipboard
void CreateAssetDialog::coinControlClipboardAfterFee()
{
    GUIUtil::setClipboard(ui->labelCoinControlAfterFee->text().left(ui->labelCoinControlAfterFee->text().indexOf(" ")).replace(ASYMP_UTF8, ""));
}

// Coin Control: copy label "Bytes" to clipboard
void CreateAssetDialog::coinControlClipboardBytes()
{
    GUIUtil::setClipboard(ui->labelCoinControlBytes->text().replace(ASYMP_UTF8, ""));
}

// Coin Control: copy label "Dust" to clipboard
void CreateAssetDialog::coinControlClipboardLowOutput()
{
    GUIUtil::setClipboard(ui->labelCoinControlLowOutput->text());
}

// Coin Control: copy label "Change" to clipboard
void CreateAssetDialog::coinControlClipboardChange()
{
    GUIUtil::setClipboard(ui->labelCoinControlChange->text().left(ui->labelCoinControlChange->text().indexOf(" ")).replace(ASYMP_UTF8, ""));
}

// Coin Control: settings menu - coin control enabled/disabled by user
void CreateAssetDialog::coinControlFeatureChanged(bool checked)
{
    ui->frameCoinControl->setVisible(checked);
    ui->addressText->setVisible(checked);
    ui->addressLabel->setVisible(checked);

    if (!checked && model) // coin control features disabled
        s_coinControl() = wallet::CCoinControl();

    coinControlUpdateLabels();
}

// Coin Control: settings menu - coin control enabled/disabled by user
void CreateAssetDialog::feeControlFeatureChanged(bool checked)
{
    ui->frameFee->setVisible(checked);
}

// Coin Control: button inputs -> show actual coin control dialog
void CreateAssetDialog::coinControlButtonClicked()
{
    CoinControlDialog dlg(s_coinControl(), model, platformStyle);
    dlg.exec();
    coinControlUpdateLabels();
}

// Coin Control: checkbox custom change address
void CreateAssetDialog::coinControlChangeChecked(int state)
{
    if (state == Qt::Unchecked)
    {
        s_coinControl().destChange = CNoDestination();
        ui->labelCoinControlChangeLabel->clear();
    }
    else
        // use this to re-validate an already entered address
        coinControlChangeEdited(ui->lineEditCoinControlChange->text());

    ui->lineEditCoinControlChange->setEnabled((state == Qt::Checked));
}

// Coin Control: custom change address changed
void CreateAssetDialog::coinControlChangeEdited(const QString& text)
{
    if (model && model->getAddressTableModel())
    {
        // Default to no change address until verified
        s_coinControl().destChange = CNoDestination();
        ui->labelCoinControlChangeLabel->setStyleSheet("QLabel{color:red;}");

        const CTxDestination dest = DecodeDestination(text.toStdString());

        if (text.isEmpty()) // Nothing entered
        {
            ui->labelCoinControlChangeLabel->setText("");
        }
        else if (!IsValidDestination(dest)) // Invalid address
        {
            ui->labelCoinControlChangeLabel->setText(tr("Warning: Invalid Avian address"));
        }
        else // Valid address
        {
            if (!model->wallet().isSpendable(dest)) {
                ui->labelCoinControlChangeLabel->setText(tr("Warning: Unknown change address"));

                // confirmation dialog
                QMessageBox::StandardButton btnRetVal = QMessageBox::question(this, tr("Confirm custom change address"), tr("The address you selected for change is not part of this wallet. Any or all funds in your wallet may be sent to this address. Are you sure?"),
                                                                              QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);

                if(btnRetVal == QMessageBox::Yes)
                    s_coinControl().destChange = dest;
                else
                {
                    ui->lineEditCoinControlChange->setText("");
                    ui->labelCoinControlChangeLabel->setStyleSheet("QLabel{color:black;}");
                    ui->labelCoinControlChangeLabel->setText("");
                }
            }
            else // Known change address
            {
                ui->labelCoinControlChangeLabel->setStyleSheet("QLabel{color:black;}");

                // Query label
                QString associatedLabel = model->getAddressTableModel()->labelForAddress(text);
                if (!associatedLabel.isEmpty())
                    ui->labelCoinControlChangeLabel->setText(associatedLabel);
                else
                    ui->labelCoinControlChangeLabel->setText(tr("(no label)"));

                s_coinControl().destChange = dest;
            }
        }
    }
}

// Coin Control: update labels
void CreateAssetDialog::coinControlUpdateLabels()
{
    if (!model || !model->getOptionsModel())
        return;

    updateCoinControlState(s_coinControl());

    // set pay amounts
    CoinControlDialog::payAmounts.clear();
    CoinControlDialog::fSubtractFeeFromAmount = false;

    CoinControlDialog::payAmounts.append(GetBurnAmount(getBurnType()));

    if (s_coinControl().HasSelected())
    {
        // actual coin control calculation
        CoinControlDialog::updateLabels(s_coinControl(), model, this);

        // show coin control stats
        ui->labelCoinControlAutomaticallySelected->hide();
        ui->widgetCoinControl->show();
    }
    else
    {
        // hide coin control stats
        ui->labelCoinControlAutomaticallySelected->show();
        ui->widgetCoinControl->hide();
        ui->labelCoinControlInsuffFunds->hide();
    }
}

void CreateAssetDialog::minimizeFeeSection(bool fMinimize)
{
    ui->labelFeeMinimized->setVisible(fMinimize);
    ui->buttonChooseFee  ->setVisible(fMinimize);
    ui->buttonMinimizeFee->setVisible(!fMinimize);
    ui->frameFeeSelection->setVisible(!fMinimize);
    ui->horizontalLayoutSmartFee->setContentsMargins(0, (fMinimize ? 0 : 6), 0, 0);
    fFeeMinimized = fMinimize;
}

void CreateAssetDialog::on_buttonChooseFee_clicked()
{
    minimizeFeeSection(false);
}

void CreateAssetDialog::on_buttonMinimizeFee_clicked()
{
    updateFeeMinimizedLabel();
    minimizeFeeSection(true);
}

void CreateAssetDialog::setMinimumFee()
{
    ui->customFee->setValue(model->wallet().getRequiredFee(1000));
}

void CreateAssetDialog::updateFeeSectionControls()
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

void CreateAssetDialog::updateFeeMinimizedLabel()
{
    if(!model || !model->getOptionsModel())
        return;

    if (ui->radioSmartFee->isChecked())
        ui->labelFeeMinimized->setText(ui->labelSmartFee->text());
    else {
        ui->labelFeeMinimized->setText(BitcoinUnits::formatWithUnit(model->getOptionsModel()->getDisplayUnit(), ui->customFee->value()) + "/kB");
    }
}

void CreateAssetDialog::updateMinFeeLabel()
{
    if (model && model->getOptionsModel())
        ui->checkBoxMinimumFee->setText(tr("Pay only the required fee of %1").arg(
                BitcoinUnits::formatWithUnit(model->getOptionsModel()->getDisplayUnit(), model->wallet().getRequiredFee(1000)) + "/kB")
        );
}

void CreateAssetDialog::setUniqueSelected()
{
    ui->quantitySpinBox->setValue(1);
    ui->quantitySpinBox->setDisabled(true);

    ui->unitBox->setValue(0);
    ui->unitBox->setDisabled(true);

    ui->reissuableBox->setChecked(false);
    ui->reissuableBox->setDisabled(true);
}

void CreateAssetDialog::setQualifierSelected()
{
    ui->quantitySpinBox->setValue(1);
    ui->quantitySpinBox->setMaximum(10);
    ui->quantitySpinBox->setDisabled(false);

    ui->unitBox->setValue(0);
    ui->unitBox->setDisabled(true);

    ui->reissuableBox->setChecked(false);
    ui->reissuableBox->setDisabled(true);
}

void CreateAssetDialog::clearSelected()
{
    ui->quantitySpinBox->setMaximum(21000000000);
    ui->quantitySpinBox->setDisabled(false);

    ui->unitBox->setValue(0);
    ui->unitBox->setDisabled(false);

    ui->reissuableBox->setChecked(true);
    ui->reissuableBox->setDisabled(false);
}

void CreateAssetDialog::setANSAssetSelected()
{
    ui->quantitySpinBox->setValue(1);
    ui->quantitySpinBox->setDisabled(true);

    ui->unitBox->setValue(0);
    ui->unitBox->setDisabled(true);

    ui->reissuableBox->setChecked(true);
    ui->reissuableBox->setDisabled(true);

    ui->ipfsBox->setDisabled(true);
    ui->ipfsBox->setChecked(false);
    ui->ipfsText->hide();
    ui->openIpfsButton->hide();

    if (IsAvianNameSystemDeployed()) {
        ui->ansBox->setChecked(true);
        ui->ansBox->setDisabled(true);
        ui->ansType->show();
        ui->ansType->setCurrentIndex(1); // default to PROFILE type; user may change to ADDR
        ui->ansType->setDisabled(false);
        ui->ansCborWidget->show();
        ui->ansText->hide();
    }
}

int CreateAssetDialog::getBurnType() const
{
    return type == ANS_ASSET_TYPE_INDEX ? IntFromAssetType(AssetType::ROOT) : type;
}

void CreateAssetDialog::updateAssetList()
{
    QStringList list;
    list << "";

    wallet::CWallet* pwallet = model ? model->wallet().wallet() : nullptr;
    if (pwallet) {
        LOCK(pwallet->cs_wallet);
        wallet::CoinFilterParams params;
        params.min_amount = 0;
        wallet::CoinsResult available = wallet::AvailableCoinsWithAssets(*pwallet, nullptr, std::nullopt, params);
        for (const auto& [assetName, assetOutputs] : available.mapAssetCoins) {
            if (IsAssetNameAnOwner(assetName)) {
                std::string baseName = assetName;
                baseName.pop_back(); // Remove '!' suffix
                list << QString::fromStdString(baseName);
            }
        }
    }

    stringModel->setStringList(list);
}

void CreateAssetDialog::updateAssetListForRestrictedIssuance()
{
    QStringList list;
    list << "";

    wallet::CWallet* pwallet = model ? model->wallet().wallet() : nullptr;
    if (pwallet) {
        LOCK(pwallet->cs_wallet);
        wallet::CoinFilterParams params;
        params.min_amount = 0;
        wallet::CoinsResult available = wallet::AvailableCoinsWithAssets(*pwallet, nullptr, std::nullopt, params);
        for (const auto& [assetName, assetOutputs] : available.mapAssetCoins) {
            if (IsAssetNameAnOwner(assetName)) {
                std::string baseName = assetName;
                baseName.pop_back(); // Remove '!' suffix
                list << QString::fromStdString(baseName);
            }
        }
    }

    stringModel->setStringList(list);
}

void CreateAssetDialog::updateAssetListForSubQualifierIssuance()
{
    QStringList list;
    list << "";

    wallet::CWallet* pwallet = model ? model->wallet().wallet() : nullptr;
    if (pwallet) {
        LOCK(pwallet->cs_wallet);
        wallet::CoinFilterParams params;
        params.min_amount = 0;
        wallet::CoinsResult available = wallet::AvailableCoinsWithAssets(*pwallet, nullptr, std::nullopt, params);
        for (const auto& [assetName, assetOutputs] : available.mapAssetCoins) {
            // For sub-qualifier issuance, show qualifier assets (starting with '#')
            if (!IsAssetNameAnOwner(assetName) && assetName.size() > 0 && assetName[0] == '#') {
                list << QString::fromStdString(assetName);
            }
        }
    }

    stringModel->setStringList(list);
}

void CreateAssetDialog::clear()
{
    ui->assetType->setCurrentIndex(0);
    ui->nameText->clear();
    ui->addressText->clear();
    ui->quantitySpinBox->setValue(1);
    ui->unitBox->setValue(0);
    ui->reissuableBox->setChecked(true);
    ui->ipfsBox->setChecked(false);
    ui->ipfsText->hide();
    ui->openIpfsButton->hide();
    ui->assetList->hide();
    ui->assetList->setCurrentIndex(0);
    type = 0;
    ui->assetFullName->clear();
    ui->unitBox->setDisabled(false);
    ui->quantitySpinBox->setDisabled(false);
    ui->quantitySpinBox->setMaximum(21000000000);
    ui->nameText->setEnabled(true);

    ui->reissuableBox->setDisabled(false);
    hideMessage();
    disableCreateButton();
}

void CreateAssetDialog::onClearButtonClicked()
{
    clear();
}

void CreateAssetDialog::focusSubAsset(const QModelIndex &index)
{
    selectTypeName(1,index.data(AssetTableModel::AssetNameRole).toString());
}

void CreateAssetDialog::focusUniqueAsset(const QModelIndex &index)
{
    selectTypeName(2,index.data(AssetTableModel::AssetNameRole).toString());
}

void CreateAssetDialog::selectTypeName(int type, QString name)
{
    clear();

    if (IsAssetNameAnOwner(name.toStdString()))
        name = name.left(name.size() - 1);

    ui->assetType->setCurrentIndex(type);
    onAssetTypeActivated(type);

    ui->assetList->setCurrentIndex(ui->assetList->findText(name));
    onAssetListActivated(ui->assetList->currentIndex());

    ui->nameText->setFocus();
}

void CreateAssetDialog::restrictedAssetSelected()
{
    updateAssetListForRestrictedIssuance();

    ui->nameText->clear();
    ui->nameText->setEnabled(false);

    ui->labelVerifierString->show();
    ui->lineEditVerifierString->show();

    ui->addressText->show();
    ui->addressLabel->show();
}

void CreateAssetDialog::restrictedAssetNotSelected()
{
    updateAssetList();

    ui->nameText->clear();
    ui->nameText->setEnabled(true);
    ui->assetFullName->show();

    ui->labelVerifierString->hide();
    ui->lineEditVerifierString->hide();
    ui->labelVerifierStringErrorMessage->hide();

    bool fCoinControlEnabled = this->model->getOptionsModel()->getCoinControlFeatures();
    ui->addressText->setVisible(fCoinControlEnabled);
    ui->addressLabel->setVisible(fCoinControlEnabled);
}
