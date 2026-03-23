// Copyright (c) 2017-2019 The Raven Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef AVIAN_QT_RESTRICTEDFREEZEADDRESS_H
#define AVIAN_QT_RESTRICTEDFREEZEADDRESS_H

#include <QWidget>

class PlatformStyle;
class WalletModel;

namespace Ui {
    class FreezeAddress;
}

class FreezeAddress : public QWidget
{
    Q_OBJECT
public:
    explicit FreezeAddress(const PlatformStyle *platformStyle, QWidget *parent = nullptr);
    ~FreezeAddress();

    void setWalletModel(WalletModel *model);
    Ui::FreezeAddress *getUI() { return ui; }
    void clear();

private:
    Ui::FreezeAddress *ui;
    WalletModel *model{nullptr};
    const PlatformStyle *platformStyle;
};

#endif // AVIAN_QT_RESTRICTEDFREEZEADDRESS_H
