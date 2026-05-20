// Copyright (c) 2026 The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_VAULTDIALOG_H
#define BITCOIN_QT_VAULTDIALOG_H

#include <consensus/amount.h>

#include <memory>

#include <QList>
#include <QWidget>

class ClientModel;
class BitcoinAmountField;
class PlatformStyle;
class WalletModel;
enum class SynchronizationState;
namespace wallet { class CCoinControl; }

QT_BEGIN_NAMESPACE
class QCheckBox;
class QComboBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;
class QTableWidget;
class QTabWidget;
QT_END_NAMESPACE

/** Data stored for each created vault. */
struct VaultRecord {
    QString label;
    QString p2shAddress;
    QString redeemScript;   // hex of witnessScript: <pubkey> OP_CHECKSIGVERIFY <N> OP_CLTV
    QString descriptor;     // wsh(and_v(v:pk(PUBKEY),after(N)))#checksum
    QString recipientAddress;
    int64_t locktime{0};
    bool    isTimestamp{false};
    CAmount amount{0};      // satoshis
    QString txid;
    int     vout{-1};
    bool    isVesting{false};
    int     trancheIndex{-1};
    QString vestingId;
    bool    isPQVault{false}; // true for WITNESS_V2_MLDSA44_CLTV (RIP-25 PQ vault)
};

/**
 * VaultDialog — AVN Vault creation and management page.
 *
 * Provides:
 *   • Create Vault tab: build P2SH-CLTV outputs (single lock or vesting schedule)
 *   • My Vaults tab: list created vaults and unlock status
 */
class VaultDialog : public QWidget
{
    Q_OBJECT

public:
    explicit VaultDialog(const PlatformStyle* platformStyle, QWidget* parent = nullptr);
    ~VaultDialog();

    void setModel(WalletModel* model);
    void setClientModel(ClientModel* clientModel);

public Q_SLOTS:
    void refresh();

private Q_SLOTS:
    void onCreateVault();
    void onCoinControlButtonClicked();
    void onCoinControlUpdateLabels();
    void onAddTranche();
    void onRemoveTranche();
    void onPresetSelected(int index);
    void onVestingToggled(bool enabled);
    void onSelfButtonClicked();
    void onLockModeChanged();
    void onRefreshVaults();
    void onScanForVaults();
    void onRenameVault();
    void onCopyAddress();
    void onCopyRedeemScript();
    void onReimportScripts();
    void onSpendVault();

private:
    // ── Widgets: Create Vault tab ──────────────────────────
    QPushButton*  m_coinControlButton{nullptr};
    QLabel*       m_coinControlLabel{nullptr};
    QLineEdit*    m_labelEdit{nullptr};
    QLineEdit*    m_recipientEdit{nullptr};
    QPushButton*  m_selfButton{nullptr};
    QRadioButton* m_radioBlockHeight{nullptr};
    QRadioButton* m_radioTimestamp{nullptr};
    QLineEdit*    m_lockValueEdit{nullptr};
    QLabel*       m_currentBlockLabel{nullptr};
    QLabel*       m_currentTimeLabel{nullptr};
    BitcoinAmountField* m_amountEdit{nullptr};
    QCheckBox*    m_vestingCheck{nullptr};
    QGroupBox*    m_vestingGroup{nullptr};
    QTableWidget* m_trancheTable{nullptr};
    QPushButton*  m_addTrancheButton{nullptr};
    QPushButton*  m_removeTrancheButton{nullptr};
    QComboBox*    m_presetCombo{nullptr};
    QPushButton*  m_createButton{nullptr};

    // ── Widgets: My Vaults tab ─────────────────────────────
    QTableWidget* m_vaultsTable{nullptr};
    QPushButton*  m_refreshButton{nullptr};
    QPushButton*  m_scanButton{nullptr};
    QPushButton*  m_renameButton{nullptr};
    QPushButton*  m_copyAddressButton{nullptr};
    QPushButton*  m_copyRedeemScriptButton{nullptr};
    QPushButton*  m_reimportScriptsButton{nullptr};
    QPushButton*  m_spendButton{nullptr};

    // ── State ──────────────────────────────────────────────
    std::unique_ptr<wallet::CCoinControl> m_coin_control;
    WalletModel*          m_model{nullptr};
    ClientModel*          m_clientModel{nullptr};
    const PlatformStyle*  m_platformStyle;
    QList<VaultRecord>    m_vaults;

    // ── Setup helpers ──────────────────────────────────────
    void setupCreateTab(QWidget* tab);
    void setupMyVaultsTab(QWidget* tab);

    // ── Vault creation ─────────────────────────────────────
    bool buildAndSendSingleVault(const QString& label,
                                 const QString& recipient,
                                 int64_t locktime,
                                 bool isTimestamp,
                                 CAmount amount,
                                 QString& outP2shAddr,
                                 QString& outRedeemScript,
                                 QString& outDescriptor,
                                 QString& outTxid,
                                 int& outVout,
                                 QString& outError);
    /** RIP-25 PQ vault: build and send a WITNESS_V2_MLDSA44_CLTV vault output. */
    bool buildAndSendPQVault(const QString& label,
                             const QString& recipient,
                             int64_t locktime,
                             bool isTimestamp,
                             CAmount amount,
                             QString& outVaultAddr,
                             QString& outTxid,
                             int& outVout,
                             QString& outError);
    static std::string buildVaultOpReturnHex(const std::vector<uint8_t>& keyId,
                                             const std::vector<int64_t>& locktimes,
                                             bool isPQ, bool isTimestamp);

    // ── Persistence ────────────────────────────────────────
    void saveVaultRecord(const VaultRecord& record);
    void loadVaultRecords();
    void refreshVaultsTable();
    QString vaultStoragePath() const;

    // ── Helpers ────────────────────────────────────────────
    int     currentBlockHeight() const;
    int64_t currentUnixTime() const;
    CAmount parseAmount(const QString& text, bool& ok) const;
};

#endif // BITCOIN_QT_VAULTDIALOG_H
