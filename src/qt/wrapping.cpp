// Copyright (c) 2022 The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#if defined(HAVE_CONFIG_H)
#include "config/avian-config.h"
#endif

#include "wrapping.h"
#include "ui_wrapping.h"

#include "addresstablemodel.h"
#include "avianunits.h"
#include "clientmodel.h"
#include "coincontroldialog.h"
#include "guiutil.h"
#include "optionsmodel.h"
#include "platformstyle.h"
#include "sendcoinsentry.h"
#include "walletmodel.h"
#include "guiconstants.h"

#include <QMessageBox>
#include <QSignalMapper>
#include <QStringList>

#include <QPainter>
#include <QPainterPath>
#include <QGraphicsDropShadowEffect>

#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>

#if QT_VERSION < 0x050000
#include <QUrl>
#endif

WrapPage::WrapPage(const PlatformStyle *platformStyle, QWidget *parent) :
    QDialog(parent),
    model(0),
    ui(new Ui::WrapPage)
{
    ui->setupUi(this);

    // Connection example
    // connect(ui->peerTest, SIGNAL(clicked()), this, SLOT(on_testPeer_clicked()));

    /** Set the the frames colors and padding */
    ui->wrapFrame->setStyleSheet(QString(".QFrame {background-color: %1; padding-top: 10px; padding-right: 5px; border-radius: 10px;}").arg(platformStyle->WidgetBackGroundColor().name()));
    ui->unwrapFrame->setStyleSheet(QString(".QFrame {background-color: %1; padding-bottom: 10px; padding-right: 5px; border-radius: 10px;}").arg(platformStyle->WidgetBackGroundColor().name()));

    /** Create the shadow effects on the frames */
    ui->wrapFrame->setGraphicsEffect(GUIUtil::getShadowEffect());
    ui->unwrapFrame->setGraphicsEffect(GUIUtil::getShadowEffect());

    /** Connect signals */
    connect(ui->wrapButton, SIGNAL(clicked()), this, SLOT(wrapped_clicked()));
}

void WrapPage::wrapped_clicked()
{
    /** Show warning */
    QMessageBox msgBox;
    msgBox.setIcon(QMessageBox::Information);
    msgBox.setWindowTitle("Wrapping Notice");
    msgBox.setText("Wrapped Avian is NOT managed by Avian Core and is run by a 3rd party. We are not responsible for any coin LOSS. By using this feature, you must understand the RISK.");

    QAbstractButton *buttonWrap = msgBox.addButton(tr("I understand the risk"), QMessageBox::YesRole);
    QAbstractButton *buttonAbort = msgBox.addButton(tr("Cancel request"), QMessageBox::NoRole);

    msgBox.exec();

    if (msgBox.clickedButton() == buttonWrap) {
    // Wrap
    }
}

void WrapPage::setModel(WalletModel *_model)
{
    this->model = _model;

    if(_model && _model->getOptionsModel())
    {
        setBalance(_model->getBalance(), _model->getUnconfirmedBalance(), _model->getImmatureBalance(),
                   _model->getWatchBalance(), _model->getWatchUnconfirmedBalance(), _model->getWatchImmatureBalance());
        connect(_model, SIGNAL(balanceChanged(CAmount,CAmount,CAmount,CAmount,CAmount,CAmount)), this, SLOT(setBalance(CAmount,CAmount,CAmount,CAmount,CAmount,CAmount)));
    }
}

void WrapPage::setBalance(const CAmount& balance, const CAmount& unconfirmedBalance, const CAmount& immatureBalance,
                                 const CAmount& watchBalance, const CAmount& watchUnconfirmedBalance, const CAmount& watchImmatureBalance)
{
    Q_UNUSED(unconfirmedBalance);
    Q_UNUSED(immatureBalance);
    Q_UNUSED(watchBalance);
    Q_UNUSED(watchUnconfirmedBalance);
    Q_UNUSED(watchImmatureBalance);

    if(model && model->getOptionsModel())
    {
        ui->labelBalance->setText(AvianUnits::formatWithUnit(model->getOptionsModel()->getDisplayUnit(), balance));
    }
}

WrapPage::~WrapPage()
{
    delete ui;
}
