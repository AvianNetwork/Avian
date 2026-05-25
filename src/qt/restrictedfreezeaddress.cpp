// Copyright (c) 2017-2019 The Raven Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/restrictedfreezeaddress.h>
#include "ui_restrictedfreezeaddress.h"

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
        if (!name.endsWith('!'))
            continue;

        // Owner token "FOO!" → restricted asset "$FOO"
        ui->assetComboBox->addItem("$" + name.chopped(1));
    }

    // Restore previous selection if it is still present
    int idx = ui->assetComboBox->findText(current);
    if (idx >= 0)
        ui->assetComboBox->setCurrentIndex(idx);
}

void FreezeAddress::clear()
{
}
