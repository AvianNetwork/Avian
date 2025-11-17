// Copyright (c) 2024 The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "psbtoperationsdialog.h"
#include "ui_psbtoperationsdialog.h"

#include "clientmodel.h"
#include "guiutil.h"
#include "walletmodel.h"

#include "base58.h"
#include "consensus/validation.h"
#include "psbt.h"
#include "script/interpreter.h"
#include "script/sign.h"
#include "script/standard.h"
#include "utilmoneystr.h"
#include "utilstrencodings.h"
#include "validation.h"
#include "wallet/psbtwallet.h"
#include "wallet/wallet.h"

#include <QApplication>
#include <QClipboard>
#include <QFileDialog>
#include <QMessageBox>
#include <QRegularExpression>
#include <QStringList>
#include <QTextStream>
#include <algorithm>
#include <fstream>

PSBTOperationsDialog::PSBTOperationsDialog(QWidget* parent, WalletModel* walletModel, ClientModel* clientModel) : QDialog(parent),
                                                                                                                  m_ui(new Ui::PSBTOperationsDialog),
                                                                                                                  m_wallet_model(walletModel),
                                                                                                                  m_client_model(clientModel)
{
    m_ui->setupUi(this);

    connect(m_ui->signTransactionButton, &QPushButton::clicked, this, &PSBTOperationsDialog::signTransaction);
    connect(m_ui->broadcastTransactionButton, &QPushButton::clicked, this, &PSBTOperationsDialog::broadcastTransaction);
    connect(m_ui->copyToClipboardButton, &QPushButton::clicked, this, &PSBTOperationsDialog::copyToClipboard);
    connect(m_ui->saveButton, &QPushButton::clicked, this, &PSBTOperationsDialog::saveTransaction);
    connect(m_ui->closeButton, &QPushButton::clicked, this, &PSBTOperationsDialog::close);

    m_ui->signTransactionButton->setEnabled(false);
    m_ui->broadcastTransactionButton->setEnabled(false);
}

PSBTOperationsDialog::~PSBTOperationsDialog()
{
    delete m_ui;
}

void PSBTOperationsDialog::openWithPSBT(const PartiallySignedTransaction& psbtx)
{
    m_transaction_data = psbtx;
    sanitizeTransaction(m_transaction_data);

    if (m_wallet_model) {
        CWallet* pwallet = m_wallet_model->getWallet();
        if (pwallet) {
            std::string utxoError;
            if (!EnsurePSBTInputUTXOs(pwallet, m_transaction_data, utxoError)) {
                showStatus(tr("Warning: %1").arg(QString::fromStdString(utxoError)), StatusLevel::WARN);
            }
        }
    }

    // Combine any partial signatures that may already be present.
    FinalizePSBT(m_transaction_data);

    // Analyze and display the PSBT
    updateTransactionDisplay();

    this->show();
}

void PSBTOperationsDialog::signTransaction()
{
    if (!m_wallet_model) {
        showStatus(tr("Wallet not available"), StatusLevel::ERR);
        return;
    }

    // Request wallet unlock if needed
    WalletModel::UnlockContext ctx(m_wallet_model->requestUnlock());
    if (!ctx.isValid()) {
        showStatus(tr("Cannot sign inputs while wallet is locked."), StatusLevel::WARN);
        return;
    }

    try {
        showStatus(tr("Signing transaction..."), StatusLevel::INFO);

        CWallet* pwallet = m_wallet_model->getWallet();
        if (!pwallet) {
            showStatus(tr("Wallet not accessible"), StatusLevel::ERR);
            return;
        }

        std::string utxoError;
        if (!EnsurePSBTInputUTXOs(pwallet, m_transaction_data, utxoError)) {
            showStatus(tr("Warning: %1").arg(QString::fromStdString(utxoError)), StatusLevel::WARN);
        }

        int inputs_signed = 0;
        int inputs_missing = 0;
        int nHashType = SIGHASH_ALL;
        if (IsForkIDUAHFenabledForCurrentBlock()) {
            nHashType |= SIGHASH_FORKID;
        }

        for (size_t i = 0; i < m_transaction_data.inputs.size() && i < m_transaction_data.tx.vin.size(); ++i) {
            PSBTInput& input = m_transaction_data.inputs[i];
            const CTxIn& txin = m_transaction_data.tx.vin[i];

            if (input.IsSigned()) {
                continue;
            }

            // Make sure wallet metadata (redeem scripts, derivations) is attached
            FillPSBTInputWalletData(*pwallet, txin.prevout, m_transaction_data, i);

            CAmount amount = -1;
            CScript scriptPubKey;
            if (input.utxo && txin.prevout.n < input.utxo->vout.size()) {
                const CTxOut& prevout = input.utxo->vout[txin.prevout.n];
                amount = prevout.nValue;
                scriptPubKey = prevout.scriptPubKey;
            } else if (input.txout.nValue >= 0) {
                amount = input.txout.nValue;
                scriptPubKey = input.txout.scriptPubKey;
            }

            if (amount < 0 || scriptPubKey.empty()) {
                inputs_missing++;
                continue;
            }

            CMutableTransaction txToSign(m_transaction_data.tx);
            SignatureData sigdata;
            bool signed_ok = ProduceSignature(MutableTransactionSignatureCreator(pwallet, &txToSign, i, amount, nHashType), scriptPubKey, sigdata);

            if (signed_ok) {
                input.final_script_sig.assign(sigdata.scriptSig.begin(), sigdata.scriptSig.end());
                if (!sigdata.scriptWitness.IsNull()) {
                    CDataStream witness_stream(SER_NETWORK, PROTOCOL_VERSION);
                    witness_stream << static_cast<uint64_t>(sigdata.scriptWitness.stack.size());
                    for (const auto& item : sigdata.scriptWitness.stack) {
                        witness_stream << item;
                    }
                    input.final_script_witness.assign(witness_stream.begin(), witness_stream.end());
                }
                inputs_signed++;
            }
        }

        FinalizePSBT(m_transaction_data);
        updateTransactionDisplay();

        if (inputs_signed > 0) {
            if (inputs_missing > 0) {
                showStatus(tr("Transaction partially signed. %1 input(s) signed, %2 input(s) missing data.").arg(inputs_signed).arg(inputs_missing), StatusLevel::WARN);
            } else {
                showStatus(tr("Transaction signed successfully. %1 input(s) signed.").arg(inputs_signed), StatusLevel::INFO);
            }
        } else if (inputs_missing > 0) {
            showStatus(tr("Unable to sign: %1 input(s) still missing UTXO data.").arg(inputs_missing), StatusLevel::ERR);
        } else {
            showStatus(tr("No inputs could be signed. Signature data may be invalid."), StatusLevel::WARN);
        }
    } catch (const std::exception& e) {
        showStatus(tr("Failed to sign transaction: %1").arg(e.what()), StatusLevel::ERR);
    }
}

void PSBTOperationsDialog::broadcastTransaction()
{
    if (!m_client_model) {
        showStatus(tr("Client model not available"), StatusLevel::ERR);
        return;
    }

    try {
        PartiallySignedTransaction psbt_copy = m_transaction_data;
        CMutableTransaction tx;
        if (!FinalizeAndExtractPSBT(psbt_copy, tx)) {
            showStatus(tr("Failed to finalize PSBT for broadcast"), StatusLevel::ERR);
            return;
        }

        CTransactionRef txRef = MakeTransactionRef(tx);
        std::string err_string;
        TransactionError error = BroadcastTransaction(txRef, err_string, true);

        if (error == TransactionError::OK) {
            showStatus(tr("Transaction successfully broadcast!\nTXID: %1").arg(QString::fromStdString(tx.GetHash().GetHex())), StatusLevel::INFO);
            m_transaction_data = psbt_copy;
            m_ui->signTransactionButton->setEnabled(false);
            m_ui->broadcastTransactionButton->setEnabled(false);
        } else {
            const QString detail = err_string.empty() ? QString::fromUtf8(TransactionErrorString(error)) : QString::fromStdString(err_string);
            showStatus(tr("Transaction broadcast failed: %1").arg(detail), StatusLevel::ERR);
        }
    } catch (const std::exception& e) {
        showStatus(tr("Transaction broadcast failed: %1").arg(e.what()), StatusLevel::ERR);
    }
}

void PSBTOperationsDialog::copyToClipboard()
{
    PartiallySignedTransaction exportPsbt = psbtForExport();
    // Serialize PSBT to base64
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << exportPsbt;
    std::string encoded = EncodeBase64(std::string(ss.begin(), ss.end()));

    QApplication::clipboard()->setText(QString::fromStdString(encoded));
    showStatus(tr("PSBT copied to clipboard."), StatusLevel::INFO);
}

void PSBTOperationsDialog::saveTransaction()
{
    PartiallySignedTransaction exportPsbt = psbtForExport();
    const QString suggestedName = suggestedPsbtFilename();
    QString filename = QFileDialog::getSaveFileName(this,
        tr("Save Binary PSBT"), suggestedName, tr("Partially Signed Transaction (*.psbt);;All Files (*)"));

    if (filename.isEmpty()) {
        return;
    }

    try {
        CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
        ss << exportPsbt;

        std::ofstream out(filename.toStdString().c_str(), std::ofstream::out | std::ofstream::binary);
        out << ss.str();
        out.close();

        showStatus(tr("PSBT saved as binary .psbt."), StatusLevel::INFO);
    } catch (const std::exception& e) {
        showStatus(tr("Failed to save PSBT: %1").arg(e.what()), StatusLevel::ERR);
    }
}

void PSBTOperationsDialog::updateTransactionDisplay()
{
    const PSBTAnalysis analysis = AnalyzePSBT(m_transaction_data);
    m_ui->transactionDescription->setText(renderTransaction(m_transaction_data, analysis));
    showTransactionStatus(analysis);

    const bool has_error = !analysis.error.empty();
    const bool has_inputs = !analysis.inputs.empty();
    const bool has_unsigned = std::any_of(analysis.inputs.begin(), analysis.inputs.end(), [](const PSBTInputAnalysis& input) {
        return !input.is_final;
    });

    const bool can_sign = m_wallet_model && has_inputs && !has_error && has_unsigned;
    const bool ready_to_broadcast = !has_error && has_inputs && !has_unsigned && analysis.next >= PSBTRole::FINALIZER;

    m_ui->signTransactionButton->setEnabled(can_sign);
    m_ui->broadcastTransactionButton->setEnabled(ready_to_broadcast);
}

QString PSBTOperationsDialog::renderTransaction(const PartiallySignedTransaction& psbtx, const PSBTAnalysis& analysis)
{
    QString tx_description;

    if (psbtx.tx.vout.empty()) {
        return tr("Invalid PSBT: No transaction outputs");
    }

    CAmount total_amount = 0;

    // List outputs
    for (const CTxOut& out : psbtx.tx.vout) {
        total_amount += out.nValue;
        CTxDestination address;
        ExtractDestination(out.scriptPubKey, address);

        QString amount_str = QString::fromStdString(FormatMoney(out.nValue));
        QString address_str = QString::fromStdString(EncodeDestination(address));

        tx_description.append(tr("• Sends %1 to %2").arg(amount_str, address_str));
        tx_description.append("<br>");
    }

    if (analysis.fee) {
        tx_description.append("<br><b>" + tr("Estimated fee") + ": </b>");
        tx_description.append(QString::fromStdString(FormatMoney(*analysis.fee)));
        if (analysis.estimated_vsize) {
            tx_description.append("<br><b>" + tr("Estimated vsize") + ": </b>");
            tx_description.append(QString::number(*analysis.estimated_vsize) + " " + tr("vB"));
        }
        if (analysis.estimated_feerate) {
            const double sats_per_vb = analysis.estimated_feerate->GetFeePerK() / 1000.0;
            tx_description.append("<br><b>" + tr("Effective feerate") + ": </b>");
            tx_description.append(tr("%1 sat/vB").arg(QString::number(sats_per_vb, 'f', 2)));
        }
    } else {
        tx_description.append("<br><b>" + tr("Fee") + ": </b>");
        tx_description.append(tr("Unable to calculate without complete UTXO data."));
    }

    const size_t num_unsigned = std::count_if(analysis.inputs.begin(), analysis.inputs.end(), [](const PSBTInputAnalysis& input) {
        return !input.is_final;
    });

    if (num_unsigned > 0) {
        tx_description.append("<br>");
        tx_description.append(tr("Transaction has %1 unsigned input(s).").arg(QString::number(num_unsigned)));
    }

    if (!analysis.error.empty()) {
        tx_description.append("<br><span style='color:red;'>");
        tx_description.append(tr("Analysis warning: %1").arg(QString::fromStdString(analysis.error)));
        tx_description.append("</span>");
    }

    return tx_description;
}

QString PSBTOperationsDialog::suggestedPsbtFilename() const
{
    QString shortId;
    if (!m_transaction_data.tx.vin.empty() || !m_transaction_data.tx.vout.empty()) {
        shortId = QString::fromStdString(m_transaction_data.tx.GetHash().ToString().substr(0, 12));
    } else {
        shortId = QStringLiteral("draft");
    }

    QString destinationLabel;
    for (const auto& txout : m_transaction_data.tx.vout) {
        CTxDestination dest;
        if (ExtractDestination(txout.scriptPubKey, dest)) {
            destinationLabel = QString::fromStdString(EncodeDestination(dest));
            break;
        }
    }

    if (destinationLabel.isEmpty()) {
        destinationLabel = QStringLiteral("psbt");
    }

    destinationLabel.replace(QRegularExpression("[^A-Za-z0-9_-]"), "-");
    destinationLabel = destinationLabel.left(24);

    return QStringLiteral("%1-%2.psbt").arg(destinationLabel, shortId);
}

void PSBTOperationsDialog::sanitizeTransaction(PartiallySignedTransaction& psbtx) const
{
    for (auto& txin : psbtx.tx.vin) {
        txin.scriptSig.clear();
        txin.scriptWitness.SetNull();
    }
}

PartiallySignedTransaction PSBTOperationsDialog::psbtForExport() const
{
    PartiallySignedTransaction copy = m_transaction_data;
    sanitizeTransaction(copy);

    if (m_wallet_model) {
        CWallet* pwallet = m_wallet_model->getWallet();
        if (pwallet) {
            std::string utxoError;
            EnsurePSBTInputUTXOs(pwallet, copy, utxoError);
        }
    }

    return copy;
}

void PSBTOperationsDialog::showStatus(const QString& msg, StatusLevel level)
{
    m_ui->statusBar->setText(msg);

    switch (level) {
    case StatusLevel::INFO:
        m_ui->statusBar->setStyleSheet("QLabel { background-color: #00AA00; color: white; padding: 5px; }");
        break;
    case StatusLevel::WARN:
        m_ui->statusBar->setStyleSheet("QLabel { background-color: #FFA500; color: black; padding: 5px; }");
        break;
    case StatusLevel::ERR:
        m_ui->statusBar->setStyleSheet("QLabel { background-color: #FF0000; color: white; padding: 5px; }");
        break;
    }
}

void PSBTOperationsDialog::showTransactionStatus(const PSBTAnalysis& analysis)
{
    if (!analysis.error.empty()) {
        showStatus(tr("PSBT error: %1").arg(QString::fromStdString(analysis.error)), StatusLevel::ERR);
        return;
    }

    switch (analysis.next) {
    case PSBTRole::UPDATER:
        showStatus(tr("Transaction is missing some information about inputs."), StatusLevel::WARN);
        break;
    case PSBTRole::SIGNER:
        if (m_wallet_model && m_wallet_model->getWallet()) {
            showStatus(tr("Transaction still needs signature(s)."), StatusLevel::INFO);
        } else {
            showStatus(tr("Transaction still needs signature(s) and no wallet is available."), StatusLevel::WARN);
        }
        break;
    case PSBTRole::FINALIZER:
    case PSBTRole::EXTRACTOR:
        showStatus(tr("Transaction is fully signed and ready for broadcast."), StatusLevel::INFO);
        break;
    default:
        showStatus(tr("Transaction status is unknown."), StatusLevel::WARN);
        break;
    }
}
