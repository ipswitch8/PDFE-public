#include "CreateCertificateDialog.h"

#include "TestDialogSeam.h"
#include "signatures/Pkcs7OpenSslBridge.h"

#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace pdfe::ui {

CreateCertificateDialog::CreateCertificateDialog(QWidget* parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("createCertDialog"));
    setAccessibleName(tr("Create Self-Signed Certificate"));
    setWindowTitle(tr("Create Self-Signed Certificate"));

    auto* outer = new QVBoxLayout(this);

    auto* intro = new QLabel(
        tr("Creates a certificate and private key you can sign with. It is "
           "self-signed: other people's software will show your signature as "
           "valid but not trusted until they choose to trust this certificate."),
        this);
    intro->setObjectName(QStringLiteral("certIntroLabel"));
    intro->setWordWrap(true);
    outer->addWidget(intro);

    auto* form = new QFormLayout;
    outer->addLayout(form);

    cnEdit_ = new QLineEdit(this);
    cnEdit_->setObjectName(QStringLiteral("certCnEdit"));
    cnEdit_->setAccessibleName(tr("Certificate Common Name"));
    cnEdit_->setPlaceholderText(tr("Your name, e.g. Jane Doe (required)"));
    form->addRow(tr("Common Name (CN):"), cnEdit_);

    orgEdit_ = new QLineEdit(this);
    orgEdit_->setObjectName(QStringLiteral("certOrgEdit"));
    orgEdit_->setAccessibleName(tr("Certificate Organization"));
    orgEdit_->setPlaceholderText(tr("Optional"));
    form->addRow(tr("Organization (O):"), orgEdit_);

    daysSpin_ = new QSpinBox(this);
    daysSpin_->setObjectName(QStringLiteral("certValidityDaysSpin"));
    daysSpin_->setAccessibleName(tr("Certificate Validity Days"));
    daysSpin_->setRange(1, 7300);
    daysSpin_->setValue(365);
    form->addRow(tr("Validity (days):"), daysSpin_);

    passEdit_ = new QLineEdit(this);
    passEdit_->setObjectName(QStringLiteral("certPassphraseEdit"));
    passEdit_->setAccessibleName(tr("Certificate Passphrase"));
    passEdit_->setEchoMode(QLineEdit::Password);
    form->addRow(tr("Passphrase:"), passEdit_);

    confirmEdit_ = new QLineEdit(this);
    confirmEdit_->setObjectName(QStringLiteral("certPassphraseConfirmEdit"));
    confirmEdit_->setAccessibleName(tr("Confirm Certificate Passphrase"));
    confirmEdit_->setEchoMode(QLineEdit::Password);
    form->addRow(tr("Confirm passphrase:"), confirmEdit_);

    auto* pathRow = new QHBoxLayout;
    pathEdit_ = new QLineEdit(this);
    pathEdit_->setObjectName(QStringLiteral("certOutputPathEdit"));
    pathEdit_->setAccessibleName(tr("Certificate File"));
    pathEdit_->setPlaceholderText(tr("Where to save the .pfx file"));
    browseButton_ = new QPushButton(tr("Browse..."), this);
    browseButton_->setObjectName(QStringLiteral("certBrowseButton"));
    browseButton_->setAccessibleName(tr("Browse for Certificate File"));
    pathRow->addWidget(pathEdit_, 1);
    pathRow->addWidget(browseButton_);
    form->addRow(tr("Save as:"), pathRow);
    connect(browseButton_, &QPushButton::clicked, this, &CreateCertificateDialog::onBrowse);

    errorLabel_ = new QLabel(this);
    errorLabel_->setObjectName(QStringLiteral("certErrorLabel"));
    errorLabel_->setAccessibleName(tr("Certificate Error"));
    errorLabel_->setWordWrap(true);
    errorLabel_->setStyleSheet(QStringLiteral("color: #d32f2f; font-weight: bold;"));
    errorLabel_->setVisible(false);
    outer->addWidget(errorLabel_);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->setObjectName(QStringLiteral("certButtonBox"));
    okButton_ = buttons->button(QDialogButtonBox::Ok);
    okButton_->setText(tr("Create"));
    okButton_->setObjectName(QStringLiteral("certOkButton"));
    okButton_->setAccessibleName(tr("Create Certificate"));
    if (QPushButton* cancel = buttons->button(QDialogButtonBox::Cancel)) {
        cancel->setObjectName(QStringLiteral("certCancelButton"));
        cancel->setAccessibleName(tr("Cancel Certificate Creation"));
    }
    connect(buttons, &QDialogButtonBox::accepted, this, &CreateCertificateDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    outer->addWidget(buttons);

    // Clear a stale error as soon as the user starts fixing it.
    for (QLineEdit* e : {cnEdit_, passEdit_, confirmEdit_, pathEdit_}) {
        connect(e, &QLineEdit::textEdited, errorLabel_, [this]() {
            errorLabel_->clear();
            errorLabel_->setVisible(false);
        });
    }

    pdfe::ui::testseam::registerDialogIntrospection(this);
}

QString CreateCertificateDialog::passphrase() const
{
    return passEdit_->text();
}

QString CreateCertificateDialog::validationError(const QString& commonName,
                                                 const QString& passphrase,
                                                 const QString& confirmation,
                                                 const QString& outputPath)
{
    if (commonName.trimmed().isEmpty()) {
        return tr("Enter a Common Name (CN). It is the signer name the signature shows.");
    }
    if (passphrase.isEmpty()) {
        return tr("Enter a passphrase. It protects the private key in the .pfx file.");
    }
    if (passphrase != confirmation) {
        return tr("The two passphrases do not match.");
    }
    if (outputPath.trimmed().isEmpty()) {
        return tr("Choose where to save the certificate file.");
    }
    return QString();
}

QString CreateCertificateDialog::withPfxSuffix(const QString& path)
{
    const QString p = path.trimmed();
    if (p.isEmpty()) return p;
    if (p.endsWith(QStringLiteral(".pfx"), Qt::CaseInsensitive) ||
        p.endsWith(QStringLiteral(".p12"), Qt::CaseInsensitive)) {
        return p;
    }
    return p + QStringLiteral(".pfx");
}

void CreateCertificateDialog::onBrowse()
{
    QString chosen = pdfe::ui::testseam::takeFileDialogPath();
    if (chosen.isEmpty()) {
        chosen = QFileDialog::getSaveFileName(this, tr("Save New Certificate"),
                                              pathEdit_->text(), tr("PFX Files (*.pfx)"));
    }
    if (chosen.isEmpty()) return;  // cancelled
    const QString withSuffix = withPfxSuffix(chosen);
    // The save dialog confirmed overwriting `chosen`. Only when no suffix was
    // appended is that the file accept() will write.
    browsedPath_ = (withSuffix == chosen.trimmed()) ? withSuffix : QString();
    pathEdit_->setText(withSuffix);
    errorLabel_->clear();
    errorLabel_->setVisible(false);
}

void CreateCertificateDialog::showError(const QString& message, QWidget* focus)
{
    errorLabel_->setText(message);
    errorLabel_->setVisible(true);
    if (focus) focus->setFocus(Qt::OtherFocusReason);
}

void CreateCertificateDialog::accept()
{
    const QString error = validationError(cnEdit_->text(), passEdit_->text(),
                                          confirmEdit_->text(), pathEdit_->text());
    if (!error.isEmpty()) {
        QWidget* focus = cnEdit_;
        if (!cnEdit_->text().trimmed().isEmpty()) {
            focus = passEdit_->text().isEmpty() ? passEdit_
                  : (passEdit_->text() != confirmEdit_->text()) ? confirmEdit_
                  : static_cast<QWidget*>(pathEdit_);
        }
        showError(error, focus);
        return;
    }

    const QString path = withPfxSuffix(pathEdit_->text());
    if (path != pathEdit_->text()) pathEdit_->setText(path);
    if (QFileInfo::exists(path) && path != browsedPath_) {
        showError(tr("A file already exists at %1. Choose another name.")
                      .arg(QDir::toNativeSeparators(path)),
                  pathEdit_);
        return;
    }

    pdfe::signatures::Pkcs7OpenSslBridge::SelfSignedCertOptions opts;
    opts.commonName = cnEdit_->text().trimmed().toStdString();
    opts.organization = orgEdit_->text().trimmed().toStdString();
    opts.validityDays = daysSpin_->value();
    const auto res = pdfe::signatures::Pkcs7OpenSslBridge::createSelfSignedPfx(
        path.toStdString(), passEdit_->text().toStdString(), opts);
    if (!res.success) {
        // res.errorMessage is secret-free by contract (GitHub #361).
        showError(tr("The certificate could not be created: %1")
                      .arg(QString::fromStdString(res.errorMessage)),
                  pathEdit_);
        return;
    }

    writtenPath_ = path;
    QDialog::accept();
}

} // namespace pdfe::ui
