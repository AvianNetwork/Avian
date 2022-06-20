// Copyright (c) 2021 The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef AVIAN_POOLPICKER_H
#define AVIAN_POOLPICKER_H

#include "guiutil.h"
#include "net.h"

#include <QDialog>
#include <QCompleter>
#include <QThread>

class PlatformStyle;

namespace Ui {
    class PoolPicker;
}

class PoolPicker: public QDialog
{
    Q_OBJECT

public:
    explicit PoolPicker(QWidget *parent);
    ~PoolPicker();

private Q_SLOTS:
    /** test slot */
    // void on_addPeer_clicked();

private:
    Ui::PoolPicker *ui;
};

#endif // AVIAN_POOLPICKER_H
