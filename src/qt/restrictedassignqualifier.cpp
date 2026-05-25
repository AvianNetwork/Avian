// Copyright (c) 2017-2019 The Raven Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/restrictedassignqualifier.h>
#include "ui_restrictedassignqualifier.h"

#include <qt/assettablemodel.h>
#include <qt/walletmodel.h>

AssignQualifier::AssignQualifier(const PlatformStyle *_platformStyle, QWidget *parent) :
    QWidget(parent),
    ui(new Ui::AssignQualifier),
    platformStyle(_platformStyle)
{
    ui->setupUi(this);

    // Populate static assign/remove type options
    ui->assignTypeComboBox->addItem(tr("Assign"));
    ui->assignTypeComboBox->addItem(tr("Remove"));

    // Toggle change-address field when the checkbox is clicked
    connect(ui->checkBoxChangeAddress, &QCheckBox::toggled,
            ui->lineEditChangeAddress, &QLineEdit::setEnabled);
}

AssignQualifier::~AssignQualifier()
{
    delete ui;
}

void AssignQualifier::setWalletModel(WalletModel *_model)
{
    this->model = _model;
    if (!_model)
        return;

    AssetTableModel *assetModel = _model->getAssetTableModel();
    connect(assetModel, &QAbstractItemModel::dataChanged,   this, &AssignQualifier::populateAssetComboBox);
    connect(assetModel, &QAbstractItemModel::layoutChanged, this, &AssignQualifier::populateAssetComboBox);
    connect(assetModel, &QAbstractItemModel::rowsInserted,  this, &AssignQualifier::populateAssetComboBox);
    connect(assetModel, &QAbstractItemModel::rowsRemoved,   this, &AssignQualifier::populateAssetComboBox);

    populateAssetComboBox();
}

void AssignQualifier::populateAssetComboBox()
{
    if (!model)
        return;

    AssetTableModel *assetModel = model->getAssetTableModel();
    QString current = ui->assetComboBox->currentText();
    ui->assetComboBox->clear();

    for (int row = 0; row < assetModel->rowCount(QModelIndex()); ++row) {
        QModelIndex idx = assetModel->index(row, AssetTableModel::Name);
        QString name = assetModel->data(idx, AssetTableModel::AssetNameRole).toString();
        // Include qualifier assets (start with '#') but not their owner tokens (end with '!')
        if (name.startsWith('#') && !name.endsWith('!'))
            ui->assetComboBox->addItem(name);
    }

    // Restore previous selection if it is still present
    int idx = ui->assetComboBox->findText(current);
    if (idx >= 0)
        ui->assetComboBox->setCurrentIndex(idx);
}

void AssignQualifier::clear()
{
}
