// Copyright (c) 2017-2019 The Raven Core developers
// Copyright (c) 2022 The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bitcoin-build-config.h> // IWYU pragma: keep

#include <qt/paperwalletdialog.h>
#include <qt/forms/ui_paperwalletdialog.h>

#include <qt/bitcoinunits.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>
#include <qt/walletmodel.h>
#include <qt/sendcoinsrecipient.h>
#include <qt/walletmodeltransaction.h>

#include <key.h>
#include <key_io.h>
#include <util/strencodings.h>
#include <wallet/coincontrol.h>

#include <QFont>
#include <QInputDialog>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>

#ifdef USE_QRCODE
#include <qrencode.h>
#endif

#include <QtPrintSupport/QPrinter>
#include <QtPrintSupport/QPrintDialog>

PaperWalletDialog::PaperWalletDialog(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::PaperWalletDialog),
    model(nullptr)
{
    ui->setupUi(this);

    ui->buttonBox->addButton(tr("Close"), QDialogButtonBox::RejectRole);

    // Begin with a small bold monospace font for the textual version of the key and address.
    QFont font("Monospace");
    font.setBold(true);
    font.setStyleHint(QFont::TypeWriter);
    font.setPixelSize(1);
    ui->addressText->setFont(font);
    ui->addressText->setStyleSheet("{background-color:transparent;}");
    ui->privateKeyText->setFont(font);
    ui->privateKeyText->setStyleSheet("{background-color:transparent;}");
    ui->addressText->setAlignment(Qt::AlignJustify);
    ui->privateKeyText->setAlignment(Qt::AlignJustify);

    QMessageBox::critical(this, "Warning",
        tr("It is recommended to disconnect from the internet before printing paper wallets. "
           "Even though paper wallets are generated on your local computer, it is still possible "
           "to unknowingly have malware that transmits your screen to a remote location. It is "
           "also recommended to print to a local printer vs a network printer since that network "
           "traffic can be monitored. Some advanced printers also store copies of each printed "
           "document. Proceed with caution relative to the amount of value you plan to store on "
           "each address."),
        QMessageBox::Ok, QMessageBox::Ok);
}

void PaperWalletDialog::setModel(WalletModel *model)
{
    this->model = model;
    this->on_getNewAddress_clicked();
}

PaperWalletDialog::~PaperWalletDialog()
{
    delete ui;
}

void PaperWalletDialog::on_getNewAddress_clicked()
{
    // Create a new private key
    CKey privKey;
    privKey.MakeNewKey(true);

    // Derive the public key
    CPubKey pubkey = privKey.GetPubKey();

    // Create String versions of each
    std::string myPrivKey = EncodeSecret(privKey);
    std::string myPubKey = HexStr(pubkey);
    std::string myAddress = EncodeDestination(PKHash(pubkey));

#ifdef USE_QRCODE
    // Generate the address QR code
    QRcode *code = QRcode_encodeString(myAddress.c_str(), 0, QR_ECLEVEL_M, QR_MODE_8, 1);
    if (!code) {
        ui->addressQRCode->setText(tr("Error encoding Address into QR Code."));
        return;
    }
    QImage myImage = QImage(code->width, code->width, QImage::Format_ARGB32);
    myImage.fill(QColor(0, 0, 0, 0));
    unsigned char* p = code->data;
    for (int y = 0; y < code->width; y++) {
        for (int x = 0; x < code->width; x++) {
            myImage.setPixel(x, y, ((*p & 1) ? 0xff000000 : 0x0));
            p++;
        }
    }
    QRcode_free(code);

    // Generate the private key QR code
    code = QRcode_encodeString(myPrivKey.c_str(), 0, QR_ECLEVEL_M, QR_MODE_8, 1);
    if (!code) {
        ui->privateKeyQRCode->setText(tr("Error encoding private key into QR Code."));
        return;
    }
    QImage myImagePriv = QImage(code->width, code->width, QImage::Format_ARGB32);
    myImagePriv.fill(QColor(0, 0, 0, 0));
    p = code->data;
    for (int y = 0; y < code->width; y++) {
        for (int x = 0; x < code->width; x++) {
            myImagePriv.setPixel(x, y, ((*p & 1) ? 0xff000000 : 0x0));
            p++;
        }
    }
    QRcode_free(code);

    // Populate the QR Codes
    ui->addressQRCode->setPixmap(QPixmap::fromImage(myImage).scaled(ui->addressQRCode->width(), ui->addressQRCode->height()));
    ui->addressQRCode->setStyleSheet("{color: #ffffff;}");
    ui->privateKeyQRCode->setPixmap(QPixmap::fromImage(myImagePriv).scaled(ui->privateKeyQRCode->width(), ui->privateKeyQRCode->height()));
    ui->privateKeyQRCode->setStyleSheet("{color: #ffffff;}");
#endif

    // Populate the Texts
    ui->addressText->setText(QString::fromStdString(myAddress));
    ui->privateKeyText->setText(QString::fromStdString(myPrivKey));

    ui->publicKey->setHtml(QString::fromStdString(myPubKey));

    // Update the fonts to fit the height of the wallet.
    double paperHeight = (double)ui->paperTemplate->height();
    double maxTextWidth = paperHeight * 0.99;
    double minTextWidth = paperHeight * 0.95;
    int pixelSizeStep = 1;

    int addressTextLength = ui->addressText->fontMetrics().boundingRect(ui->addressText->text()).width();
    QFont font = ui->addressText->font();
    for (int i = 0; i < PAPER_WALLET_READJUST_LIMIT; i++) {
        if (addressTextLength < minTextWidth) {
            font.setPixelSize(font.pixelSize() + pixelSizeStep);
            ui->addressText->setFont(font);
            addressTextLength = ui->addressText->fontMetrics().boundingRect(ui->addressText->text()).width();
        } else {
            break;
        }
    }
    if (addressTextLength > maxTextWidth) {
        font.setPixelSize(font.pixelSize() - pixelSizeStep);
        ui->addressText->setFont(font);
    }

    int privateKeyTextLength = ui->privateKeyText->fontMetrics().boundingRect(ui->privateKeyText->text()).width();
    font = ui->privateKeyText->font();
    for (int i = 0; i < PAPER_WALLET_READJUST_LIMIT; i++) {
        if (privateKeyTextLength < minTextWidth) {
            font.setPixelSize(font.pixelSize() + pixelSizeStep);
            ui->privateKeyText->setFont(font);
            privateKeyTextLength = ui->privateKeyText->fontMetrics().boundingRect(ui->privateKeyText->text()).width();
        } else {
            break;
        }
    }
    if (privateKeyTextLength > maxTextWidth) {
        font.setPixelSize(font.pixelSize() - pixelSizeStep);
        ui->privateKeyText->setFont(font);
    }
}

void PaperWalletDialog::on_printButton_clicked()
{
    QPrinter printer(QPrinter::HighResolution);
    QPrintDialog *qpd = new QPrintDialog(&printer, this);

    qpd->setPrintRange(QAbstractPrintDialog::AllPages);

    QList<QString> recipientPubKeyHashes;

    if (qpd->exec() != QDialog::Accepted) {
        delete qpd;
        return;
    }
    delete qpd;

    // Hardcode these values
    printer.setPageOrientation(QPageLayout::Portrait);
    printer.setPageSize(QPageSize::A4);
    printer.setFullPage(true);

    QPainter painter;
    if (!painter.begin(&printer)) {
        QMessageBox::critical(this, "Printing Error",
            tr("failed to open file, is it writable?"),
            QMessageBox::Ok, QMessageBox::Ok);
        return;
    }

    int walletCount = ui->walletCount->currentIndex() + 1;
    int walletsPerPage = 4;

    int pageHeight = printer.pageLayout().paintRectPixels(printer.resolution()).height() - PAPER_WALLET_PAGE_MARGIN;
    int walletHeight = ui->paperTemplate->height();
    double computedWalletHeight = 0.9 * pageHeight / walletsPerPage;
    double scale = computedWalletHeight / walletHeight;
    double walletPadding = pageHeight * 0.05 / (walletsPerPage - 1) / scale;

    QRegion walletRegion = QRegion(ui->paperTemplate->x(), ui->paperTemplate->y(),
                                   ui->paperTemplate->width(), ui->paperTemplate->height());
    painter.scale(scale, scale);

    for (int i = 0; i < walletCount; i++) {
        QPoint point = QPoint(PAPER_WALLET_PAGE_MARGIN,
                              (PAPER_WALLET_PAGE_MARGIN / 2) + (i % walletsPerPage) * (walletHeight + walletPadding));
        this->render(&painter, point, walletRegion);
        recipientPubKeyHashes.append(ui->addressText->text());

        if (i % walletsPerPage == (walletsPerPage - 1)) {
            printer.newPage();
        }

        this->on_getNewAddress_clicked();
    }

    painter.end();

#ifdef ENABLE_WALLET
    if (!model) return;

    QStringList formatted;
    WalletModelTransaction *tx = nullptr;

    while (true) {
        bool ok;

        double amountInput = QInputDialog::getDouble(this,
            tr("Load Paper Wallets"),
            tr("The paper wallet printing process has begun.<br/>"
               "Please wait for the wallets to print completely and verify that everything "
               "printed correctly.<br/>"
               "Check for misalignments, ink bleeding, smears, or anything else that could "
               "make the private keys unreadable.<br/>"
               "Now, enter the number of AVN you wish to send to each wallet:"),
            0, 0, 2147483647, 8, &ok);

        if (!ok) {
            return;
        }

        WalletModel::UnlockContext ctx(model->requestUnlock());
        if (!ctx.isValid()) {
            return;
        }

        QList<SendCoinsRecipient> recipients;
        CAmount amount = (CAmount)(amountInput * COIN);
        for (const QString& dest : recipientPubKeyHashes) {
            recipients.append(SendCoinsRecipient(dest, tr("Paper wallet %1").arg(dest), amount, ""));
            formatted.append(tr("<b>%1</b> to Paper Wallet <span style='font-family: monospace;'>%2</span>")
                .arg(QString::number(amountInput, 'f', 8), GUIUtil::HtmlEscape(dest)));
        }

        tx = new WalletModelTransaction(recipients);

        wallet::CCoinControl ctrl;

        WalletModel::SendCoinsReturn prepareStatus;
        prepareStatus = model->prepareTransaction(*tx, ctrl);

        if (prepareStatus.status == WalletModel::InvalidAddress) {
            QMessageBox::critical(this, tr("Send Coins"),
                tr("The recipient address is not valid, please recheck."),
                QMessageBox::Ok, QMessageBox::Ok);
        } else if (prepareStatus.status == WalletModel::InvalidAmount) {
            QMessageBox::critical(this, tr("Send Coins"),
                tr("The amount to pay must be larger than 0"),
                QMessageBox::Ok, QMessageBox::Ok);
        } else if (prepareStatus.status == WalletModel::AmountExceedsBalance) {
            QMessageBox::critical(this, tr("Send Coins"),
                tr("The amount exceeds your balance."),
                QMessageBox::Ok, QMessageBox::Ok);
        } else if (prepareStatus.status == WalletModel::AmountWithFeeExceedsBalance) {
            QMessageBox::critical(this, tr("Send Coins"),
                tr("The total exceeds your balance when the transaction fee is included"),
                QMessageBox::Ok, QMessageBox::Ok);
        } else if (prepareStatus.status == WalletModel::DuplicateAddress) {
            QMessageBox::critical(this, tr("Send Coins"),
                tr("Duplicate address found, can only send to each address once per send operation."),
                QMessageBox::Ok, QMessageBox::Ok);
        } else if (prepareStatus.status == WalletModel::TransactionCreationFailed) {
            QMessageBox::critical(this, tr("Send Coins"),
                tr("Transaction creation failed!"),
                QMessageBox::Ok, QMessageBox::Ok);
        } else if (prepareStatus.status == WalletModel::OK) {
            break;
        } else {
            delete tx;
            return;
        }
    }

    CAmount txFee = tx->getTransactionFee();
    QString questionString = tr("Are you sure you want to send?");
    questionString.append("<br /><br />%1");

    if (txFee > 0) {
        questionString.append("<hr /><span style='color:#aa0000;'>");
        questionString.append(BitcoinUnits::formatWithUnit(model->getOptionsModel()->getDisplayUnit(), txFee));
        questionString.append("</span> ");
        questionString.append(tr("added as transaction fee"));
    }

    questionString.append("<hr />");
    CAmount totalAmount = tx->getTotalTransactionAmount() + txFee;
    QStringList alternativeUnits;
    for (const BitcoinUnits::Unit& u : BitcoinUnits::availableUnits()) {
        if (u != model->getOptionsModel()->getDisplayUnit())
            alternativeUnits.append(BitcoinUnits::formatWithUnit(u, totalAmount));
    }

    questionString.append(tr("Total Amount %1 (= %2)")
                              .arg(BitcoinUnits::formatWithUnit(model->getOptionsModel()->getDisplayUnit(), totalAmount))
                              .arg(alternativeUnits.join(" " + tr("or") + " ")));

    QMessageBox::StandardButton retval = QMessageBox::question(this,
        tr("Confirm send coins"),
        questionString.arg(formatted.join("<br />")),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);

    if (retval != QMessageBox::Yes) {
        delete tx;
        return;
    }

    model->sendCoins(*tx);
    delete tx;
#endif
}
