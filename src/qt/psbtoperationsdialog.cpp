// Copyright (c) 2011-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/psbtoperationsdialog.h>

#include <common/messages.h>
#include <core_io.h>
#include <interfaces/node.h>
#include <key_io.h>
#include <node/psbt.h>
#include <node/types.h>
#include <policy/policy.h>
#include <qt/bitcoinunits.h>
#include <qt/forms/ui_psbtoperationsdialog.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>

#include <QMessageBox>
#include <util/fs.h>
#include <assets/assets.h>
#include <assets/assettypes.h>
#include <script/interpreter.h>
#include <util/strencodings.h>

#include <fstream>
#include <iostream>
#include <string>

using common::TransactionErrorString;
using node::AnalyzePSBT;
using node::DEFAULT_MAX_RAW_TX_FEE_RATE;
using node::PSBTAnalysis;
using node::TransactionError;

PSBTOperationsDialog::PSBTOperationsDialog(
    QWidget* parent, WalletModel* wallet_model, ClientModel* client_model) : QDialog(parent, GUIUtil::dialog_flags),
                                                                             m_ui(new Ui::PSBTOperationsDialog),
                                                                             m_wallet_model(wallet_model),
                                                                             m_client_model(client_model)
{
    m_ui->setupUi(this);

    connect(m_ui->signTransactionButton, &QPushButton::clicked, this, &PSBTOperationsDialog::signTransaction);
    connect(m_ui->broadcastTransactionButton, &QPushButton::clicked, this, &PSBTOperationsDialog::broadcastTransaction);
    connect(m_ui->copyToClipboardButton, &QPushButton::clicked, this, &PSBTOperationsDialog::copyToClipboard);
    connect(m_ui->saveButton, &QPushButton::clicked, this, &PSBTOperationsDialog::saveTransaction);

    connect(m_ui->closeButton, &QPushButton::clicked, this, &PSBTOperationsDialog::close);

    // Populate sighash type selector with all Avian FORKID variants
    m_ui->sighashTypeCombo->addItem(tr("ALL|FORKID (default)"),       SIGHASH_ALL|SIGHASH_FORKID);
    m_ui->sighashTypeCombo->addItem(tr("NONE|FORKID"),                SIGHASH_NONE|SIGHASH_FORKID);
    m_ui->sighashTypeCombo->addItem(tr("SINGLE|FORKID"),              SIGHASH_SINGLE|SIGHASH_FORKID);
    m_ui->sighashTypeCombo->addItem(tr("ALL|FORKID|ANYONECANPAY"),    SIGHASH_ALL|SIGHASH_FORKID|SIGHASH_ANYONECANPAY);
    m_ui->sighashTypeCombo->addItem(tr("NONE|FORKID|ANYONECANPAY"),   SIGHASH_NONE|SIGHASH_FORKID|SIGHASH_ANYONECANPAY);
    m_ui->sighashTypeCombo->addItem(tr("SINGLE|FORKID|ANYONECANPAY"), SIGHASH_SINGLE|SIGHASH_FORKID|SIGHASH_ANYONECANPAY);

    m_ui->signTransactionButton->setEnabled(false);
    m_ui->broadcastTransactionButton->setEnabled(false);
    m_ui->sighashTypeCombo->setEnabled(false);
}

PSBTOperationsDialog::~PSBTOperationsDialog()
{
    delete m_ui;
}

void PSBTOperationsDialog::openWithPSBT(PartiallySignedTransaction psbtx)
{
    m_transaction_data = psbtx;

    bool complete = FinalizePSBT(psbtx); // Make sure all existing signatures are fully combined before checking for completeness.
    if (m_wallet_model) {
        size_t n_could_sign;
        bool fill_complete = complete;
        const auto err{m_wallet_model->wallet().fillPSBT(std::nullopt, /*sign=*/false, /*bip32derivs=*/true, &n_could_sign, m_transaction_data, fill_complete)};
        if (err) {
            showStatus(tr("Failed to load transaction: %1")
                           .arg(QString::fromStdString(PSBTErrorString(*err).translated)),
                       StatusLevel::ERR);
            return;
        }
        // Use either result — FinalizePSBT may recognize completeness that
        // FillPSBT (descriptor-wallet-only verification) does not.
        complete = complete || fill_complete;
        const bool can_sign = !complete && !m_wallet_model->wallet().privateKeysDisabled() && n_could_sign > 0;
        m_ui->signTransactionButton->setEnabled(can_sign);
        m_ui->sighashTypeCombo->setEnabled(can_sign);
    } else {
        m_ui->signTransactionButton->setEnabled(false);
        m_ui->sighashTypeCombo->setEnabled(false);
    }

    m_ui->broadcastTransactionButton->setEnabled(complete);

    updateTransactionDisplay();
}

void PSBTOperationsDialog::signTransaction()
{
    bool complete;
    size_t n_signed;

    WalletModel::UnlockContext ctx(m_wallet_model->requestUnlock());

    const int sighash_type = m_ui->sighashTypeCombo->currentData().toInt();
    const auto err{m_wallet_model->wallet().fillPSBT(sighash_type, /*sign=*/true, /*bip32derivs=*/true, &n_signed, m_transaction_data, complete)};

    if (err) {
        showStatus(tr("Failed to sign transaction: %1")
            .arg(QString::fromStdString(PSBTErrorString(*err).translated)), StatusLevel::ERR);
        return;
    }

    // FillPSBT's completeness check (PSBTInputSignedAndVerified) may fail for
    // legacy-type wallets where the descriptor SPKM can't verify the script.
    // Fall back to FinalizePSBT which only checks that final_scriptSig is set.
    if (!complete) {
        PartiallySignedTransaction psbtx_copy = m_transaction_data;
        complete = FinalizePSBT(psbtx_copy);
    }

    updateTransactionDisplay();

    if (!complete && !ctx.isValid()) {
        showStatus(tr("Cannot sign inputs while wallet is locked."), StatusLevel::WARN);
    } else if (!complete && n_signed < 1) {
        showStatus(tr("Could not sign any more inputs."), StatusLevel::WARN);
    } else if (!complete) {
        showStatus(tr("Signed %1 inputs, but more signatures are still required.").arg(n_signed),
            StatusLevel::INFO);
    } else {
        showStatus(tr("Signed transaction successfully. Transaction is ready to broadcast."),
            StatusLevel::INFO);
        m_ui->broadcastTransactionButton->setEnabled(true);
    }
}

void PSBTOperationsDialog::broadcastTransaction()
{
    // Warn if any input was signed with ANYONECANPAY. Such a PSBT is typically
    // a partial Marketplace offer that requires a buyer to add inputs before
    // broadcasting. Broadcasting it standalone will burn the excess input value
    // as miner fees and the node will likely reject it for excessive fee rate.
    bool has_anyonecanpay = false;
    for (const PSBTInput& input : m_transaction_data.inputs) {
        for (const auto& [keyid, sig_pair] : input.partial_sigs) {
            if (!sig_pair.second.empty() && (sig_pair.second.back() & SIGHASH_ANYONECANPAY)) {
                has_anyonecanpay = true;
                break;
            }
        }
        // A finalized input has no partial signatures left; its type is in the field
        if (input.sighash_type && (*input.sighash_type & SIGHASH_ANYONECANPAY)) has_anyonecanpay = true;
        if (has_anyonecanpay) break;
    }
    if (has_anyonecanpay) {
        const auto reply = QMessageBox::warning(this,
            tr("ANYONECANPAY Transaction Warning"),
            tr("This transaction was signed with ANYONECANPAY and is likely intended as a "
               "partial Marketplace offer. A buyer must add their own inputs before "
               "broadcasting — sending it now as a standalone transaction will almost "
               "certainly result in the excess input value being lost to miner fees, "
               "and the node may reject it for exceeding the maximum fee rate.\n\n"
               "Are you sure you want to broadcast?"),
            QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel);
        if (reply != QMessageBox::Yes) return;
    }

    CMutableTransaction mtx;
    if (!FinalizeAndExtractPSBT(m_transaction_data, mtx)) {
        // This is never expected to fail unless we were given a malformed PSBT
        // (e.g. with an invalid signature.)
        showStatus(tr("Unknown error processing transaction."), StatusLevel::ERR);
        return;
    }

    CTransactionRef tx = MakeTransactionRef(mtx);
    std::string err_string;
    TransactionError error =
        m_client_model->node().broadcastTransaction(tx, DEFAULT_MAX_RAW_TX_FEE_RATE.GetFeePerK(), err_string);

    if (error == TransactionError::OK) {
        showStatus(tr("Transaction broadcast successfully! Transaction ID: %1")
            .arg(QString::fromStdString(tx->GetHash().GetHex())), StatusLevel::INFO);
    } else {
        showStatus(tr("Transaction broadcast failed: %1")
            .arg(QString::fromStdString(TransactionErrorString(error).translated)), StatusLevel::ERR);
    }
}

void PSBTOperationsDialog::copyToClipboard() {
    DataStream ssTx{};
    ssTx << m_transaction_data;
    GUIUtil::setClipboard(EncodeBase64(ssTx.str()).c_str());
    showStatus(tr("PSBT copied to clipboard."), StatusLevel::INFO);
}

void PSBTOperationsDialog::saveTransaction() {
    DataStream ssTx{};
    ssTx << m_transaction_data;

    QString selected_filter;
    QString filename_suggestion = "";
    bool first = true;
    for (const CTxOut& out : m_transaction_data.tx->vout) {
        if (!first) {
            filename_suggestion.append("-");
        }
        CTxDestination address;
        ExtractDestination(out.scriptPubKey, address);
        QString amount = BitcoinUnits::format(m_client_model->getOptionsModel()->getDisplayUnit(), out.nValue);
        QString address_str = QString::fromStdString(EncodeDestination(address));
        filename_suggestion.append(address_str + "-" + amount);
        first = false;
    }
    filename_suggestion.append(".psbt");
    QString filename = GUIUtil::getSaveFileName(this,
        tr("Save Transaction Data"), filename_suggestion,
        //: Expanded name of the binary PSBT file format. See: BIP 174.
        tr("Partially Signed Transaction (Binary)") + QLatin1String(" (*.psbt)"), &selected_filter);
    if (filename.isEmpty()) {
        return;
    }
    std::ofstream out{filename.toLocal8Bit().data(), std::ofstream::out | std::ofstream::binary};
    out << ssTx.str();
    out.close();
    showStatus(tr("PSBT saved to disk."), StatusLevel::INFO);
}

void PSBTOperationsDialog::updateTransactionDisplay() {
    m_ui->transactionDescription->setText(renderTransaction(m_transaction_data));
    showTransactionStatus(m_transaction_data);
}

QString PSBTOperationsDialog::renderTransaction(const PartiallySignedTransaction &psbtx)
{
    QString tx_description;
    QLatin1String bullet_point(" * ");
    CAmount totalAmount = 0;
    for (const CTxOut& out : psbtx.tx->vout) {
        CTxDestination address;
        ExtractDestination(out.scriptPubKey, address);

        // Check if this is an asset output
        CAssetOutputEntry assetData;
        if (GetAssetData(out.scriptPubKey, assetData)) {
            // An issue, a reissue and a transfer are different things to sign: say which.
            // Names come from the transaction, so they are escaped.
            const QString amount{QString::fromStdString(ValueFromAmountString(assetData.nAmount, 8))};
            const QString name{GUIUtil::HtmlEscape(assetData.assetName)};
            const QString to{QString::fromStdString(EncodeDestination(address))};
            const auto yes_no = [](bool value) { return value ? tr("yes") : tr("no"); };
            std::string script_address;
            CNewAsset new_asset;
            CReissueAsset reissue;
            if (assetData.type == TX_REISSUE_ASSET && ReissueAssetFromScript(out.scriptPubKey, reissue, script_address)) {
                QStringList changes;
                changes << tr("reissuable afterwards: %1").arg(yes_no(reissue.nReissuable > 0));
                if (reissue.nUnits >= 0) changes << tr("units set to %1").arg(reissue.nUnits);
                if (!reissue.strIPFSHash.empty()) changes << tr("IPFS or transaction hash set to %1").arg(GUIUtil::HtmlEscape(EncodeAssetData(reissue.strIPFSHash)));
                tx_description.append(bullet_point).append(tr("Reissues asset %1: %2 more to %3 (%4)")
                    .arg(name).arg(amount).arg(to).arg(changes.join(", ")));
            } else if (assetData.type == TX_NEW_ASSET && IsAssetNameAnOwner(assetData.assetName)) {
                tx_description.append(bullet_point).append(tr("Creates owner token %1 at %2").arg(name).arg(to));
            } else if (assetData.type == TX_NEW_ASSET && AssetFromScript(out.scriptPubKey, new_asset, script_address)) {
                QStringList details;
                details << tr("units %1").arg(new_asset.units) << tr("reissuable: %1").arg(yes_no(new_asset.nReissuable > 0));
                if (new_asset.nHasIPFS > 0) details << tr("IPFS or transaction hash %1").arg(GUIUtil::HtmlEscape(EncodeAssetData(new_asset.strIPFSHash)));
                tx_description.append(bullet_point).append(tr("Issues new asset %1: %2 to %3 (%4)")
                    .arg(name).arg(amount).arg(to).arg(details.join(", ")));
            } else {
                tx_description.append(bullet_point).append(tr("Transfers %1 %2 to %3").arg(amount).arg(name).arg(to));
            }
        } else if (out.scriptPubKey.IsNullAsset()) {
            // Null asset output: a verifier string, a tag on an address, a freeze
            CNullAssetTxVerifierString verifier;
            CNullAssetTxData null_data;
            std::string null_address;
            if (out.scriptPubKey.IsNullAssetVerifierTxDataScript()) {
                if (AssetNullVerifierDataFromScript(out.scriptPubKey, verifier)) {
                    tx_description.append(bullet_point).append(tr("Sets verifier string: \"%1\"")
                        .arg(GUIUtil::HtmlEscape(verifier.verifier_string)));
                }
            } else if (AssetNullDataFromScript(out.scriptPubKey, null_data, null_address)) {
                const QString name{GUIUtil::HtmlEscape(null_data.asset_name)};
                const QString whom{GUIUtil::HtmlEscape(null_address)};
                if (IsAssetNameAnRestricted(null_data.asset_name)) {
                    tx_description.append(bullet_point).append(
                        (null_data.flag ? tr("Freezes address %1 for restricted asset %2") : tr("Unfreezes address %1 for restricted asset %2")).arg(whom).arg(name));
                } else {
                    tx_description.append(bullet_point).append(
                        (null_data.flag ? tr("Adds tag %1 to address %2") : tr("Removes tag %1 from address %2")).arg(name).arg(whom));
                }
            } else if (GlobalAssetNullDataFromScript(out.scriptPubKey, null_data)) {
                tx_description.append(bullet_point).append(
                    (null_data.flag ? tr("Freezes restricted asset %1 for all addresses") : tr("Unfreezes restricted asset %1 for all addresses"))
                        .arg(GUIUtil::HtmlEscape(null_data.asset_name)));
            } else {
                tx_description.append(bullet_point).append(tr("Carries asset data this wallet cannot read"));
            }
        } else {
            // Regular AVN output
            totalAmount += out.nValue;
            tx_description.append(bullet_point).append(tr("Sends %1 to %2")
                .arg(BitcoinUnits::formatWithUnit(BitcoinUnit::BTC, out.nValue))
                .arg(QString::fromStdString(EncodeDestination(address))));
        }
        // Check if the address is one of ours
        if (m_wallet_model != nullptr && m_wallet_model->wallet().txoutIsMine(out)) tx_description.append(" (" + tr("own address") + ")");
        tx_description.append("<br>");
    }

    PSBTAnalysis analysis = AnalyzePSBT(psbtx);
    tx_description.append(bullet_point);
    if (!*analysis.fee) {
        // This happens if the transaction is missing input UTXO information.
        tx_description.append(tr("Unable to calculate transaction fee or total transaction amount."));
    } else {
        tx_description.append(tr("Pays transaction fee: "));
        tx_description.append(BitcoinUnits::formatWithUnit(BitcoinUnit::BTC, *analysis.fee));

        // add total amount in all subdivision units
        tx_description.append("<hr />");
        QStringList alternativeUnits;
        for (const BitcoinUnits::Unit u : BitcoinUnits::availableUnits())
        {
            if(u != m_client_model->getOptionsModel()->getDisplayUnit()) {
                alternativeUnits.append(BitcoinUnits::formatHtmlWithUnit(u, totalAmount));
            }
        }
        tx_description.append(QString("<b>%1</b>: <b>%2</b>").arg(tr("Total Amount"))
            .arg(BitcoinUnits::formatHtmlWithUnit(m_client_model->getOptionsModel()->getDisplayUnit(), totalAmount)));
        tx_description.append(QString("<br /><span style='font-size:10pt; font-weight:normal;'>(=%1)</span>")
            .arg(alternativeUnits.join(" " + tr("or") + " ")));
    }

    size_t num_unsigned = CountPSBTUnsignedInputs(psbtx);
    if (num_unsigned > 0) {
        tx_description.append("<br><br>");
        tx_description.append(tr("Transaction has %1 unsigned inputs.").arg(QString::number(num_unsigned)));
    }

    return tx_description;
}

void PSBTOperationsDialog::showStatus(const QString &msg, StatusLevel level) {
    m_ui->statusBar->setText(msg);
    switch (level) {
        case StatusLevel::INFO: {
            m_ui->statusBar->setStyleSheet("QLabel { background-color : lightgreen }");
            break;
        }
        case StatusLevel::WARN: {
            m_ui->statusBar->setStyleSheet("QLabel { background-color : orange }");
            break;
        }
        case StatusLevel::ERR: {
            m_ui->statusBar->setStyleSheet("QLabel { background-color : red }");
            break;
        }
    }
    m_ui->statusBar->show();
}

size_t PSBTOperationsDialog::couldSignInputs(const PartiallySignedTransaction &psbtx) {
    if (!m_wallet_model) {
        return 0;
    }

    size_t n_signed;
    bool complete;
    const auto err{m_wallet_model->wallet().fillPSBT(std::nullopt, /*sign=*/false, /*bip32derivs=*/false, &n_signed, m_transaction_data, complete)};

    if (err) {
        return 0;
    }
    return n_signed;
}

void PSBTOperationsDialog::showTransactionStatus(const PartiallySignedTransaction &psbtx) {
    PSBTAnalysis analysis = AnalyzePSBT(psbtx);
    size_t n_could_sign = couldSignInputs(psbtx);

    switch (analysis.next) {
        case PSBTRole::UPDATER: {
            showStatus(tr("Transaction is missing some information about inputs."), StatusLevel::WARN);
            break;
        }
        case PSBTRole::SIGNER: {
            QString need_sig_text = tr("Transaction still needs signature(s).");
            StatusLevel level = StatusLevel::INFO;
            if (!m_wallet_model) {
                need_sig_text += " " + tr("(But no wallet is loaded.)");
                level = StatusLevel::WARN;
            } else if (m_wallet_model->wallet().privateKeysDisabled()) {
                need_sig_text += " " + tr("(But this wallet cannot sign transactions.)");
                level = StatusLevel::WARN;
            } else if (n_could_sign < 1) {
                need_sig_text += " " + tr("(But this wallet does not have the right keys.)"); // XXX wording
                level = StatusLevel::WARN;
            }
            showStatus(need_sig_text, level);
            break;
        }
        case PSBTRole::FINALIZER:
        case PSBTRole::EXTRACTOR: {
            showStatus(tr("Transaction is fully signed and ready for broadcast."), StatusLevel::INFO);
            break;
        }
        default: {
            showStatus(tr("Transaction status is unknown."), StatusLevel::ERR);
            break;
        }
    }
}
