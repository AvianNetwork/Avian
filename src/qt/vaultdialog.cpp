// Copyright (c) 2026 The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/vaultdialog.h>

#include <qt/bitcoinunits.h>
#include <qt/clientmodel.h>
#include <qt/platformstyle.h>
#include <qt/bitcoinamountfield.h>
#include <qt/coincontroldialog.h>
#include <qt/optionsmodel.h>
#include <qt/guiutil.h>
#include <qt/walletmodel.h>
#include <qt/walletmodeltransaction.h>
#include <qt/sendcoinsrecipient.h>

#include <addresstype.h>
#include <common/args.h>
#include <consensus/amount.h>
#include <interfaces/node.h>
#include <key_io.h>
#include <script/script.h>
#include <util/moneystr.h>
#include <util/strencodings.h>
#include <wallet/coincontrol.h>

#include <univalue.h>

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTabWidget>
#include <QVBoxLayout>
#include <hash.h>
#include <QUuid>

// ─────────────────────────────────────────────────────────────────────────────
//  Script builder
// ─────────────────────────────────────────────────────────────────────────────

/**
 * Build the Miniscript witness script for a CLTV vault:
 *   wsh(and_v(v:pk(PUBKEY),after(LOCKTIME)))
 * Inner witnessScript = <pubkey> OP_CHECKSIGVERIFY <locktime> OP_CHECKLOCKTIMEVERIFY
 */
static CScript BuildCLTVWitnessScript(const std::vector<unsigned char>& pubkeyBytes, int64_t locktime)
{
    CScript script;
    script << pubkeyBytes << OP_CHECKSIGVERIFY << locktime << OP_CHECKLOCKTIMEVERIFY;
    return script;
}

/**
 * Build the OP_RETURN payload for an on-chain vault marker.
 * Format: AVAN(4) | version=0x01(1) | flags(1) | locktime_LE8 x N | keyId (20B non-PQ / 32B PQ)
 * Flags: bits 0-3 = num_tranches-1, bit 4 = isPQ, bit 5 = isTimestamp
 */
/*static*/ std::string VaultDialog::buildVaultOpReturnHex(
    const std::vector<uint8_t>& keyId,
    const std::vector<int64_t>& locktimes,
    bool isPQ,
    bool isTimestamp)
{
    std::vector<uint8_t> payload;
    // Magic: "AVAN"
    payload.push_back(0x41); payload.push_back(0x56);
    payload.push_back(0x41); payload.push_back(0x4E);
    // Version
    payload.push_back(0x01);
    // Flags
    uint8_t flags = static_cast<uint8_t>((locktimes.size() - 1) & 0x0F);
    if (isPQ)        flags |= 0x10;
    if (isTimestamp) flags |= 0x20;
    payload.push_back(flags);
    // Locktimes (8 bytes each, little-endian)
    for (int64_t lt : locktimes) {
        for (int b = 0; b < 8; b++)
            payload.push_back(static_cast<uint8_t>((lt >> (8 * b)) & 0xFF));
    }
    // Key ID (HASH160 for non-PQ, SHA256 for PQ)
    payload.insert(payload.end(), keyId.begin(), keyId.end());
    return HexStr(payload);
}

// ─────────────────────────────────────────────────────────────────────────────
//  VaultDialog
// ─────────────────────────────────────────────────────────────────────────────

VaultDialog::VaultDialog(const PlatformStyle* platformStyle, QWidget* parent)
    : QWidget(parent),
      m_coin_control(std::make_unique<wallet::CCoinControl>()),
      m_platformStyle(platformStyle)
{
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);

    QTabWidget* tabs = new QTabWidget(this);
    mainLayout->addWidget(tabs);

    QWidget* createTab = new QWidget();
    setupCreateTab(createTab);
    tabs->addTab(createTab, tr("Create Vault"));

    QWidget* myVaultsTab = new QWidget();
    setupMyVaultsTab(myVaultsTab);
    tabs->addTab(myVaultsTab, tr("My Vaults"));

    setLayout(mainLayout);
}

VaultDialog::~VaultDialog() = default;

void VaultDialog::setModel(WalletModel* model)
{
    m_model = model;
    if (model && model->getOptionsModel()) {
        m_amountEdit->setDisplayUnit(model->getOptionsModel()->getDisplayUnit());
        connect(model->getOptionsModel(), &OptionsModel::displayUnitChanged,
                m_amountEdit, &BitcoinAmountField::setDisplayUnit);
    }
}

void VaultDialog::setClientModel(ClientModel* clientModel)
{
    m_clientModel = clientModel;
    if (clientModel) {
        connect(clientModel, &ClientModel::numBlocksChanged, this, [this](int count, const QDateTime&, double, SyncType, SynchronizationState) {
            m_currentBlockLabel->setText(tr("Current block: %1").arg(count));
            m_currentTimeLabel->setText(tr("Current Unix time: %1").arg(QDateTime::currentSecsSinceEpoch()));
        });
        // Populate initial values
        int h = clientModel->getNumBlocks();
        m_currentBlockLabel->setText(tr("Current block: %1").arg(h));
        m_currentTimeLabel->setText(tr("Current Unix time: %1").arg(QDateTime::currentSecsSinceEpoch()));
    }
}

void VaultDialog::refresh()
{
    loadVaultRecords();
    refreshVaultsTable();
}

// ─────────────────────────────────────────────────────────────────────────────
//  UI setup
// ─────────────────────────────────────────────────────────────────────────────

void VaultDialog::setupCreateTab(QWidget* tab)
{
    QVBoxLayout* layout = new QVBoxLayout(tab);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(10);

    // ── Coin Control ──────────────────────────────────────
    {
        QGroupBox* ccBox = new QGroupBox(tr("Coin Control Features"), tab);
        QHBoxLayout* row = new QHBoxLayout(ccBox);
        m_coinControlButton = new QPushButton(tr("Inputs..."), ccBox);
        m_coinControlButton->setFixedWidth(100);
        m_coinControlLabel = new QLabel(tr("automatically selected"), ccBox);
        row->addWidget(m_coinControlButton);
        row->addWidget(m_coinControlLabel);
        row->addStretch();
        layout->addWidget(ccBox);
        connect(m_coinControlButton, &QPushButton::clicked, this, &VaultDialog::onCoinControlButtonClicked);
    }

    // ── Label ─────────────────────────────────────────────
    {
        QHBoxLayout* row = new QHBoxLayout();
        row->addWidget(new QLabel(tr("Vault label:"), tab));
        m_labelEdit = new QLineEdit(tab);
        m_labelEdit->setPlaceholderText(tr("e.g. Team allocation 2026"));
        row->addWidget(m_labelEdit);
        layout->addLayout(row);
    }

    // ── Recipient ─────────────────────────────────────────
    {
        QHBoxLayout* row = new QHBoxLayout();
        row->addWidget(new QLabel(tr("Recipient address:"), tab));
        m_recipientEdit = new QLineEdit(tab);
        m_recipientEdit->setPlaceholderText(tr("Avian P2PKH or P2WPKH address"));
        row->addWidget(m_recipientEdit);
        m_selfButton = new QPushButton(tr("Self"), tab);
        m_selfButton->setToolTip(tr("Use one of your own wallet addresses"));
        m_selfButton->setFixedWidth(60);
        row->addWidget(m_selfButton);
        layout->addLayout(row);
        connect(m_selfButton, &QPushButton::clicked, this, &VaultDialog::onSelfButtonClicked);
    }

    // ── Lock mode ─────────────────────────────────────────
    {
        QHBoxLayout* row = new QHBoxLayout();
        row->addWidget(new QLabel(tr("Lock mode:"), tab));
        m_radioBlockHeight = new QRadioButton(tr("Block height"), tab);
        m_radioTimestamp   = new QRadioButton(tr("Unix timestamp"), tab);
        m_radioBlockHeight->setChecked(true);
        row->addWidget(m_radioBlockHeight);
        row->addWidget(m_radioTimestamp);
        row->addStretch();
        layout->addLayout(row);
        connect(m_radioBlockHeight, &QRadioButton::toggled, this, &VaultDialog::onLockModeChanged);
        connect(m_radioTimestamp,   &QRadioButton::toggled, this, &VaultDialog::onLockModeChanged);
    }

    // ── Lock value ────────────────────────────────────────
    {
        QHBoxLayout* row = new QHBoxLayout();
        row->addWidget(new QLabel(tr("Lock value:"), tab));
        m_lockValueEdit = new QLineEdit(tab);
        m_lockValueEdit->setValidator(new QRegularExpressionValidator(QRegularExpression(QStringLiteral("\\d{1,15}")), m_lockValueEdit));
        m_lockValueEdit->setPlaceholderText(tr("Block height or Unix timestamp"));
        row->addWidget(m_lockValueEdit);
        layout->addLayout(row);

        // Hint labels
        m_currentBlockLabel = new QLabel(tr("Current block: —"), tab);
        m_currentTimeLabel  = new QLabel(tr("Current Unix time: —"), tab);
        QFont small = m_currentBlockLabel->font();
        small.setPointSizeF(small.pointSizeF() * 0.85);
        m_currentBlockLabel->setFont(small);
        m_currentTimeLabel->setFont(small);
        layout->addWidget(m_currentBlockLabel);
        layout->addWidget(m_currentTimeLabel);
    }

    // ── Amount ────────────────────────────────────────────
    {
        QHBoxLayout* row = new QHBoxLayout();
        row->addWidget(new QLabel(tr("Amount (AVN):"), tab));
        m_amountEdit = new BitcoinAmountField(tab);
        m_amountEdit->SetAllowEmpty(false);
        row->addWidget(m_amountEdit);
        layout->addLayout(row);
    }

    // ── Vesting toggle ────────────────────────────────────
    m_vestingCheck = new QCheckBox(tr("Enable vesting schedule (multiple tranches)"), tab);
    layout->addWidget(m_vestingCheck);
    connect(m_vestingCheck, &QCheckBox::toggled, this, &VaultDialog::onVestingToggled);

    // ── Vesting group ─────────────────────────────────────
    m_vestingGroup = new QGroupBox(tr("Vesting tranches"), tab);
    m_vestingGroup->setVisible(false);
    QVBoxLayout* vestLayout = new QVBoxLayout(m_vestingGroup);

    // Preset selector
    {
        QHBoxLayout* row = new QHBoxLayout();
        row->addWidget(new QLabel(tr("Preset:"), m_vestingGroup));
        m_presetCombo = new QComboBox(m_vestingGroup);
        m_presetCombo->addItem(tr("Manual"));
        m_presetCombo->addItem(tr("Linear — 4 quarters (blocks)"));
        m_presetCombo->addItem(tr("Linear — 6 months (blocks)"));
        m_presetCombo->addItem(tr("Linear — 12 months (blocks)"));
        m_presetCombo->addItem(tr("Cliff 1 yr + Linear 4 qtrs (blocks)"));
        m_presetCombo->addItem(tr("Back-loaded 4 qtrs 10/20/30/40 (blocks)"));
        m_presetCombo->addItem(tr("Linear — 4 quarters (timestamps)"));
        m_presetCombo->addItem(tr("Linear — 12 months (timestamps)"));
        row->addWidget(m_presetCombo);
        row->addStretch();
        vestLayout->addLayout(row);
        connect(m_presetCombo, qOverload<int>(&QComboBox::activated), this, &VaultDialog::onPresetSelected);
    }

    // Tranche table
    m_trancheTable = new QTableWidget(0, 3, m_vestingGroup);
    m_trancheTable->setHorizontalHeaderLabels({tr("#"), tr("Lock Value"), tr("Amount (AVN)")});
    m_trancheTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_trancheTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_trancheTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_trancheTable->setMinimumHeight(120);
    vestLayout->addWidget(m_trancheTable);

    // Add/Remove buttons
    {
        QHBoxLayout* row = new QHBoxLayout();
        m_addTrancheButton    = new QPushButton(tr("Add tranche"), m_vestingGroup);
        m_removeTrancheButton = new QPushButton(tr("Remove tranche"), m_vestingGroup);
        row->addWidget(m_addTrancheButton);
        row->addWidget(m_removeTrancheButton);
        row->addStretch();
        vestLayout->addLayout(row);
        connect(m_addTrancheButton,    &QPushButton::clicked, this, &VaultDialog::onAddTranche);
        connect(m_removeTrancheButton, &QPushButton::clicked, this, &VaultDialog::onRemoveTranche);
    }

    layout->addWidget(m_vestingGroup);

    // ── Create button ─────────────────────────────────────
    {
        QHBoxLayout* row = new QHBoxLayout();
        row->addStretch();
        m_createButton = new QPushButton(tr("Create Vault"), tab);
        m_createButton->setFixedHeight(36);
        m_createButton->setDefault(true);
        row->addWidget(m_createButton);
        layout->addLayout(row);
        connect(m_createButton, &QPushButton::clicked, this, &VaultDialog::onCreateVault);
    }

    layout->addStretch();
}

void VaultDialog::setupMyVaultsTab(QWidget* tab)
{
    QVBoxLayout* layout = new QVBoxLayout(tab);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(8);

    // Button bar
    {
        QHBoxLayout* row = new QHBoxLayout();
        m_refreshButton         = new QPushButton(tr("Refresh"), tab);
        m_scanButton            = new QPushButton(tr("Scan for Vaults"), tab);
        m_scanButton->setToolTip(tr("Scan wallet transaction history for AVAN OP_RETURN vault markers and recover any missing vault records"));
        m_renameButton          = new QPushButton(tr("Rename"), tab);
        m_renameButton->setToolTip(tr("Rename the selected vault label"));
        m_renameButton->setEnabled(false);
        m_copyAddressButton     = new QPushButton(tr("Copy P2SH address"), tab);
        m_copyRedeemScriptButton = new QPushButton(tr("Copy redeem script"), tab);
        m_reimportScriptsButton  = new QPushButton(tr("Re-import scripts"), tab);
        m_reimportScriptsButton->setToolTip(tr("Re-register all vault redeem scripts with the wallet (fixes ismine/solvable)"));
        m_spendButton           = new QPushButton(tr("Spend"), tab);
        m_spendButton->setToolTip(tr("Spend unlocked vault back to a destination address"));
        m_spendButton->setEnabled(false);
        m_copyAddressButton->setEnabled(false);
        m_copyRedeemScriptButton->setEnabled(false);
        row->addWidget(m_refreshButton);
        row->addWidget(m_scanButton);
        row->addWidget(m_reimportScriptsButton);
        row->addStretch();
        row->addWidget(m_renameButton);
        row->addWidget(m_spendButton);
        row->addWidget(m_copyAddressButton);
        row->addWidget(m_copyRedeemScriptButton);
        layout->addLayout(row);
        connect(m_refreshButton,          &QPushButton::clicked, this, &VaultDialog::onRefreshVaults);
        connect(m_scanButton,             &QPushButton::clicked, this, &VaultDialog::onScanForVaults);
        connect(m_reimportScriptsButton,  &QPushButton::clicked, this, &VaultDialog::onReimportScripts);
        connect(m_renameButton,           &QPushButton::clicked, this, &VaultDialog::onRenameVault);
        connect(m_spendButton,            &QPushButton::clicked, this, &VaultDialog::onSpendVault);
        connect(m_copyAddressButton,      &QPushButton::clicked, this, &VaultDialog::onCopyAddress);
        connect(m_copyRedeemScriptButton, &QPushButton::clicked, this, &VaultDialog::onCopyRedeemScript);
    }

    // Vault table
    m_vaultsTable = new QTableWidget(0, 7, tab);
    m_vaultsTable->setHorizontalHeaderLabels({
        tr("Label"), tr("P2SH Address"), tr("Amount (AVN)"),
        tr("Lock Value"), tr("Lock Type"), tr("Recipient"), tr("Status")
    });
    m_vaultsTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_vaultsTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_vaultsTable->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);
    m_vaultsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_vaultsTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_vaultsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_vaultsTable->setAlternatingRowColors(true);
    layout->addWidget(m_vaultsTable);

    connect(m_vaultsTable, &QTableWidget::itemSelectionChanged, this, [this]() {
        int row = m_vaultsTable->currentRow();
        bool hasSel = row >= 0 && row < m_vaults.size();
        m_copyAddressButton->setEnabled(hasSel);
        m_renameButton->setEnabled(hasSel);
        if (hasSel) {
            const VaultRecord& rec = m_vaults[row];
            m_copyRedeemScriptButton->setEnabled(!rec.isPQVault);
            bool unlocked = rec.isTimestamp
                ? (currentUnixTime()    >= rec.locktime)
                : (currentBlockHeight() >= static_cast<int>(rec.locktime));
            m_spendButton->setEnabled(unlocked);
        } else {
            m_copyRedeemScriptButton->setEnabled(false);
            m_spendButton->setEnabled(false);
        }
    });
}

// ─────────────────────────────────────────────────────────────────────────────
//  Slots — Create Vault tab
// ─────────────────────────────────────────────────────────────────────────────

// Import the vault descriptor into the wallet so the P2WSH address is
// watchable (ismine/solvable). Fails silently if the clientModel is not set.
static void importVaultDescriptor(ClientModel* clientModel,
                                  const std::string& descriptor,
                                  const std::string& label,
                                  const std::string& walletName)
{
    if (!clientModel || descriptor.empty()) return;
    try {
        UniValue descObj(UniValue::VOBJ);
        descObj.pushKV("desc",      descriptor);
        descObj.pushKV("timestamp", std::string("now"));
        descObj.pushKV("label",     label);
        UniValue descArr(UniValue::VARR);
        descArr.push_back(descObj);
        UniValue params(UniValue::VARR);
        params.push_back(descArr);
        clientModel->node().executeRpc("importdescriptors", params,
            walletName.empty() ? "" : "/wallet/" + walletName);
    } catch (const UniValue&) {
        // RPC error — non-fatal, vault still saved
    } catch (...) {}
}

void VaultDialog::onLockModeChanged()
{
    bool isBlock = m_radioBlockHeight->isChecked();
    m_lockValueEdit->setPlaceholderText(isBlock ? tr("Block height") : tr("Unix timestamp (seconds since epoch)"));
}

void VaultDialog::onCoinControlButtonClicked()
{
    if (!m_model) return;
    auto dlg = new CoinControlDialog(*m_coin_control, m_model, m_platformStyle);
    connect(dlg, &QDialog::finished, this, &VaultDialog::onCoinControlUpdateLabels);
    GUIUtil::ShowModalDialogAsynchronously(dlg);
}

void VaultDialog::onCoinControlUpdateLabels()
{
    if (!m_coin_control->HasSelected()) {
        m_coinControlLabel->setText(tr("automatically selected"));
    } else {
        int n = static_cast<int>(m_coin_control->ListSelected().size());
        m_coinControlLabel->setText(tr("%n input(s) manually selected", "", n));
    }
}

void VaultDialog::onSelfButtonClicked()
{
    if (!m_model) return;

    auto dest = m_model->wallet().getNewDestination(OutputType::LEGACY, "Vault recipient");
    if (dest) {
        m_recipientEdit->setText(QString::fromStdString(EncodeDestination(*dest)));
    } else {
        QMessageBox::warning(this, tr("Vault"), tr("Could not get a wallet address. Is the wallet unlocked?"));
    }
}

void VaultDialog::onVestingToggled(bool enabled)
{
    m_vestingGroup->setVisible(enabled);
    // When toggled on with an empty table, add one starter tranche
    if (enabled && m_trancheTable->rowCount() == 0) {
        onAddTranche();
    }
}

void VaultDialog::onAddTranche()
{
    int row = m_trancheTable->rowCount();
    m_trancheTable->insertRow(row);

    // Column 0: tranche number (read-only)
    auto* numItem = new QTableWidgetItem(QString::number(row + 1));
    numItem->setFlags(numItem->flags() & ~Qt::ItemIsEditable);
    m_trancheTable->setItem(row, 0, numItem);

    // Column 1: lock value
    m_trancheTable->setItem(row, 1, new QTableWidgetItem(QStringLiteral("0")));
    // Column 2: amount
    m_trancheTable->setItem(row, 2, new QTableWidgetItem(QStringLiteral("0.00")));
}

void VaultDialog::onRemoveTranche()
{
    int row = m_trancheTable->currentRow();
    if (row < 0) row = m_trancheTable->rowCount() - 1;
    if (row >= 0) {
        m_trancheTable->removeRow(row);
        // Renumber remaining rows
        for (int i = 0; i < m_trancheTable->rowCount(); i++) {
            if (m_trancheTable->item(i, 0))
                m_trancheTable->item(i, 0)->setText(QString::number(i + 1));
        }
    }
}

void VaultDialog::onPresetSelected(int index)
{
    if (index == 0) return; // Manual — do nothing

    // Parse total amount from the main amount field
    bool amtOk = false;
    CAmount totalSats = m_amountEdit->value(&amtOk);
    if (!amtOk || totalSats <= 0) {
        QMessageBox::warning(this, tr("Vault"),
            tr("Enter a total amount (AVN) before applying a preset."));
        m_presetCombo->setCurrentIndex(0);
        return;
    }

    int baseBlock = currentBlockHeight();
    int64_t baseTime  = currentUnixTime();

    // Block counts per period (Avian ≈ 1 min blocks)
    // const int DAY = 1440;  // unused directly, kept for reference
    const int MONTH  = 43200;   // 30 days
    const int QUARTER = 131400; // ~91.25 days

    // Per-second constants
    const int64_t SEC_MONTH   = 2592000LL;  // 30 days
    const int64_t SEC_QUARTER = 7884000LL;  // ~91.25 days

    // Build (lockValue, fraction) lists
    struct Tranche { int64_t lock; double frac; };
    QList<Tranche> tranches;

    switch (index) {
    case 1: // Linear 4 quarters (blocks)
        for (int i = 1; i <= 4; i++)
            tranches << Tranche{baseBlock + i * QUARTER, 0.25};
        break;
    case 2: // Linear 6 months (blocks)
        for (int i = 1; i <= 6; i++)
            tranches << Tranche{baseBlock + i * MONTH, 1.0 / 6.0};
        break;
    case 3: // Linear 12 months (blocks)
        for (int i = 1; i <= 12; i++)
            tranches << Tranche{baseBlock + i * MONTH, 1.0 / 12.0};
        break;
    case 4: // Cliff 1yr + Linear 4 quarters (blocks)
        for (int i = 1; i <= 4; i++)
            tranches << Tranche{baseBlock + 12 * MONTH + i * QUARTER, 0.25};
        break;
    case 5: // Back-loaded 10/20/30/40
        tranches << Tranche{baseBlock + 1 * QUARTER, 0.10};
        tranches << Tranche{baseBlock + 2 * QUARTER, 0.20};
        tranches << Tranche{baseBlock + 3 * QUARTER, 0.30};
        tranches << Tranche{baseBlock + 4 * QUARTER, 0.40};
        break;
    case 6: // Linear 4 quarters (timestamps)
        m_radioTimestamp->setChecked(true);
        for (int i = 1; i <= 4; i++)
            tranches << Tranche{baseTime + i * SEC_QUARTER, 0.25};
        break;
    case 7: // Linear 12 months (timestamps)
        m_radioTimestamp->setChecked(true);
        for (int i = 1; i <= 12; i++)
            tranches << Tranche{baseTime + i * SEC_MONTH, 1.0 / 12.0};
        break;
    default:
        return;
    }

    // Populate the tranche table
    m_trancheTable->setRowCount(0);
    CAmount distributed = 0;
    for (int i = 0; i < tranches.size(); i++) {
        // Last tranche gets remainder to avoid rounding drift
        CAmount trancheAmt = (i == tranches.size() - 1)
            ? (totalSats - distributed)
            : static_cast<CAmount>(std::round(totalSats * tranches[i].frac));
        distributed += trancheAmt;

        int row = m_trancheTable->rowCount();
        m_trancheTable->insertRow(row);

        auto* numItem = new QTableWidgetItem(QString::number(row + 1));
        numItem->setFlags(numItem->flags() & ~Qt::ItemIsEditable);
        m_trancheTable->setItem(row, 0, numItem);
        m_trancheTable->setItem(row, 1, new QTableWidgetItem(QString::number(tranches[i].lock)));
        m_trancheTable->setItem(row, 2, new QTableWidgetItem(
            BitcoinUnits::format(BitcoinUnits::Unit::BTC, trancheAmt, false, BitcoinUnits::SeparatorStyle::ALWAYS)));
    }
}

void VaultDialog::onCreateVault()
{
    if (!m_model) {
        QMessageBox::warning(this, tr("Vault"), tr("No wallet loaded."));
        return;
    }

    QString label    = m_labelEdit->text().trimmed();
    QString recipient = m_recipientEdit->text().trimmed();
    bool isTimestamp  = m_radioTimestamp->isChecked();
    bool isVesting    = m_vestingCheck->isChecked() && m_vestingGroup->isVisible();

    if (label.isEmpty()) {
        QMessageBox::warning(this, tr("Vault"), tr("Please enter a vault label."));
        return;
    }
    if (recipient.isEmpty()) {
        QMessageBox::warning(this, tr("Vault"), tr("Please enter a recipient address."));
        return;
    }

    // Validate recipient address format (basic check; pubkey fetched from wallet later)
    if (!IsValidDestination(DecodeDestination(recipient.toStdString()))) {
        QMessageBox::warning(this, tr("Vault"),
            tr("Please enter a valid recipient address."));
        return;
    }

    // Warn if the recipient address is not owned by any loaded wallet.
    // If it isn't, the vault cannot be spent from this UI.
    {
        bool isMineInAnyWallet = false;
        if (m_clientModel) {
            // Check all loaded wallets
            try {
                UniValue lw(UniValue::VARR);
                UniValue wallets = m_clientModel->node().executeRpc("listwallets", lw, "");
                for (size_t wi = 0; wi < wallets.size() && !isMineInAnyWallet; ++wi) {
                    try {
                        UniValue aiParams(UniValue::VARR);
                        aiParams.push_back(recipient.toStdString());
                        const std::string wuri = "/wallet/" + wallets[wi].get_str();
                        UniValue ai = m_clientModel->node().executeRpc("getaddressinfo", aiParams, wuri);
                        if (ai.exists("ismine") && ai["ismine"].get_bool())
                            isMineInAnyWallet = true;
                    } catch (...) {}
                }
            } catch (...) {}
        }
        if (!isMineInAnyWallet) {
            auto btn = QMessageBox::warning(this, tr("Vault"),
                tr("The recipient address:\n%1\n\n"
                   "is NOT in any loaded wallet.\n\n"
                   "You will be unable to spend this vault from this application.\n\n"
                   "Did you mean to use your own address? Click 'Use Self' to pick one.\n\n"
                   "Continue anyway?").arg(recipient),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (btn != QMessageBox::Yes) return;
        }
    }

    // ── Simple single-lock vault ───────────────────────────
    if (!isVesting) {
        bool lockOk = false;
        int64_t locktime = m_lockValueEdit->text().toLongLong(&lockOk);
        if (!lockOk || locktime <= 0) {
            QMessageBox::warning(this, tr("Vault"), tr("Please enter a valid lock value."));
            return;
        }

        bool amtOk = false;
        CAmount amount = m_amountEdit->value(&amtOk);
        if (!amtOk || amount <= 0) {
            QMessageBox::warning(this, tr("Vault"), tr("Please enter a valid amount."));
            return;
        }

        // ── PQ recipient: use WITNESS_V2_MLDSA44_CLTV vault ──
        CTxDestination recipientDest = DecodeDestination(recipient.toStdString());
        if (std::holds_alternative<WitnessV2MLDsa44>(recipientDest)) {
            QString vaultAddr, txid, pqError;
            int pqVout = 0;
            if (!buildAndSendPQVault(label, recipient, locktime, isTimestamp, amount, vaultAddr, txid, pqVout, pqError)) {
                QMessageBox::critical(this, tr("Vault creation failed"), pqError);
                return;
            }
            VaultRecord rec;
            rec.label            = label;
            rec.p2shAddress      = vaultAddr;
            rec.redeemScript     = QString(); // native PQ witness — no redeem script
            rec.descriptor       = QString();
            rec.recipientAddress = recipient;
            rec.locktime         = locktime;
            rec.isTimestamp      = isTimestamp;
            rec.amount           = amount;
            rec.txid             = txid;
            rec.vout             = pqVout;
            rec.isVesting        = false;
            rec.trancheIndex     = -1;
            rec.isPQVault        = true;
            saveVaultRecord(rec);
            QMessageBox::information(this, tr("Vault created"),
                tr("PQ Vault created!\n\nVault address: %1\nTxid: %2").arg(vaultAddr).arg(txid));
            refresh();
            return;
        }

        QString p2shAddr, redeemHex, descriptor, txid, error;
        int singleVout = 0;
        if (!buildAndSendSingleVault(label, recipient, locktime, isTimestamp, amount,
                                     p2shAddr, redeemHex, descriptor, txid, singleVout, error)) {
            QMessageBox::critical(this, tr("Vault creation failed"), error);
            return;
        }

        VaultRecord rec;
        rec.label            = label;
        rec.p2shAddress      = p2shAddr;
        rec.redeemScript     = redeemHex; // stores witnessScript hex
        rec.descriptor       = descriptor;
        rec.recipientAddress = recipient;
        rec.locktime         = locktime;
        rec.isTimestamp      = isTimestamp;
        rec.amount           = amount;
        rec.txid             = txid;
        rec.vout             = singleVout;
        rec.isVesting        = false;
        rec.trancheIndex     = -1;

        saveVaultRecord(rec);
        // descriptor was already imported inside buildAndSendSingleVault
        QMessageBox::information(this, tr("Vault created"),
            tr("Vault created successfully!\n\nVault address: %1\nTxid: %2")
                .arg(p2shAddr).arg(txid));
        refresh();
        return;
    }

    // ── Vesting: multiple tranches in one transaction ──────
    if (m_trancheTable->rowCount() == 0) {
        QMessageBox::warning(this, tr("Vault"), tr("Please add at least one tranche."));
        return;
    }

    // Parse tranche data
    struct TrancheData { int64_t locktime; CAmount amount; };
    QList<TrancheData> tranches;
    for (int i = 0; i < m_trancheTable->rowCount(); i++) {
        auto* lockItem = m_trancheTable->item(i, 1);
        auto* amtItem  = m_trancheTable->item(i, 2);
        if (!lockItem || !amtItem) {
            QMessageBox::warning(this, tr("Vault"), tr("Row %1 is incomplete.").arg(i + 1));
            return;
        }
        bool lockOk = false;
        int64_t lt = lockItem->text().toLongLong(&lockOk);
        if (!lockOk || lt <= 0) {
            QMessageBox::warning(this, tr("Vault"), tr("Invalid lock value in row %1.").arg(i + 1));
            return;
        }
        bool amtOk = false;
        CAmount amt = parseAmount(amtItem->text(), amtOk);
        if (!amtOk || amt <= 0) {
            QMessageBox::warning(this, tr("Vault"), tr("Invalid amount in row %1.").arg(i + 1));
            return;
        }
        tranches << TrancheData{lt, amt};
    }

    if (!m_clientModel) {
        QMessageBox::critical(this, tr("Vault creation failed"), tr("No client model."));
        return;
    }
    const std::string wuri = "/wallet/" + (m_model ? m_model->wallet().getWalletName() : "");

    // Build all WSH outputs — one per tranche
    // Fetch pubkey once for all tranches from the active wallet
    std::string pubkeyHexVesting;

    // ── PQ vesting: build WITNESS_V2_MLDSA44_CLTV outputs directly ──────────
    CTxDestination vestingRecipientDest = DecodeDestination(recipient.toStdString());
    if (std::holds_alternative<WitnessV2MLDsa44>(vestingRecipientDest)) {
        const auto* pqDest = std::get_if<WitnessV2MLDsa44>(&vestingRecipientDest);
        uint256 pqHash;
        std::copy(pqDest->begin(), pqDest->end(), pqHash.begin());

        QList<QString> pqAddresses;
        for (const auto& t : tranches) {
            WitnessV2MLDsa44CLTV cltvDest{pqHash, t.locktime};
            pqAddresses << QString::fromStdString(EncodeDestination(cltvDest));
        }

        // ── Build OP_RETURN marker (all locktimes, PQ key hash) ──────────────
        std::vector<int64_t> allLocktimes;
        for (const auto& t : tranches) allLocktimes.push_back(t.locktime);
        std::vector<uint8_t> keyId(pqHash.begin(), pqHash.end());
        std::string opReturnHex = buildVaultOpReturnHex(keyId, allLocktimes, true, isTimestamp);

        // ── createrawtransaction (all vault outputs + OP_RETURN) ─────────────
        std::string rawTxHex;
        {
            UniValue txOutputs(UniValue::VARR);
            for (int i = 0; i < tranches.size(); i++) {
                UniValue out(UniValue::VOBJ);
                out.pushKV(pqAddresses[i].toStdString(), UniValue(UniValue::VNUM, FormatMoney(tranches[i].amount)));
                txOutputs.push_back(out);
            }
            UniValue opRetOut(UniValue::VOBJ);
            opRetOut.pushKV("data", opReturnHex);
            txOutputs.push_back(opRetOut);
            UniValue crParams(UniValue::VARR);
            crParams.push_back(UniValue(UniValue::VARR));
            crParams.push_back(txOutputs);
            try {
                rawTxHex = m_clientModel->node().executeRpc("createrawtransaction", crParams, "").get_str();
            } catch (const UniValue& e) {
                QMessageBox::critical(this, tr("Vault creation failed"),
                    tr("createrawtransaction failed: %1").arg(QString::fromStdString(
                        e.exists("message") ? e["message"].get_str() : e.write())));
                return;
            }
        }

        // ── fundrawtransaction ────────────────────────────────────────────────
        std::string fundedHex;
        {
            UniValue frParams(UniValue::VARR);
            frParams.push_back(rawTxHex);
            frParams.push_back(UniValue(UniValue::VOBJ));
            try {
                fundedHex = m_clientModel->node().executeRpc("fundrawtransaction", frParams, wuri)["hex"].get_str();
            } catch (const UniValue& e) {
                QMessageBox::critical(this, tr("Vault creation failed"),
                    tr("fundrawtransaction failed: %1").arg(QString::fromStdString(
                        e.exists("message") ? e["message"].get_str() : e.write())));
                return;
            }
        }

        // ── Find actual vout for each tranche ─────────────────────────────────
        QList<int> pqVouts;
        for (int i = 0; i < tranches.size(); i++) pqVouts << i; // defaults
        {
            try {
                UniValue decParams(UniValue::VARR);
                decParams.push_back(fundedHex);
                UniValue decoded = m_clientModel->node().executeRpc("decoderawtransaction", decParams, "");
                const UniValue& voutArr = decoded["vout"];
                for (int i = 0; i < tranches.size(); i++) {
                    for (size_t vi = 0; vi < voutArr.size(); vi++) {
                        const UniValue& spk = voutArr[vi]["scriptPubKey"];
                        if (spk.exists("address") &&
                            spk["address"].get_str() == pqAddresses[i].toStdString()) {
                            pqVouts[i] = static_cast<int>(vi);
                            break;
                        }
                    }
                }
            } catch (...) {}
        }

        // ── signrawtransactionwithwallet ──────────────────────────────────────
        std::string signedHex;
        {
            UniValue signParams(UniValue::VARR);
            signParams.push_back(fundedHex);
            try {
                UniValue sr = m_clientModel->node().executeRpc(
                    "signrawtransactionwithwallet", signParams, wuri);
                if (!sr.exists("complete") || !sr["complete"].get_bool()) {
                    QMessageBox::critical(this, tr("Vault creation failed"),
                        tr("Transaction signing incomplete (wallet locked?)."));
                    return;
                }
                signedHex = sr["hex"].get_str();
            } catch (const UniValue& e) {
                QMessageBox::critical(this, tr("Vault creation failed"),
                    tr("signrawtransactionwithwallet failed: %1").arg(QString::fromStdString(
                        e.exists("message") ? e["message"].get_str() : e.write())));
                return;
            }
        }

        // ── sendrawtransaction ────────────────────────────────────────────────
        QString pqTxid;
        {
            UniValue sendParams(UniValue::VARR);
            sendParams.push_back(signedHex);
            try {
                pqTxid = QString::fromStdString(
                    m_clientModel->node().executeRpc("sendrawtransaction", sendParams, "").get_str());
            } catch (const UniValue& e) {
                QMessageBox::critical(this, tr("Vault creation failed"),
                    tr("sendrawtransaction failed: %1").arg(QString::fromStdString(
                        e.exists("message") ? e["message"].get_str() : e.write())));
                return;
            }
        }

        QString pqVestingId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        for (int i = 0; i < tranches.size(); i++) {
            VaultRecord rec;
            rec.label            = label;
            rec.p2shAddress      = pqAddresses[i];
            rec.redeemScript     = QString();
            rec.descriptor       = QString();
            rec.recipientAddress = recipient;
            rec.locktime         = tranches[i].locktime;
            rec.isTimestamp      = isTimestamp;
            rec.amount           = tranches[i].amount;
            rec.txid             = pqTxid;
            rec.vout             = pqVouts[i];
            rec.isVesting        = true;
            rec.trancheIndex     = i;
            rec.vestingId        = pqVestingId;
            rec.isPQVault        = true;
            saveVaultRecord(rec);
        }
        QMessageBox::information(this, tr("Vault created"),
            tr("PQ vesting vault created — %1 tranche(s) in txid:\n%2")
                .arg(tranches.size()).arg(pqTxid));
        refresh();
        return;
    }

    {
        try {
            UniValue aiParams(UniValue::VARR);
            aiParams.push_back(recipient.toStdString());
            UniValue ai = m_clientModel->node().executeRpc("getaddressinfo", aiParams, wuri);
            if (ai.exists("pubkey"))
                pubkeyHexVesting = ai["pubkey"].get_str();
        } catch (...) {}

        if (pubkeyHexVesting.empty()) {
            QMessageBox::critical(this, tr("Vault creation failed"),
                tr("Cannot get public key for recipient address.\n"
                   "The address must be a P2PKH or P2WPKH address in the active wallet.\n"
                   "P2WSH / PQ / multisig addresses are not supported as vault recipients."));
            return;
        }
    }

    QList<QString> trancheDescriptors;
    QList<QString> trancheAddresses;
    QList<QString> trancheWitnessScripts;

    for (int ti = 0; ti < tranches.size(); ++ti) {
        const auto& t = tranches[ti];
        std::string rawDesc = "wsh(and_v(v:pk(" + pubkeyHexVesting + "),after(" + std::to_string(t.locktime) + ")))";
        std::string checksummedDesc;
        std::string vaultAddr;
        try {
            UniValue diParams(UniValue::VARR);
            diParams.push_back(rawDesc);
            UniValue di = m_clientModel->node().executeRpc("getdescriptorinfo", diParams, "");
            checksummedDesc = di["descriptor"].get_str();
            UniValue daParams(UniValue::VARR);
            daParams.push_back(checksummedDesc);
            UniValue addrs = m_clientModel->node().executeRpc("deriveaddresses", daParams, "");
            vaultAddr = addrs[0].get_str();
        } catch (const UniValue& e) {
            QMessageBox::critical(this, tr("Vault creation failed"),
                tr("Descriptor error (tranche %1): %2").arg(ti + 1)
                    .arg(QString::fromStdString(e.exists("message") ? e["message"].get_str() : e.write())));
            return;
        }
        std::vector<unsigned char> pubkeyBytes = ParseHex(pubkeyHexVesting);
        CScript ws = BuildCLTVWitnessScript(pubkeyBytes, t.locktime);

        trancheDescriptors << QString::fromStdString(checksummedDesc);
        trancheAddresses   << QString::fromStdString(vaultAddr);
        trancheWitnessScripts << QString::fromStdString(HexStr(ws));
    }

    // ── Import descriptors before creating transaction ────────────────────────
    for (int i = 0; i < tranches.size(); i++) {
        importVaultDescriptor(m_clientModel, trancheDescriptors[i].toStdString(),
                              label.toStdString(), wuri.substr(8)); // strip /wallet/
    }

    // ── Build OP_RETURN marker (all locktimes, HASH160 of pubkey) ─────────────
    {
        std::vector<unsigned char> pubkeyBytesV = ParseHex(pubkeyHexVesting);
        uint160 pkHash = Hash160(pubkeyBytesV);
        std::vector<uint8_t> keyIdV(pkHash.begin(), pkHash.end());
        std::vector<int64_t> allLt;
        for (const auto& t : tranches) allLt.push_back(t.locktime);
        std::string opRetHex = buildVaultOpReturnHex(keyIdV, allLt, false, isTimestamp);

        // ── createrawtransaction ──────────────────────────────────────────────
        std::string rawTxHex;
        {
            UniValue txOutputs(UniValue::VARR);
            for (int i = 0; i < tranches.size(); i++) {
                UniValue out(UniValue::VOBJ);
                out.pushKV(trancheAddresses[i].toStdString(), UniValue(UniValue::VNUM, FormatMoney(tranches[i].amount)));
                txOutputs.push_back(out);
            }
            UniValue opRetOut(UniValue::VOBJ);
            opRetOut.pushKV("data", opRetHex);
            txOutputs.push_back(opRetOut);
            UniValue crParams(UniValue::VARR);
            crParams.push_back(UniValue(UniValue::VARR));
            crParams.push_back(txOutputs);
            try {
                rawTxHex = m_clientModel->node().executeRpc("createrawtransaction", crParams, "").get_str();
            } catch (const UniValue& e) {
                QMessageBox::critical(this, tr("Vault creation failed"),
                    tr("createrawtransaction failed: %1").arg(QString::fromStdString(
                        e.exists("message") ? e["message"].get_str() : e.write())));
                return;
            }
        }

        // ── fundrawtransaction ────────────────────────────────────────────────
        std::string fundedHex;
        {
            UniValue frParams(UniValue::VARR);
            frParams.push_back(rawTxHex);
            frParams.push_back(UniValue(UniValue::VOBJ));
            try {
                fundedHex = m_clientModel->node().executeRpc("fundrawtransaction", frParams, wuri)["hex"].get_str();
            } catch (const UniValue& e) {
                QMessageBox::critical(this, tr("Vault creation failed"),
                    tr("fundrawtransaction failed: %1").arg(QString::fromStdString(
                        e.exists("message") ? e["message"].get_str() : e.write())));
                return;
            }
        }

        // ── Find actual vout for each tranche ─────────────────────────────────
        QList<int> trancheVouts;
        for (int i = 0; i < tranches.size(); i++) trancheVouts << i;
        {
            try {
                UniValue decParams(UniValue::VARR);
                decParams.push_back(fundedHex);
                UniValue decoded = m_clientModel->node().executeRpc("decoderawtransaction", decParams, "");
                const UniValue& voutArr = decoded["vout"];
                for (int i = 0; i < tranches.size(); i++) {
                    for (size_t vi = 0; vi < voutArr.size(); vi++) {
                        const UniValue& spk = voutArr[vi]["scriptPubKey"];
                        if (spk.exists("address") &&
                            spk["address"].get_str() == trancheAddresses[i].toStdString()) {
                            trancheVouts[i] = static_cast<int>(vi);
                            break;
                        }
                    }
                }
            } catch (...) {}
        }

        // ── signrawtransactionwithwallet ──────────────────────────────────────
        std::string signedHex;
        {
            UniValue signParams(UniValue::VARR);
            signParams.push_back(fundedHex);
            try {
                UniValue sr = m_clientModel->node().executeRpc(
                    "signrawtransactionwithwallet", signParams, wuri);
                if (!sr.exists("complete") || !sr["complete"].get_bool()) {
                    QMessageBox::critical(this, tr("Vault creation failed"),
                        tr("Transaction signing incomplete (wallet locked?)."));
                    return;
                }
                signedHex = sr["hex"].get_str();
            } catch (const UniValue& e) {
                QMessageBox::critical(this, tr("Vault creation failed"),
                    tr("signrawtransactionwithwallet failed: %1").arg(QString::fromStdString(
                        e.exists("message") ? e["message"].get_str() : e.write())));
                return;
            }
        }

        // ── sendrawtransaction ────────────────────────────────────────────────
        QString txid;
        {
            UniValue sendParams(UniValue::VARR);
            sendParams.push_back(signedHex);
            try {
                txid = QString::fromStdString(
                    m_clientModel->node().executeRpc("sendrawtransaction", sendParams, "").get_str());
            } catch (const UniValue& e) {
                QMessageBox::critical(this, tr("Vault creation failed"),
                    tr("sendrawtransaction failed: %1").arg(QString::fromStdString(
                        e.exists("message") ? e["message"].get_str() : e.write())));
                return;
            }
        }

        QString vestingId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        for (int i = 0; i < tranches.size(); i++) {
            VaultRecord rec;
            rec.label            = label;
            rec.p2shAddress      = trancheAddresses[i];
            rec.redeemScript     = trancheWitnessScripts[i];
            rec.descriptor       = trancheDescriptors[i];
            rec.recipientAddress = recipient;
            rec.locktime         = tranches[i].locktime;
            rec.isTimestamp      = isTimestamp;
            rec.amount           = tranches[i].amount;
            rec.txid             = txid;
            rec.vout             = trancheVouts[i];
            rec.isVesting        = true;
            rec.trancheIndex     = i;
            rec.vestingId        = vestingId;
            saveVaultRecord(rec);
        }

        QMessageBox::information(this, tr("Vault created"),
            tr("Vesting vault created — %1 tranche(s) in txid:\n%2")
                .arg(tranches.size()).arg(txid));
        refresh();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Slots — My Vaults tab
// ─────────────────────────────────────────────────────────────────────────────

void VaultDialog::onRefreshVaults()
{
    loadVaultRecords();
    refreshVaultsTable();
}

void VaultDialog::onRenameVault()
{
    const int row = m_vaultsTable->currentRow();
    if (row < 0 || row >= m_vaults.size()) return;

    bool ok = false;
    const QString newLabel = QInputDialog::getText(
        this, tr("Rename vault"),
        tr("New label:"),
        QLineEdit::Normal,
        m_vaults[row].label,
        &ok);

    if (!ok || newLabel.trimmed().isEmpty()) return;

    // Reload to ensure m_vaults is current, then update the matching record by txid+vout
    const QString txid = m_vaults[row].txid;
    const int     vout = m_vaults[row].vout;
    loadVaultRecords();

    for (VaultRecord& r : m_vaults) {
        if (r.txid == txid && r.vout == vout) {
            r.label = newLabel.trimmed();
            break;
        }
    }

    // Persist all records
    QJsonArray arr;
    for (const auto& r : m_vaults) {
        QJsonObject obj;
        obj[QStringLiteral("label")]            = r.label;
        obj[QStringLiteral("p2shAddress")]       = r.p2shAddress;
        obj[QStringLiteral("redeemScript")]      = r.redeemScript;
        obj[QStringLiteral("descriptor")]        = r.descriptor;
        obj[QStringLiteral("recipientAddress")]  = r.recipientAddress;
        obj[QStringLiteral("locktime")]          = static_cast<qint64>(r.locktime);
        obj[QStringLiteral("isTimestamp")]       = r.isTimestamp;
        obj[QStringLiteral("amount")]            = static_cast<qint64>(r.amount);
        obj[QStringLiteral("txid")]              = r.txid;
        obj[QStringLiteral("vout")]              = r.vout;
        obj[QStringLiteral("isVesting")]         = r.isVesting;
        obj[QStringLiteral("trancheIndex")]      = r.trancheIndex;
        obj[QStringLiteral("vestingId")]         = r.vestingId;
        obj[QStringLiteral("isPQVault")]         = r.isPQVault;
        arr.append(obj);
    }
    QFile file(vaultStoragePath());
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        file.write(QJsonDocument(arr).toJson());

    refreshVaultsTable();
}

void VaultDialog::onScanForVaults()
{
    if (!m_clientModel || !m_model) {
        QMessageBox::warning(this, tr("Scan for Vaults"), tr("Wallet not available."));
        return;
    }

    const std::string walletUri = "/wallet/" + m_model->wallet().getWalletName();

    // ── Step 1: collect all wallet txids via listtransactions ────────────────
    std::set<std::string> txids;
    try {
        UniValue p(UniValue::VARR);
        p.push_back(std::string("*")); // all labels/categories
        p.push_back(99999);            // count
        p.push_back(0);                // skip
        p.push_back(true);             // include_watchonly
        UniValue list = m_clientModel->node().executeRpc("listtransactions", p, walletUri);
        if (list.isArray()) {
            for (size_t i = 0; i < list.size(); ++i)
                if (list[i].exists("txid"))
                    txids.insert(list[i]["txid"].get_str());
        }
    } catch (...) {
        QMessageBox::critical(this, tr("Scan for Vaults"),
            tr("Failed to retrieve transaction list from wallet."));
        return;
    }

    if (txids.empty()) {
        QMessageBox::information(this, tr("Scan for Vaults"),
            tr("No transactions found in wallet."));
        return;
    }

    // ── Step 2: build set of already-known vault keys (txid:vout) ────────────
    loadVaultRecords();
    std::set<std::string> knownKeys;
    for (const VaultRecord& r : m_vaults)
        knownKeys.insert(r.txid.toStdString() + ":" + std::to_string(r.vout));

    // ── Step 3: helper — extract AVAN payload from OP_RETURN scriptPubKey hex ─
    // Script: 6a <push> <data> where push is 1 byte (<=0x4b) or 4c <len> (PUSHDATA1)
    auto extractAvan = [](const std::string& spkHex) -> std::vector<uint8_t> {
        if (spkHex.size() < 4 || spkHex.substr(0, 2) != "6a") return {};
        uint8_t pushOp = static_cast<uint8_t>(std::stoul(spkHex.substr(2, 2), nullptr, 16));
        std::string dataHex;
        if (pushOp <= 0x4b)       dataHex = spkHex.substr(4); // 6a <len> <data>
        else if (pushOp == 0x4c)  dataHex = spkHex.substr(6); // 6a 4c <len> <data>
        else return {};
        if (dataHex.size() < 8 || dataHex.substr(0, 8) != "4156414e") return {}; // "AVAN"
        return ParseHex(dataHex);
    };

    // ── Step 4: scan each transaction ────────────────────────────────────────
    int found = 0;
    for (const std::string& txid : txids) {
        // Get raw tx hex
        std::string rawHex;
        try {
            UniValue p(UniValue::VARR);
            p.push_back(txid);
            UniValue gt = m_clientModel->node().executeRpc("gettransaction", p, walletUri);
            if (!gt.exists("hex")) continue;
            rawHex = gt["hex"].get_str();
        } catch (...) { continue; }

        // Decode the transaction
        UniValue decoded;
        try {
            UniValue p(UniValue::VARR);
            p.push_back(rawHex);
            decoded = m_clientModel->node().executeRpc("decoderawtransaction", p, "");
        } catch (...) { continue; }

        if (!decoded.exists("vout") || !decoded["vout"].isArray()) continue;
        const UniValue& vouts = decoded["vout"];

        // Find AVAN OP_RETURN marker
        std::vector<uint8_t> avanPayload;
        for (size_t vi = 0; vi < vouts.size(); ++vi) {
            const UniValue& v = vouts[vi];
            if (!v.exists("scriptPubKey")) continue;
            const UniValue& spk = v["scriptPubKey"];
            if (!spk.exists("type") || spk["type"].get_str() != "nulldata") continue;
            if (!spk.exists("hex")) continue;
            avanPayload = extractAvan(spk["hex"].get_str());
            if (!avanPayload.empty()) break;
        }
        if (avanPayload.empty()) continue;

        // Parse AVAN payload: AVAN(4) | ver(1) | flags(1) | locktimes(8B each) | keyId
        if (avanPayload.size() < 6 || avanPayload[4] != 0x01) continue;
        const uint8_t flags    = avanPayload[5];
        const int  numTranches = (flags & 0x0F) + 1;
        const bool isPQ        = (flags & 0x10) != 0;
        const bool isTimestamp = (flags & 0x20) != 0;
        const size_t keyIdLen  = isPQ ? 32 : 20;
        if (avanPayload.size() < 6 + static_cast<size_t>(numTranches) * 8 + keyIdLen) continue;

        std::vector<int64_t> locktimes;
        locktimes.reserve(numTranches);
        for (int t = 0; t < numTranches; ++t) {
            int64_t lt = 0;
            for (int b = 0; b < 8; ++b)
                lt |= static_cast<int64_t>(avanPayload[6 + t * 8 + b]) << (8 * b);
            locktimes.push_back(lt);
        }

        // ── For non-PQ vaults: recover pubkey from HASH160 keyId in OP_RETURN ──
        // The keyId is HASH160(pubkey). We derive the P2WPKH and P2PKH addresses
        // from that HASH160 and call getaddressinfo on each to retrieve the pubkey
        // — allowing us to fully reconstruct the witnessScript and descriptor.
        std::string recoveredPubkey;
        std::string recoveredRecipient;
        if (!isPQ) {
            const size_t keyIdOffset = 6 + static_cast<size_t>(numTranches) * 8;
            if (avanPayload.size() >= keyIdOffset + 20) {
                uint160 keyID;
                std::copy(avanPayload.begin() + keyIdOffset,
                          avanPayload.begin() + keyIdOffset + 20,
                          keyID.begin());
                // Try P2WPKH first (bech32), then P2PKH (base58)
                const std::vector<std::string> candidates = {
                    EncodeDestination(WitnessV0KeyHash{keyID}),
                    EncodeDestination(PKHash{keyID})
                };
                for (const std::string& candidate : candidates) {
                    if (!recoveredPubkey.empty()) break;
                    try {
                        UniValue p(UniValue::VARR);
                        p.push_back(candidate);
                        UniValue ai = m_clientModel->node().executeRpc("getaddressinfo", p, walletUri);
                        if (ai.exists("pubkey")) {
                            const std::string pk = ai["pubkey"].get_str();
                            if (!pk.empty()) {
                                recoveredPubkey    = pk;
                                recoveredRecipient = candidate;
                            }
                        }
                    } catch (...) {}
                }
            }
        }

        // Walk outputs to find vault outputs
        // Non-PQ vaults are P2WSH (witness_v0_scripthash).
        // PQ vaults are WITNESS_V2 — may appear as "witness_v2", "witness_unknown",
        // or "nonstandard" depending on node version.
        // Change typically goes to P2PKH / P2WPKH. Exclude those + OP_RETURN.
        static const std::set<std::string> kSkipTypes = {
            "nulldata", "pubkeyhash", "witness_v0_keyhash"
        };

        int vaultOutputIdx = 0; // ordinal of vault outputs encountered in this tx
        for (size_t vi = 0; vi < vouts.size(); ++vi) {
            const UniValue& v = vouts[vi];
            if (!v.exists("scriptPubKey") || !v.exists("value") || !v.exists("n")) continue;
            const UniValue& spk = v["scriptPubKey"];
            if (!spk.exists("type")) continue;
            const std::string type = spk["type"].get_str();

            bool isVaultCandidate = false;
            if (!isPQ)
                isVaultCandidate = (type == "witness_v0_scripthash");
            else
                isVaultCandidate = (kSkipTypes.find(type) == kSkipTypes.end());

            if (!isVaultCandidate) continue;

            // Need an address to record
            std::string addr;
            if (spk.exists("address"))
                addr = spk["address"].get_str();
            else if (spk.exists("addresses") && spk["addresses"].isArray() && spk["addresses"].size() > 0)
                addr = spk["addresses"][0].get_str();
            if (addr.empty()) continue;

            const int voutIdx = v["n"].getInt<int>();
            const std::string key = txid + ":" + std::to_string(voutIdx);

            if (knownKeys.count(key)) { ++vaultOutputIdx; continue; }

            // Assign locktime by output order within this transaction
            const int tidx = vaultOutputIdx;
            const int64_t lt = (tidx < (int)locktimes.size()) ? locktimes[tidx] : locktimes.back();
            const CAmount amt = static_cast<CAmount>(v["value"].get_real() * COIN);

            // ── Reconstruct script + descriptor from recovered pubkey ──────────
            std::string descriptor, redeemScript, recipientAddress;
            if (!recoveredPubkey.empty()) {
                recipientAddress = recoveredRecipient;
                const std::vector<uint8_t> pubkeyBytes = ParseHex(recoveredPubkey);
                const CScript ws = BuildCLTVWitnessScript(pubkeyBytes, lt);
                redeemScript = HexStr(ws);
                // Build descriptor and get its checksum via getdescriptorinfo
                const std::string descRaw =
                    "wsh(and_v(v:pk(" + recoveredPubkey + "),after(" + std::to_string(lt) + ")))";
                try {
                    UniValue p(UniValue::VARR);
                    p.push_back(descRaw);
                    UniValue di = m_clientModel->node().executeRpc("getdescriptorinfo", p, "");
                    descriptor = di.exists("descriptor") ? di["descriptor"].get_str() : descRaw;
                } catch (...) { descriptor = descRaw; }
            } else {
                // Fallback: ask getaddressinfo on the vault P2WSH address itself
                try {
                    UniValue p(UniValue::VARR);
                    p.push_back(addr);
                    UniValue ai = m_clientModel->node().executeRpc("getaddressinfo", p, walletUri);
                    if (ai.exists("desc"))          descriptor   = ai["desc"].get_str();
                    if (ai.exists("witnessScript")) redeemScript = ai["witnessScript"].get_str();
                } catch (...) {}
            }

            VaultRecord rec;
            rec.txid             = QString::fromStdString(txid);
            rec.vout             = voutIdx;
            rec.p2shAddress      = QString::fromStdString(addr);
            rec.amount           = amt;
            rec.isPQVault        = isPQ;
            rec.isTimestamp      = isTimestamp;
            rec.locktime         = lt;
            rec.descriptor       = QString::fromStdString(descriptor);
            rec.redeemScript     = QString::fromStdString(redeemScript);
            rec.recipientAddress = QString::fromStdString(recipientAddress);
            rec.label            = tr("Recovered %1").arg(QString::fromStdString(txid.substr(0, 8)));
            rec.isVesting        = (numTranches > 1);
            rec.trancheIndex     = (numTranches > 1) ? tidx : -1;
            rec.vestingId        = (numTranches > 1) ? QString::fromStdString(txid) : QString();

            saveVaultRecord(rec);
            knownKeys.insert(key);
            ++found;
            ++vaultOutputIdx;
        }
    }

    loadVaultRecords();
    refreshVaultsTable();

    QMessageBox::information(this, tr("Scan for Vaults"),
        found > 0
            ? tr("Recovered %1 new vault(s) from transaction history.").arg(found)
            : tr("No new vaults found in wallet transaction history."));
}

void VaultDialog::onCopyAddress()
{
    int row = m_vaultsTable->currentRow();
    if (row < 0 || row >= m_vaults.size()) return;
    QApplication::clipboard()->setText(m_vaults[row].p2shAddress);
}

void VaultDialog::onCopyRedeemScript()
{
    int row = m_vaultsTable->currentRow();
    if (row < 0 || row >= m_vaults.size()) return;
    const VaultRecord& rec = m_vaults[row];
    if (rec.isPQVault) {
        QMessageBox::information(this, tr("Vault"),
            tr("PQ vaults are self-describing.\nThe vault address encodes all spending conditions — no separate redeem script is needed."));
        return;
    }
    QApplication::clipboard()->setText(rec.redeemScript);
}

void VaultDialog::onReimportScripts()
{
    loadVaultRecords();
    int count = 0;
    const std::string walletName = m_model ? m_model->wallet().getWalletName() : "";
    for (const VaultRecord& rec : m_vaults) {
        if (!rec.descriptor.isEmpty()) {
            importVaultDescriptor(m_clientModel, rec.descriptor.toStdString(),
                                  rec.label.toStdString(), walletName);
            ++count;
        }
    }
    QMessageBox::information(this, tr("Re-import scripts"),
        tr("Re-imported %1 vault descriptor(s) into the wallet.\n"
           "Run getaddressinfo on your vault addresses to verify ismine/solvable.").arg(count));
}

void VaultDialog::onSpendVault()
{
    int row = m_vaultsTable->currentRow();
    if (row < 0 || row >= m_vaults.size()) return;
    const VaultRecord& rec = m_vaults[row];

    bool unlocked = rec.isTimestamp
        ? (currentUnixTime()    >= rec.locktime)
        : (currentBlockHeight() >= static_cast<int>(rec.locktime));

    if (!unlocked) {
        QMessageBox::warning(this, tr("Vault"), tr("This vault is still locked."));
        return;
    }
    if (!m_clientModel) {
        QMessageBox::warning(this, tr("Vault"), tr("No client model available."));
        return;
    }
    if (!rec.isPQVault && rec.redeemScript.isEmpty()) {
        QMessageBox::warning(this, tr("Vault"), tr("No redeem script stored for this vault."));
        return;
    }

    // Wallet URI — required when multiple wallets are loaded (for wallet-specific RPCs)
    const std::string walletUri = "/wallet/" + (m_model ? m_model->wallet().getWalletName() : "");

    // ── Step 1: find UTXOs via scantxoutset (works regardless of ismine/watchonly status) ──
    UniValue scanDescs(UniValue::VARR);
    UniValue descObj(UniValue::VOBJ);
    descObj.pushKV("desc", "addr(" + rec.p2shAddress.toStdString() + ")");
    scanDescs.push_back(descObj);

    UniValue scanParams(UniValue::VARR);
    scanParams.push_back(std::string("start"));
    scanParams.push_back(scanDescs);

    UniValue scanResult;
    try {
        scanResult = m_clientModel->node().executeRpc("scantxoutset", scanParams, "");
    } catch (const UniValue& e) {
        QMessageBox::critical(this, tr("Spend vault"),
            tr("Failed to scan UTXOs: %1").arg(QString::fromStdString(
                e.exists("message") ? e["message"].get_str() : e.write())));
        return;
    } catch (const std::exception& e) {
        QMessageBox::critical(this, tr("Spend vault"),
            tr("Failed to scan UTXOs: %1").arg(e.what()));
        return;
    }

    const bool scanOk = scanResult.exists("success") && scanResult["success"].get_bool();
    const bool hasUtxos = scanResult.exists("unspents") && !scanResult["unspents"].empty();
    if (!scanOk || !hasUtxos) {
        QMessageBox::warning(this, tr("Spend vault"),
            tr("No spendable outputs found at:\n%1\n\n"
               "The transaction may be unconfirmed, or the vault has already been spent.")
                .arg(rec.p2shAddress));
        return;
    }
    const UniValue& utxos = scanResult["unspents"];

    // Sum all UTXOs
    CAmount totalSats = 0;
    for (size_t i = 0; i < utxos.size(); i++)
        totalSats += static_cast<CAmount>(std::round(utxos[i]["amount"].get_real() * COIN));

    // ── Step 2: ask for destination address ───────────────────────────────────
    bool ok = false;
    QString destAddr = QInputDialog::getText(this, tr("Spend vault"),
        tr("Destination address\n\nAvailable: %1 AVN (fee ~0.001 AVN will be deducted):")
            .arg(BitcoinUnits::format(BitcoinUnits::Unit::BTC, totalSats, false,
                                     BitcoinUnits::SeparatorStyle::ALWAYS)),
        QLineEdit::Normal, rec.recipientAddress, &ok);

    if (!ok || destAddr.trimmed().isEmpty()) return;
    destAddr = destAddr.trimmed();

    if (!IsValidDestination(DecodeDestination(destAddr.toStdString()))) {
        QMessageBox::warning(this, tr("Spend vault"), tr("Invalid destination address."));
        return;
    }

    const CAmount fee      = 100000; // 0.001 AVN
    const CAmount sendAmt  = totalSats - fee;
    if (sendAmt <= 0) {
        QMessageBox::warning(this, tr("Spend vault"), tr("Amount too small to cover the fee."));
        return;
    }

    // ── Step 3: build inputs / prevouts arrays ────────────────────────────────
    // prevouts includes redeemScript so signrawtransactionwithwallet can sign
    // even before importaddress has been run (redeemScript in prevouts is enough).
    UniValue inputs(UniValue::VARR);
    UniValue prevouts(UniValue::VARR);
    for (size_t i = 0; i < utxos.size(); i++) {
        const UniValue& u = utxos[i];
        UniValue inp(UniValue::VOBJ);
        inp.pushKV("txid",     u["txid"].get_str());
        inp.pushKV("vout",     u["vout"].getInt<int>());
        inp.pushKV("sequence", int64_t{4294967294}); // 0xFFFFFFFE — required by CLTV
        inputs.push_back(inp);

        UniValue prev(UniValue::VOBJ);
        prev.pushKV("txid",          u["txid"].get_str());
        prev.pushKV("vout",          u["vout"].getInt<int>());
        prev.pushKV("scriptPubKey",  u["scriptPubKey"].get_str());
        // redeemScript field stores the witnessScript hex for WSH vaults
        if (!rec.redeemScript.isEmpty())
            prev.pushKV("witnessScript", rec.redeemScript.toStdString());
        prev.pushKV("amount",        u["amount"]);
        prevouts.push_back(prev);
    }

    UniValue outputs(UniValue::VOBJ);
    // Use FormatMoney to avoid floating-point precision issues with createrawtransaction
    outputs.pushKV(destAddr.toStdString(),
                   UniValue(UniValue::VNUM, FormatMoney(sendAmt)));

    // ── Step 4: createrawtransaction ─────────────────────────────────────────
    UniValue createParams(UniValue::VARR);
    createParams.push_back(inputs);
    createParams.push_back(outputs);
    createParams.push_back(int64_t{rec.locktime}); // nLockTime must be >= locktime

    UniValue rawTx;
    try {
        rawTx = m_clientModel->node().executeRpc("createrawtransaction", createParams, "");
    } catch (const UniValue& e) {
        QMessageBox::critical(this, tr("Spend vault"),
            tr("createrawtransaction failed:\n%1").arg(QString::fromStdString(
                e.exists("message") ? e["message"].get_str() : e.write())));
        return;
    } catch (const std::exception& e) {
        QMessageBox::critical(this, tr("Spend vault"),
            tr("createrawtransaction failed:\n%1").arg(e.what()));
        return;
    }

    // ── Step 5: signrawtransactionwithwallet (try every loaded wallet) ──────────
    // The key may live in a different wallet than the currently active one,
    // especially if the vault was created before the wallet URI was passed correctly.
    // Fetch all loaded wallets via listwallets and try each in turn.
    UniValue walletList(UniValue::VARR);
    try {
        UniValue lw(UniValue::VARR);
        walletList = m_clientModel->node().executeRpc("listwallets", lw, "");
    } catch (...) {
        // fallback: try only the currently active wallet
        if (m_model) walletList.push_back(m_model->wallet().getWalletName());
    }
    // Always ensure the active wallet is attempted first
    if (m_model) {
        const std::string activeName = m_model->wallet().getWalletName();
        bool found = false;
        for (size_t wi = 0; wi < walletList.size(); ++wi)
            if (walletList[wi].get_str() == activeName) { found = true; break; }
        if (!found) walletList.push_back(activeName);
    }

    UniValue signResult;
    bool signingComplete = false;
    std::string lastSignError;
    for (size_t wi = 0; wi < walletList.size() && !signingComplete; ++wi) {
        const std::string wuri = "/wallet/" + walletList[wi].get_str();
        UniValue signParams(UniValue::VARR);
        signParams.push_back(rawTx.get_str());
        signParams.push_back(prevouts);
        try {
            UniValue res = m_clientModel->node().executeRpc(
                "signrawtransactionwithwallet", signParams, wuri);
            if (res.exists("complete") && res["complete"].get_bool()) {
                signResult = res;
                signingComplete = true;
            } else {
                signResult = res;
                if (res.exists("errors") && res["errors"].size() > 0)
                    lastSignError = res["errors"][0]["error"].get_str();
            }
        } catch (const UniValue& e) {
            lastSignError = e.exists("message") ? e["message"].get_str() : e.write();
        } catch (const std::exception& e) {
            lastSignError = e.what();
        }
    }

    if (!signingComplete) {
        QMessageBox::critical(this, tr("Spend vault"),
            tr("Signing incomplete — make sure the wallet holding the key for:\n%1\nis unlocked.\n\nErrors:\n%2")
                .arg(rec.recipientAddress)
                .arg(QString::fromStdString(lastSignError)));
        return;
    }

    // ── Step 6: sendrawtransaction ────────────────────────────────────────────
    UniValue sendParams(UniValue::VARR);
    sendParams.push_back(signResult["hex"].get_str());

    UniValue txidVal;
    try {
        txidVal = m_clientModel->node().executeRpc("sendrawtransaction", sendParams, "");
    } catch (const UniValue& e) {
        QMessageBox::critical(this, tr("Spend vault"),
            tr("sendrawtransaction failed:\n%1").arg(QString::fromStdString(
                e.exists("message") ? e["message"].get_str() : e.write())));
        return;
    } catch (const std::exception& e) {
        QMessageBox::critical(this, tr("Spend vault"),
            tr("sendrawtransaction failed:\n%1").arg(e.what()));
        return;
    }

    QMessageBox::information(this, tr("Vault spent!"),
        tr("Vault spent successfully!\n\n"
           "Txid: %1\n\nSent: %2 AVN\nFee:  %3 AVN")
            .arg(QString::fromStdString(txidVal.get_str()))
            .arg(BitcoinUnits::format(BitcoinUnits::Unit::BTC, sendAmt, false,
                                     BitcoinUnits::SeparatorStyle::ALWAYS))
            .arg(BitcoinUnits::format(BitcoinUnits::Unit::BTC, fee, false,
                                     BitcoinUnits::SeparatorStyle::ALWAYS)));
    refresh();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Core vault creation
// ─────────────────────────────────────────────────────────────────────────────

bool VaultDialog::buildAndSendSingleVault(
    const QString& label,
    const QString& recipient,
    int64_t locktime,
    bool isTimestamp,
    CAmount amount,
    QString& outP2shAddr,
    QString& outRedeemScript,
    QString& outDescriptor,
    QString& outTxid,
    int& outVout,
    QString& outError)
{
    if (!m_clientModel) {
        outError = tr("No client model available.");
        return false;
    }
    const std::string wuri = "/wallet/" + (m_model ? m_model->wallet().getWalletName() : "");

    // ── Step 1: get the compressed pubkey for the recipient address ──────────
    std::string pubkeyHex;
    {
        try {
            UniValue params(UniValue::VARR);
            params.push_back(recipient.toStdString());
            UniValue ai = m_clientModel->node().executeRpc("getaddressinfo", params, wuri);
            if (ai.exists("pubkey"))
                pubkeyHex = ai["pubkey"].get_str();
        } catch (...) {}

        if (pubkeyHex.empty()) {
            outError = tr("Cannot retrieve pubkey for:\n%1\n\n"
                          "The address must be a P2PKH or P2WPKH address in the active wallet.\n"
                          "P2WSH / PQ / multisig addresses are not supported as vault recipients.")
                          .arg(recipient);
            return false;
        }
    }

    // ── Step 2: build and verify descriptor ─────────────────────────────────
    //  wsh(and_v(v:pk(PUBKEY),after(LOCKTIME)))
    const std::string rawDesc = "wsh(and_v(v:pk(" + pubkeyHex + "),after(" +
                                 std::to_string(locktime) + ")))";
    std::string checksummedDesc;
    try {
        UniValue params(UniValue::VARR);
        params.push_back(rawDesc);
        UniValue di = m_clientModel->node().executeRpc("getdescriptorinfo", params, "");
        checksummedDesc = di["descriptor"].get_str();
    } catch (const UniValue& e) {
        outError = tr("getdescriptorinfo failed: %1").arg(QString::fromStdString(
            e.exists("message") ? e["message"].get_str() : e.write()));
        return false;
    }

    // ── Step 3: derive vault address ─────────────────────────────────────────
    try {
        UniValue params(UniValue::VARR);
        params.push_back(checksummedDesc);
        UniValue addrs = m_clientModel->node().executeRpc("deriveaddresses", params, "");
        outP2shAddr = QString::fromStdString(addrs[0].get_str());
    } catch (const UniValue& e) {
        outError = tr("deriveaddresses failed: %1").arg(QString::fromStdString(
            e.exists("message") ? e["message"].get_str() : e.write()));
        return false;
    }

    // ── Step 4: build witnessScript (stored in redeemScript field) ───────────
    //  and_v(v:pk(KEY),after(N)) compiles to: <KEY> OP_CHECKSIGVERIFY <N> OP_CLTV
    std::vector<unsigned char> pubkeyBytes = ParseHex(pubkeyHex);
    CScript witnessScript = BuildCLTVWitnessScript(pubkeyBytes, locktime);
    outRedeemScript = QString::fromStdString(HexStr(witnessScript));
    outDescriptor   = QString::fromStdString(checksummedDesc);

    // ── Step 5: import descriptor into wallet ────────────────────────────────
    importVaultDescriptor(m_clientModel, checksummedDesc, label.toStdString(),
                          m_model ? m_model->wallet().getWalletName() : "");

    // ── Step 6: build OP_RETURN vault marker ─────────────────────────────────
    uint160 pubkeyHash = Hash160(pubkeyBytes);
    std::vector<uint8_t> keyId(pubkeyHash.begin(), pubkeyHash.end());
    std::string opReturnHex = buildVaultOpReturnHex(keyId, {locktime}, false, isTimestamp);

    // ── Step 7: createrawtransaction (vault output + OP_RETURN) ──────────────
    std::string rawTxHex;
    {
        UniValue outputs(UniValue::VOBJ);
        outputs.pushKV(outP2shAddr.toStdString(), UniValue(UniValue::VNUM, FormatMoney(amount)));
        outputs.pushKV("data", opReturnHex);
        UniValue crParams(UniValue::VARR);
        crParams.push_back(UniValue(UniValue::VARR)); // empty inputs — fundrawtransaction fills these
        crParams.push_back(outputs);
        try {
            rawTxHex = m_clientModel->node().executeRpc("createrawtransaction", crParams, "").get_str();
        } catch (const UniValue& e) {
            outError = tr("createrawtransaction failed: %1").arg(QString::fromStdString(
                e.exists("message") ? e["message"].get_str() : e.write()));
            return false;
        }
    }

    // ── Step 8: fundrawtransaction (select inputs + add change) ──────────────
    std::string fundedHex;
    {
        UniValue frParams(UniValue::VARR);
        frParams.push_back(rawTxHex);
        frParams.push_back(UniValue(UniValue::VOBJ)); // default options
        try {
            UniValue funded = m_clientModel->node().executeRpc("fundrawtransaction", frParams, wuri);
            fundedHex = funded["hex"].get_str();
        } catch (const UniValue& e) {
            outError = tr("fundrawtransaction failed: %1").arg(QString::fromStdString(
                e.exists("message") ? e["message"].get_str() : e.write()));
            return false;
        }
    }

    // ── Step 9: find actual vault vout (change position may vary) ────────────
    outVout = 0;
    {
        try {
            UniValue decParams(UniValue::VARR);
            decParams.push_back(fundedHex);
            UniValue decoded = m_clientModel->node().executeRpc("decoderawtransaction", decParams, "");
            const UniValue& vouts = decoded["vout"];
            for (size_t i = 0; i < vouts.size(); i++) {
                const UniValue& spk = vouts[i]["scriptPubKey"];
                if (spk.exists("address") &&
                    spk["address"].get_str() == outP2shAddr.toStdString()) {
                    outVout = static_cast<int>(i);
                    break;
                }
            }
        } catch (...) {}
    }

    // ── Step 10: sign ────────────────────────────────────────────────────────
    std::string signedHex;
    {
        UniValue signParams(UniValue::VARR);
        signParams.push_back(fundedHex);
        try {
            UniValue sr = m_clientModel->node().executeRpc(
                "signrawtransactionwithwallet", signParams, wuri);
            if (!sr.exists("complete") || !sr["complete"].get_bool()) {
                outError = tr("Transaction signing incomplete (wallet locked?).");
                return false;
            }
            signedHex = sr["hex"].get_str();
        } catch (const UniValue& e) {
            outError = tr("signrawtransactionwithwallet failed: %1").arg(QString::fromStdString(
                e.exists("message") ? e["message"].get_str() : e.write()));
            return false;
        }
    }

    // ── Step 11: broadcast ───────────────────────────────────────────────────
    {
        UniValue sendParams(UniValue::VARR);
        sendParams.push_back(signedHex);
        try {
            outTxid = QString::fromStdString(
                m_clientModel->node().executeRpc("sendrawtransaction", sendParams, "").get_str());
        } catch (const UniValue& e) {
            outError = tr("sendrawtransaction failed: %1").arg(QString::fromStdString(
                e.exists("message") ? e["message"].get_str() : e.write()));
            return false;
        }
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  PQ vault creation (WITNESS_V2_MLDSA44_CLTV)
// ─────────────────────────────────────────────────────────────────────────────

bool VaultDialog::buildAndSendPQVault(
    const QString& label,
    const QString& recipient,
    int64_t locktime,
    bool isTimestamp,
    CAmount amount,
    QString& outVaultAddr,
    QString& outTxid,
    int& outVout,
    QString& outError)
{
    if (!m_clientModel) {
        outError = tr("No wallet loaded.");
        return false;
    }
    const std::string wuri = "/wallet/" + (m_model ? m_model->wallet().getWalletName() : "");

    // Decode the recipient PQ address → 32-byte pubkey hash.
    CTxDestination recipientDest = DecodeDestination(recipient.toStdString());
    const auto* pqDest = std::get_if<WitnessV2MLDsa44>(&recipientDest);
    if (!pqDest) {
        outError = tr("Expected a ML-DSA-44 (witness v2) address for PQ vault.");
        return false;
    }

    uint256 pqHash;
    std::copy(pqDest->begin(), pqDest->end(), pqHash.begin());
    WitnessV2MLDsa44CLTV cltvDest{pqHash, locktime};
    outVaultAddr = QString::fromStdString(EncodeDestination(cltvDest));

    // ── Build OP_RETURN vault marker ─────────────────────────────────────────
    std::vector<uint8_t> keyId(pqHash.begin(), pqHash.end());
    std::string opReturnHex = buildVaultOpReturnHex(keyId, {locktime}, true, isTimestamp);

    // ── createrawtransaction (vault output + OP_RETURN) ──────────────────────
    std::string rawTxHex;
    {
        UniValue outputs(UniValue::VOBJ);
        outputs.pushKV(outVaultAddr.toStdString(), UniValue(UniValue::VNUM, FormatMoney(amount)));
        outputs.pushKV("data", opReturnHex);
        UniValue crParams(UniValue::VARR);
        crParams.push_back(UniValue(UniValue::VARR)); // empty inputs
        crParams.push_back(outputs);
        try {
            rawTxHex = m_clientModel->node().executeRpc("createrawtransaction", crParams, "").get_str();
        } catch (const UniValue& e) {
            outError = tr("createrawtransaction failed: %1").arg(QString::fromStdString(
                e.exists("message") ? e["message"].get_str() : e.write()));
            return false;
        }
    }

    // ── fundrawtransaction ────────────────────────────────────────────────────
    std::string fundedHex;
    {
        UniValue frParams(UniValue::VARR);
        frParams.push_back(rawTxHex);
        frParams.push_back(UniValue(UniValue::VOBJ));
        try {
            UniValue funded = m_clientModel->node().executeRpc("fundrawtransaction", frParams, wuri);
            fundedHex = funded["hex"].get_str();
        } catch (const UniValue& e) {
            outError = tr("fundrawtransaction failed: %1").arg(QString::fromStdString(
                e.exists("message") ? e["message"].get_str() : e.write()));
            return false;
        }
    }

    // ── Find actual vault vout index ──────────────────────────────────────────
    outVout = 0;
    {
        try {
            UniValue decParams(UniValue::VARR);
            decParams.push_back(fundedHex);
            UniValue decoded = m_clientModel->node().executeRpc("decoderawtransaction", decParams, "");
            const UniValue& vouts = decoded["vout"];
            for (size_t i = 0; i < vouts.size(); i++) {
                const UniValue& spk = vouts[i]["scriptPubKey"];
                if (spk.exists("address") &&
                    spk["address"].get_str() == outVaultAddr.toStdString()) {
                    outVout = static_cast<int>(i);
                    break;
                }
            }
        } catch (...) {}
    }

    // ── signrawtransactionwithwallet ──────────────────────────────────────────
    std::string signedHex;
    {
        UniValue signParams(UniValue::VARR);
        signParams.push_back(fundedHex);
        try {
            UniValue sr = m_clientModel->node().executeRpc(
                "signrawtransactionwithwallet", signParams, wuri);
            if (!sr.exists("complete") || !sr["complete"].get_bool()) {
                outError = tr("Transaction signing incomplete (wallet locked?).");
                return false;
            }
            signedHex = sr["hex"].get_str();
        } catch (const UniValue& e) {
            outError = tr("signrawtransactionwithwallet failed: %1").arg(QString::fromStdString(
                e.exists("message") ? e["message"].get_str() : e.write()));
            return false;
        }
    }

    // ── sendrawtransaction ────────────────────────────────────────────────────
    {
        UniValue sendParams(UniValue::VARR);
        sendParams.push_back(signedHex);
        try {
            outTxid = QString::fromStdString(
                m_clientModel->node().executeRpc("sendrawtransaction", sendParams, "").get_str());
        } catch (const UniValue& e) {
            outError = tr("sendrawtransaction failed: %1").arg(QString::fromStdString(
                e.exists("message") ? e["message"].get_str() : e.write()));
            return false;
        }
    }
    return true;
}
// ─────────────────────────────────────────────────────────────────────────────

QString VaultDialog::vaultStoragePath() const
{
    fs::path dataDir = gArgs.GetDataDirNet();
    return QString::fromStdString(fs::PathToString(dataDir / "vaults.json"));
}

void VaultDialog::saveVaultRecord(const VaultRecord& rec)
{
    // Load existing records
    loadVaultRecords();

    m_vaults.append(rec);

    // Serialise all records to JSON
    QJsonArray arr;
    for (const auto& r : m_vaults) {
        QJsonObject obj;
        obj[QStringLiteral("label")]            = r.label;
        obj[QStringLiteral("p2shAddress")]       = r.p2shAddress;
        obj[QStringLiteral("redeemScript")]      = r.redeemScript;
        obj[QStringLiteral("descriptor")]        = r.descriptor;
        obj[QStringLiteral("recipientAddress")]  = r.recipientAddress;
        obj[QStringLiteral("locktime")]          = static_cast<qint64>(r.locktime);
        obj[QStringLiteral("isTimestamp")]       = r.isTimestamp;
        obj[QStringLiteral("amount")]            = static_cast<qint64>(r.amount);
        obj[QStringLiteral("txid")]              = r.txid;
        obj[QStringLiteral("vout")]              = r.vout;
        obj[QStringLiteral("isVesting")]         = r.isVesting;
        obj[QStringLiteral("trancheIndex")]      = r.trancheIndex;
        obj[QStringLiteral("vestingId")]         = r.vestingId;
        obj[QStringLiteral("isPQVault")]         = r.isPQVault;
        arr.append(obj);
    }

    QJsonDocument doc(arr);
    QFile file(vaultStoragePath());
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write(doc.toJson());
    }
}

void VaultDialog::loadVaultRecords()
{
    m_vaults.clear();

    QFile file(vaultStoragePath());
    if (!file.exists() || !file.open(QIODevice::ReadOnly)) return;

    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isArray()) return;

    for (const QJsonValue& val : doc.array()) {
        if (!val.isObject()) continue;
        QJsonObject obj = val.toObject();

        VaultRecord rec;
        rec.label            = obj[QStringLiteral("label")].toString();
        rec.p2shAddress      = obj[QStringLiteral("p2shAddress")].toString();
        rec.redeemScript     = obj[QStringLiteral("redeemScript")].toString();
        rec.descriptor       = obj[QStringLiteral("descriptor")].toString();
        rec.recipientAddress = obj[QStringLiteral("recipientAddress")].toString();
        rec.locktime         = static_cast<int64_t>(obj[QStringLiteral("locktime")].toDouble());
        rec.isTimestamp      = obj[QStringLiteral("isTimestamp")].toBool();
        rec.amount           = static_cast<CAmount>(obj[QStringLiteral("amount")].toDouble());
        rec.txid             = obj[QStringLiteral("txid")].toString();
        rec.vout             = obj[QStringLiteral("vout")].toInt();
        rec.isVesting        = obj[QStringLiteral("isVesting")].toBool();
        rec.trancheIndex     = obj[QStringLiteral("trancheIndex")].toInt(-1);
        rec.vestingId        = obj[QStringLiteral("vestingId")].toString();
        rec.isPQVault        = obj[QStringLiteral("isPQVault")].toBool();
        m_vaults.append(rec);
    }
}

void VaultDialog::refreshVaultsTable()
{
    m_vaultsTable->setRowCount(0);

    int curBlock = currentBlockHeight();
    int64_t curTime  = currentUnixTime();

    for (int i = 0; i < m_vaults.size(); i++) {
        const VaultRecord& rec = m_vaults[i];

        bool unlocked = rec.isTimestamp
            ? (curTime >= rec.locktime)
            : (curBlock >= static_cast<int>(rec.locktime));

        QString statusStr = unlocked
            ? tr("UNLOCKED ✓")
            : (rec.isTimestamp
               ? tr("Locked (unlocks %1)").arg(QDateTime::fromSecsSinceEpoch(rec.locktime).toString(Qt::ISODate))
               : tr("Locked (unlocks block %1, ~%2 blocks away)")
                   .arg(rec.locktime)
                   .arg(rec.locktime - curBlock));

        int row = m_vaultsTable->rowCount();
        m_vaultsTable->insertRow(row);

        auto setCell = [&](int col, const QString& text) {
            auto* item = new QTableWidgetItem(text);
            item->setFlags(item->flags() & ~Qt::ItemIsEditable);
            m_vaultsTable->setItem(row, col, item);
        };

        QString displayLabel = rec.label;
        if (rec.isVesting)
            displayLabel += QStringLiteral(" [T%1]").arg(rec.trancheIndex + 1);

        setCell(0, displayLabel);
        setCell(1, rec.p2shAddress);
        setCell(2, BitcoinUnits::format(BitcoinUnits::Unit::BTC, rec.amount, false, BitcoinUnits::SeparatorStyle::ALWAYS));
        setCell(3, QString::number(rec.locktime));
        setCell(4, rec.isTimestamp ? tr("Timestamp") : tr("Block height"));
        setCell(5, rec.recipientAddress);
        setCell(6, statusStr);

        // Colour the status cell
        if (m_vaultsTable->item(row, 6)) {
            m_vaultsTable->item(row, 6)->setForeground(
                unlocked ? QColor(Qt::darkGreen) : QColor(Qt::darkRed));
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Helpers
// ─────────────────────────────────────────────────────────────────────────────

int VaultDialog::currentBlockHeight() const
{
    return m_clientModel ? m_clientModel->getNumBlocks() : 0;
}

int64_t VaultDialog::currentUnixTime() const
{
    return QDateTime::currentSecsSinceEpoch();
}

CAmount VaultDialog::parseAmount(const QString& text, bool& ok) const
{
    // Parse a decimal string like "100.5" into satoshis (8 decimal places)
    CAmount amount = 0;
    ok = BitcoinUnits::parse(BitcoinUnits::Unit::BTC, text, &amount);
    return amount;
}
