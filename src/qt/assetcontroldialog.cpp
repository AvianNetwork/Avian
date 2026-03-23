// Copyright (c) 2011-2016 The Bitcoin Core developers
// Copyright (c) 2017-2019 The Raven Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/assetcontroldialog.h>
#include "ui_assetcontroldialog.h"

#include <qt/addresstablemodel.h>
#include <qt/bitcoinunits.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <qt/walletmodel.h>

#include <wallet/coincontrol.h>
#include <wallet/wallet.h>
#include <wallet/spend.h>
#include <assets/assets.h>
#include <key_io.h>
#include <policy/policy.h>
#include <validation.h>

#include <QApplication>
#include <QCheckBox>
#include <QCursor>
#include <QDialogButtonBox>
#include <QFlags>
#include <QIcon>
#include <QRegularExpression>
#include <QSettings>
#include <QString>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QStringListModel>
#include <QSortFilterProxyModel>
#include <QCompleter>
#include <QLineEdit>

// Helper: format amount with a custom asset name instead of the coin unit
static QString formatWithCustomName(const QString& name, CAmount amount)
{
    // Format as 8-decimal display
    bool sign = amount < 0;
    int64_t n_abs = (sign ? -amount : amount);
    int64_t quotient = n_abs / COIN;
    int64_t remainder = n_abs % COIN;
    QString str = QString::number(quotient);
    if (remainder > 0)
        str += QString(".%1").arg(remainder, 8, 10, QLatin1Char('0')).remove(QRegularExpression("0+$"));
    if (sign) str.prepend("-");
    return str + " " + name;
}

QList<CAmount> AssetControlDialog::payAmounts;
wallet::CCoinControl* AssetControlDialog::assetControl()
{
    static wallet::CCoinControl instance;
    return &instance;
}
bool AssetControlDialog::fSubtractFeeFromAmount = false;

bool CAssetControlWidgetItem::operator<(const QTreeWidgetItem &other) const {
    int column = treeWidget()->sortColumn();
    if (column == AssetControlDialog::COLUMN_AMOUNT || column == AssetControlDialog::COLUMN_DATE || column == AssetControlDialog::COLUMN_CONFIRMATIONS)
        return data(column, Qt::UserRole).toLongLong() < other.data(column, Qt::UserRole).toLongLong();
    return QTreeWidgetItem::operator<(other);
}

AssetControlDialog::AssetControlDialog(const PlatformStyle *_platformStyle, QWidget *parent) :
    QDialog(parent),
    ui(new Ui::AssetControlDialog),
    model(0),
    platformStyle(_platformStyle)
{
    ui->setupUi(this);

    // context menu actions
    QAction *copyAddressAction = new QAction(tr("Copy address"), this);
    QAction *copyLabelAction = new QAction(tr("Copy label"), this);
    QAction *copyAmountAction = new QAction(tr("Copy amount"), this);
             copyTransactionHashAction = new QAction(tr("Copy transaction ID"), this);
             lockAction = new QAction(tr("Lock unspent"), this);
             unlockAction = new QAction(tr("Unlock unspent"), this);

    // context menu
    contextMenu = new QMenu(this);
    contextMenu->addAction(copyAddressAction);
    contextMenu->addAction(copyLabelAction);
    contextMenu->addAction(copyAmountAction);
    contextMenu->addAction(copyTransactionHashAction);
    contextMenu->addSeparator();
    contextMenu->addAction(lockAction);
    contextMenu->addAction(unlockAction);

    // context menu signals
    connect(ui->treeWidget, SIGNAL(customContextMenuRequested(QPoint)), this, SLOT(showMenu(QPoint)));
    connect(copyAddressAction, SIGNAL(triggered()), this, SLOT(copyAddress()));
    connect(copyLabelAction, SIGNAL(triggered()), this, SLOT(copyLabel()));
    connect(copyAmountAction, SIGNAL(triggered()), this, SLOT(copyAmount()));
    connect(copyTransactionHashAction, SIGNAL(triggered()), this, SLOT(copyTransactionHash()));
    connect(lockAction, SIGNAL(triggered()), this, SLOT(lockCoin()));
    connect(unlockAction, SIGNAL(triggered()), this, SLOT(unlockCoin()));

    // clipboard actions
    QAction *clipboardQuantityAction = new QAction(tr("Copy quantity"), this);
    QAction *clipboardAmountAction = new QAction(tr("Copy amount"), this);
    QAction *clipboardFeeAction = new QAction(tr("Copy fee"), this);
    QAction *clipboardAfterFeeAction = new QAction(tr("Copy after fee"), this);
    QAction *clipboardBytesAction = new QAction(tr("Copy bytes"), this);
    QAction *clipboardLowOutputAction = new QAction(tr("Copy dust"), this);
    QAction *clipboardChangeAction = new QAction(tr("Copy change"), this);

    connect(clipboardQuantityAction, SIGNAL(triggered()), this, SLOT(clipboardQuantity()));
    connect(clipboardAmountAction, SIGNAL(triggered()), this, SLOT(clipboardAmount()));
    connect(clipboardFeeAction, SIGNAL(triggered()), this, SLOT(clipboardFee()));
    connect(clipboardAfterFeeAction, SIGNAL(triggered()), this, SLOT(clipboardAfterFee()));
    connect(clipboardBytesAction, SIGNAL(triggered()), this, SLOT(clipboardBytes()));
    connect(clipboardLowOutputAction, SIGNAL(triggered()), this, SLOT(clipboardLowOutput()));
    connect(clipboardChangeAction, SIGNAL(triggered()), this, SLOT(clipboardChange()));

    ui->labelAssetControlQuantity->addAction(clipboardQuantityAction);
    ui->labelAssetControlAmount->addAction(clipboardAmountAction);
    ui->labelAssetControlFee->addAction(clipboardFeeAction);
    ui->labelAssetControlAfterFee->addAction(clipboardAfterFeeAction);
    ui->labelAssetControlBytes->addAction(clipboardBytesAction);
    ui->labelAssetControlLowOutput->addAction(clipboardLowOutputAction);
    ui->labelAssetControlChange->addAction(clipboardChangeAction);

    // toggle tree/list mode
    connect(ui->radioTreeMode, SIGNAL(toggled(bool)), this, SLOT(radioTreeMode(bool)));
    connect(ui->radioListMode, SIGNAL(toggled(bool)), this, SLOT(radioListMode(bool)));

    // click on checkbox
    connect(ui->treeWidget, SIGNAL(itemChanged(QTreeWidgetItem*, int)), this, SLOT(viewItemChanged(QTreeWidgetItem*, int)));

    // click on header
    ui->treeWidget->header()->setSectionsClickable(true);
    connect(ui->treeWidget->header(), SIGNAL(sectionClicked(int)), this, SLOT(headerSectionClicked(int)));

    // ok button
    connect(ui->buttonBox, SIGNAL(clicked( QAbstractButton*)), this, SLOT(buttonBoxClicked(QAbstractButton*)));

    // (un)select all
    connect(ui->pushButtonSelectAll, SIGNAL(clicked()), this, SLOT(buttonSelectAllClicked()));

    // change coin control first column label
    ui->treeWidget->headerItem()->setText(COLUMN_CHECKBOX, QString());

    ui->treeWidget->setColumnWidth(COLUMN_CHECKBOX, 84);
    ui->treeWidget->setColumnWidth(COLUMN_AMOUNT, 110);
    ui->treeWidget->setColumnWidth(COLUMN_LABEL, 190);
    ui->treeWidget->setColumnWidth(COLUMN_ADDRESS, 320);
    ui->treeWidget->setColumnWidth(COLUMN_DATE, 130);
    ui->treeWidget->setColumnWidth(COLUMN_CONFIRMATIONS, 110);
    ui->treeWidget->setColumnHidden(COLUMN_TXHASH, true);
    ui->treeWidget->setColumnHidden(COLUMN_VOUT_INDEX, true);

    // default view is sorted by amount desc
    sortView(COLUMN_AMOUNT, Qt::DescendingOrder);

    // restore list mode and sortorder as a convenience feature
    QSettings settings;
    if (settings.contains("nCoinControlMode") && !settings.value("nCoinControlMode").toBool())
        ui->radioTreeMode->click();
    if (settings.contains("nCoinControlSortColumn") && settings.contains("nCoinControlSortOrder"))
        sortView(settings.value("nCoinControlSortColumn").toInt(), ((Qt::SortOrder)settings.value("nCoinControlSortOrder").toInt()));

    // Add the assets into the dropdown menu
    connect(ui->viewAdministrator, SIGNAL(clicked()), this, SLOT(viewAdministratorClicked()));
    connect(ui->assetList, SIGNAL(currentIndexChanged(QString)), this, SLOT(onAssetSelected(QString)));

    /** Setup the asset list combobox */
    stringModel = new QStringListModel;

    proxy = new QSortFilterProxyModel;
    proxy->setSourceModel(stringModel);
    proxy->setFilterCaseSensitivity(Qt::CaseInsensitive);

    ui->assetList->setModel(proxy);
    ui->assetList->setEditable(true);
    ui->assetList->lineEdit()->setPlaceholderText("Select an asset");
    ui->assetList->lineEdit()->setStyleSheet("border: none; background: transparent;");

    completer = new QCompleter(proxy,this);
    completer->setCompletionMode(QCompleter::PopupCompletion);
    completer->setCaseSensitivity(Qt::CaseInsensitive);
    ui->assetList->setCompleter(completer);
}

AssetControlDialog::~AssetControlDialog()
{
    QSettings settings;
    settings.setValue("nCoinControlMode", ui->radioListMode->isChecked());
    settings.setValue("nCoinControlSortColumn", sortColumn);
    settings.setValue("nCoinControlSortOrder", (int)sortOrder);

    delete ui;
}

void AssetControlDialog::setModel(WalletModel *_model)
{
    this->model = _model;

    if(_model && _model->getOptionsModel() && _model->getAddressTableModel())
    {
        updateView();
        updateAssetList(true);
        updateLabelLocked();
        AssetControlDialog::updateLabels(_model, this);
    }
}

// ok button
void AssetControlDialog::buttonBoxClicked(QAbstractButton* button)
{
    if (ui->buttonBox->buttonRole(button) == QDialogButtonBox::AcceptRole) {
        if (AssetControlDialog::assetControl()->HasAssetSelected())
            AssetControlDialog::assetControl()->strAssetSelected = ui->assetList->currentText().toStdString();
        done(QDialog::Accepted);
    }
}

// (un)select all
void AssetControlDialog::buttonSelectAllClicked()
{
    Qt::CheckState state = Qt::Checked;
    for (int i = 0; i < ui->treeWidget->topLevelItemCount(); i++)
    {
        if (ui->treeWidget->topLevelItem(i)->checkState(COLUMN_CHECKBOX) != Qt::Unchecked)
        {
            state = Qt::Unchecked;
            break;
        }
    }
    ui->treeWidget->setEnabled(false);
    for (int i = 0; i < ui->treeWidget->topLevelItemCount(); i++)
            if (ui->treeWidget->topLevelItem(i)->checkState(COLUMN_CHECKBOX) != state)
                ui->treeWidget->topLevelItem(i)->setCheckState(COLUMN_CHECKBOX, state);
    ui->treeWidget->setEnabled(true);
    if (state == Qt::Unchecked)
        assetControl()->UnSelectAll();
    AssetControlDialog::updateLabels(model, this);
}

// context menu
void AssetControlDialog::showMenu(const QPoint &point)
{
    QTreeWidgetItem *item = ui->treeWidget->itemAt(point);
    if(item)
    {
        contextMenuItem = item;

        if (item->text(COLUMN_TXHASH).length() == 64)
        {
            copyTransactionHashAction->setEnabled(true);
            if (model->wallet().isLockedCoin(COutPoint(Txid::FromUint256(uint256::FromHex(item->text(COLUMN_TXHASH).toStdString()).value()), item->text(COLUMN_VOUT_INDEX).toUInt())))
            {
                lockAction->setEnabled(false);
                unlockAction->setEnabled(true);
            }
            else
            {
                lockAction->setEnabled(true);
                unlockAction->setEnabled(false);
            }
        }
        else
        {
            copyTransactionHashAction->setEnabled(false);
            lockAction->setEnabled(false);
            unlockAction->setEnabled(false);
        }

        contextMenu->exec(QCursor::pos());
    }
}

// context menu action: copy amount
void AssetControlDialog::copyAmount()
{
    GUIUtil::setClipboard(BitcoinUnits::removeSpaces(contextMenuItem->text(COLUMN_AMOUNT)));
}

// context menu action: copy label
void AssetControlDialog::copyLabel()
{
    if (ui->radioTreeMode->isChecked() && contextMenuItem->text(COLUMN_LABEL).length() == 0 && contextMenuItem->parent())
        GUIUtil::setClipboard(contextMenuItem->parent()->text(COLUMN_LABEL));
    else
        GUIUtil::setClipboard(contextMenuItem->text(COLUMN_LABEL));
}

// context menu action: copy address
void AssetControlDialog::copyAddress()
{
    if (ui->radioTreeMode->isChecked() && contextMenuItem->text(COLUMN_ADDRESS).length() == 0 && contextMenuItem->parent())
        GUIUtil::setClipboard(contextMenuItem->parent()->text(COLUMN_ADDRESS));
    else
        GUIUtil::setClipboard(contextMenuItem->text(COLUMN_ADDRESS));
}

// context menu action: copy transaction id
void AssetControlDialog::copyTransactionHash()
{
    GUIUtil::setClipboard(contextMenuItem->text(COLUMN_TXHASH));
}

// context menu action: lock coin
void AssetControlDialog::lockCoin()
{
    if (contextMenuItem->checkState(COLUMN_CHECKBOX) == Qt::Checked)
        contextMenuItem->setCheckState(COLUMN_CHECKBOX, Qt::Unchecked);

    COutPoint outpt(Txid::FromUint256(uint256::FromHex(contextMenuItem->text(COLUMN_TXHASH).toStdString()).value()), contextMenuItem->text(COLUMN_VOUT_INDEX).toUInt());
    model->wallet().lockCoin(outpt, true);
    contextMenuItem->setDisabled(true);
    contextMenuItem->setIcon(COLUMN_CHECKBOX, platformStyle->SingleColorIcon(":/icons/lock_closed"));
    updateLabelLocked();
}

// context menu action: unlock coin
void AssetControlDialog::unlockCoin()
{
    COutPoint outpt(Txid::FromUint256(uint256::FromHex(contextMenuItem->text(COLUMN_TXHASH).toStdString()).value()), contextMenuItem->text(COLUMN_VOUT_INDEX).toUInt());
    model->wallet().unlockCoin(outpt);
    contextMenuItem->setDisabled(false);
    contextMenuItem->setIcon(COLUMN_CHECKBOX, QIcon());
    updateLabelLocked();
}

// copy label "Quantity" to clipboard
void AssetControlDialog::clipboardQuantity()
{
    GUIUtil::setClipboard(ui->labelAssetControlQuantity->text());
}

// copy label "Amount" to clipboard
void AssetControlDialog::clipboardAmount()
{
    GUIUtil::setClipboard(ui->labelAssetControlAmount->text().left(ui->labelAssetControlAmount->text().indexOf(" ")));
}

// copy label "Fee" to clipboard
void AssetControlDialog::clipboardFee()
{
    GUIUtil::setClipboard(ui->labelAssetControlFee->text().left(ui->labelAssetControlFee->text().indexOf(" ")).replace(ASYMP_UTF8, ""));
}

// copy label "After fee" to clipboard
void AssetControlDialog::clipboardAfterFee()
{
    GUIUtil::setClipboard(ui->labelAssetControlAfterFee->text().left(ui->labelAssetControlAfterFee->text().indexOf(" ")).replace(ASYMP_UTF8, ""));
}

// copy label "Bytes" to clipboard
void AssetControlDialog::clipboardBytes()
{
    GUIUtil::setClipboard(ui->labelAssetControlBytes->text().replace(ASYMP_UTF8, ""));
}

// copy label "Dust" to clipboard
void AssetControlDialog::clipboardLowOutput()
{
    GUIUtil::setClipboard(ui->labelAssetControlLowOutput->text());
}

// copy label "Change" to clipboard
void AssetControlDialog::clipboardChange()
{
    GUIUtil::setClipboard(ui->labelAssetControlChange->text().left(ui->labelAssetControlChange->text().indexOf(" ")).replace(ASYMP_UTF8, ""));
}

// treeview: sort
void AssetControlDialog::sortView(int column, Qt::SortOrder order)
{
    sortColumn = column;
    sortOrder = order;
    ui->treeWidget->sortItems(column, order);
    ui->treeWidget->header()->setSortIndicator(sortColumn, sortOrder);
}

// treeview: clicked on header
void AssetControlDialog::headerSectionClicked(int logicalIndex)
{
    if (logicalIndex == COLUMN_CHECKBOX)
    {
        ui->treeWidget->header()->setSortIndicator(sortColumn, sortOrder);
    }
    else
    {
        if (sortColumn == logicalIndex)
            sortOrder = ((sortOrder == Qt::AscendingOrder) ? Qt::DescendingOrder : Qt::AscendingOrder);
        else
        {
            sortColumn = logicalIndex;
            sortOrder = ((sortColumn == COLUMN_LABEL || sortColumn == COLUMN_ADDRESS) ? Qt::AscendingOrder : Qt::DescendingOrder);
        }

        sortView(sortColumn, sortOrder);
    }
}

// toggle tree mode
void AssetControlDialog::radioTreeMode(bool checked)
{
    if (checked && model)
        updateView();
}

// toggle list mode
void AssetControlDialog::radioListMode(bool checked)
{
    if (checked && model)
        updateView();
}

// checkbox clicked by user
void AssetControlDialog::viewItemChanged(QTreeWidgetItem* item, int column)
{
    if (column == COLUMN_CHECKBOX && item->text(COLUMN_TXHASH).length() == 64)
    {
        COutPoint outpt(Txid::FromUint256(uint256::FromHex(item->text(COLUMN_TXHASH).toStdString()).value()), item->text(COLUMN_VOUT_INDEX).toUInt());

        if (item->checkState(COLUMN_CHECKBOX) == Qt::Unchecked)
            assetControl()->UnSelectAsset(outpt);
        else if (item->isDisabled())
            item->setCheckState(COLUMN_CHECKBOX, Qt::Unchecked);
        else
            assetControl()->SelectAsset(outpt);

        if (ui->treeWidget->isEnabled())
            AssetControlDialog::updateLabels(model, this);
    }
    else if (column == COLUMN_CHECKBOX && item->childCount() > 0)
    {
        if (item->checkState(COLUMN_CHECKBOX) == Qt::PartiallyChecked && item->child(0)->checkState(COLUMN_CHECKBOX) == Qt::PartiallyChecked)
            item->setCheckState(COLUMN_CHECKBOX, Qt::Checked);
    }
}

// shows count of locked unspent outputs
void AssetControlDialog::updateLabelLocked()
{
    std::vector<COutPoint> vOutpts;
    model->wallet().listLockedCoins(vOutpts);
    if (vOutpts.size() > 0)
    {
       ui->labelLocked->setText(tr("(%1 locked)").arg(vOutpts.size()));
       ui->labelLocked->setVisible(true);
    }
    else ui->labelLocked->setVisible(false);
}

void AssetControlDialog::updateLabels(WalletModel *model, QDialog* dialog)
{
    if (!model)
        return;

    // nPayAmount
    CAmount nPayAmount = 0;
    for (const CAmount &amount : AssetControlDialog::payAmounts)
    {
        nPayAmount += amount;
    }

    std::string strAssetName = assetControl()->strAssetSelected;
    CAmount nAssetAmount        = 0;
    CAmount nPayFee             = 0;
    CAmount nAfterFee           = 0;
    CAmount nChange             = 0;
    unsigned int nBytes         = 0;
    unsigned int nQuantity      = 0;
    bool fDust                  = false;

    // Calculate from selected coins
    if (!strAssetName.empty() && model) {
        wallet::CWallet* pwallet = model->wallet().wallet();
        if (pwallet) {
            std::vector<COutPoint> vSelected = assetControl()->ListSelected();
            LOCK(pwallet->cs_wallet);
            wallet::CoinFilterParams params;
            params.min_amount = 0;
            params.check_version_trucness = false;
            wallet::CoinsResult available = wallet::AvailableCoinsWithAssets(*pwallet, nullptr, std::nullopt, params);
            auto it = available.mapAssetCoins.find(strAssetName);
            if (it != available.mapAssetCoins.end()) {
                for (const auto& output : it->second) {
                    if (assetControl()->IsSelected(output.outpoint)) {
                        CAssetOutputEntry data;
                        if (GetAssetData(output.txout.scriptPubKey, data)) {
                            nAssetAmount += data.nAmount;
                            nQuantity++;
                            nBytes += 148; // Estimate per-input size
                        }
                    }
                }
            }
        }

        if (nQuantity > 0) {
            nBytes += 34; // Output size estimate
            nBytes += 10; // Overhead
            nPayFee = model->wallet().getRequiredFee(nBytes);
            nAfterFee = nAssetAmount;
            nChange = nAssetAmount - nPayAmount;
        }
    }

    // actually update labels
    BitcoinUnits::Unit nDisplayUnit = BitcoinUnits::Unit::BTC;
    if (model && model->getOptionsModel())
        nDisplayUnit = model->getOptionsModel()->getDisplayUnit();

    QLabel *l1 = dialog->findChild<QLabel *>("labelAssetControlQuantity");
    QLabel *l2 = dialog->findChild<QLabel *>("labelAssetControlAmount");
    QLabel *l3 = dialog->findChild<QLabel *>("labelAssetControlFee");
    QLabel *l4 = dialog->findChild<QLabel *>("labelAssetControlAfterFee");
    QLabel *l5 = dialog->findChild<QLabel *>("labelAssetControlBytes");
    QLabel *l7 = dialog->findChild<QLabel *>("labelAssetControlLowOutput");
    QLabel *l8 = dialog->findChild<QLabel *>("labelAssetControlChange");

    // enable/disable "dust" and "change"
    dialog->findChild<QLabel *>("labelAssetControlLowOutputText")->setEnabled(nPayAmount > 0);
    dialog->findChild<QLabel *>("labelAssetControlLowOutput")    ->setEnabled(nPayAmount > 0);
    dialog->findChild<QLabel *>("labelAssetControlChangeText")   ->setEnabled(nPayAmount > 0);
    dialog->findChild<QLabel *>("labelAssetControlChange")       ->setEnabled(nPayAmount > 0);

    // stats
    l1->setText(QString::number(nQuantity));
    l2->setText(formatWithCustomName(QString::fromStdString(strAssetName), nAssetAmount));
    l3->setText(BitcoinUnits::formatWithUnit(nDisplayUnit, nPayFee));
    l4->setText(BitcoinUnits::formatWithUnit(nDisplayUnit, nAfterFee));
    l5->setText(((nBytes > 0) ? ASYMP_UTF8 : "") + QString::number(nBytes));
    l7->setText(fDust ? tr("yes") : tr("no"));
    l8->setText(formatWithCustomName(QString::fromStdString(strAssetName), nChange));
    if (nPayFee > 0)
    {
        l3->setText(ASYMP_UTF8 + l3->text());
        l4->setText(ASYMP_UTF8 + l4->text());
    }

    // turn label red when dust
    l7->setStyleSheet((fDust) ? "color:red;" : "");

    // tool tips
    QString toolTipDust = tr("This label turns red if any recipient receives an amount smaller than the current dust threshold.");
    double dFeeVary = (nBytes != 0) ? (double)nPayFee / nBytes : 0;
    QString toolTip4 = tr("Can vary +/- %1 avianshi(s) per input.").arg(dFeeVary);

    l3->setToolTip(toolTip4);
    l4->setToolTip(toolTip4);
    l7->setToolTip(toolTipDust);
    l8->setToolTip(toolTip4);
    dialog->findChild<QLabel *>("labelAssetControlFeeText")      ->setToolTip(l3->toolTip());
    dialog->findChild<QLabel *>("labelAssetControlAfterFeeText") ->setToolTip(l4->toolTip());
    dialog->findChild<QLabel *>("labelAssetControlBytesText")    ->setToolTip(l5->toolTip());
    dialog->findChild<QLabel *>("labelAssetControlLowOutputText")->setToolTip(l7->toolTip());
    dialog->findChild<QLabel *>("labelAssetControlChangeText")   ->setToolTip(l8->toolTip());

    // Insufficient funds
    QLabel *label = dialog->findChild<QLabel *>("labelAssetControlInsuffFunds");
    if (label)
        label->setVisible(nChange < 0);
}

void AssetControlDialog::updateView()
{
    if (!model || !model->getOptionsModel() || !model->getAddressTableModel())
        return;

    bool treeMode = ui->radioTreeMode->isChecked();

    ui->treeWidget->clear();
    ui->treeWidget->setEnabled(false);
    ui->treeWidget->setAlternatingRowColors(!treeMode);

    std::string strSelectedAsset = assetControl()->strAssetSelected;
    if (strSelectedAsset.empty()) {
        sortView(sortColumn, sortOrder);
        ui->treeWidget->setEnabled(true);
        return;
    }

    wallet::CWallet* pwallet = model->wallet().wallet();
    if (!pwallet) {
        sortView(sortColumn, sortOrder);
        ui->treeWidget->setEnabled(true);
        return;
    }

    // Get asset UTXOs for the selected asset
    std::map<std::string, std::vector<wallet::COutput>> mapAssetCoins;
    {
        LOCK(pwallet->cs_wallet);
        wallet::CoinFilterParams params;
        params.min_amount = 0;
        params.check_version_trucness = false;
        wallet::CoinsResult available = wallet::AvailableCoinsWithAssets(*pwallet, nullptr, std::nullopt, params);
        mapAssetCoins = available.mapAssetCoins;
    }

    auto it = mapAssetCoins.find(strSelectedAsset);
    if (it == mapAssetCoins.end()) {
        sortView(sortColumn, sortOrder);
        ui->treeWidget->setEnabled(true);
        return;
    }

    // Group by address for tree mode
    std::map<QString, std::vector<const wallet::COutput*>> mapAddressOutputs;
    for (const auto& output : it->second) {
        CTxDestination address;
        if (ExtractDestination(output.txout.scriptPubKey, address)) {
            QString sAddress = QString::fromStdString(EncodeDestination(address));
            mapAddressOutputs[sAddress].push_back(&output);
        }
    }

    for (const auto& [sAddress, outputs] : mapAddressOutputs) {
        CAssetControlWidgetItem* itemWalletAddress = nullptr;
        if (treeMode) {
            itemWalletAddress = new CAssetControlWidgetItem(ui->treeWidget);
            itemWalletAddress->setFlags(itemWalletAddress->flags() | Qt::ItemIsUserCheckable);
            itemWalletAddress->setCheckState(COLUMN_CHECKBOX, Qt::Unchecked);

            // Address
            itemWalletAddress->setText(COLUMN_ADDRESS, sAddress);

            // Label
            QString sLabel = model->getAddressTableModel()->labelForAddress(sAddress);
            itemWalletAddress->setText(COLUMN_LABEL, sLabel);

            CAmount nSum = 0;
            for (const auto* out : outputs) {
                CAssetOutputEntry data;
                if (GetAssetData(out->txout.scriptPubKey, data))
                    nSum += data.nAmount;
            }
            itemWalletAddress->setText(COLUMN_AMOUNT, BitcoinUnits::format(BitcoinUnits::Unit::BTC, nSum));
            itemWalletAddress->setData(COLUMN_AMOUNT, Qt::UserRole, QVariant((qlonglong)nSum));
            itemWalletAddress->setText(COLUMN_ASSET_NAME, QString::fromStdString(strSelectedAsset));
        }

        for (const auto* out : outputs) {
            CAssetOutputEntry assetData;
            if (!GetAssetData(out->txout.scriptPubKey, assetData))
                continue;

            CAssetControlWidgetItem* itemOutput;
            if (treeMode) {
                itemOutput = new CAssetControlWidgetItem(itemWalletAddress);
            } else {
                itemOutput = new CAssetControlWidgetItem(ui->treeWidget);
            }
            itemOutput->setFlags(itemOutput->flags() | Qt::ItemIsUserCheckable);
            itemOutput->setCheckState(COLUMN_CHECKBOX, Qt::Unchecked);

            // Asset name
            itemOutput->setText(COLUMN_ASSET_NAME, QString::fromStdString(strSelectedAsset));

            // Amount
            itemOutput->setText(COLUMN_AMOUNT, BitcoinUnits::format(BitcoinUnits::Unit::BTC, assetData.nAmount));
            itemOutput->setData(COLUMN_AMOUNT, Qt::UserRole, QVariant((qlonglong)assetData.nAmount));

            // Address
            itemOutput->setText(COLUMN_ADDRESS, sAddress);

            // Label
            QString sLabel = model->getAddressTableModel()->labelForAddress(sAddress);
            if (sLabel.isEmpty())
                sLabel = tr("(no label)");
            itemOutput->setText(COLUMN_LABEL, sLabel);

            // Confirmations
            itemOutput->setText(COLUMN_CONFIRMATIONS, QString::number(out->depth));
            itemOutput->setData(COLUMN_CONFIRMATIONS, Qt::UserRole, QVariant((qlonglong)out->depth));

            // Transaction hash
            itemOutput->setText(COLUMN_TXHASH, QString::fromStdString(out->outpoint.hash.GetHex()));

            // Vout index
            itemOutput->setText(COLUMN_VOUT_INDEX, QString::number(out->outpoint.n));

            // Set checkbox state if selected in coin control
            if (assetControl()->IsSelected(out->outpoint))
                itemOutput->setCheckState(COLUMN_CHECKBOX, Qt::Checked);
        }
    }

    // sort view
    sortView(sortColumn, sortOrder);
    ui->treeWidget->setEnabled(true);
}

void AssetControlDialog::viewAdministratorClicked()
{
    assetControl()->UnSelectAll();
    AssetControlDialog::updateLabels(model, this);
    updateAssetList();
}

void AssetControlDialog::updateAssetList(bool fSetOnStart)
{
    if (!model || !model->getOptionsModel() || !model->getAddressTableModel())
        return;

    QStringList list;
    list << "";
    wallet::CWallet* pwallet = model->wallet().wallet();
    if (pwallet) {
        LOCK(pwallet->cs_wallet);
        wallet::CoinFilterParams params;
        params.min_amount = 0;
        wallet::CoinsResult available = wallet::AvailableCoinsWithAssets(*pwallet, nullptr, std::nullopt, params);
        for (const auto& [assetName, assetOutputs] : available.mapAssetCoins) {
            list << QString::fromStdString(assetName);
        }
    }
    stringModel->setStringList(list);

    int index = ui->assetList->findText(QString::fromStdString(assetControl()->strAssetSelected));
    if (index != -1 ) {
        fOnStartUp = fSetOnStart;
        ui->assetList->setCurrentIndex(index);
    }

    updateView();
}

void AssetControlDialog::onAssetSelected(QString name)
{
    if (fOnStartUp) {
        fOnStartUp = false;
    } else {
        assetControl()->UnSelectAll();
    }

    AssetControlDialog::updateLabels(model, this);
    updateView();
}
