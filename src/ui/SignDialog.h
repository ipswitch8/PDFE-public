#pragma once

#include <QDialog>
#include <string>

class QLineEdit;
class QCheckBox;
class QComboBox;
class QPushButton;
class QLabel;

namespace pdfe::signatures {
struct SigningOptions;
}

namespace pdfe::ui {

/**
 * @brief Dialog for signing a PDF signature field with a PFX certificate.
 *
 * Provides fields for PFX file path, password, reason, location,
 * and appearance flags. Validates that PFX path is provided and
 * password field is non-empty before enabling OK.
 */
class SignDialog : public QDialog {
    Q_OBJECT

public:
    explicit SignDialog(QWidget* parent = nullptr);
    ~SignDialog() override;

    /// Get the signing options from the dialog.
    pdfe::signatures::SigningOptions signingOptions() const;

public slots:
    /// Override QDialog::accept to validate inputs even when the OK
    /// button is reached via UIA InvokePattern (which may bypass the
    /// enabled-state gate on some Qt6 builds).  P12-INFRA-1 sub-issue
    /// 3: pywinauto UIA SetValue on QLineEdit::Password does not fire
    /// textChanged in this environment, so the okButton_ enabled
    /// state may lag behind the actual field contents.  Validating
    /// in accept() keeps the contract honest regardless of the
    /// button-state signal path.
    void accept() override;

private slots:
    void onBrowsePfx();
    void validateInput();

private:
    QLineEdit* pfxPathEdit_ = nullptr;
    QPushButton* browseButton_ = nullptr;
    QLineEdit* pfxPasswordEdit_ = nullptr;
    QLineEdit* reasonEdit_ = nullptr;
    QLineEdit* locationEdit_ = nullptr;
    QCheckBox* showLabelsCheck_ = nullptr;
    QCheckBox* showNameCheck_ = nullptr;
    QCheckBox* showDateCheck_ = nullptr;
    QCheckBox* showDnCheck_ = nullptr;
    QCheckBox* showLogoCheck_ = nullptr;
    QCheckBox* tsaTimestampCheck_ = nullptr;
    QLineEdit* tsaUrlEdit_ = nullptr;

    // Invisible-signature toggle (no visible appearance stream) and
    // certification-level picker (NotCertification / NoChanges /
    // FormFillOnly / AnnotationsAllowed). See
    // pdfe::signatures::SigningOptions for the field semantics.
    QCheckBox* invisibleCheck_ = nullptr;
    QComboBox* certificationCombo_ = nullptr;

    QLabel* previewLabel_ = nullptr;
    QLabel* validationLabel_ = nullptr;
    QPushButton* okButton_ = nullptr;
    QPushButton* cancelButton_ = nullptr;
};

} // namespace pdfe::ui
