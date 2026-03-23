// Copyright (c) 2017-2019 The Raven Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/restrictedfreezeaddress.h>
#include "ui_restrictedfreezeaddress.h"

FreezeAddress::FreezeAddress(const PlatformStyle *_platformStyle, QWidget *parent) :
    QWidget(parent),
    ui(new Ui::FreezeAddress),
    platformStyle(_platformStyle)
{
    ui->setupUi(this);
}

FreezeAddress::~FreezeAddress()
{
    delete ui;
}

void FreezeAddress::setWalletModel(WalletModel *_model)
{
    this->model = _model;
}

void FreezeAddress::clear()
{
}
