// Copyright (c) 2022 The Avian Core developers
// Copyright (c) 2022 Shafil Alam
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#if defined(HAVE_CONFIG_H)
#include "config/avian-config.h"
#endif

#include "flightplans.h"
#include "ui_flightplans.h"

#include "guiutil.h"
#include "optionsmodel.h"
#include "platformstyle.h"
#include "guiconstants.h"
#include "validation.h"
#include "util.h"

#include <QMessageBox>
#include <QSignalMapper>
#include <QStringList>

#include <QPainter>
#include <QPainterPath>
#include <QGraphicsDropShadowEffect>

Flightplans::Flightplans(const PlatformStyle *platformStyle, QWidget *parent) :
    QDialog(parent),
    ui(new Ui::Flightplans)
{
    ui->setupUi(this);

    ui->label_sub->setText(tr("List of flightplans in: ") + QString::fromStdString((GetDataDir() / "flightplans" ).string()));

    if (!AreFlightPlansDeployed()) {
        ui->labelAlerts->setText(tr("Warning: Avian Flight Plans are not deployed."));
    }

    if (!gArgs.IsArgSet("-flightplans") && AreFlightPlansDeployed()) {
        ui->labelAlerts->setText(tr("Warning: Avian Flight Plans are deployed but is disabled."));
    }

    if (gArgs.IsArgSet("-flightplans") && AreFlightPlansDeployed()) {
        ui->labelAlerts->setText(tr("Warning: Avian Flight Plans are ACTIVE! Please exercise extreme caution."));
    }

    /** Connect signals */
    // connect(ui->wrapButton, SIGNAL(clicked()), this, SLOT(wrapped_clicked()));
}

void Flightplans::wrapped_clicked()
{
    /** Show warning */
}

Flightplans::~Flightplans()
{
    delete ui;
}
