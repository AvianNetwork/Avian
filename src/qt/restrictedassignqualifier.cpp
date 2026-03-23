// Copyright (c) 2017-2019 The Raven Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/restrictedassignqualifier.h>
#include "ui_restrictedassignqualifier.h"

AssignQualifier::AssignQualifier(const PlatformStyle *_platformStyle, QWidget *parent) :
    QWidget(parent),
    ui(new Ui::AssignQualifier),
    platformStyle(_platformStyle)
{
    ui->setupUi(this);
}

AssignQualifier::~AssignQualifier()
{
    delete ui;
}

void AssignQualifier::setWalletModel(WalletModel *_model)
{
    this->model = _model;
}

void AssignQualifier::clear()
{
}
