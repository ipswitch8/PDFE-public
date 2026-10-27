#include "ui/EncryptionDialog.h"
#include <QVBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QComboBox>
#include <QLineEdit>
#include <QCheckBox>
#include <QPushButton>
#include <QLabel>
#include <QProgressBar>
#include <QDialogButtonBox>
#include <QRegularExpression>
#include "ui/TestDialogSeam.h"

namespace pdfe::ui {

EncryptionDialog::EncryptionDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle("Encryption Settings");
    setObjectName("encryptionDialog");
    setMinimumSize(450, 600);

    auto* mainLayout = new QVBoxLayout(this);

    // Algorithm section
    auto* algorithmLayout = new QFormLayout();
    algorithmCombo_ = new QComboBox(this);
    algorithmCombo_->setObjectName("algorithmCombo");
    algorithmCombo_->setAccessibleName(tr("Algorithm"));
    algorithmCombo_->addItem(tr("None"));
    algorithmCombo_->addItem(tr("RC4 40-bit"));
    algorithmCombo_->addItem(tr("RC4 128-bit"));
    algorithmCombo_->addItem(tr("AES-128"));
    algorithmCombo_->addItem(tr("AES-256"));
    algorithmCombo_->setCurrentIndex(0);
    connect(algorithmCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &EncryptionDialog::onAlgorithmChanged);
    algorithmLayout->addRow("Algorithm:", algorithmCombo_);
    mainLayout->addLayout(algorithmLayout);

    // Password section
    auto* passwordGroup = new QGroupBox("Passwords", this);
    passwordGroup->setObjectName("passwordGroup");
    auto* passwordLayout = new QFormLayout(passwordGroup);

    userPasswordEdit_ = new QLineEdit(this);
    userPasswordEdit_->setObjectName("userPasswordEdit");
    userPasswordEdit_->setAccessibleName(tr("User Password"));
    userPasswordEdit_->setEchoMode(QLineEdit::Password);
    userPasswordEdit_->setEnabled(false); // Initially disabled (algorithm = None)
    connect(userPasswordEdit_, &QLineEdit::textChanged,
            this, &EncryptionDialog::validateInput);
    passwordLayout->addRow("User Password:", userPasswordEdit_);

    userPasswordConfirmEdit_ = new QLineEdit(this);
    userPasswordConfirmEdit_->setObjectName("userPasswordConfirmEdit");
    userPasswordConfirmEdit_->setAccessibleName(tr("Confirm User Password"));
    userPasswordConfirmEdit_->setEchoMode(QLineEdit::Password);
    userPasswordConfirmEdit_->setEnabled(false);
    connect(userPasswordConfirmEdit_, &QLineEdit::textChanged,
            this, &EncryptionDialog::validateInput);
    passwordLayout->addRow("Confirm User Password:", userPasswordConfirmEdit_);

    ownerPasswordEdit_ = new QLineEdit(this);
    ownerPasswordEdit_->setObjectName("ownerPasswordEdit");
    ownerPasswordEdit_->setAccessibleName(tr("Owner Password"));
    ownerPasswordEdit_->setEchoMode(QLineEdit::Password);
    ownerPasswordEdit_->setEnabled(false);
    connect(ownerPasswordEdit_, &QLineEdit::textChanged,
            this, &EncryptionDialog::validateInput);
    passwordLayout->addRow("Owner Password:", ownerPasswordEdit_);

    ownerPasswordConfirmEdit_ = new QLineEdit(this);
    ownerPasswordConfirmEdit_->setObjectName("ownerPasswordConfirmEdit");
    ownerPasswordConfirmEdit_->setAccessibleName(tr("Confirm Owner Password"));
    ownerPasswordConfirmEdit_->setEchoMode(QLineEdit::Password);
    ownerPasswordConfirmEdit_->setEnabled(false);
    connect(ownerPasswordConfirmEdit_, &QLineEdit::textChanged,
            this, &EncryptionDialog::validateInput);
    passwordLayout->addRow("Confirm Owner Password:", ownerPasswordConfirmEdit_);

    // Password strength meter
    auto* strengthLayout = new QHBoxLayout();
    auto* strengthCaption = new QLabel(tr("Password Strength:"), this);
    strengthLayout->addWidget(strengthCaption);

    passwordStrengthBar_ = new QProgressBar(this);
    passwordStrengthBar_->setObjectName("passwordStrengthBar");
    passwordStrengthBar_->setAccessibleName(tr("Password Strength"));
    passwordStrengthBar_->setRange(0, 100);
    passwordStrengthBar_->setValue(0);
    passwordStrengthBar_->setEnabled(false);
    passwordStrengthBar_->setTextVisible(false);
    passwordStrengthBar_->setFixedHeight(16);
    strengthLayout->addWidget(passwordStrengthBar_);

    passwordStrengthLabel_ = new QLabel(this);
    passwordStrengthLabel_->setObjectName("passwordStrengthLabel");
    passwordStrengthLabel_->setAccessibleName(tr("Password Strength Level"));
    passwordStrengthLabel_->setMinimumWidth(60);
    strengthLayout->addWidget(passwordStrengthLabel_);

    passwordLayout->addRow(strengthLayout);

    connect(ownerPasswordEdit_, &QLineEdit::textChanged,
            this, &EncryptionDialog::updatePasswordStrength);

    mainLayout->addWidget(passwordGroup);

    // Permission section
    auto* permissionGroup = new QGroupBox("Permissions", this);
    permissionGroup->setObjectName("permissionGroup");
    auto* permissionLayout = new QVBoxLayout(permissionGroup);

    printCheck_ = new QCheckBox("Allow printing", this);
    printCheck_->setObjectName("printCheck");
    printCheck_->setAccessibleName(tr("Allow printing"));
    printCheck_->setChecked(true);
    printCheck_->setEnabled(false); // Initially disabled
    permissionLayout->addWidget(printCheck_);

    printHqCheck_ = new QCheckBox("Allow high-quality printing", this);
    printHqCheck_->setObjectName("printHqCheck");
    printHqCheck_->setAccessibleName(tr("Allow high-quality printing"));
    printHqCheck_->setChecked(true);
    printHqCheck_->setEnabled(false);
    permissionLayout->addWidget(printHqCheck_);

    modifyCheck_ = new QCheckBox("Allow modification", this);
    modifyCheck_->setObjectName("modifyCheck");
    modifyCheck_->setAccessibleName(tr("Allow modification"));
    modifyCheck_->setChecked(true);
    modifyCheck_->setEnabled(false);
    permissionLayout->addWidget(modifyCheck_);

    copyCheck_ = new QCheckBox("Allow copying", this);
    copyCheck_->setObjectName("copyCheck");
    copyCheck_->setAccessibleName(tr("Allow copying"));
    copyCheck_->setChecked(true);
    copyCheck_->setEnabled(false);
    permissionLayout->addWidget(copyCheck_);

    annotateCheck_ = new QCheckBox("Allow annotations", this);
    annotateCheck_->setObjectName("annotateCheck");
    annotateCheck_->setAccessibleName(tr("Allow annotations"));
    annotateCheck_->setChecked(true);
    annotateCheck_->setEnabled(false);
    permissionLayout->addWidget(annotateCheck_);

    formFillCheck_ = new QCheckBox("Allow form filling", this);
    formFillCheck_->setObjectName("formFillCheck");
    formFillCheck_->setAccessibleName(tr("Allow form filling"));
    formFillCheck_->setChecked(true);
    formFillCheck_->setEnabled(false);
    permissionLayout->addWidget(formFillCheck_);

    assembleCheck_ = new QCheckBox("Allow assembly", this);
    assembleCheck_->setObjectName("assembleCheck");
    assembleCheck_->setAccessibleName(tr("Allow assembly"));
    assembleCheck_->setChecked(true);
    assembleCheck_->setEnabled(false);
    permissionLayout->addWidget(assembleCheck_);

    accessibilityCheck_ = new QCheckBox("Allow accessibility", this);
    accessibilityCheck_->setObjectName("accessibilityCheck");
    accessibilityCheck_->setAccessibleName(tr("Allow accessibility"));
    accessibilityCheck_->setChecked(true);
    accessibilityCheck_->setEnabled(false);
    permissionLayout->addWidget(accessibilityCheck_);

    mainLayout->addWidget(permissionGroup);

    // Options section
    encryptMetadataCheck_ = new QCheckBox("Encrypt metadata", this);
    encryptMetadataCheck_->setObjectName("encryptMetadataCheck");
    encryptMetadataCheck_->setAccessibleName(tr("Encrypt metadata"));
    encryptMetadataCheck_->setChecked(true);
    encryptMetadataCheck_->setEnabled(false); // Initially disabled
    mainLayout->addWidget(encryptMetadataCheck_);

    // F24 — owner-only-protection warning. This is the POINT OF SETTING: it
    // appears the instant the user has an owner password typed and the user
    // password still empty, while the intent can still be corrected. Before
    // F24 the only place this was said was the Document Permissions dialog,
    // which nothing prompts the user to open — a warning nobody sees is not an
    // effective control.
    emptyUserPasswordWarningLabel_ = new QLabel(this);
    emptyUserPasswordWarningLabel_->setObjectName("emptyUserPasswordWarningLabel");
    emptyUserPasswordWarningLabel_->setAccessibleName(
        tr("Empty user password warning"));
    emptyUserPasswordWarningLabel_->setStyleSheet(
        "QLabel { color: #b36b00; font-weight: bold; }");
    emptyUserPasswordWarningLabel_->setWordWrap(true);
    emptyUserPasswordWarningLabel_->setVisible(false);
    mainLayout->addWidget(emptyUserPasswordWarningLabel_);

    // Validation label
    validationLabel_ = new QLabel(this);
    validationLabel_->setObjectName("validationLabel");
    validationLabel_->setStyleSheet("QLabel { color: red; }");
    validationLabel_->setWordWrap(true);
    mainLayout->addWidget(validationLabel_);

    // Button box
    auto* buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttonBox->setObjectName("buttonBox");
    okButton_ = buttonBox->button(QDialogButtonBox::Ok);
    okButton_->setObjectName("okButton");
    connect(buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    mainLayout->addWidget(buttonBox);

    // Initial validation
    validateInput();

    // Register with the dialog-introspection seam so tests can observe
    // this dialog while its modal exec() loop runs (UIA cannot enumerate
    // it). MUST stay last: the seam snapshots the child inventory.
    pdfe::ui::testseam::registerDialogIntrospection(this);
}

void EncryptionDialog::seedFromExistingEncryption(
    const pdfe::encryption::EncryptionInfo& info)
{
    // GitHub #180. `metadataEncrypted` only means something when the document
    // actually has an /Encrypt dictionary; on an unencrypted document the field
    // is EncryptionInfo's default and reading it as a seed would be inventing
    // an observation. Seeding from the document is the whole point, so this
    // distinction matters: the fallback is the conservative "encrypt", not
    // whatever the struct happened to be initialised to.
    const bool seed = info.isEncrypted ? info.metadataEncrypted : true;
    seededEncryptMetadata_ = seed;
    encryptMetadataCheck_->setChecked(seed);
}

bool EncryptionDialog::encryptMetadataOverridden() const
{
    return encryptMetadataCheck_->isChecked() != seededEncryptMetadata_;
}

void EncryptionDialog::onAlgorithmChanged(int index)
{
    bool enableFields = (index != 0);

    // Enable/disable password fields
    userPasswordEdit_->setEnabled(enableFields);
    userPasswordConfirmEdit_->setEnabled(enableFields);
    ownerPasswordEdit_->setEnabled(enableFields);
    ownerPasswordConfirmEdit_->setEnabled(enableFields);

    // Enable/disable permission checkboxes
    printCheck_->setEnabled(enableFields);
    printHqCheck_->setEnabled(enableFields);
    modifyCheck_->setEnabled(enableFields);
    copyCheck_->setEnabled(enableFields);
    annotateCheck_->setEnabled(enableFields);
    formFillCheck_->setEnabled(enableFields);
    assembleCheck_->setEnabled(enableFields);
    accessibilityCheck_->setEnabled(enableFields);

    // Enable/disable metadata checkbox
    encryptMetadataCheck_->setEnabled(enableFields);

    // Clear password fields when switching to None
    if (!enableFields) {
        userPasswordEdit_->clear();
        userPasswordConfirmEdit_->clear();
        ownerPasswordEdit_->clear();
        ownerPasswordConfirmEdit_->clear();
    }

    validateInput();
}

void EncryptionDialog::updateEmptyUserPasswordWarning()
{
    // Build the settings the dialog would hand back RIGHT NOW and ask the one
    // predicate that owns this condition (EncryptionSettings, so the dialog,
    // the save report and the permissions dialog cannot drift apart).
    pdfe::encryption::EncryptionSettings probe;
    probe.algorithm = (algorithmCombo_->currentIndex() == 0)
                          ? pdfe::encryption::EncryptionAlgorithm::None
                          : pdfe::encryption::EncryptionAlgorithm::AES_256;
    probe.userPassword = userPasswordEdit_->text().toStdString();
    probe.ownerPassword = ownerPasswordEdit_->text().toStdString();

    const bool warn = probe.isOwnerOnlyProtection();
    emptyUserPasswordWarningLabel_->setText(
        warn ? tr(pdfe::encryption::emptyUserPasswordWarningText())
             : QString());
    emptyUserPasswordWarningLabel_->setVisible(warn);
}

void EncryptionDialog::validateInput()
{
    QString errorMessage;
    bool isValid = true;

    updateEmptyUserPasswordWarning();

    // If algorithm is None, always valid
    if (algorithmCombo_->currentIndex() == 0) {
        validationLabel_->clear();
        okButton_->setEnabled(true);
        return;
    }

    if (algorithmCombo_->currentIndex() != 0) {
        // Pipeline 3e9b1d7c phase-6 (E4 / P-GAP-G4-v2): enforce minimum
        // password length at the Qt layer. Disabling the OK button (not
        // just showing a strength label) prevents weak passwords from
        // being committed to a saved encrypted PDF. 8 chars matches the
        // score threshold in updatePasswordStrength() (>= 8 → +20).
        //
        // Pipeline a7c4f891 phase-1 (A2 / Unicode length gap): we
        // intentionally use QString::length() (UTF-16 code-unit count)
        // rather than toUtf8().size() (byte count) for two reasons:
        //   1. The PDF encryption backends (MuPDF + QPDF) operate on
        //      the raw password bytes — so a 4-char CJK password
        //      (12 UTF-8 bytes) IS more entropy than 8 ASCII chars.
        //      Allowing it through is correct.
        //   2. UI-user expectation is "minimum 8 characters", not
        //      "minimum 8 bytes" — counting code units matches the
        //      user mental model.
        // Edge case: 4-char emoji password counts as 4 code units (or
        // 8 surrogate-pair code units depending on the emoji). This
        // is acceptable — emoji passwords are unusual and the user
        // who types them is making an informed choice.
        constexpr int kMinPasswordLength = 8;

        // User password must not be empty (P12-INFRA-1 sub-issue 2 contract).
        if (userPasswordEdit_->text().isEmpty()) {
            errorMessage = "User password is required";
            isValid = false;
        }
        // User password must meet minimum length
        else if (userPasswordEdit_->text().length() < kMinPasswordLength) {
            errorMessage = QString("User password must be at least %1 characters")
                              .arg(kMinPasswordLength);
            isValid = false;
        }
        // Owner password must not be empty
        else if (ownerPasswordEdit_->text().isEmpty()) {
            errorMessage = "Owner password is required";
            isValid = false;
        }
        // Owner password must meet minimum length
        else if (ownerPasswordEdit_->text().length() < kMinPasswordLength) {
            errorMessage = QString("Owner password must be at least %1 characters")
                              .arg(kMinPasswordLength);
            isValid = false;
        }
        // User password must match confirm field
        else if (userPasswordEdit_->text() != userPasswordConfirmEdit_->text()) {
            errorMessage = "User passwords do not match";
            isValid = false;
        }
        // Owner password must match confirm field
        else if (ownerPasswordEdit_->text() != ownerPasswordConfirmEdit_->text()) {
            errorMessage = "Owner passwords do not match";
            isValid = false;
        }
    }

    validationLabel_->setText(errorMessage);
    okButton_->setEnabled(isValid);
}

pdfe::encryption::EncryptionSettings EncryptionDialog::settings() const
{
    pdfe::encryption::EncryptionSettings settings;

    switch (algorithmCombo_->currentIndex()) {
        case 1: settings.algorithm = pdfe::encryption::EncryptionAlgorithm::RC4_40; break;
        case 2: settings.algorithm = pdfe::encryption::EncryptionAlgorithm::RC4_128; break;
        case 3: settings.algorithm = pdfe::encryption::EncryptionAlgorithm::AES_128; break;
        case 4: settings.algorithm = pdfe::encryption::EncryptionAlgorithm::AES_256; break;
        default: settings.algorithm = pdfe::encryption::EncryptionAlgorithm::None; break;
    }

    // Set passwords (only if algorithm is not None)
    if (settings.algorithm != pdfe::encryption::EncryptionAlgorithm::None) {
        settings.userPassword = userPasswordEdit_->text().toStdString();
        settings.ownerPassword = ownerPasswordEdit_->text().toStdString();

        // Set permissions
        settings.permissions.print = printCheck_->isChecked();
        settings.permissions.printHighQuality = printHqCheck_->isChecked();
        settings.permissions.modify = modifyCheck_->isChecked();
        settings.permissions.copy = copyCheck_->isChecked();
        settings.permissions.annotate = annotateCheck_->isChecked();
        settings.permissions.formFill = formFillCheck_->isChecked();
        settings.permissions.assemble = assembleCheck_->isChecked();
        settings.permissions.accessibility = accessibilityCheck_->isChecked();

        // Set metadata encryption
        settings.encryptMetadata = encryptMetadataCheck_->isChecked();
    }

    return settings;
}

void EncryptionDialog::updatePasswordStrength()
{
    const QString pw = ownerPasswordEdit_->text();
    int score = 0;

    if (pw.length() >= 8) score += 20;
    if (pw.length() >= 12) score += 10;
    if (pw.length() >= 16) score += 10;
    if (pw.contains(QRegularExpression("[a-z]"))) score += 10;
    if (pw.contains(QRegularExpression("[A-Z]"))) score += 10;
    if (pw.contains(QRegularExpression("[0-9]"))) score += 15;
    if (pw.contains(QRegularExpression("[^a-zA-Z0-9]"))) score += 25;

    score = qBound(0, score, 100);
    passwordStrengthBar_->setValue(score);

    QString label;
    QString color;
    if (score < 30) {
        label = tr("Weak");
        color = "red";
    } else if (score < 60) {
        label = tr("Fair");
        color = "orange";
    } else if (score < 80) {
        label = tr("Good");
        color = "green";
    } else {
        label = tr("Strong");
        color = "darkgreen";
    }

    passwordStrengthLabel_->setText(label);
    passwordStrengthLabel_->setStyleSheet(
        QStringLiteral("QLabel { color: %1; font-weight: bold; }").arg(color));
    passwordStrengthBar_->setStyleSheet(
        QStringLiteral("QProgressBar::chunk { background-color: %1; }").arg(color));
}

} // namespace pdfe::ui
