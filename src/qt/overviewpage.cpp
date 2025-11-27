// Copyright (c) 2011-2016 The Bitcoin Core developers
// Copyright (c) 2017-2020 The Raven Core developers
// Copyright (c) 2022 The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "overviewpage.h"
#include "ui_overviewpage.h"

#include "assetfilterproxy.h"
#include "assetrecord.h"
#include "assets/ans.h"
#include "assettablemodel.h"
#include "avianunits.h"
#include "clientmodel.h"
#include "guiconstants.h"
#include "guiutil.h"
#include "optionsmodel.h"
#include "platformstyle.h"
#include "transactionfilterproxy.h"
#include "transactiontablemodel.h"
#include "walletmodel.h"

#include <QAbstractItemDelegate>
#include <QDateTime>
#include <QDesktopServices>
#include <QMouseEvent>
#include <QPainter>
#include <utiltime.h>
#include <validation.h>

#define DECORATION_SIZE 54
#define NUM_ITEMS 8

#include <QDebug>
#include <QGraphicsDropShadowEffect>
#include <QPainterPath>
#include <QScrollBar>
#include <QTimer>
#include <QUrl>

#include <QtConcurrent/QtConcurrentRun>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>

#if QT_VERSION < QT_VERSION_CHECK(5, 11, 0)
#define QTversionPreFiveEleven
#endif

class TxViewDelegate : public QAbstractItemDelegate
{
    Q_OBJECT
public:
    explicit TxViewDelegate(const PlatformStyle* _platformStyle, QObject* parent = nullptr) : QAbstractItemDelegate(parent), unit(AvianUnits::AVN),
                                                                                              platformStyle(_platformStyle)
    {
    }

    inline void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const
    {
        painter->save();

        QIcon icon = qvariant_cast<QIcon>(index.data(TransactionTableModel::RawDecorationRole));
        QRect mainRect = option.rect;
        QRect decorationRect(mainRect.topLeft(), QSize(DECORATION_SIZE, DECORATION_SIZE));
        int xspace = DECORATION_SIZE + 8;
        int ypad = 6;
        int halfheight = (mainRect.height() - 2 * ypad) / 2;
        QRect amountRect(mainRect.left() + xspace, mainRect.top() + ypad, mainRect.width() - xspace, halfheight);
        QRect addressRect(mainRect.left() + xspace, mainRect.top() + ypad + halfheight, mainRect.width() - xspace, halfheight);

        icon = platformStyle->SingleColorIcon(icon, COLOR_AVIAN_18A7B7);
        icon.paint(painter, decorationRect);

        QDateTime date = index.data(TransactionTableModel::DateRole).toDateTime();
        QString address = index.data(Qt::DisplayRole).toString();
        qint64 amount = index.data(TransactionTableModel::AmountRole).toLongLong();
        bool confirmed = index.data(TransactionTableModel::ConfirmedRole).toBool();
        QVariant value = index.data(Qt::ForegroundRole);
        QColor foreground = platformStyle->TextColor();
        if (value.canConvert<QBrush>()) {
            QBrush brush = qvariant_cast<QBrush>(value);
            foreground = brush.color();
        }

        QString amountText = index.data(TransactionTableModel::FormattedAmountRole).toString();
        if (!confirmed) {
            amountText = QString("[") + amountText + QString("]");
        }

        painter->setFont(GUIUtil::getSubLabelFont());
// Concatenate the strings if needed before painting
#ifndef QTversionPreFiveEleven
        GUIUtil::concatenate(painter, address, painter->fontMetrics().horizontalAdvance(amountText), addressRect.left(), addressRect.right());
#else
        GUIUtil::concatenate(painter, address, painter->fontMetrics().width(amountText), addressRect.left(), addressRect.right());
#endif
        painter->setPen(foreground);
        QRect boundingRect;
        painter->drawText(addressRect, Qt::AlignLeft | Qt::AlignVCenter, address, &boundingRect);

        if (index.data(TransactionTableModel::WatchonlyRole).toBool()) {
            QIcon iconWatchonly = qvariant_cast<QIcon>(index.data(TransactionTableModel::WatchonlyDecorationRole));
            QRect watchonlyRect(boundingRect.right() + 5, mainRect.top() + ypad + halfheight, 16, halfheight);
            iconWatchonly.paint(painter, watchonlyRect);
        }

        if (amount < 0) {
            foreground = COLOR_NEGATIVE;
        } else if (!confirmed) {
            foreground = COLOR_UNCONFIRMED;
        } else {
            foreground = platformStyle->TextColor();
        }

        painter->setPen(foreground);
        painter->drawText(addressRect, Qt::AlignRight | Qt::AlignVCenter, amountText);

        QString assetName = index.data(TransactionTableModel::AssetNameRole).toString();

// Concatenate the strings if needed before painting
#ifndef QTversionPreFiveEleven
        GUIUtil::concatenate(painter, assetName, painter->fontMetrics().horizontalAdvance(GUIUtil::dateTimeStr(date)), amountRect.left(), amountRect.right());
#else
        GUIUtil::concatenate(painter, assetName, painter->fontMetrics().width(GUIUtil::dateTimeStr(date)), amountRect.left(), amountRect.right());
#endif
        painter->drawText(amountRect, Qt::AlignRight | Qt::AlignVCenter, assetName);

        painter->setPen(platformStyle->TextColor());
        painter->drawText(amountRect, Qt::AlignLeft | Qt::AlignVCenter, GUIUtil::dateTimeStr(date));

        painter->restore();
    }

    inline QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const
    {
        return QSize(DECORATION_SIZE, DECORATION_SIZE);
    }

    int unit;
    const PlatformStyle* platformStyle;
};

class AssetViewDelegate : public QAbstractItemDelegate
{
    Q_OBJECT
public:
    explicit AssetViewDelegate(const PlatformStyle* _platformStyle, QObject* parent = nullptr) : QAbstractItemDelegate(parent), unit(AvianUnits::AVN),
                                                                                                 platformStyle(_platformStyle)
    {
    }

    inline void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const
    {
        painter->save();

        /** Get the icon for the administrator of the asset */
        QPixmap pixmap = qvariant_cast<QPixmap>(index.data(Qt::DecorationRole));
        QPixmap ipfspixmap = qvariant_cast<QPixmap>(index.data(AssetTableModel::AssetIPFSHashDecorationRole));
        QPixmap anspixmap = qvariant_cast<QPixmap>(index.data(AssetTableModel::AssetANSDecorationRole));

        bool admin = index.data(AssetTableModel::AdministratorRole).toBool();

        /** Need to know the height to the pixmap. If it is 0 we don't own this asset so don't have room for the icon */
        int nIconSize = admin ? pixmap.height() : 0;
        int nIPFSIconSize = ipfspixmap.height();
        int nANSIconSize = anspixmap.height();
        int extraNameSpacing = 12;
        int extraDataSpacing = 0;
        if (nIconSize)
            extraNameSpacing = 0;

        /** Get basic padding and half height */
        QRect mainRect = option.rect;
        int xspace = nIconSize + 32;
        int ypad = 2;

        // Create the gradient rect to draw the gradient over
        QRect gradientRect = mainRect;
        gradientRect.setTop(gradientRect.top() + 2);
        gradientRect.setBottom(gradientRect.bottom() - 2);
        gradientRect.setRight(gradientRect.right() - 20);

        int halfheight = (gradientRect.height() - 2 * ypad) / 2;

        /** Create the three main rectangles (Icon, Name, Amount) */
        QRect assetAdministratorRect(QPoint(20, gradientRect.top() + halfheight / 2 - 3 * ypad), QSize(nIconSize, nIconSize));
        QRect assetNameRect(gradientRect.left() + xspace - extraNameSpacing, gradientRect.top() + ypad + (halfheight / 2), gradientRect.width() - xspace, halfheight + ypad);
        QRect amountRect(gradientRect.left() + xspace, gradientRect.top() + ypad + (halfheight / 2), gradientRect.width() - xspace - 24, halfheight);
        QRect ipfsLinkRect(QPoint(gradientRect.right() - nIconSize / 2, gradientRect.top() + halfheight / 1.5), QSize(nIconSize / 2, nIconSize / 2));
        QRect ansRect(QPoint(4, gradientRect.top() + halfheight / 1.5), QSize(nIconSize / 2, nIconSize / 2));

        // Create the gradient for the asset items
        QLinearGradient gradient(mainRect.topLeft(), mainRect.bottomRight());

        // Select the color of the gradient
        gradient.setColorAt(0, COLOR_AVIAN_19827B);
        gradient.setColorAt(1, COLOR_AVIAN_18A7B7);

        // Using 4 are the radius because the pixels are solid
        QPainterPath path;
        path.addRoundedRect(gradientRect, 4, 4);

        // Paint the gradient
        painter->setRenderHint(QPainter::Antialiasing);
        painter->fillPath(path, gradient);

        /** Draw asset administrator icon */
        if (nIconSize)
            painter->drawPixmap(assetAdministratorRect, pixmap);

        if (nIPFSIconSize)
            painter->drawPixmap(ipfsLinkRect, ipfspixmap);

        if (nANSIconSize)
            painter->drawPixmap(ansRect, anspixmap);

        /** Create the font that is used for painting the asset name */
        QFont nameFont;
#if !defined(Q_OS_MAC)
        nameFont.setFamily("Manrope");
#endif
        nameFont.setPixelSize(18);
        nameFont.setWeight(QFont::Weight::Normal);

        /** Create the font that is used for painting the asset amount */
        QFont amountFont;
#if !defined(Q_OS_MAC)
        amountFont.setFamily("Manrope");
#endif
        amountFont.setPixelSize(14);
        amountFont.setWeight(QFont::Weight::Normal);

        /** Get the name and formatted amount from the data */
        QString name = index.data(AssetTableModel::AssetNameRole).toString();
        QString amountText = index.data(AssetTableModel::FormattedAmountRole).toString();

        // Setup the pens
        QColor textColor = COLOR_WHITE;
        // if (darkModeEnabled)
        //     textColor = COLOR_TOOLBAR_SELECTED_TEXT_DARK_MODE;

        QPen penName(textColor);

        /** Start Concatenation of Asset Name */
        // Get the width in pixels that the amount takes up (because they are different font,
        // we need to do this before we call the concatenate function
        painter->setFont(amountFont);
#ifndef QTversionPreFiveEleven
        int amount_width = painter->fontMetrics().horizontalAdvance(amountText);
#else
        int amount_width = painter->fontMetrics().width(amountText);
#endif
        // Set the painter for the font used for the asset name, so that the concatenate function estimated width correctly
        painter->setFont(nameFont);

        GUIUtil::concatenate(painter, name, amount_width, assetNameRect.left(), amountRect.right());

        /** Paint the asset name */
        painter->setPen(penName);
        painter->drawText(assetNameRect, Qt::AlignLeft | Qt::AlignVCenter, name);

        /** Paint the amount */
        painter->setFont(amountFont);
        painter->drawText(amountRect, Qt::AlignRight | Qt::AlignVCenter, amountText);

        painter->restore();
    }

    inline QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const
    {
        return QSize(42, 42);
    }

    int unit;
    const PlatformStyle* platformStyle;
};
#include "aviangui.h"
#include "overviewpage.moc"
#include <QFontDatabase>

OverviewPage::OverviewPage(const PlatformStyle* platformStyle, QWidget* parent) : QWidget(parent),
                                                                                  ui(new Ui::OverviewPage),
                                                                                  clientModel(0),
                                                                                  walletModel(0),
                                                                                  currentBalance(-1),
                                                                                  currentUnconfirmedBalance(-1),
                                                                                  currentImmatureBalance(-1),
                                                                                  currentWatchOnlyBalance(-1),
                                                                                  currentWatchUnconfBalance(-1),
                                                                                  currentWatchImmatureBalance(-1),
                                                                                  pricingTimer(0),
                                                                                  networkManager(0),
                                                                                  txdelegate(new TxViewDelegate(platformStyle, this)),
                                                                                  assetdelegate(new AssetViewDelegate(platformStyle, this))
{
    ui->setupUi(this);

    /** AVN START */
    pricingTimer = new QTimer();
    networkManager = new QNetworkAccessManager();
    /** AVN END */

    QIcon icon(":/icons/warning");
    icon.addPixmap(icon.pixmap(QSize(64, 64), QIcon::Normal), QIcon::Disabled); // also set the disabled icon because we are using a disabled QPushButton to work around missing HiDPI support of QLabel (https://bugreports.qt.io/browse/QTBUG-42503)
    ui->labelTransactionsStatus->setIcon(icon);
    ui->labelWalletStatus->setIcon(icon);
    ui->labelAssetStatus->setIcon(icon);

    // Recent transactions
    ui->listTransactions->setItemDelegate(txdelegate);
    ui->listTransactions->setIconSize(QSize(DECORATION_SIZE, DECORATION_SIZE));
    ui->listTransactions->setMinimumHeight(NUM_ITEMS * (DECORATION_SIZE + 2));
    ui->listTransactions->setAttribute(Qt::WA_MacShowFocusRect, false);

    /** Create the list of assets */
    ui->listAssets->setItemDelegate(assetdelegate);
    ui->listAssets->setIconSize(QSize(42, 42));
    ui->listAssets->setMinimumHeight(5 * (42 + 2));
    ui->listAssets->viewport()->setAutoFillBackground(false);

    // Delay before filtering assetes in ms
    static const int input_filter_delay = 200;

    QTimer* asset_typing_delay;
    asset_typing_delay = new QTimer(this);
    asset_typing_delay->setSingleShot(true);
    asset_typing_delay->setInterval(input_filter_delay);
    connect(ui->assetSearch, SIGNAL(textChanged(QString)), asset_typing_delay, SLOT(start()));
    connect(asset_typing_delay, SIGNAL(timeout()), this, SLOT(assetSearchChanged()));

    connect(ui->listTransactions, SIGNAL(clicked(QModelIndex)), this, SLOT(handleTransactionClicked(QModelIndex)));
    ui->listAssets->viewport()->installEventFilter(this);

    // start with displaying the "out of sync" warnings
    showOutOfSyncWarning(true);
    connect(ui->labelWalletStatus, SIGNAL(clicked()), this, SLOT(handleOutOfSyncWarningClicks()));
    connect(ui->labelAssetStatus, SIGNAL(clicked()), this, SLOT(handleOutOfSyncWarningClicks()));
    connect(ui->labelTransactionsStatus, SIGNAL(clicked()), this, SLOT(handleOutOfSyncWarningClicks()));

    /** Create the shadow effects on the frames */
    ui->assetFrame->setGraphicsEffect(GUIUtil::getShadowEffect());
    ui->frame->setGraphicsEffect(GUIUtil::getShadowEffect());
    ui->frame_2->setGraphicsEffect(GUIUtil::getShadowEffect());

    /** Create the search bar for assets */
    ui->assetSearch->setAttribute(Qt::WA_MacShowFocusRect, 0);
    // ui->assetSearch->setStyleSheet(QString(".QLineEdit {border: 1px solid %1; border-radius: 5px;}").arg(COLOR_LABELS.name()));
    ui->assetSearch->setAlignment(Qt::AlignVCenter);
    QFont font = ui->assetSearch->font();
    font.setPointSize(12);
    ui->assetSearch->setFont(font);

    QFontMetrics fm = QFontMetrics(ui->assetSearch->font());
    ui->assetSearch->setFixedHeight(fm.height() + 5);

    // Trigger the call to show the assets table if assets are active
    showAssets();

    // context menu actions
    sendAction = new QAction(tr("Send Asset"), this);
    QAction* copyAmountAction = new QAction(tr("Copy Amount"), this);
    QAction* copyNameAction = new QAction(tr("Copy Name"), this);
    copyHashAction = new QAction(tr("Copy Hash"), this);
    issueSub = new QAction(tr("Issue Sub Asset"), this);
    issueUnique = new QAction(tr("Issue Unique Asset"), this);
    reissue = new QAction(tr("Reissue Asset"), this);
    openURL = new QAction(tr("Open IPFS in Browser"), this);
    viewANS = new QAction(tr("View ANS info"), this);

    sendAction->setObjectName("Send");
    issueSub->setObjectName("Sub");
    issueUnique->setObjectName("Unique");
    reissue->setObjectName("Reissue");
    copyNameAction->setObjectName("Copy Name");
    copyAmountAction->setObjectName("Copy Amount");
    copyHashAction->setObjectName("Copy Hash");
    openURL->setObjectName("Browse");
    viewANS->setObjectName("View ANS");

    // context menu
    contextMenu = new QMenu(this);
    contextMenu->addAction(sendAction);
    contextMenu->addAction(issueSub);
    contextMenu->addAction(issueUnique);
    contextMenu->addAction(reissue);
    contextMenu->addSeparator();
    contextMenu->addAction(openURL);
    contextMenu->addAction(copyHashAction);
    contextMenu->addSeparator();
    contextMenu->addAction(viewANS);
    contextMenu->addSeparator();
    contextMenu->addAction(copyNameAction);
    contextMenu->addAction(copyAmountAction);
    // context menu signals

    // Network request code for price
    QObject::connect(networkManager, &QNetworkAccessManager::finished,
        this, [=](QNetworkReply* reply) {
            // Default values
            int unit = 0;
            QString currency = "usd";

            if (reply->error()) {
                qDebug() << reply->errorString();
                // Failed to get price info, just display total balance.
                ui->labelTotal->setText(AvianUnits::formatWithUnit(unit, currentBalance + currentUnconfirmedBalance + currentImmatureBalance, false, AvianUnits::separatorAlways));
                return;
            }

            if (walletModel->getOptionsModel()) {
                // Get selected unit
                unit = walletModel->getOptionsModel()->getDisplayUnit();

                // Get user currency unit
                currency = walletModel->getOptionsModel()->getDisplayCurrency();
            }


            // Get the data from the network request
            QString answer = reply->readAll();

            // Convert into JSON document
            QJsonDocument doc(QJsonDocument::fromJson(answer.toUtf8()));
            QJsonObject obj = doc.object();

            // Get market_data object
            QJsonObject market_data = obj.value("market_data").toObject();

            // Get current_price object
            QJsonObject current_price = market_data.value("current_price").toObject();

            // Access price
            double num = current_price.value(currency).toDouble();

            // Get curreny value
            double coinValue = AvianUnits::format(0, currentBalance + currentUnconfirmedBalance + currentImmatureBalance, false, AvianUnits::separatorAlways).simplified().remove(' ').toDouble() * num;

            // Set total with curreny value
            ui->labelTotal->setText(AvianUnits::formatWithUnit(unit, currentBalance + currentUnconfirmedBalance + currentImmatureBalance, false, AvianUnits::separatorAlways) + " (" + QString().setNum(coinValue, 'f', 2) + " " + currency.toUpper() + ")");
        });

    // Create the timer
    connect(pricingTimer, SIGNAL(timeout()), this, SLOT(getPriceInfo()));
    pricingTimer->start(600000);
    getPriceInfo();
}

bool OverviewPage::eventFilter(QObject* object, QEvent* event)
{
    // If the asset viewport is being clicked
    if (object == ui->listAssets->viewport() && event->type() == QEvent::MouseButtonPress) {
        // Grab the mouse event
        QMouseEvent* mouseEv = static_cast<QMouseEvent*>(event);

        // Select the current index at the mouse location
        QModelIndex currentIndex = ui->listAssets->indexAt(mouseEv->pos());

        // Open the menu on right click, direct url on left click
        if (mouseEv->buttons() & Qt::RightButton) {
            handleAssetRightClicked(currentIndex);
        } else if (mouseEv->buttons() & Qt::LeftButton) {
            openDataForAsset(currentIndex, false);
        }
    }

    return QWidget::eventFilter(object, event);
}

void OverviewPage::handleTransactionClicked(const QModelIndex& index)
{
    if (filter)
        Q_EMIT transactionClicked(filter->mapToSource(index));
}

void OverviewPage::handleAssetRightClicked(const QModelIndex& index)
{
    if (assetFilter) {
        // Grab the data elements from the index that we need to disable and enable menu items
        QString name = index.data(AssetTableModel::AssetNameRole).toString();
        QString ipfshash = index.data(AssetTableModel::AssetIPFSHashRole).toString();
        QString ansid = index.data(AssetTableModel::AssetANSRole).toString();
        QString ipfsbrowser = walletModel->getOptionsModel()->getIpfsUrl();

        if (IsAssetNameAnOwner(name.toStdString())) {
            name = name.left(name.size() - 1);
            sendAction->setDisabled(true);
        } else {
            sendAction->setDisabled(false);
        }

        // If the ipfs hash isn't there or doesn't start with Qm, disable the action item
        if (ipfshash.count() > 0 && ipfshash.indexOf("Qm") == 0 && ipfsbrowser.indexOf("http") == 0) {
            openURL->setDisabled(false);
        } else {
            openURL->setDisabled(true);
        }

        if (ipfshash.count() > 0) {
            copyHashAction->setDisabled(false);
        } else {
            copyHashAction->setDisabled(true);
        }

        if (ansid.count() > 0) {
            viewANS->setDisabled(false);
        } else {
            viewANS->setDisabled(true);
        }

        if (!index.data(AssetTableModel::AdministratorRole).toBool()) {
            issueSub->setDisabled(true);
            issueUnique->setDisabled(true);
            reissue->setDisabled(true);
        } else {
            issueSub->setDisabled(false);
            issueUnique->setDisabled(false);
            reissue->setDisabled(true);
            CNewAsset asset;
            auto currentActiveAssetCache = GetCurrentAssetCache();
            if (currentActiveAssetCache && currentActiveAssetCache->GetAssetMetaDataIfExists(name.toStdString(), asset))
                if (asset.nReissuable)
                    reissue->setDisabled(false);
        }

        QAction* action = contextMenu->exec(QCursor::pos());

        if (action) {
            if (action->objectName() == "Send")
                Q_EMIT assetSendClicked(assetFilter->mapToSource(index));
            else if (action->objectName() == "Sub")
                Q_EMIT assetIssueSubClicked(assetFilter->mapToSource(index));
            else if (action->objectName() == "Unique")
                Q_EMIT assetIssueUniqueClicked(assetFilter->mapToSource(index));
            else if (action->objectName() == "Reissue")
                Q_EMIT assetReissueClicked(assetFilter->mapToSource(index));
            else if (action->objectName() == "Copy Name")
                GUIUtil::setClipboard(index.data(AssetTableModel::AssetNameRole).toString());
            else if (action->objectName() == "Copy Amount")
                GUIUtil::setClipboard(index.data(AssetTableModel::FormattedAmountRole).toString());
            else if (action->objectName() == "Copy Hash")
                GUIUtil::setClipboard(ipfshash);
            else if (action->objectName() == "Browse")
                QDesktopServices::openUrl(QUrl::fromUserInput(ipfsbrowser.replace("%s", ipfshash)));
            else if (action->objectName() == "View ANS") {
                openDataForAsset(index, true);
            }
        }
    }
}

void OverviewPage::handleOutOfSyncWarningClicks()
{
    Q_EMIT outOfSyncWarningClicked();
}

OverviewPage::~OverviewPage()
{
    delete ui;
}

void OverviewPage::setBalance(const CAmount& balance, const CAmount& unconfirmedBalance, const CAmount& immatureBalance, const CAmount& watchOnlyBalance, const CAmount& watchUnconfBalance, const CAmount& watchImmatureBalance)
{
    int unit = walletModel->getOptionsModel()->getDisplayUnit();
    currentBalance = balance;
    currentUnconfirmedBalance = unconfirmedBalance;
    currentImmatureBalance = immatureBalance;
    currentWatchOnlyBalance = watchOnlyBalance;
    currentWatchUnconfBalance = watchUnconfBalance;
    currentWatchImmatureBalance = watchImmatureBalance;
    getPriceInfo();
    ui->labelBalance->setText(AvianUnits::formatWithUnit(unit, balance, false, AvianUnits::separatorAlways));
    ui->labelUnconfirmed->setText(AvianUnits::formatWithUnit(unit, unconfirmedBalance, false, AvianUnits::separatorAlways));
    ui->labelImmature->setText(AvianUnits::formatWithUnit(unit, immatureBalance, false, AvianUnits::separatorAlways));
    ui->labelWatchAvailable->setText(AvianUnits::formatWithUnit(unit, watchOnlyBalance, false, AvianUnits::separatorAlways));
    ui->labelWatchPending->setText(AvianUnits::formatWithUnit(unit, watchUnconfBalance, false, AvianUnits::separatorAlways));
    ui->labelWatchImmature->setText(AvianUnits::formatWithUnit(unit, watchImmatureBalance, false, AvianUnits::separatorAlways));
    ui->labelWatchTotal->setText(AvianUnits::formatWithUnit(unit, watchOnlyBalance + watchUnconfBalance + watchImmatureBalance, false, AvianUnits::separatorAlways));

    // only show immature (newly mined) balance if it's non-zero, so as not to complicate things
    // for the non-mining users
    bool showImmature = immatureBalance != 0;
    bool showWatchOnlyImmature = watchImmatureBalance != 0;

    // for symmetry reasons also show immature label when the watch-only one is shown
    ui->labelImmature->setVisible(showImmature || showWatchOnlyImmature);
    ui->labelImmatureText->setVisible(showImmature || showWatchOnlyImmature);
    ui->labelWatchImmature->setVisible(showWatchOnlyImmature); // show watch-only immature balance
}

// show/hide watch-only labels
void OverviewPage::updateWatchOnlyLabels(bool showWatchOnly)
{
    ui->labelSpendable->setVisible(showWatchOnly);      // show spendable label (only when watch-only is active)
    ui->labelWatchonly->setVisible(showWatchOnly);      // show watch-only label
    ui->lineWatchBalance->setVisible(showWatchOnly);    // show watch-only balance separator line
    ui->labelWatchAvailable->setVisible(showWatchOnly); // show watch-only available balance
    ui->labelWatchPending->setVisible(showWatchOnly);   // show watch-only pending balance
    ui->labelWatchTotal->setVisible(showWatchOnly);     // show watch-only total balance

    if (!showWatchOnly)
        ui->labelWatchImmature->hide();
}

void OverviewPage::setClientModel(ClientModel* model)
{
    this->clientModel = model;
    if (model) {
        // Show warning if this is a prerelease version
        connect(model, SIGNAL(alertsChanged(QString)), this, SLOT(updateAlerts(QString)));
        updateAlerts(model->getStatusBarWarnings());
    }
}

void OverviewPage::setWalletModel(WalletModel* model)
{
    this->walletModel = model;
    if (model && model->getOptionsModel()) {
        // Set up transaction list
        filter.reset(new TransactionFilterProxy());
        filter->setSourceModel(model->getTransactionTableModel());
        filter->setLimit(NUM_ITEMS);
        filter->setDynamicSortFilter(true);
        filter->setSortRole(Qt::EditRole);
        filter->setShowInactive(false);
        filter->sort(TransactionTableModel::Date, Qt::DescendingOrder);

        ui->listTransactions->setModel(filter.get());
        ui->listTransactions->setModelColumn(TransactionTableModel::ToAddress);

        assetFilter.reset(new AssetFilterProxy());
        assetFilter->setSourceModel(model->getAssetTableModel());
        assetFilter->sort(AssetTableModel::AssetNameRole, Qt::DescendingOrder);
        ui->listAssets->setModel(assetFilter.get());
        ui->listAssets->setAutoFillBackground(false);

        // Keep up to date with wallet
        setBalance(model->getBalance(), model->getUnconfirmedBalance(), model->getImmatureBalance(),
            model->getWatchBalance(), model->getWatchUnconfirmedBalance(), model->getWatchImmatureBalance());
        connect(model, SIGNAL(balanceChanged(CAmount, CAmount, CAmount, CAmount, CAmount, CAmount)), this, SLOT(setBalance(CAmount, CAmount, CAmount, CAmount, CAmount, CAmount)));

        connect(model->getOptionsModel(), SIGNAL(displayUnitChanged(int)), this, SLOT(updateDisplayUnit()));

        updateWatchOnlyLabels(model->haveWatchOnly());
        connect(model, SIGNAL(notifyWatchonlyChanged(bool)), this, SLOT(updateWatchOnlyLabels(bool)));
    }

    // update the display unit, to not use the default ("AVN")
    updateDisplayUnit();
}

void OverviewPage::updateDisplayUnit()
{
    if (walletModel && walletModel->getOptionsModel()) {
        if (currentBalance != -1)
            setBalance(currentBalance, currentUnconfirmedBalance, currentImmatureBalance,
                currentWatchOnlyBalance, currentWatchUnconfBalance, currentWatchImmatureBalance);

        // Update txdelegate->unit with the current unit
        txdelegate->unit = walletModel->getOptionsModel()->getDisplayUnit();

        ui->listTransactions->update();
    }
}

void OverviewPage::updateAlerts(const QString& warnings)
{
    this->ui->labelAlerts->setVisible(!warnings.isEmpty());
    this->ui->labelAlerts->setText(warnings);
}

void OverviewPage::showOutOfSyncWarning(bool fShow)
{
    ui->labelWalletStatus->setVisible(fShow);
    ui->labelTransactionsStatus->setVisible(fShow);
    if (AreAssetsDeployed()) {
        ui->labelAssetStatus->setVisible(fShow);
    }
}

void OverviewPage::showAssets()
{
    if (AreAssetsDeployed()) {
        ui->assetFrame->show();
        ui->assetBalanceLabel->show();
        ui->labelAssetStatus->show();

        // Disable the vertical space so that listAssets goes to the bottom of the screen
        ui->assetVerticalSpaceWidget->hide();
        ui->assetVerticalSpaceWidget2->hide();
    } else {
        ui->assetFrame->hide();
        ui->assetBalanceLabel->hide();
        ui->labelAssetStatus->hide();

        // This keeps the balance grid from expanding and looking terrible when asset balance is hidden
        ui->assetVerticalSpaceWidget->show();
        ui->assetVerticalSpaceWidget2->show();
    }
}

void OverviewPage::assetSearchChanged()
{
    if (!assetFilter)
        return;
    assetFilter->setAssetNamePrefix(ui->assetSearch->text());
}

void OverviewPage::openDataForAsset(const QModelIndex& index, bool forceANS)
{
    QString assetname = index.data(AssetTableModel::AssetNameRole).toString();

    // Get the ipfs hash of the asset clicked
    QString ipfshash = index.data(AssetTableModel::AssetIPFSHashRole).toString();
    QString ipfsbrowser = walletModel->getOptionsModel()->getIpfsUrl();

    // Get ANS ID of the asset clicked
    QString ansid = index.data(AssetTableModel::AssetANSRole).toString();

    // If the ipfs hash isn't there or doesn't start with Qm, disable the action item
    if (ipfshash.count() > 0 && ipfshash.indexOf("Qm") == 0 && ipfsbrowser.indexOf("http") == 0 && !forceANS) {
        QUrl ipfsurl = QUrl::fromUserInput(ipfsbrowser.replace("%s", ipfshash));

        // Create the box with everything.
        if (QMessageBox::Yes == QMessageBox::question(this, tr("Open IPFS content?"), tr("Open the following IPFS content in your default browser?\n") + ipfsurl.toString()))
            QDesktopServices::openUrl(ipfsurl);
    }
    // Show ANS data if attached to asset
    else if (ansid.count() > 0) {
        CAvianNameSystemID ans(ansid.toStdString());
        QString ansData = "";

        if (ans.type() == CAvianNameSystemID::ADDR) ansData = "Address: " + QString::fromStdString(ans.addr());
        if (ans.type() == CAvianNameSystemID::IP) ansData = "IPv4: " + QString::fromStdString(ans.ip());

        QMessageBox::information(this, "ANS Info", assetname + " links to:\n" + ansData);
    }
}

void OverviewPage::getPriceInfo()
{
    // Run network request in a background thread to prevent UI blocking
    QtConcurrent::run([this]() {
        // Execute on main thread to use QNetworkAccessManager
        QMetaObject::invokeMethod(this, [this]() {
            QString url = "https://api.coingecko.com/api/v3/coins/avian-network/";

            QNetworkRequest request;
            QSslConfiguration sslConfiguration;
            sslConfiguration.setProtocol(QSsl::TlsV1_2OrLater);
            sslConfiguration.setPeerVerifyMode(QSslSocket::QueryPeer);
            request.setSslConfiguration(sslConfiguration);
            request.setUrl(QUrl(url));
            
            networkManager->get(request); }, Qt::QueuedConnection);
    });
}
