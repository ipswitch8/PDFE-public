#pragma once

#include <QDialog>
#include <QString>
#include <memory>
#include <optional>
#include <string>

class QLineEdit;
class QCheckBox;
class QComboBox;
class QPushButton;
class QLabel;
class QTemporaryFile;

namespace pdfe::signatures {
struct SigningOptions;
}

namespace pdfe::annotations {
class SignatureImageLibrary;  // GitHub #362
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

    /// GitHub #361 — headless seam for the "Create certificate..." flow.
    ///
    /// onCreateCert() normally opens a modal subject/validity/passphrase entry
    /// dialog and a Save-As modal to choose where to write the .pfx. Neither can
    /// be driven from a GoogleTest (UIA cannot enumerate a Qt modal exec()). When
    /// these inputs are set, onCreateCert() SKIPS both modals and uses the
    /// injected values, then runs the REAL
    /// Pkcs7OpenSslBridge::createSelfSignedPfx + PFX-field-population path. Inert
    /// in production: unset by default, mutated only by *ForTesting() below.
    struct CreateCertTestInputs {
        bool accepted = true;      ///< false => simulate cancelling the entry dialog
        QString commonName;        ///< Subject CN
        QString organization;      ///< Subject O (optional)
        int validityDays = 365;    ///< Validity period in days
        QString passphrase;        ///< Dummy test passphrase for the throwaway PFX
        QString outputPath;        ///< Where to write the .pfx (a QTemporaryDir in tests)
    };
    void setCreateCertTestInputsForTesting(
        std::optional<CreateCertTestInputs> inputs) {
        createCertTestInputs_ = std::move(inputs);
    }

    /// GitHub #361 — last create-cert error (secret-free), for tests to assert
    /// the rejection path without depending on the inline label string. Empty
    /// after a successful create.
    QString lastCreateCertErrorForTesting() const { return createCertLastError_; }

    /// GitHub #362 — headless seam for the "Save signature image..." flow.
    ///
    /// onSaveImageToLibrary() normally opens a modal QInputDialog to ask for
    /// the name to store the currently-browsed signature image under (UIA
    /// cannot enumerate a Qt modal exec()). When this is set, that modal is
    /// SKIPPED and the injected name is used, then the REAL
    /// SignatureImageLibrary save + combo-refresh path runs. Inert in
    /// production: unset by default, mutated only by the setter below.
    void setSaveImageNameForTesting(std::optional<QString> name) {
        saveImageNameForTesting_ = std::move(name);
    }

    /// GitHub #362 — read-side test accessors for the saved-signature combo,
    /// so a headless test can assert the re-select path without UIA.
    int savedSignatureImageCountForTesting() const;
    bool selectSavedSignatureImageForTesting(const QString& name);

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
    void onBrowseImage();
    /// GitHub #362 — save the currently-browsed signature image into the
    /// persisted library under a user-supplied name, then refresh the combo.
    void onSaveImageToLibrary();
    /// GitHub #362 — re-select a previously saved signature image from the
    /// combo; repopulates imagePathEdit_ and imagePreviewLabel_.
    void onSelectSavedImage(int index);
    /// GitHub #361 — open the create-self-signed-certificate flow (or, under the
    /// test seam, run it headlessly) and populate the PFX path + password fields.
    void onCreateCert();
    void validateInput();

private:
    QLineEdit* pfxPathEdit_ = nullptr;
    QPushButton* browseButton_ = nullptr;

    // GitHub #327 — image-of-signature controls. imagePathEdit_ is
    // read-only (populated via the Browse button's file dialog) and
    // imagePreviewLabel_ shows a thumbnail of the chosen image. Wired
    // into SigningOptions::imagePath in signingOptions().
    QLineEdit* imagePathEdit_ = nullptr;
    QPushButton* imageBrowseButton_ = nullptr;
    QLabel* imagePreviewLabel_ = nullptr;

    // GitHub #362 — persisted signature-image library + its re-select/save
    // controls. savedImageCombo_ lists previously saved names (index 0 is a
    // non-selectable placeholder); saveImageButton_ stores the currently
    // browsed image under a name.
    QComboBox* savedImageCombo_ = nullptr;
    QPushButton* saveImageButton_ = nullptr;
    QLineEdit* pfxPasswordEdit_ = nullptr;
    QPushButton* createCertButton_ = nullptr;  // GitHub #361
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

    // GitHub #361 — headless create-cert seam state (unset in production) and
    // the last secret-free create-cert error.
    std::optional<CreateCertTestInputs> createCertTestInputs_;
    QString createCertLastError_;

    // GitHub #362 — persisted signature-image library, loaded on construction
    // and written on each save. Held by unique_ptr so the header needs only a
    // forward declaration.
    std::unique_ptr<pdfe::annotations::SignatureImageLibrary> signatureImages_;
    // Headless save-name seam (unset in production; see
    // setSaveImageNameForTesting).
    std::optional<QString> saveImageNameForTesting_;
    // Backing temp file for a re-selected library image: the selected QImage
    // is materialised to a temp PNG so the existing imagePath signing path
    // works unchanged. Kept alive for the dialog's lifetime.
    std::unique_ptr<QTemporaryFile> selectedImageTempFile_;

    // GitHub #362 — rebuild savedImageCombo_ from the library contents.
    void refreshSavedImageCombo();
};

} // namespace pdfe::ui
