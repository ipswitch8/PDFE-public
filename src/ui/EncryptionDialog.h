#pragma once
#include "encryption/EncryptionSettings.h"
#include <QDialog>

class QComboBox;
class QLineEdit;
class QCheckBox;
class QPushButton;
class QLabel;
class QProgressBar;

namespace pdfe::ui {

class EncryptionDialog : public QDialog {
    Q_OBJECT
public:
    explicit EncryptionDialog(QWidget* parent = nullptr);
    ~EncryptionDialog() override = default;

    /// Get the encryption settings from the dialog.
    pdfe::encryption::EncryptionSettings settings() const;

    /// GitHub #180 — seed the "Encrypt metadata" checkbox from the document's
    /// EXISTING `/EncryptMetadata` state.
    ///
    /// Before this existed the checkbox was hardcoded checked at construction,
    /// so re-saving a document that had metadata encryption OFF silently turned
    /// it back ON. That is a confidentiality change the user never asked for,
    /// in whichever direction it happens to run, and nothing in the UI said it
    /// had happened. The dialog now OPENS on the document's own answer.
    ///
    /// Only an already-encrypted document carries a meaningful
    /// `/EncryptMetadata`. For an unencrypted one `EncryptionInfo::
    /// metadataEncrypted` is the struct's default rather than an observation,
    /// so it is not treated as a seed — the conservative default (encrypt)
    /// stands.
    ///
    /// The user remains free to change the box afterwards; `settings()` reads
    /// the live checkbox, so an override always wins over the seed.
    void seedFromExistingEncryption(const pdfe::encryption::EncryptionInfo& info);

    /// GitHub #180 — the value the checkbox was SEEDED with, independent of any
    /// later user change. Exists so a test can pin the seed and the override as
    /// two separate properties rather than reading one through the other.
    bool seededEncryptMetadata() const { return seededEncryptMetadata_; }

    /// GitHub #180 — true when the live checkbox disagrees with the seed, i.e.
    /// the user deliberately changed the document's existing state.
    bool encryptMetadataOverridden() const;

private slots:
    void onAlgorithmChanged(int index);
    void validateInput();
    void updatePasswordStrength();

private:
    /// F24 — recompute the owner-only-protection warning from the live field
    /// contents. Called from validateInput(), which every password field's
    /// textChanged is connected to, so the warning tracks typing.
    void updateEmptyUserPasswordWarning();

    QComboBox* algorithmCombo_ = nullptr;
    QLineEdit* userPasswordEdit_ = nullptr;
    QLineEdit* userPasswordConfirmEdit_ = nullptr;
    QLineEdit* ownerPasswordEdit_ = nullptr;
    QLineEdit* ownerPasswordConfirmEdit_ = nullptr;
    QCheckBox* printCheck_ = nullptr;
    QCheckBox* printHqCheck_ = nullptr;
    QCheckBox* modifyCheck_ = nullptr;
    QCheckBox* copyCheck_ = nullptr;
    QCheckBox* annotateCheck_ = nullptr;
    QCheckBox* formFillCheck_ = nullptr;
    QCheckBox* assembleCheck_ = nullptr;
    QCheckBox* accessibilityCheck_ = nullptr;
    QCheckBox* encryptMetadataCheck_ = nullptr;
    /// GitHub #180 — the seed the checkbox was last set from. Defaults to the
    /// conservative "encrypt" so a dialog that is never seeded behaves exactly
    /// as it did before.
    bool seededEncryptMetadata_ = true;
    QPushButton* okButton_ = nullptr;
    QLabel* validationLabel_ = nullptr;
    /// F24 — owner-only-protection warning, shown live at the moment the user
    /// has typed an owner password and left the user password empty.
    /// objectName "emptyUserPasswordWarningLabel" for UI automation.
    QLabel* emptyUserPasswordWarningLabel_ = nullptr;
    QProgressBar* passwordStrengthBar_ = nullptr;
    QLabel* passwordStrengthLabel_ = nullptr;
};

} // namespace pdfe::ui
