// Copyright (c) 2011-2016 The Bitcoin Core developers
// Copyright (c) 2017-2019 The Raven Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/myrestrictedassettablemodel.h>

#include <qt/addresstablemodel.h>
#include <qt/guiconstants.h>
#include <qt/guiutil.h>
#include <qt/myrestrictedassetrecord.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <qt/walletmodel.h>

#include <core_io.h>
#include <sync.h>
#include <uint256.h>
#include <common/args.h>
#include <assets/assets.h>
#include <assets/myassetsdb.h>
#include <validation.h>
#include <wallet/wallet.h>

#include <QColor>
#include <QDateTime>
#include <QDebug>
#include <QIcon>
#include <QList>

#include <functional>
using namespace std::placeholders;

extern CMyRestrictedDB* pmyrestricteddb;

// Amount column is right-aligned it contains numbers
static int column_alignments[] = {
    Qt::AlignLeft | Qt::AlignVCenter, /* date */
    Qt::AlignLeft | Qt::AlignVCenter, /* type */
    Qt::AlignLeft | Qt::AlignVCenter, /* address */
    Qt::AlignLeft | Qt::AlignVCenter  /* assetName */
};

// Comparison operator for sort/binary search of model tx list
struct TxLessThan {
    bool operator()(const MyRestrictedAssetRecord& a, const MyRestrictedAssetRecord& b) const
    {
        return a.hash < b.hash;
    }
    bool operator()(const MyRestrictedAssetRecord& a, const uint256& b) const
    {
        return a.hash < b;
    }
    bool operator()(const uint256& a, const MyRestrictedAssetRecord& b) const
    {
        return a < b.hash;
    }
};

// Private implementation
class MyRestrictedAssetsTablePriv
{
public:
    MyRestrictedAssetsTablePriv(wallet::CWallet* _wallet, MyRestrictedAssetsTableModel* _parent) : wallet(_wallet),
                                                                                           parent(_parent)
    {
    }

    wallet::CWallet* wallet;
    MyRestrictedAssetsTableModel* parent;

    QMap<QPair<QString, QString>, MyRestrictedAssetRecord> cacheMyAssetData;
    QList<QPair<QString, QString>> vectAssetData;

    void refreshWallet()
    {
        qDebug() << "MyRestrictedAssetsTablePriv::refreshWallet";
        cacheMyAssetData.clear();
        vectAssetData.clear();

        if (!pmyrestricteddb)
            return;

        // Load tagged addresses (qualifier assignments)
        std::vector<std::tuple<std::string, std::string, bool, uint32_t>> vecTagged;
        pmyrestricteddb->LoadMyTaggedAddresses(vecTagged);
        for (const auto& [address, tag, fAdd, nHeight] : vecTagged) {
            MyRestrictedAssetRecord rec;
            rec.type = fAdd ? MyRestrictedAssetRecord::Tagged : MyRestrictedAssetRecord::UnTagged;
            rec.address = address;
            rec.assetName = tag;
            rec.time = 0; // Height-based, no timestamp available
            rec.involvesWatchAddress = false;

            QPair<QString, QString> pair(QString::fromStdString(address), QString::fromStdString(tag));
            cacheMyAssetData[pair] = rec;
            vectAssetData.push_back(pair);
        }

        // Load restricted addresses (freeze/unfreeze)
        std::vector<std::tuple<std::string, std::string, bool, uint32_t>> vecRestricted;
        pmyrestricteddb->LoadMyRestrictedAddresses(vecRestricted);
        for (const auto& [address, asset, fAdd, nHeight] : vecRestricted) {
            MyRestrictedAssetRecord rec;
            rec.type = fAdd ? MyRestrictedAssetRecord::Frozen : MyRestrictedAssetRecord::UnFrozen;
            rec.address = address;
            rec.assetName = asset;
            rec.time = 0;
            rec.involvesWatchAddress = false;

            QPair<QString, QString> pair(QString::fromStdString(address), QString::fromStdString(asset));
            cacheMyAssetData[pair] = rec;
            vectAssetData.push_back(pair);
        }
    }

    void updateMyRestrictedAssets(const QString& address, const QString& asset_name, const int type, const qint64& date)
    {
        MyRestrictedAssetRecord rec;

        if (IsAssetNameAQualifier(asset_name.toStdString())) {
            rec.type = type ? MyRestrictedAssetRecord::Tagged : MyRestrictedAssetRecord::UnTagged;
        } else {
            rec.type = type ? MyRestrictedAssetRecord::Frozen : MyRestrictedAssetRecord::UnFrozen;
        }

        rec.time = date;
        rec.assetName = asset_name.toStdString();
        rec.address = address.toStdString();

        QPair<QString, QString> pair(address, asset_name);
        if (cacheMyAssetData.contains(pair)) {
            rec.involvesWatchAddress = cacheMyAssetData[pair].involvesWatchAddress;
            cacheMyAssetData[pair] = rec;
        } else {
            rec.involvesWatchAddress = false;
            parent->beginInsertRows(QModelIndex(), 0, 0);
            cacheMyAssetData[pair] = rec;
            vectAssetData.push_front(pair);
            parent->endInsertRows();
        }
    }

    int size()
    {
        return cacheMyAssetData.size();
    }

    MyRestrictedAssetRecord* index(int idx)
    {
        if (idx >= 0 && idx < vectAssetData.size()) {
            auto pair = vectAssetData[idx];
            if (cacheMyAssetData.contains(pair)) {
                MyRestrictedAssetRecord* rec = &cacheMyAssetData[pair];
                return rec;
            }
        }
        return 0;
    }
};

MyRestrictedAssetsTableModel::MyRestrictedAssetsTableModel(const PlatformStyle* _platformStyle, wallet::CWallet* _wallet, WalletModel* parent) : QAbstractTableModel(parent),
                                                                                                                                         wallet(_wallet),
                                                                                                                                         walletModel(parent),
                                                                                                                                         priv(new MyRestrictedAssetsTablePriv(_wallet, this)),
                                                                                                                                         fProcessingQueuedTransactions(false),
                                                                                                                                         platformStyle(_platformStyle)
{
    columns << tr("Date") << tr("Type") << tr("Address") << tr("Asset Name");

    priv->refreshWallet();

    connect(walletModel->getOptionsModel(), &OptionsModel::displayUnitChanged, this, &MyRestrictedAssetsTableModel::updateDisplayUnit);

    subscribeToCoreSignals();
}

MyRestrictedAssetsTableModel::~MyRestrictedAssetsTableModel()
{
    unsubscribeFromCoreSignals();
    delete priv;
}

void MyRestrictedAssetsTableModel::updateMyRestrictedAssets(const QString& address, const QString& asset_name, const int type, const qint64 date)
{
    priv->updateMyRestrictedAssets(address, asset_name, type, date);
}

void MyRestrictedAssetsTableModel::updateDisplayUnit()
{
    Q_EMIT headerDataChanged(Qt::Horizontal, 0, columnCount(QModelIndex()) - 1);
}

int MyRestrictedAssetsTableModel::rowCount(const QModelIndex& parent) const
{
    Q_UNUSED(parent);
    return priv->size();
}

int MyRestrictedAssetsTableModel::columnCount(const QModelIndex& parent) const
{
    Q_UNUSED(parent);
    return columns.length();
}

QString MyRestrictedAssetsTableModel::formatTxDate(const MyRestrictedAssetRecord* wtx) const
{
    if (wtx->time) {
        return GUIUtil::dateTimeStr(wtx->time);
    }
    return QString();
}

QString MyRestrictedAssetsTableModel::lookupAddress(const std::string& address, bool tooltip) const
{
    QString label = walletModel->getAddressTableModel()->labelForAddress(QString::fromStdString(address));
    QString description;
    if (!label.isEmpty()) {
        description += label;
    }
    if (label.isEmpty() || tooltip) {
        description += QString(" (") + QString::fromStdString(address) + QString(")");
    }
    return description;
}

QString MyRestrictedAssetsTableModel::formatTxType(const MyRestrictedAssetRecord* wtx) const
{
    switch (wtx->type) {
    case MyRestrictedAssetRecord::Tagged:
        return tr("Tagged");
    case MyRestrictedAssetRecord::UnTagged:
        return tr("Untagged");
    case MyRestrictedAssetRecord::Frozen:
        return tr("Frozen");
    case MyRestrictedAssetRecord::UnFrozen:
        return tr("Unfrozen");
    case MyRestrictedAssetRecord::Other:
        return tr("Other");
    default:
        return QString();
    }
}

QVariant MyRestrictedAssetsTableModel::txAddressDecoration(const MyRestrictedAssetRecord* wtx) const
{
    if (wtx->involvesWatchAddress)
        return QIcon(":/icons/eye");

    return QVariant();
}

QString MyRestrictedAssetsTableModel::formatTxToAddress(const MyRestrictedAssetRecord* wtx, bool tooltip) const
{
    QString watchAddress;
    if (tooltip) {
        watchAddress = wtx->involvesWatchAddress ? QString(" (") + tr("watch-only") + QString(")") : QString("");
    }

    return QString::fromStdString(wtx->address) + watchAddress;
}

QVariant MyRestrictedAssetsTableModel::addressColor(const MyRestrictedAssetRecord* wtx) const
{
    return QVariant();
}


QVariant MyRestrictedAssetsTableModel::txWatchonlyDecoration(const MyRestrictedAssetRecord* wtx) const
{
    if (wtx->involvesWatchAddress)
        return QIcon(":/icons/eye");
    else
        return QVariant();
}

QString MyRestrictedAssetsTableModel::formatTooltip(const MyRestrictedAssetRecord* rec) const
{
    QString tooltip = formatTxType(rec);
    return tooltip;
}

QVariant MyRestrictedAssetsTableModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid())
        return QVariant();
    MyRestrictedAssetRecord* rec = static_cast<MyRestrictedAssetRecord*>(index.internalPointer());

    switch (role) {
    case RawDecorationRole:
        switch (index.column()) {
        case ToAddress:
            return txAddressDecoration(rec);
        case AssetName:
            return QString::fromStdString(rec->assetName);
        }
        break;
    case Qt::DecorationRole: {
        QIcon icon = qvariant_cast<QIcon>(index.data(RawDecorationRole));
        return platformStyle->TextColorIcon(icon);
    }
    case Qt::DisplayRole:
        switch (index.column()) {
        case Date:
            return formatTxDate(rec);
        case Type:
            return formatTxType(rec);
        case ToAddress:
            return formatTxToAddress(rec, false);
        case AssetName:
            return QString::fromStdString(rec->assetName);
        }
        break;
    case Qt::EditRole:
        switch (index.column()) {
        case Date:
            return rec->time;
        case Type:
            return formatTxType(rec);
        case ToAddress:
            return formatTxToAddress(rec, true);
        case AssetName:
            return QString::fromStdString(rec->assetName);
        }
        break;
    case Qt::ToolTipRole:
        return formatTooltip(rec);
    case Qt::TextAlignmentRole:
        return column_alignments[index.column()];
    case Qt::ForegroundRole:
        if (index.column() == ToAddress) {
            return addressColor(rec);
        }
        break;
    case TypeRole:
        return rec->type;
    case DateRole:
        return QDateTime::fromSecsSinceEpoch(static_cast<qint64>(rec->time));
    case WatchonlyRole:
        return rec->involvesWatchAddress;
    case WatchonlyDecorationRole:
        return txWatchonlyDecoration(rec);
    case AddressRole:
        return QString::fromStdString(rec->address);
    case LabelRole:
        return walletModel->getAddressTableModel()->labelForAddress(QString::fromStdString(rec->address));
    case TxIDRole:
        return rec->getTxID();
    case TxHashRole:
        return QString::fromStdString(rec->hash.ToString());
    case TxHexRole:
        return "";
    case TxPlainTextRole: {
        QString details;
        QDateTime date = QDateTime::fromSecsSinceEpoch(static_cast<qint64>(rec->time));
        QString txLabel = walletModel->getAddressTableModel()->labelForAddress(QString::fromStdString(rec->address));

        details.append(date.toString("M/d/yy HH:mm"));
        details.append(" ");
        details.append(". ");
        if (!formatTxType(rec).isEmpty()) {
            details.append(formatTxType(rec));
            details.append(" ");
        }
        if (!rec->address.empty()) {
            if (txLabel.isEmpty())
                details.append(tr("(no label)") + " ");
            else {
                details.append("(");
                details.append(txLabel);
                details.append(") ");
            }
            details.append(QString::fromStdString(rec->address));
            details.append(" ");
        }
        return details;
    }
    case AssetNameRole: {
        QString assetName;
        assetName.append(QString::fromStdString(rec->assetName));
        return assetName;
    }
    }
    return QVariant();
}

QVariant MyRestrictedAssetsTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation == Qt::Horizontal) {
        if (role == Qt::DisplayRole) {
            return columns[section];
        } else if (role == Qt::TextAlignmentRole) {
            return column_alignments[section];
        } else if (role == Qt::ToolTipRole) {
            switch (section) {
            case Date:
                return tr("Date and time that the transaction was received.");
            case Type:
                return tr("Type of transaction.");
            case ToAddress:
                return tr("User-defined intent/purpose of the transaction.");
            case AssetName:
                return tr("The asset (or AVN) removed or added to balance.");
            }
        }
    }
    return QVariant();
}

QModelIndex MyRestrictedAssetsTableModel::index(int row, int column, const QModelIndex& parent) const
{
    Q_UNUSED(parent);
    MyRestrictedAssetRecord* data = priv->index(row);
    if (data) {
        return createIndex(row, column, priv->index(row));
    }
    return QModelIndex();
}

void MyRestrictedAssetsTableModel::subscribeToCoreSignals()
{
    // Restricted asset notifications (qualifier assignment, freeze/unfreeze) are
    // currently populated via refreshWallet() reading from pmyrestricteddb.
    // Real-time push notifications would require hooks in ConnectBlock/DisconnectBlock
    // that fire when qualifier/freeze transactions are processed.
}

void MyRestrictedAssetsTableModel::unsubscribeFromCoreSignals()
{
    // No real-time signals connected yet — see subscribeToCoreSignals()
}
