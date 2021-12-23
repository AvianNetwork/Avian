// Copyright (c) 2021 The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#if defined(HAVE_CONFIG_H)
#include "config/avian-config.h"
#endif

#include "poolpicker.h"
#include "ui_poolpicker.h"

#include "guiutil.h"
#include "util.h"

#include <QMessageBox>
#include <QSignalMapper>
#include <QStringList>

#if QT_VERSION < 0x050000
#include <QUrl>
#endif

#include "poolpicker.moc"

PoolPicker::PoolPicker(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::PoolPicker)
{
    ui->setupUi(this);

    // Connection example
    // connect(ui->peerTest, SIGNAL(clicked()), this, SLOT(on_testPeer_clicked()));
}

PoolPicker::~PoolPicker()
{
    delete ui;
}
