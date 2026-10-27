#include "SignDialog.h"
#include "signatures/SignatureData.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QLineEdit>
#include <QCheckBox>
#include <QComboBox>
#include <QPushButton>
#include <QLabel>
#include <QFileDialog>
#include <QScrollArea>
#include <QFrame>
#include "ui/TestDialogSeam.h"

namespace pdfe::ui {

SignDialog::SignDialog(QWidget* parent)
    : QDialog(parent)
{
    setObjectName("signDialog");
    setAccessibleName(tr("Sign Document Dialog"));
    setWindowTitle(tr("Sign Document"));
    setMinimumWidth(450);
    // Pipeline a7c2e541 phase-17: cap the dialog at a height that fits
    // on 600 px displays / RDP sessions. Combined with the QScrollArea
    // wrapper below, this lets the dialog shrink to fit while still
    // exposing every interactive control via scrolling.
    setMinimumHeight(300);
    resize(500, 550);

    // Pipeline a7c2e541 phase-17 (closes KNOWN_WEAK P12-INFRA-2):
    // wrap the dialog content in a QScrollArea so the natural height
    // of all groupboxes (cert + sig info + appearance + tsa + adv +
    // preview + validation + buttons) does not force a minimum-height
    // that exceeds available screen height on small / RDP displays.
    // The previous implementation set its contents as the dialog's
    // direct layout, which gave a minimum-height of ~691 px and
    // produced a Qt6 setGeometry warning on 600 px displays.
    auto* outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(0, 0, 0, 0);

    auto* scrollArea = new QScrollArea(this);
    scrollArea->setObjectName("signDialogScrollArea");
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    outerLayout->addWidget(scrollArea);

    auto* contentWidget = new QWidget(scrollArea);
    contentWidget->setObjectName("signDialogContent");
    scrollArea->setWidget(contentWidget);

    auto* mainLayout = new QVBoxLayout(contentWidget);

    // Certificate group
    auto* certGroup = new QGroupBox(tr("Certificate"), this);
    certGroup->setObjectName("certificateGroup");
    certGroup->setAccessibleName(tr("Certificate"));
    auto* certLayout = new QFormLayout(certGroup);

    auto* pfxRow = new QHBoxLayout();
    pfxPathEdit_ = new QLineEdit(this);
    pfxPathEdit_->setObjectName("pfxPathEdit");
    pfxPathEdit_->setAccessibleName(tr("PFX certificate path"));
    pfxPathEdit_->setPlaceholderText(tr("Path to .pfx certificate file"));
    pfxRow->addWidget(pfxPathEdit_, 1);

    browseButton_ = new QPushButton(tr("Browse..."), this);
    browseButton_->setObjectName("browseButton");
    browseButton_->setAccessibleName(tr("Browse for PFX File"));
    pfxRow->addWidget(browseButton_);

    certLayout->addRow(tr("PFX File:"), pfxRow);

    pfxPasswordEdit_ = new QLineEdit(this);
    pfxPasswordEdit_->setObjectName("pfxPasswordEdit");
    pfxPasswordEdit_->setAccessibleName(tr("PFX password"));
    pfxPasswordEdit_->setEchoMode(QLineEdit::Password);
    pfxPasswordEdit_->setPlaceholderText(tr("Certificate password"));
    certLayout->addRow(tr("Password:"), pfxPasswordEdit_);

    mainLayout->addWidget(certGroup);

    // Signature info group
    auto* infoGroup = new QGroupBox(tr("Signature Information"), this);
    infoGroup->setObjectName("signatureInfoGroup");
    infoGroup->setAccessibleName(tr("Signature Information"));
    auto* infoLayout = new QFormLayout(infoGroup);

    reasonEdit_ = new QLineEdit(this);
    reasonEdit_->setObjectName("reasonEdit");
    reasonEdit_->setAccessibleName(tr("Reason"));
    reasonEdit_->setPlaceholderText(tr("e.g., Approval, Review"));
    infoLayout->addRow(tr("Reason:"), reasonEdit_);

    locationEdit_ = new QLineEdit(this);
    locationEdit_->setObjectName("locationEdit");
    locationEdit_->setAccessibleName(tr("Location"));
    locationEdit_->setPlaceholderText(tr("e.g., New York"));
    infoLayout->addRow(tr("Location:"), locationEdit_);

    mainLayout->addWidget(infoGroup);

    // Appearance group
    auto* appearGroup = new QGroupBox(tr("Appearance"), this);
    appearGroup->setObjectName("appearanceGroup");
    appearGroup->setAccessibleName(tr("Appearance"));
    auto* appearLayout = new QVBoxLayout(appearGroup);

    showLabelsCheck_ = new QCheckBox(tr("Show Labels"), this);
    showLabelsCheck_->setObjectName("showLabelsCheck");
    showLabelsCheck_->setAccessibleName(tr("Show Labels"));
    showLabelsCheck_->setChecked(true);
    appearLayout->addWidget(showLabelsCheck_);

    showNameCheck_ = new QCheckBox(tr("Show Name"), this);
    showNameCheck_->setObjectName("showNameCheck");
    showNameCheck_->setAccessibleName(tr("Show Name"));
    showNameCheck_->setChecked(true);
    appearLayout->addWidget(showNameCheck_);

    showDateCheck_ = new QCheckBox(tr("Show Date"), this);
    showDateCheck_->setObjectName("showDateCheck");
    showDateCheck_->setAccessibleName(tr("Show Date"));
    showDateCheck_->setChecked(true);
    appearLayout->addWidget(showDateCheck_);

    showDnCheck_ = new QCheckBox(tr("Show Distinguished Name"), this);
    showDnCheck_->setObjectName("showDnCheck");
    showDnCheck_->setAccessibleName(tr("Show Distinguished Name"));
    showDnCheck_->setChecked(true);
    appearLayout->addWidget(showDnCheck_);

    showLogoCheck_ = new QCheckBox(tr("Show Logo"), this);
    showLogoCheck_->setObjectName("showLogoCheck");
    showLogoCheck_->setAccessibleName(tr("Show Logo"));
    showLogoCheck_->setChecked(true);
    appearLayout->addWidget(showLogoCheck_);

    mainLayout->addWidget(appearGroup);

    // Timestamp group
    auto* tsaGroup = new QGroupBox(tr("Timestamp"), this);
    tsaGroup->setObjectName("tsaGroup");
    auto* tsaLayout = new QFormLayout(tsaGroup);

    tsaTimestampCheck_ = new QCheckBox(tr("Include TSA Timestamp"), this);
    tsaTimestampCheck_->setObjectName("tsaCheckBox");
    tsaTimestampCheck_->setAccessibleName(tr("Include TSA Timestamp"));
    tsaLayout->addRow(tsaTimestampCheck_);

    tsaUrlEdit_ = new QLineEdit(this);
    tsaUrlEdit_->setObjectName("tsaUrlEdit");
    tsaUrlEdit_->setAccessibleName(tr("TSA URL"));
    tsaUrlEdit_->setPlaceholderText(tr("e.g., http://timestamp.digicert.com"));
    tsaUrlEdit_->setEnabled(false);
    tsaLayout->addRow(tr("TSA URL:"), tsaUrlEdit_);

    connect(tsaTimestampCheck_, &QCheckBox::toggled, tsaUrlEdit_, &QLineEdit::setEnabled);

    mainLayout->addWidget(tsaGroup);

    // Advanced / certification group — invisible signature toggle +
    // certification level. Wired to SigningOptions.invisible and
    // SigningOptions.certification in signingOptions().
    auto* advGroup = new QGroupBox(tr("Advanced"), this);
    advGroup->setObjectName("advancedGroup");
    auto* advLayout = new QFormLayout(advGroup);

    // Pipeline F phase-3 (AC compliance): objectNames renamed
    // invisibleCheck -> invisibleCheckBox and certificationCombo
    // -> certificationLevelCombo to match the parity-doc / matrix
    // naming convention. All test usages updated in lockstep.
    invisibleCheck_ = new QCheckBox(tr("Invisible signature (no visible appearance)"), this);
    invisibleCheck_->setObjectName("invisibleCheckBox");
    invisibleCheck_->setAccessibleName(tr("Invisible signature"));
    advLayout->addRow(invisibleCheck_);

    certificationCombo_ = new QComboBox(this);
    certificationCombo_->setObjectName("certificationLevelCombo");
    certificationCombo_->setAccessibleName(tr("Certification level"));
    certificationCombo_->addItem(tr("Regular signature (no certification)"), 0);
    certificationCombo_->addItem(tr("Certify — no changes allowed"), 1);
    certificationCombo_->addItem(tr("Certify — form fill-in + signing"), 2);
    certificationCombo_->addItem(tr("Certify — annotations + form + signing"), 3);
    advLayout->addRow(tr("Certification:"), certificationCombo_);

    mainLayout->addWidget(advGroup);

    // Preview
    previewLabel_ = new QLabel(this);
    previewLabel_->setObjectName("previewLabel");
    previewLabel_->setAccessibleName(tr("Signature Preview"));
    previewLabel_->setMinimumHeight(60);
    previewLabel_->setFrameStyle(QFrame::StyledPanel | QFrame::Sunken);
    previewLabel_->setAlignment(Qt::AlignCenter);
    previewLabel_->setText(tr("Signature preview will appear here"));
    mainLayout->addWidget(previewLabel_);

    // Validation label
    validationLabel_ = new QLabel(this);
    validationLabel_->setObjectName("validationLabel");
    validationLabel_->setAccessibleName(tr("Validation Message"));
    validationLabel_->setStyleSheet("color: red;");
    validationLabel_->setText("");
    mainLayout->addWidget(validationLabel_);

    // Buttons
    auto* buttonLayout = new QHBoxLayout();
    buttonLayout->addStretch();

    okButton_ = new QPushButton(tr("Sign"), this);
    okButton_->setObjectName("okButton");
    okButton_->setAccessibleName(tr("Sign"));
    okButton_->setDefault(true);
    // Keep OK enabled at all times so UIA-driven clicks always
    // dispatch to accept().  Validation of empty inputs runs in
    // SignDialog::accept() and shows an inline validation message
    // instead of silently rejecting the click (P12-INFRA-1 sub-issue
    // 3 / SignDialog reachability flake).
    okButton_->setEnabled(true);
    buttonLayout->addWidget(okButton_);

    cancelButton_ = new QPushButton(tr("Cancel"), this);
    cancelButton_->setObjectName("cancelButton");
    cancelButton_->setAccessibleName(tr("Cancel"));
    buttonLayout->addWidget(cancelButton_);

    mainLayout->addLayout(buttonLayout);

    // Connections
    connect(browseButton_, &QPushButton::clicked, this, &SignDialog::onBrowsePfx);
    connect(pfxPathEdit_, &QLineEdit::textChanged, this, &SignDialog::validateInput);
    connect(pfxPasswordEdit_, &QLineEdit::textChanged, this, &SignDialog::validateInput);
    connect(okButton_, &QPushButton::clicked, this, &QDialog::accept);
    connect(cancelButton_, &QPushButton::clicked, this, &QDialog::reject);

    // Register with the dialog-introspection seam so tests can observe
    // this dialog while its modal exec() loop runs (UIA cannot enumerate
    // it). MUST stay last: the seam snapshots the child inventory.
    pdfe::ui::testseam::registerDialogIntrospection(this);
}

SignDialog::~SignDialog() = default;

pdfe::signatures::SigningOptions SignDialog::signingOptions() const
{
    pdfe::signatures::SigningOptions opts;
    opts.pfxPath = pfxPathEdit_->text().toStdString();
    opts.pfxPassword = pfxPasswordEdit_->text().toStdString();
    opts.reason = reasonEdit_->text().toStdString();
    opts.location = locationEdit_->text().toStdString();

    int flags = 0;
    if (showLabelsCheck_->isChecked()) flags |= 1;  // PDF_SIGNATURE_SHOW_LABELS
    if (showDnCheck_->isChecked())     flags |= 2;  // PDF_SIGNATURE_SHOW_DN
    if (showDateCheck_->isChecked())   flags |= 4;  // PDF_SIGNATURE_SHOW_DATE
    if (showNameCheck_->isChecked())   flags |= 8;  // PDF_SIGNATURE_SHOW_TEXT_NAME
    if (showLogoCheck_->isChecked())   flags |= 32; // PDF_SIGNATURE_SHOW_LOGO
    opts.appearanceFlags = flags;

    opts.useTimestamp = tsaTimestampCheck_->isChecked();
    if (opts.useTimestamp && !tsaUrlEdit_->text().trimmed().isEmpty()) {
        opts.tsaUrl = tsaUrlEdit_->text().trimmed().toStdString();
    }

    opts.invisible = invisibleCheck_->isChecked();
    opts.certification = static_cast<pdfe::signatures::CertificationLevel>(
        certificationCombo_->currentData().toInt());

    return opts;
}

void SignDialog::onBrowsePfx()
{
    QString path = QFileDialog::getOpenFileName(
        this, tr("Select PFX Certificate"),
        QString(), tr("PFX Files (*.pfx *.p12);;All Files (*)"));

    if (!path.isEmpty()) {
        pfxPathEdit_->setText(path);
    }
}

void SignDialog::validateInput()
{
    QString message;

    if (pfxPathEdit_->text().trimmed().isEmpty()) {
        message = tr("PFX certificate file is required.");
    } else if (pfxPasswordEdit_->text().isEmpty()) {
        message = tr("Certificate password is required.");
    }

    // OK stays enabled at all times — accept() does the real
    // validation so UIA-driven clicks dispatch reliably.  This is
    // only an inline hint label for human users.
    validationLabel_->setText(message);
}

void SignDialog::accept()
{
    // Re-validate at accept time so the dialog cannot be accepted
    // with empty inputs even if the OK button is reached via UIA
    // InvokePattern (which on some Qt6 builds bypasses the enabled
    // gate when set via ValuePattern.SetValue, leaving okButton_'s
    // enabled flag stale).
    if (pfxPathEdit_->text().trimmed().isEmpty()) {
        validationLabel_->setText(
            tr("PFX certificate file is required."));
        return;
    }
    if (pfxPasswordEdit_->text().isEmpty()) {
        validationLabel_->setText(
            tr("Certificate password is required."));
        return;
    }
    QDialog::accept();
}

} // namespace pdfe::ui
