// Copyright (c) 2021 The Avian Core developers
// Copyright (c) 2021 Shafil Alam
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef AVIAN_QT_PEERDIALOG_H
#define AVIAN_QT_PEERDIALOG_H

#include <QObject>
#include <QWidget>
#include <string>
#include "guiutil.h"

class PeerTools;

namespace Ui {
    class AddPeerDialog;
    class RemovePeerDialog;
    class TestPeerDialog;
}

/** Class to manage peers */
class PeerTools : public QObject
{
    Q_OBJECT

public:
    static QString ManagePeer(QString type, QString peer);
    static bool CheckPeerAddress(QString address); 
    static bool CheckIPAddress(QString ip);
    static bool CheckDNS(QString dns);
    static QString GetPort();
};

/** "Add peer" dialog box */
class AddPeerDialog : public QWidget
{
    Q_OBJECT

public:
    explicit AddPeerDialog(QWidget *parent);
    ~AddPeerDialog();
private:
    Ui::AddPeerDialog *ui;
public Q_SLOTS:
    void on_addPeerClicked();
};

/** "Test peer" dialog box */
class TestPeerDialog : public QWidget
{
    Q_OBJECT

public:
    explicit TestPeerDialog(QWidget *parent);
    ~TestPeerDialog();
private:
    Ui::TestPeerDialog *ui;
public Q_SLOTS:
    void on_testPeerClicked();
};

#endif
