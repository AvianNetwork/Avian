// Copyright (c) 2017-2019 The Raven Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/restrictedfreezeaddress.h>
#include <qt/forms/ui_restrictedfreezeaddress.h>

#include <qt/assettablemodel.h>
#include <qt/walletmodel.h>

FreezeAddress::FreezeAddress(const PlatformStyle *_platformStyle, QWidget *parent) :
    QWidget(parent),
    ui(new Ui::FreezeAddress),
    platformStyle(_platformStyle)
{
    ui->setupUi(this);

    // Toggle change-address field when the checkbox is clicked
    connect(ui->checkBoxChangeAddress, &QCheckBox::toggled,
            ui->lineEditChangeAddress, &QLineEdit::setEnabled);
}

FreezeAddress::~FreezeAddress()
{
    delete ui;
}

void FreezeAddress::setWalletModel(WalletModel *_model)
{
    this->model = _model;
    if (!_model)
        return;

    AssetTableModel *assetModel = _model->getAssetTableModel();
    connect(assetModel, &QAbstractItemModel::dataChanged,   this, &FreezeAddress::populateAssetComboBox);
    connect(assetModel, &QAbstractItemModel::layoutChanged, this, &FreezeAddress::populateAssetComboBox);
    connect(assetModel, &QAbstractItemModel::rowsInserted,  this, &FreezeAddress::populateAssetComboBox);
    connect(assetModel, &QAbstractItemModel::rowsRemoved,   this, &FreezeAddress::populateAssetComboBox);

    populateAssetComboBox();
}

void FreezeAddress::populateAssetComboBox()
{
    if (!model)
        return;

    AssetTableModel *assetModel = model->getAssetTableModel();
    QString current = ui->assetComboBox->currentText();
    ui->assetComboBox->clear();

    for (int row = 0; row < assetModel->rowCount(QModelIndex()); ++row) {
        QModelIndex idx = assetModel->index(row, AssetTableModel::Name);
        if (!assetModel->data(idx, AssetTableModel::AdministratorRole).toBool())
            continue;

        QString name = assetModel->data(idx, AssetTableModel::AssetNameRole).toString();

        // AssetTableModel has two cases for admin assets:
        //   1. Wallet holds both "FOO" and "FOO!" → row shows "FOO"  (AdministratorRole=true, FOO! skipped)
        //   2. Wallet holds only "FOO!"           → row shows "FOO!" (AdministratorRole=true)
        QString baseName = name.endsWith('!') ? name.chopped(1) : name;

        // Only root assets own restricted assets; skip sub-assets, qualifiers, and restricted entries
        if (baseName.contains('/') || baseName.startsWith('#') || baseName.startsWith('$'))
            continue;

        ui->assetComboBox->addItem("$" + baseName);
    }

    // Restore previous selection if it is still present
    int idx = ui->assetComboBox->findText(current);
    if (idx >= 0)
        ui->assetComboBox->setCurrentIndex(idx);
}

void FreezeAddress::clear()
{
}
