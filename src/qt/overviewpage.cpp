// Copyright (c) 2011-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/overviewpage.h>
#include <qt/forms/ui_overviewpage.h>

#include <qt/assetfilterproxy.h>
#include <qt/assettablemodel.h>
#include <qt/bitcoinunits.h>
#include <qt/clientmodel.h>
#include <qt/guiconstants.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <qt/transactionfilterproxy.h>
#include <qt/transactionoverviewwidget.h>
#include <qt/transactiontablemodel.h>
#include <qt/walletmodel.h>

#include <assets/assets.h>
#include <assets/assettypes.h>
#include <assets/ans.h>
#include <validation.h>

#include <QAbstractItemDelegate>
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFrame>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QStatusTipEvent>
#include <QTextBrowser>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <map>

#define DECORATION_SIZE 54
#define NUM_ITEMS 5

Q_DECLARE_METATYPE(interfaces::WalletBalances)

class TxViewDelegate : public QAbstractItemDelegate
{
    Q_OBJECT
public:
    explicit TxViewDelegate(const PlatformStyle* _platformStyle, QObject* parent = nullptr)
        : QAbstractItemDelegate(parent), platformStyle(_platformStyle)
    {
        connect(this, &TxViewDelegate::width_changed, this, &TxViewDelegate::sizeHintChanged);
    }

    inline void paint(QPainter *painter, const QStyleOptionViewItem &option,
                      const QModelIndex &index ) const override
    {
        painter->save();

        QIcon icon = qvariant_cast<QIcon>(index.data(TransactionTableModel::RawDecorationRole));
        QRect mainRect = option.rect;
        QRect decorationRect(mainRect.topLeft(), QSize(DECORATION_SIZE, DECORATION_SIZE));
        int xspace = DECORATION_SIZE + 8;
        int ypad = 6;
        int halfheight = (mainRect.height() - 2*ypad)/2;
        QRect amountRect(mainRect.left() + xspace, mainRect.top()+ypad, mainRect.width() - xspace, halfheight);
        QRect addressRect(mainRect.left() + xspace, mainRect.top()+ypad+halfheight, mainRect.width() - xspace, halfheight);
        icon = platformStyle->SingleColorIcon(icon);
        icon.paint(painter, decorationRect);

        QDateTime date = index.data(TransactionTableModel::DateRole).toDateTime();
        QString address = index.data(Qt::DisplayRole).toString();
        qint64 amount = index.data(TransactionTableModel::AmountRole).toLongLong();
        bool confirmed = index.data(TransactionTableModel::ConfirmedRole).toBool();
        QVariant value = index.data(Qt::ForegroundRole);
        QColor foreground = option.palette.color(QPalette::Text);
        if(value.canConvert<QBrush>())
        {
            QBrush brush = qvariant_cast<QBrush>(value);
            foreground = brush.color();
        }

        painter->setPen(foreground);
        QRect boundingRect;
        painter->drawText(addressRect, Qt::AlignLeft | Qt::AlignVCenter, address, &boundingRect);

        if(amount < 0)
        {
            foreground = COLOR_NEGATIVE;
        }
        else if(!confirmed)
        {
            foreground = COLOR_UNCONFIRMED;
        }
        else
        {
            foreground = option.palette.color(QPalette::Text);
        }
        painter->setPen(foreground);
        QString amountText = BitcoinUnits::formatWithUnit(unit, amount, true, BitcoinUnits::SeparatorStyle::ALWAYS);
        if(!confirmed)
        {
            amountText = QString("[") + amountText + QString("]");
        }

        QRect amount_bounding_rect;
        painter->drawText(amountRect, Qt::AlignRight | Qt::AlignVCenter, amountText, &amount_bounding_rect);

        painter->setPen(option.palette.color(QPalette::Text));
        QRect date_bounding_rect;
        painter->drawText(amountRect, Qt::AlignLeft | Qt::AlignVCenter, GUIUtil::dateTimeStr(date), &date_bounding_rect);

        // 0.4*date_bounding_rect.width() is used to visually distinguish a date from an amount.
        const int minimum_width = 1.4 * date_bounding_rect.width() + amount_bounding_rect.width();
        const auto search = m_minimum_width.find(index.row());
        if (search == m_minimum_width.end() || search->second != minimum_width) {
            m_minimum_width[index.row()] = minimum_width;
            Q_EMIT width_changed(index);
        }

        painter->restore();
    }

    inline QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        const auto search = m_minimum_width.find(index.row());
        const int minimum_text_width = search == m_minimum_width.end() ? 0 : search->second;
        return {DECORATION_SIZE + 8 + minimum_text_width, DECORATION_SIZE};
    }

    BitcoinUnit unit{BitcoinUnit::BTC};

Q_SIGNALS:
    //! An intermediate signal for emitting from the `paint() const` member function.
    void width_changed(const QModelIndex& index) const;

private:
    const PlatformStyle* platformStyle;
    mutable std::map<int, int> m_minimum_width;
};

/** AVN START — Custom delegate for rendering asset rows as teal gradient cards */
class AssetViewDelegate : public QAbstractItemDelegate
{
    Q_OBJECT
public:
    bool m_privacy{false};

    explicit AssetViewDelegate(const PlatformStyle* _platformStyle, QObject* parent = nullptr)
        : QAbstractItemDelegate(parent), platformStyle(_platformStyle)
    {
    }

    inline void paint(QPainter* painter, const QStyleOptionViewItem& option,
                      const QModelIndex& index) const override
    {
        painter->save();

        QPixmap pixmap = qvariant_cast<QPixmap>(index.data(Qt::DecorationRole));
        QPixmap ipfspixmap = qvariant_cast<QPixmap>(index.data(AssetTableModel::AssetIPFSHashDecorationRole));
        QPixmap anspixmap = qvariant_cast<QPixmap>(index.data(AssetTableModel::AssetANSDecorationRole));
        bool admin = index.data(AssetTableModel::AdministratorRole).toBool();

        int nIconSize = admin ? pixmap.height() : 0;
        int nIPFSIconSize = ipfspixmap.height();
        int nANSIconSize = anspixmap.height();
        int extraNameSpacing = 12;
        if (nIconSize) extraNameSpacing = 0;

        QRect mainRect = option.rect;
        int xspace = nIconSize + 32;
        int ypad = 2;

        // Gradient rect with insets
        QRect gradientRect = mainRect;
        gradientRect.setTop(gradientRect.top() + 2);
        gradientRect.setBottom(gradientRect.bottom() - 2);
        gradientRect.setRight(gradientRect.right() - 20);
        int halfheight = (gradientRect.height() - 2 * ypad) / 2;

        // Sub-rects for icon, name, amount
        QRect assetAdministratorRect(QPoint(20, gradientRect.top() + halfheight / 2 - 3 * ypad),
                                     QSize(nIconSize, nIconSize));
        QRect assetNameRect(gradientRect.left() + xspace - extraNameSpacing,
                            gradientRect.top() + ypad + (halfheight / 2),
                            gradientRect.width() - xspace, halfheight + ypad);
        QRect amountRect(gradientRect.left() + xspace,
                         gradientRect.top() + ypad + (halfheight / 2),
                         gradientRect.width() - xspace - 24, halfheight);
        QRect ipfsLinkRect(QPoint(gradientRect.right() - nIconSize / 2,
                                  gradientRect.top() + halfheight / 1.5),
                           QSize(nIconSize / 2, nIconSize / 2));
        QRect ansRect(QPoint(4, gradientRect.top() + halfheight / 1.5),
                      QSize(nIconSize / 2, nIconSize / 2));

        // Teal gradient background
        QLinearGradient gradient(mainRect.topLeft(), mainRect.bottomRight());
        gradient.setColorAt(0, COLOR_AVIAN_19827B);
        gradient.setColorAt(1, COLOR_AVIAN_18A7B7);

        QPainterPath path;
        path.addRoundedRect(gradientRect, 4, 4);

        painter->setRenderHint(QPainter::Antialiasing);
        painter->fillPath(path, gradient);

        // Draw icons
        if (nIconSize)
            painter->drawPixmap(assetAdministratorRect, pixmap);
        if (nIPFSIconSize)
            painter->drawPixmap(ipfsLinkRect, ipfspixmap);
        if (nANSIconSize)
            painter->drawPixmap(ansRect, anspixmap);

        // Asset name font
        QFont nameFont;
#if !defined(Q_OS_MAC)
        nameFont.setFamily("Konnect");
#endif
        nameFont.setPixelSize(18);
        nameFont.setWeight(QFont::Weight::Normal);

        // Asset amount font
        QFont amountFont;
#if !defined(Q_OS_MAC)
        amountFont.setFamily("Konnect");
#endif
        amountFont.setPixelSize(14);
        amountFont.setWeight(QFont::Weight::Normal);

        // Get data
        QString name = index.data(AssetTableModel::AssetNameRole).toString();
        QString amountText = index.data(AssetTableModel::FormattedAmountRole).toString();
        if (m_privacy) {
            for (int i = 0; i < name.size(); ++i) {
                if (name[i].isLetterOrNumber()) name[i] = '#';
            }
            for (int i = 0; i < amountText.size(); ++i) {
                if (amountText[i].isDigit()) amountText[i] = '#';
            }
        }

        // White text
        QColor textColor = COLOR_WHITE;
        QPen penName(textColor);

        // Truncate name if it would overlap amount
        painter->setFont(amountFont);
        int amount_width = painter->fontMetrics().horizontalAdvance(amountText);
        painter->setFont(nameFont);
        GUIUtil::concatenate(painter, name, amount_width, assetNameRect.left(), amountRect.right());

        // Draw name (left) and amount (right)
        painter->setPen(penName);
        painter->drawText(assetNameRect, Qt::AlignLeft | Qt::AlignVCenter, name);

        painter->setFont(amountFont);
        painter->drawText(amountRect, Qt::AlignRight | Qt::AlignVCenter, amountText);

        painter->restore();
    }

    inline QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        return QSize(42, 42);
    }

    const PlatformStyle* platformStyle;
};
/** AVN END */

#include <qt/overviewpage.moc>

OverviewPage::OverviewPage(const PlatformStyle *platformStyle, QWidget *parent) :
    QWidget(parent),
    ui(new Ui::OverviewPage),
    m_platform_style{platformStyle},
    txdelegate(new TxViewDelegate(platformStyle, this)),
    assetdelegate(new AssetViewDelegate(platformStyle, this))
{
    ui->setupUi(this);

    // use a SingleColorIcon for the "out of sync warning" icon
    QIcon icon = m_platform_style->SingleColorIcon(QStringLiteral(":/icons/warning"));
    ui->labelTransactionsStatus->setIcon(icon);
    ui->labelWalletStatus->setIcon(icon);

    // Recent transactions
    ui->listTransactions->setItemDelegate(txdelegate);
    ui->listTransactions->setIconSize(QSize(DECORATION_SIZE, DECORATION_SIZE));
    ui->listTransactions->setMinimumHeight(NUM_ITEMS * (DECORATION_SIZE + 2));
    ui->listTransactions->setAttribute(Qt::WA_MacShowFocusRect, false);

    connect(ui->listTransactions, &TransactionOverviewWidget::clicked, this, &OverviewPage::handleTransactionClicked);

    /** AVN START - asset list setup */
    ui->listAssets->setItemDelegate(assetdelegate);
    ui->listAssets->setIconSize(QSize(42, 42));
    ui->listAssets->setMinimumHeight(5 * (42 + 2));
    ui->listAssets->viewport()->setAutoFillBackground(false);

    // Asset search typing delay
    static const int input_filter_delay = 200; // ms
    QTimer* asset_typing_delay = new QTimer(this);
    asset_typing_delay->setSingleShot(true);
    asset_typing_delay->setInterval(input_filter_delay);
    connect(ui->assetSearch, &QLineEdit::textChanged, asset_typing_delay, qOverload<>(&QTimer::start));
    connect(asset_typing_delay, &QTimer::timeout, this, &OverviewPage::assetSearchChanged);

    // Asset context menu
    assetSendAction = new QAction(tr("Send Asset"), this);
    assetIssueSubAction = new QAction(tr("Issue Sub Asset"), this);
    assetIssueUniqueAction = new QAction(tr("Issue Unique Asset"), this);
    assetReissueAction = new QAction(tr("Reissue Asset"), this);
    assetCopyNameAction = new QAction(tr("Copy Name"), this);
    assetCopyAmountAction = new QAction(tr("Copy Amount"), this);
    assetCopyHashAction = new QAction(tr("Copy Hash"), this);
    assetOpenIPFSAction = new QAction(tr("Open IPFS in Browser"), this);
    assetViewANSAction = new QAction(tr("View ANS info"), this);

    assetContextMenu = new QMenu(this);
    assetContextMenu->addAction(assetSendAction);
    assetContextMenu->addAction(assetIssueSubAction);
    assetContextMenu->addAction(assetIssueUniqueAction);
    assetContextMenu->addAction(assetReissueAction);
    assetContextMenu->addSeparator();
    assetContextMenu->addAction(assetOpenIPFSAction);
    assetContextMenu->addAction(assetCopyHashAction);
    assetContextMenu->addSeparator();
    assetContextMenu->addAction(assetViewANSAction);
    assetContextMenu->addSeparator();
    assetContextMenu->addAction(assetCopyNameAction);
    assetContextMenu->addAction(assetCopyAmountAction);

    connect(ui->listAssets, &QListView::customContextMenuRequested, [this](const QPoint& pos) {
        QModelIndex index = ui->listAssets->indexAt(pos);
        if (index.isValid()) {
            handleAssetRightClicked(index);
        }
    });
    ui->listAssets->setContextMenuPolicy(Qt::CustomContextMenu);

    showAssets();
    /** AVN END */

    // start with displaying the "out of sync" warnings
    showOutOfSyncWarning(true);
    connect(ui->labelWalletStatus, &QPushButton::clicked, this, &OverviewPage::outOfSyncWarningClicked);
    connect(ui->labelTransactionsStatus, &QPushButton::clicked, this, &OverviewPage::outOfSyncWarningClicked);
}

void OverviewPage::handleTransactionClicked(const QModelIndex &index)
{
    if(filter)
        Q_EMIT transactionClicked(filter->mapToSource(index));
}

void OverviewPage::setPrivacy(bool privacy)
{
    m_privacy = privacy;
    clientModel->getOptionsModel()->setOption(OptionsModel::OptionID::MaskValues, privacy);
    const auto& balances = walletModel->getCachedBalance();
    if (balances.balance != -1) {
        setBalance(balances);
    }

    ui->listTransactions->setVisible(!m_privacy);

    assetdelegate->m_privacy = m_privacy;
    ui->listAssets->viewport()->update();

    const QString status_tip = m_privacy ? tr("Privacy mode activated for the Overview tab. To unmask the values, uncheck Settings->Mask values.") : "";
    setStatusTip(status_tip);
    QStatusTipEvent event(status_tip);
    QApplication::sendEvent(this, &event);
}

OverviewPage::~OverviewPage()
{
    delete ui;
}

void OverviewPage::setBalance(const interfaces::WalletBalances& balances)
{
    BitcoinUnit unit = walletModel->getOptionsModel()->getDisplayUnit();
    ui->labelBalance->setText(BitcoinUnits::formatWithPrivacy(unit, balances.balance, BitcoinUnits::SeparatorStyle::ALWAYS, m_privacy));
    ui->labelUnconfirmed->setText(BitcoinUnits::formatWithPrivacy(unit, balances.unconfirmed_balance, BitcoinUnits::SeparatorStyle::ALWAYS, m_privacy));
    ui->labelImmature->setText(BitcoinUnits::formatWithPrivacy(unit, balances.immature_balance, BitcoinUnits::SeparatorStyle::ALWAYS, m_privacy));
    ui->labelTotal->setText(BitcoinUnits::formatWithPrivacy(unit, balances.balance + balances.unconfirmed_balance + balances.immature_balance, BitcoinUnits::SeparatorStyle::ALWAYS, m_privacy));
    // only show immature (newly mined) balance if it's non-zero, so as not to complicate things
    // for the non-mining users
    bool showImmature = balances.immature_balance != 0;

    ui->labelImmature->setVisible(showImmature);
    ui->labelImmatureText->setVisible(showImmature);
}

void OverviewPage::setClientModel(ClientModel *model)
{
    this->clientModel = model;
    if (model) {
        // Show warning, for example if this is a prerelease version
        connect(model, &ClientModel::alertsChanged, this, &OverviewPage::updateAlerts);
        updateAlerts(model->getStatusBarWarnings());

        connect(model->getOptionsModel(), &OptionsModel::fontForMoneyChanged, this, &OverviewPage::setMonospacedFont);
        setMonospacedFont(clientModel->getOptionsModel()->getFontForMoney());
    }
}

void OverviewPage::setWalletModel(WalletModel *model)
{
    this->walletModel = model;
    if(model && model->getOptionsModel())
    {
        // Set up transaction list
        filter.reset(new TransactionFilterProxy());
        filter->setSourceModel(model->getTransactionTableModel());
        filter->setDynamicSortFilter(true);
        filter->setSortRole(Qt::EditRole);
        filter->setShowInactive(false);
        filter->setLimit(NUM_ITEMS);
        filter->sort(TransactionTableModel::Date, Qt::DescendingOrder);

        ui->listTransactions->setModel(filter.get());
        ui->listTransactions->setModelColumn(TransactionTableModel::ToAddress);

        // Set up asset list
        assetFilter.reset(new AssetFilterProxy());
        assetFilter->setSourceModel(model->getAssetTableModel());
        assetFilter->setSortRole(AssetTableModel::AssetNameRole);
        assetFilter->sort(AssetTableModel::Name, Qt::AscendingOrder);
        ui->listAssets->setModel(assetFilter.get());

        // Keep up to date with wallet
        setBalance(model->getCachedBalance());
        connect(model, &WalletModel::balanceChanged, this, &OverviewPage::setBalance);

        connect(model->getOptionsModel(), &OptionsModel::displayUnitChanged, this, &OverviewPage::updateDisplayUnit);
    }

    // update the display unit, to not use the default ("AVN")
    updateDisplayUnit();
}

void OverviewPage::changeEvent(QEvent* e)
{
    if (e->type() == QEvent::PaletteChange) {
        QIcon icon = m_platform_style->SingleColorIcon(QStringLiteral(":/icons/warning"));
        ui->labelTransactionsStatus->setIcon(icon);
        ui->labelWalletStatus->setIcon(icon);
    }

    QWidget::changeEvent(e);
}

// Only show most recent NUM_ITEMS rows
void OverviewPage::updateDisplayUnit()
{
    if (walletModel && walletModel->getOptionsModel()) {
        const auto& balances = walletModel->getCachedBalance();
        if (balances.balance != -1) {
            setBalance(balances);
        }

        // Update txdelegate->unit with the current unit
        txdelegate->unit = walletModel->getOptionsModel()->getDisplayUnit();

        ui->listTransactions->update();
    }
}

void OverviewPage::updateAlerts(const QString &warnings)
{
    this->ui->labelAlerts->setVisible(!warnings.isEmpty());
    this->ui->labelAlerts->setText(warnings);
}

void OverviewPage::showOutOfSyncWarning(bool fShow)
{
    ui->labelWalletStatus->setVisible(fShow);
    ui->labelTransactionsStatus->setVisible(fShow);
}

void OverviewPage::setMonospacedFont(const QFont& f)
{
    ui->labelBalance->setFont(f);
    ui->labelUnconfirmed->setFont(f);
    ui->labelImmature->setFont(f);
    ui->labelTotal->setFont(f);
}

/** AVN START */
void OverviewPage::showAssets()
{
    bool fShowAssets = AreAssetsDeployed();
    ui->assetFrame->setVisible(fShowAssets);
}

void OverviewPage::assetSearchChanged()
{
    if (assetFilter) {
        assetFilter->setAssetNamePrefix(ui->assetSearch->text());
    }
}

void OverviewPage::handleAssetRightClicked(const QModelIndex& index)
{
    if (!index.isValid() || !assetFilter) return;

    QString assetName = index.data(AssetTableModel::AssetNameRole).toString();
    QString ipfshash = index.data(AssetTableModel::AssetIPFSHashRole).toString();
    QString ansid = index.data(AssetTableModel::AssetANSRole).toString();
    QString ipfsbrowser = "https://ipfs.avn.network/ipfs/%s";

    // Disable send for owner tokens
    if (IsAssetNameAnOwner(assetName.toStdString())) {
        assetName = assetName.left(assetName.size() - 1);
        assetSendAction->setDisabled(true);
    } else {
        assetSendAction->setDisabled(false);
    }

    // Enable/disable IPFS action based on hash availability
    assetOpenIPFSAction->setDisabled(!(ipfshash.size() > 0 && ipfshash.indexOf("Qm") == 0 && ipfsbrowser.indexOf("http") == 0));

    // Enable/disable Copy Hash based on hash availability
    assetCopyHashAction->setDisabled(ipfshash.isEmpty());

    // Enable/disable ANS based on ANS ID availability
    assetViewANSAction->setDisabled(ansid.isEmpty());

    // Enable/disable admin actions based on ownership
    if (!index.data(AssetTableModel::AdministratorRole).toBool()) {
        assetIssueSubAction->setDisabled(true);
        assetIssueUniqueAction->setDisabled(true);
        assetReissueAction->setDisabled(true);
    } else {
        assetIssueSubAction->setDisabled(false);
        assetIssueUniqueAction->setDisabled(false);
        assetReissueAction->setDisabled(true);
        CNewAsset asset;
        auto currentActiveAssetCache = GetCurrentAssetCache();
        if (currentActiveAssetCache && currentActiveAssetCache->GetAssetMetaDataIfExists(assetName.toStdString(), asset)) {
            if (asset.nReissuable)
                assetReissueAction->setDisabled(false);
        }
    }

    // Copy name action
    disconnect(assetCopyNameAction, &QAction::triggered, nullptr, nullptr);
    connect(assetCopyNameAction, &QAction::triggered, [index]() {
        GUIUtil::setClipboard(index.data(AssetTableModel::AssetNameRole).toString());
    });

    // Copy amount action
    disconnect(assetCopyAmountAction, &QAction::triggered, nullptr, nullptr);
    connect(assetCopyAmountAction, &QAction::triggered, [index]() {
        GUIUtil::setClipboard(index.data(AssetTableModel::FormattedAmountRole).toString());
    });

    // Copy hash action
    disconnect(assetCopyHashAction, &QAction::triggered, nullptr, nullptr);
    connect(assetCopyHashAction, &QAction::triggered, [ipfshash]() {
        GUIUtil::setClipboard(ipfshash);
    });

    // Open IPFS action
    disconnect(assetOpenIPFSAction, &QAction::triggered, nullptr, nullptr);
    connect(assetOpenIPFSAction, &QAction::triggered, [ipfshash, ipfsbrowser]() {
        QString url = ipfsbrowser;
        QDesktopServices::openUrl(QUrl::fromUserInput(url.replace("%s", ipfshash)));
    });

    // View ANS action
    disconnect(assetViewANSAction, &QAction::triggered, nullptr, nullptr);
    connect(assetViewANSAction, &QAction::triggered, [this, assetName, ansid, ipfsbrowser]() {
        if (!ansid.isEmpty()) {
            CAvianNameSystemID ans(ansid.toStdString());

            QString typeLabel;
            if (ans.type() == CAvianNameSystemID::ADDR)
                typeLabel = tr("AVN Address");
            else if (ans.type() == CAvianNameSystemID::XADDR)
                typeLabel = tr("External Address");
            else if (ans.type() == CAvianNameSystemID::PROFILE)
                typeLabel = tr("Profile");

            QDialog dlg(this);
            dlg.setObjectName("ANSInfoDialog");
            dlg.setWindowTitle(tr("ANS Info"));

            QVBoxLayout* vbox = new QVBoxLayout(&dlg);
            vbox->setContentsMargins(0, 0, 0, 0);
            vbox->setSpacing(0);

            // Header
            QWidget* header = new QWidget(&dlg);
            header->setObjectName("ansHeader");

            QVBoxLayout* headerLayout = new QVBoxLayout(header);
            headerLayout->setContentsMargins(16, 8, 16, 8);
            headerLayout->setSpacing(1);

            QLabel* titleLabel = new QLabel(assetName, header);
            titleLabel->setObjectName("ansTitleLabel");
            titleLabel->setTextFormat(Qt::PlainText);

            QLabel* typeLabelWidget = new QLabel(typeLabel, header);
            typeLabelWidget->setObjectName("ansTypeLabel");
            typeLabelWidget->setTextFormat(Qt::PlainText);

            headerLayout->addWidget(titleLabel);
            headerLayout->addWidget(typeLabelWidget);

            vbox->addWidget(header);

            // Body
            QWidget* body = new QWidget(&dlg);
            body->setObjectName("ansBody");

            QGridLayout* grid = new QGridLayout(body);
            grid->setContentsMargins(16, 14, 16, 14);
            grid->setHorizontalSpacing(20);
            grid->setVerticalSpacing(4);

            int row = 0;

            // Build a full IPFS gateway URL from an ipfs://CID value, or return empty.
            auto ipfsGatewayUrl = [&ipfsbrowser](const QString& val) -> QString {
                if (val.startsWith(QStringLiteral("ipfs://")))
                    return QString(ipfsbrowser).replace(QStringLiteral("%s"), val.mid(7));
                return {};
            };

            auto addRow = [&](const QString& lbl, const QString& val, const QString& href = {}) {
                QLabel* label = new QLabel(lbl, body);
                label->setObjectName("ansFieldLabel");
                label->setTextFormat(Qt::PlainText);
                label->setAlignment(Qt::AlignLeft | Qt::AlignTop);
                label->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);

                QLabel* value = new QLabel(body);
                value->setObjectName("ansFieldValue");
                value->setMinimumWidth(520);
                value->setWordWrap(true);
                value->setAlignment(Qt::AlignLeft | Qt::AlignTop);
                value->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

                if (!href.isEmpty()) {
                    value->setTextFormat(Qt::RichText);
                    value->setOpenExternalLinks(true);
                    value->setTextInteractionFlags(Qt::TextBrowserInteraction);
                    value->setText(QStringLiteral("<a href='") + href.toHtmlEscaped() +
                                   QStringLiteral("' style='color:#19827B'>") +
                                   val.toHtmlEscaped() + QStringLiteral("</a>"));
                } else {
                    value->setTextFormat(Qt::PlainText);
                    value->setTextInteractionFlags(Qt::TextSelectableByMouse);
                    value->setText(val);
                }

                grid->addWidget(label, row, 0);
                grid->addWidget(value, row, 1);
                ++row;
            };

            if (ans.type() == CAvianNameSystemID::ADDR || ans.type() == CAvianNameSystemID::XADDR) {
                addRow(typeLabel, QString::fromStdString(ans.addr()));
            } else if (ans.type() == CAvianNameSystemID::PROFILE) {
                const ANSProfileData& p = ans.profile();

                if (!p.name.empty())
                    addRow(tr("Display Name"), QString::fromStdString(p.name));

                if (!p.addr.empty())
                    addRow(tr("Address"), QString::fromStdString(p.addr));

                if (!p.avatar.empty()) {
                    QString val = p.avatar_binary
                        ? tr("[inline binary, %1 bytes]").arg((qulonglong)p.avatar.size())
                        : QString::fromStdString(p.avatar);
                    addRow(tr("Avatar"), val, p.avatar_binary ? QString{} : ipfsGatewayUrl(val));
                }

                if (!p.banner.empty()) {
                    QString val = p.banner_binary
                        ? tr("[inline binary, %1 bytes]").arg((qulonglong)p.banner.size())
                        : QString::fromStdString(p.banner);
                    addRow(tr("Banner"), val, p.banner_binary ? QString{} : ipfsGatewayUrl(val));
                }

                if (!p.url.empty()) {
                    QString val = QString::fromStdString(p.url);
                    QString href = (val.startsWith(QStringLiteral("http://")) ||
                                    val.startsWith(QStringLiteral("https://"))) ? val : QString{};
                    addRow(tr("URL"), val, href);
                }
            }

            grid->setColumnStretch(0, 0);
            grid->setColumnStretch(1, 1);

            for (int i = 0; i < row; ++i) {
                grid->setRowStretch(i, 0);
            }

            grid->setRowStretch(row, 1);

            vbox->addWidget(body, 1);

            QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Ok, &dlg);
            connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);

            buttons->layout()->setContentsMargins(12, 8, 12, 12);
            vbox->addWidget(buttons);

            const int minW = 720;
            const int maxW = 1000;
            const int extraH = 0;

            const int dlgW = qBound(minW, dlg.sizeHint().width(), maxW);
            const int dlgH = dlg.sizeHint().height() + extraH;

            dlg.resize(dlgW, dlgH);
            dlg.exec();
        }
    });

    // Send action
    disconnect(assetSendAction, &QAction::triggered, nullptr, nullptr);
    connect(assetSendAction, &QAction::triggered, [this, index]() {
        Q_EMIT assetSendClicked(assetFilter->mapToSource(index));
    });

    // Issue sub action
    disconnect(assetIssueSubAction, &QAction::triggered, nullptr, nullptr);
    connect(assetIssueSubAction, &QAction::triggered, [this, index]() {
        Q_EMIT assetIssueSubClicked(assetFilter->mapToSource(index));
    });

    // Issue unique action
    disconnect(assetIssueUniqueAction, &QAction::triggered, nullptr, nullptr);
    connect(assetIssueUniqueAction, &QAction::triggered, [this, index]() {
        Q_EMIT assetIssueUniqueClicked(assetFilter->mapToSource(index));
    });

    // Reissue action
    disconnect(assetReissueAction, &QAction::triggered, nullptr, nullptr);
    connect(assetReissueAction, &QAction::triggered, [this, index]() {
        Q_EMIT assetReissueClicked(assetFilter->mapToSource(index));
    });

    assetContextMenu->exec(QCursor::pos());
}
/** AVN END */
