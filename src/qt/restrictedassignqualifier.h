// Copyright (c) 2017-2019 The Raven Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef AVIAN_QT_RESTRICTEDASSIGNQUALIFIER_H
#define AVIAN_QT_RESTRICTEDASSIGNQUALIFIER_H

#include <QWidget>

class PlatformStyle;
class WalletModel;

namespace Ui {
    class AssignQualifier;
}

class AssignQualifier : public QWidget
{
    Q_OBJECT
public:
    explicit AssignQualifier(const PlatformStyle *platformStyle, QWidget *parent = nullptr);
    ~AssignQualifier();

    void setWalletModel(WalletModel *model);
    Ui::AssignQualifier *getUI() { return ui; }
    void clear();

private:
    Ui::AssignQualifier *ui;
    WalletModel *model{nullptr};
    const PlatformStyle *platformStyle;
};

#endif // AVIAN_QT_RESTRICTEDASSIGNQUALIFIER_H
