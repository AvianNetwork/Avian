// Copyright (c) 2022 The Avian Core developers
// Copyright (c) 2022 Shafil Alam
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#if defined(HAVE_CONFIG_H)
#include "config/avian-config.h"
#endif

#include "flightplans.h"
#include "ui_flightplans.h"
#include "flightplans/flightplans.h"

#include "guiutil.h"
#include "platformstyle.h"
#include "guiconstants.h"
#include "validation.h"
#include "util.h"

#include <vector>
#include <string>

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

    // Set data dir label
    ui->labelDatadir->setText(tr("List of flightplans in: ") + QString::fromStdString((GetDataDir() / "flightplans" ).string()));

    // Set warning
    if (!AreFlightPlansDeployed()) {
        ui->labelAlerts->setText(tr("Warning: Avian Flight Plans are not deployed."));
    }

    if (!gArgs.IsArgSet("-flightplans") && AreFlightPlansDeployed()) {
        ui->labelAlerts->setText(tr("Warning: Avian Flight Plans are deployed but is disabled."));
    }

    if (gArgs.IsArgSet("-flightplans") && AreFlightPlansDeployed()) {
        ui->labelAlerts->setText(tr("Warning: Avian Flight Plans are ACTIVE! Please exercise extreme caution."));
    }

    // List all flight plans
    auto plans = CAvianFlightPlans::GetPlans();
    ui->listWidget->addItem(QString::fromStdString(std::string("There are ") + std::to_string(plans.size()) + std::string(" flightplans.")));
    for(const std::string& plan : plans) {
        ui->listWidget->addItem(QString::fromStdString(plan));
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
