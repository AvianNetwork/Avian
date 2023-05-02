// Copyright (c) 2022 The Avian Core developers
// Copyright (c) 2022 Shafil Alam
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef AVIAN_QT_FLIGHTPLANS_H
#define AVIAN_QT_FLIGHTPLANS_H

#include <QDialog>
#include <QThread>

class PlatformStyle;

namespace Ui {
    class Flightplans;
}

class Flightplans: public QDialog
{
    Q_OBJECT

public:
    explicit Flightplans(const PlatformStyle *platformStyle, QWidget *parent = 0);
    ~Flightplans();

private Q_SLOTS:
    void wrapped_clicked();

private:
    Ui::Flightplans *ui;
};

#endif // AVIAN_QT_FLIGHTPLANS_H
