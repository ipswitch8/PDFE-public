#include "SignDialog.h"
#include "signatures/SignatureData.h"
#include "signatures/Pkcs7OpenSslBridge.h"
#include "annotations/SignatureImageLibrary.h"  // GitHub #362
#include "CreateCertificateDialog.h"           // DCA 0129

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QLineEdit>
#include <QCheckBox>
#include <QComboBox>
#include <QPushButton>
#include <QDialogButtonBox>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QLabel>
#include <QFileDialog>
#include <QInputDialog>
#include <QScrollArea>
#include <QFrame>
#include <QPixmap>
#include <QTemporaryFile>
#include <QDir>
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

    // Visual signature: the signature image plus the date and time, placed on
    // the page WITHOUT a certificate. It is not a digital signature, and the
    // notice below says so in the dialog itself, beside the option.
    visualOnlyCheck_ = new QCheckBox(
        tr("Visual signature only (image and date, no certificate)"), this);
    visualOnlyCheck_->setObjectName("visualSignatureCheck");
    visualOnlyCheck_->setAccessibleName(tr("Visual signature only"));
    mainLayout->addWidget(visualOnlyCheck_);
    visualNoticeLabel_ = new QLabel(
        tr("This is not a digital signature. It places your signature image and "
           "the date and time on the page. It does not prove who signed, and it "
           "does not detect later changes. Use a certificate for that."),
        this);
    visualNoticeLabel_->setObjectName("visualSignatureNotice");
    visualNoticeLabel_->setAccessibleName(tr("Visual signature notice"));
    visualNoticeLabel_->setWordWrap(true);
    visualNoticeLabel_->setVisible(false);
    mainLayout->addWidget(visualNoticeLabel_);

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

    // GitHub #361 — let a user who has no certificate create a self-signed PFX
    // in place, then sign with it. Clicking opens a subject/validity/passphrase
    // entry path; on success the produced .pfx path + passphrase populate the
    // fields above so signingOptions() returns them.
    createCertButton_ = new QPushButton(tr("Create certificate..."), this);
    createCertButton_->setObjectName("createCertButton");
    createCertButton_->setAccessibleName(tr("Create Self-Signed Certificate"));
    // DCA 0129: the outcome of Create certificate is shown HERE, beside the
    // button that started it, not only in validationLabel_ at the bottom of
    // the scroll area where the old CN error was never seen.
    createCertStatusLabel_ = new QLabel(this);
    createCertStatusLabel_->setObjectName("createCertStatusLabel");
    createCertStatusLabel_->setAccessibleName(tr("Certificate Creation Status"));
    createCertStatusLabel_->setWordWrap(true);
    auto* createRow = new QHBoxLayout;
    createRow->addWidget(createCertButton_);
    createRow->addWidget(createCertStatusLabel_, 1);
    certLayout->addRow(QString(), createRow);

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

    // GitHub #327 — image-of-signature picker. A read-only path field
    // plus a Browse button (mirroring the PFX pfxPathEdit_/browseButton_
    // pattern) and a small preview thumbnail. The chosen image becomes
    // the VISIBLE signature graphic; the text fields above still compose
    // on top per the Show-* flags.
    auto* imageRow = new QHBoxLayout();
    imagePathEdit_ = new QLineEdit(this);
    imagePathEdit_->setObjectName("imagePathEdit");
    imagePathEdit_->setAccessibleName(tr("Signature image path"));
    imagePathEdit_->setReadOnly(true);
    imagePathEdit_->setPlaceholderText(tr("Optional image for the signature appearance"));
    imageRow->addWidget(imagePathEdit_, 1);

    imageBrowseButton_ = new QPushButton(tr("Browse..."), this);
    imageBrowseButton_->setObjectName("imageBrowseButton");
    imageBrowseButton_->setAccessibleName(tr("Browse for Signature Image"));
    imageRow->addWidget(imageBrowseButton_);
    appearLayout->addLayout(imageRow);

    imagePreviewLabel_ = new QLabel(this);
    imagePreviewLabel_->setObjectName("imagePreviewLabel");
    imagePreviewLabel_->setAccessibleName(tr("Signature Image Preview"));
    imagePreviewLabel_->setMinimumHeight(48);
    imagePreviewLabel_->setFrameStyle(QFrame::StyledPanel | QFrame::Sunken);
    imagePreviewLabel_->setAlignment(Qt::AlignCenter);
    imagePreviewLabel_->setText(tr("No signature image selected"));
    appearLayout->addWidget(imagePreviewLabel_);

    // GitHub #362 — persisted signature-image library controls. A combo to
    // RE-SELECT a previously saved signature image (index 0 is a
    // non-selectable placeholder) and a button to SAVE the currently-browsed
    // image under a name for reuse in later sessions.
    auto* libraryRow = new QHBoxLayout();
    savedImageCombo_ = new QComboBox(this);
    savedImageCombo_->setObjectName("savedSignatureImageCombo");
    savedImageCombo_->setAccessibleName(tr("Saved Signature Images"));
    libraryRow->addWidget(savedImageCombo_, 1);

    saveImageButton_ = new QPushButton(tr("Save image..."), this);
    saveImageButton_->setObjectName("saveSignatureImageButton");
    saveImageButton_->setAccessibleName(tr("Save Signature Image to Library"));
    libraryRow->addWidget(saveImageButton_);
    appearLayout->addLayout(libraryRow);

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

    // GitHub #370 — field placement (page + rect, PDF points in fitz space).
    // Hard-coded QRectF(50,50,200,60) used to be the ONLY geometry a new
    // signature field could have; these controls let the user choose where
    // the appearance lands. Editable only in the new-field flow — see
    // setFieldPlacement(); constructed disabled so a caller that never
    // configures placement gets the pre-#370 behavior unchanged.
    placementGroup_ = new QGroupBox(tr("Field Placement"), this);
    placementGroup_->setObjectName("fieldPlacementGroup");
    placementGroup_->setAccessibleName(tr("Field Placement"));
    auto* placementLayout = new QFormLayout(placementGroup_);

    fieldPageSpin_ = new QSpinBox(this);
    fieldPageSpin_->setObjectName("fieldPageSpin");
    fieldPageSpin_->setAccessibleName(tr("Signature field page index"));
    fieldPageSpin_->setRange(0, 0);
    fieldPageSpin_->setValue(0);
    fieldPageSpin_->setToolTip(tr("0-based page index the field is placed on"));
    placementLayout->addRow(tr("Page (0-based):"), fieldPageSpin_);

    auto makePtSpin = [this](const char* objectName, const QString& accessible,
                             double minimum, double value) {
        auto* spin = new QDoubleSpinBox(this);
        spin->setObjectName(objectName);
        spin->setAccessibleName(accessible);
        spin->setRange(minimum, 14400.0);  // PDF implementation limit, points
        spin->setDecimals(2);
        spin->setSuffix(tr(" pt"));
        spin->setValue(value);
        return spin;
    };
    fieldXSpin_ = makePtSpin("fieldXSpin", tr("Signature field X"), 0.0, 50.0);
    fieldYSpin_ = makePtSpin("fieldYSpin", tr("Signature field Y"), 0.0, 50.0);
    fieldWidthSpin_ =
        makePtSpin("fieldWidthSpin", tr("Signature field width"), 1.0, 200.0);
    fieldHeightSpin_ =
        makePtSpin("fieldHeightSpin", tr("Signature field height"), 1.0, 60.0);
    placementLayout->addRow(tr("X:"), fieldXSpin_);
    placementLayout->addRow(tr("Y:"), fieldYSpin_);
    placementLayout->addRow(tr("Width:"), fieldWidthSpin_);
    placementLayout->addRow(tr("Height:"), fieldHeightSpin_);

    placementGroup_->setEnabled(false);  // armed by setFieldPlacement()
    mainLayout->addWidget(placementGroup_);

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
    connect(createCertButton_, &QPushButton::clicked, this, &SignDialog::onCreateCert);
    connect(imageBrowseButton_, &QPushButton::clicked, this, &SignDialog::onBrowseImage);
    connect(saveImageButton_, &QPushButton::clicked, this, &SignDialog::onSaveImageToLibrary);
    connect(savedImageCombo_, QOverload<int>::of(&QComboBox::activated),
            this, &SignDialog::onSelectSavedImage);
    connect(pfxPathEdit_, &QLineEdit::textChanged, this, &SignDialog::validateInput);
    connect(pfxPasswordEdit_, &QLineEdit::textChanged, this, &SignDialog::validateInput);
    connect(okButton_, &QPushButton::clicked, this, &QDialog::accept);
    connect(visualOnlyCheck_, &QCheckBox::toggled, this, &SignDialog::onVisualOnlyToggled);
    connect(imagePathEdit_, &QLineEdit::textChanged, this, &SignDialog::validateInput);
    connect(cancelButton_, &QPushButton::clicked, this, &QDialog::reject);

    // GitHub #362 — load the persisted signature-image library on construction
    // and populate the re-select combo from it.
    signatureImages_ = std::make_unique<pdfe::annotations::SignatureImageLibrary>();
    signatureImages_->loadFromSettings();
    refreshSavedImageCombo();

    // Register with the dialog-introspection seam so tests can observe
    // this dialog while its modal exec() loop runs (UIA cannot enumerate
    // it). MUST stay last: the seam snapshots the child inventory.
    pdfe::ui::testseam::registerDialogIntrospection(this);
}

SignDialog::~SignDialog() = default;

pdfe::signatures::SigningOptions SignDialog::signingOptions() const
{
    pdfe::signatures::SigningOptions opts;
    opts.visualOnly = visualOnlyCheck_->isChecked();
    if (!opts.visualOnly) {
        opts.pfxPath = pfxPathEdit_->text().toStdString();
        opts.pfxPassword = pfxPasswordEdit_->text().toStdString();
    }
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

    // GitHub #327 — image-of-signature graphic path (empty = text-only).
    opts.imagePath = imagePathEdit_->text().toStdString();
    // GitHub #374 — if that path is a library image materialised to a temp
    // file, hand its ownership along: the dialog is destroyed before
    // signField reads the path, and autoRemove would otherwise delete the
    // file with the dialog. Harmless when the path is a user-browsed file
    // (the keepalive then just holds an unrelated temp PNG until the options
    // are dropped, exactly as the dialog itself would have).
    opts.imageKeepalive = selectedImageTempFile_;

    // GitHub #370 — new-field placement. Only reported when
    // setFieldPlacement() armed the group for the new-field flow; an
    // existing-field sign never carries geometry (the field already has its
    // rect).
    if (placementForNewField_) {
        opts.hasFieldGeometry = true;
        opts.fieldPageIndex = fieldPageSpin_->value();
        opts.fieldRect = QRectF(fieldXSpin_->value(), fieldYSpin_->value(),
                                fieldWidthSpin_->value(),
                                fieldHeightSpin_->value());
    }

    return opts;
}

// GitHub #370 — see the header. `newField` arms editable placement; an
// existing field shows its rect read-only (disabled group) so the user sees
// where the appearance will land without being able to move a field that
// already exists.
void SignDialog::setFieldPlacement(bool newField, const QRectF& rect,
                                   int pageIndex, int pageCount)
{
    placementForNewField_ = newField;
    if (!placementGroup_) return;

    const int maxPage = pageCount > 0 ? pageCount - 1 : 0;
    fieldPageSpin_->setRange(0, maxPage);
    fieldPageSpin_->setValue(qBound(0, pageIndex, maxPage));
    fieldXSpin_->setValue(rect.x());
    fieldYSpin_->setValue(rect.y());
    fieldWidthSpin_->setValue(rect.width());
    fieldHeightSpin_->setValue(rect.height());

    placementGroup_->setEnabled(newField);
    placementGroup_->setTitle(newField
                                  ? tr("Field Placement (new signature field)")
                                  : tr("Field Placement (existing field)"));
}

// GitHub #371 — headless read accessor for the certificate path field.
QString SignDialog::pfxPathForTesting() const
{
    return pfxPathEdit_ ? pfxPathEdit_->text() : QString();
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

void SignDialog::onBrowseImage()
{
    QString path = QFileDialog::getOpenFileName(
        this, tr("Select Signature Image"),
        QString(),
        tr("Image Files (*.png *.jpg *.jpeg *.bmp *.gif);;All Files (*)"));

    if (path.isEmpty()) return;

    imagePathEdit_->setText(path);

    // Show a thumbnail of the chosen image so the user sees what will be
    // embedded. A load failure leaves the path set (signField/preview will
    // surface any real decode error) but swaps the thumbnail for a note.
    QPixmap pix(path);
    if (!pix.isNull()) {
        imagePreviewLabel_->setPixmap(
            pix.scaled(imagePreviewLabel_->width() > 0
                           ? imagePreviewLabel_->width()
                           : 200,
                       48, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    } else {
        imagePreviewLabel_->setText(tr("Could not preview image"));
    }
}

// GitHub #362 — rebuild the re-select combo from the library. Index 0 is a
// non-selectable placeholder so "no choice" is a distinct state; the saved
// names follow in library (insertion) order.
void SignDialog::refreshSavedImageCombo()
{
    if (!savedImageCombo_) return;
    const bool wasBlocked = savedImageCombo_->blockSignals(true);
    savedImageCombo_->clear();
    savedImageCombo_->addItem(tr("Saved signatures..."));
    if (signatureImages_) {
        savedImageCombo_->addItems(signatureImages_->signatureImageNames());
    }
    savedImageCombo_->setCurrentIndex(0);
    savedImageCombo_->blockSignals(wasBlocked);
}

// GitHub #362 — save the currently-browsed signature image into the persisted
// library under a user-supplied name. Production asks for the name via a modal
// QInputDialog; a headless test injects it via setSaveImageNameForTesting().
void SignDialog::onSaveImageToLibrary()
{
    const QString path = imagePathEdit_->text();
    if (path.isEmpty()) {
        validationLabel_->setText(
            tr("Browse for a signature image before saving it."));
        return;
    }
    QImage img(path);
    if (img.isNull()) {
        validationLabel_->setText(tr("Could not load the signature image."));
        return;
    }

    QString name;
    if (saveImageNameForTesting_) {
        name = *saveImageNameForTesting_;  // headless seam: skip the modal
    } else {
        bool ok = false;
        name = QInputDialog::getText(
            this, tr("Save Signature Image"),
            tr("Name for this signature image:"), QLineEdit::Normal,
            QString(), &ok);
        if (!ok) return;  // user cancelled
    }

    name = name.trimmed();
    if (name.isEmpty()) {
        validationLabel_->setText(tr("A name is required to save the image."));
        return;
    }

    if (!signatureImages_) {
        signatureImages_ =
            std::make_unique<pdfe::annotations::SignatureImageLibrary>();
    }
    // Same name overwrites in place (no duplicate); then persist immediately so
    // the image survives to the next session.
    //
    // GitHub #371 — record the PFX path currently in the certificate field
    // alongside the image, so re-selecting this entry later can restore the
    // certificate too. ONLY the path: the passphrase is never persisted.
    signatureImages_->addSignatureImage(name, img, pfxPathEdit_->text());
    signatureImages_->saveToSettings();
    refreshSavedImageCombo();

    // Leave the combo pointing at the freshly-saved entry for feedback.
    const int idx = savedImageCombo_->findText(name);
    if (idx >= 0) savedImageCombo_->setCurrentIndex(idx);
    validationLabel_->setText(tr("Signature image saved."));
}

// GitHub #362 — re-select a previously saved signature image. The library
// stores a QImage (no source path survives across sessions), so materialise it
// to a temp PNG and point imagePathEdit_ at that file: the existing
// imagePath-based signing path then works unchanged. Also refresh the preview.
void SignDialog::onSelectSavedImage(int index)
{
    if (index <= 0 || !signatureImages_) return;  // 0 is the placeholder
    const QString name = savedImageCombo_->itemText(index);
    const QImage img = signatureImages_->signatureImage(name);
    if (img.isNull()) return;

    auto tmp = std::make_shared<QTemporaryFile>(
        QDir(QDir::tempPath()).filePath(
            QStringLiteral("pdfe_sigimg_XXXXXX.png")));
    tmp->setAutoRemove(true);
    if (!tmp->open() || !img.save(tmp->fileName(), "PNG")) {
        validationLabel_->setText(
            tr("Could not prepare the saved signature image."));
        return;
    }
    const QString tmpPath = tmp->fileName();
    // GitHub #374 — keep the temp file alive for the dialog's lifetime AND
    // beyond it: signingOptions() copies this shared_ptr into
    // SigningOptions::imageKeepalive, so the PNG survives until the signing
    // flow (which runs AFTER the dialog is destroyed) has read it. autoRemove
    // still deletes it once the last holder releases it.
    selectedImageTempFile_ = std::move(tmp);

    imagePathEdit_->setText(tmpPath);

    // GitHub #371 — restore the PFX path saved with this entry, but ONLY when
    // one was recorded AND the file still exists. A stale path (certificate
    // moved/deleted since the save) leaves the field untouched rather than
    // populating it with a path the probe is guaranteed to reject. The
    // passphrase was never persisted, so the user still types that.
    const QString savedPfx = signatureImages_->signaturePfxPath(name);
    if (!savedPfx.isEmpty() && QFile::exists(savedPfx)) {
        pfxPathEdit_->setText(savedPfx);
    }

    QPixmap pix = QPixmap::fromImage(img);
    if (!pix.isNull()) {
        imagePreviewLabel_->setPixmap(
            pix.scaled(imagePreviewLabel_->width() > 0
                           ? imagePreviewLabel_->width()
                           : 200,
                       48, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }
}

// GitHub #362 — headless read accessors (UIA cannot drive the combo modally).
int SignDialog::savedSignatureImageCountForTesting() const
{
    return signatureImages_ ? signatureImages_->count() : 0;
}

bool SignDialog::selectSavedSignatureImageForTesting(const QString& name)
{
    if (!savedImageCombo_) return false;
    const int idx = savedImageCombo_->findText(name);
    if (idx <= 0) return false;
    savedImageCombo_->setCurrentIndex(idx);
    onSelectSavedImage(idx);
    return true;
}

void SignDialog::onCreateCert()
{
    // GitHub #361, DCA 0129. Production opens CreateCertificateDialog, which
    // validates every input and writes the certificate in its own accept(), so
    // a missing Common Name or passphrase is shown IN that form while the user
    // can still fix it. The headless seam below skips the form: it shares the
    // form's validationError() (with the passphrase standing in for its own
    // confirmation, so a mismatch cannot occur), withPfxSuffix() and the
    // create call, but has no overwrite check. Only its empty-CN rejection is
    // tested (CreateSelfSignedCert.SignDialogSeamEmptyCnRejected); the real
    // form's behaviour is tested by CreateCertificateForm.*.
    createCertLastError_.clear();
    QString path;
    QString pass;

    if (createCertTestInputs_) {
        if (!createCertTestInputs_->accepted) {
            return;  // simulate the user cancelling the form
        }
        const auto& in = *createCertTestInputs_;
        createCertLastError_ = CreateCertificateDialog::validationError(
            in.commonName, in.passphrase, in.passphrase, in.outputPath);
        if (!createCertLastError_.isEmpty()) {
            createCertStatusLabel_->setText(createCertLastError_);
            return;
        }
        path = CreateCertificateDialog::withPfxSuffix(in.outputPath);
        pdfe::signatures::Pkcs7OpenSslBridge::SelfSignedCertOptions opts;
        opts.commonName = in.commonName.trimmed().toStdString();
        opts.organization = in.organization.trimmed().toStdString();
        opts.validityDays = in.validityDays;
        const auto res = pdfe::signatures::Pkcs7OpenSslBridge::createSelfSignedPfx(
            path.toStdString(), in.passphrase.toStdString(), opts);
        if (!res.success) {
            // SECURITY (#361): res.errorMessage is secret-free by contract.
            createCertLastError_ = QString::fromStdString(res.errorMessage);
            createCertStatusLabel_->setText(
                tr("Certificate creation failed: %1").arg(createCertLastError_));
            return;
        }
        pass = in.passphrase;
    } else {
        CreateCertificateDialog form(this);
        if (form.exec() != QDialog::Accepted) {
            return;  // cancelled: nothing was written
        }
        path = form.certificatePath();
        pass = form.passphrase();
    }

    // Populate the existing PFX fields so signingOptions() returns the freshly
    // created certificate and the user can sign immediately.
    pfxPathEdit_->setText(path);
    pfxPasswordEdit_->setText(pass);
    createCertStatusLabel_->setText(
        tr("Created %1. It is filled in above.").arg(QDir::toNativeSeparators(path)));
    validationLabel_->clear();
    createCertLastError_.clear();
}

void SignDialog::validateInput()
{
    QString message;

    if (visualOnlyCheck_ && visualOnlyCheck_->isChecked()) {
        if (imagePathEdit_->text().trimmed().isEmpty())
            message = tr("Choose a signature image for a visual signature.");
    } else if (pfxPathEdit_->text().trimmed().isEmpty()) {
        message = tr("PFX certificate file is required.");
    } else if (pfxPasswordEdit_->text().isEmpty()) {
        message = tr("Certificate password is required.");
    }

    // OK stays enabled at all times — accept() does the real
    // validation so UIA-driven clicks dispatch reliably.  This is
    // only an inline hint label for human users.
    validationLabel_->setText(message);
}

void SignDialog::onVisualOnlyToggled(bool visual)
{
    // The certificate and the cryptographic options do not apply to a visual
    // signature; disable them so the dialog shows what will be used.
    for (QWidget* w : std::initializer_list<QWidget*>{
             pfxPathEdit_, browseButton_, pfxPasswordEdit_, createCertButton_,
             tsaTimestampCheck_, tsaUrlEdit_, invisibleCheck_, certificationCombo_}) {
        if (w) w->setEnabled(!visual);
    }
    visualNoticeLabel_->setVisible(visual);
    okButton_->setText(visual ? tr("Place") : tr("Sign"));
    validateInput();
}

void SignDialog::accept()
{
    if (visualOnlyCheck_->isChecked()) {
        if (imagePathEdit_->text().trimmed().isEmpty()) {
            validationLabel_->setText(
                tr("Choose a signature image for a visual signature."));
            return;
        }
        QDialog::accept();
        return;
    }
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
