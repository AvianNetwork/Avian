// Copyright (c) 2011-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_OVERVIEWPAGE_H
#define BITCOIN_QT_OVERVIEWPAGE_H

#include <interfaces/wallet.h>

#include <QWidget>
#include <QMenu>
#include <memory>

class AssetFilterProxy;
class AssetViewDelegate;
class ClientModel;
class TransactionFilterProxy;
class TxViewDelegate;
class PlatformStyle;
class WalletModel;

namespace Ui {
    class OverviewPage;
}

QT_BEGIN_NAMESPACE
class QModelIndex;
QT_END_NAMESPACE

/** Overview ("home") page widget */
class OverviewPage : public QWidget
{
    Q_OBJECT

public:
    explicit OverviewPage(const PlatformStyle *platformStyle, QWidget *parent = nullptr);
    ~OverviewPage();

    void setClientModel(ClientModel *clientModel);
    void setWalletModel(WalletModel *walletModel);
    void showOutOfSyncWarning(bool fShow);

    /** AVN START */
    void showAssets();
    /** AVN END */

public Q_SLOTS:
    void setBalance(const interfaces::WalletBalances& balances);
    void setPrivacy(bool privacy);

Q_SIGNALS:
    void transactionClicked(const QModelIndex &index);
    void outOfSyncWarningClicked();

    /** AVN START */
    void assetSendClicked(const QModelIndex& index);
    void assetIssueSubClicked(const QModelIndex& index);
    void assetIssueUniqueClicked(const QModelIndex& index);
    void assetReissueClicked(const QModelIndex& index);
    /** AVN END */

protected:
    void changeEvent(QEvent* e) override;

private:
    Ui::OverviewPage *ui;
    ClientModel* clientModel{nullptr};
    WalletModel* walletModel{nullptr};
    bool m_privacy{false};

    const PlatformStyle* m_platform_style;

    TxViewDelegate *txdelegate;
    AssetViewDelegate *assetdelegate;
    std::unique_ptr<TransactionFilterProxy> filter;

    /** AVN START */
    std::unique_ptr<AssetFilterProxy> assetFilter;
    QMenu* assetContextMenu{nullptr};
    QAction* assetSendAction{nullptr};
    QAction* assetIssueSubAction{nullptr};
    QAction* assetIssueUniqueAction{nullptr};
    QAction* assetReissueAction{nullptr};
    QAction* assetCopyNameAction{nullptr};
    QAction* assetCopyAmountAction{nullptr};
    QAction* assetCopyHashAction{nullptr};
    QAction* assetOpenIPFSAction{nullptr};
    QAction* assetViewANSAction{nullptr};
    /** AVN END */

private Q_SLOTS:
    void updateDisplayUnit();
    void handleTransactionClicked(const QModelIndex &index);
    void updateAlerts(const QString &warnings);
    void setMonospacedFont(const QFont&);

    /** AVN START */
    void assetSearchChanged();
    void handleAssetRightClicked(const QModelIndex& index);
    /** AVN END */
};

#endif // BITCOIN_QT_OVERVIEWPAGE_H
