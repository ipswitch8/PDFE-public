#pragma once

#include <QDialog>
#include <QString>

class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;

namespace pdfe::ui {

/// The "Create self-signed certificate" form opened from the Sign dialog.
///
/// DCA 0129. Everything the certificate needs is entered here -- subject,
/// validity, passphrase (twice) and where to write the .pfx -- and checked
/// HERE, in accept(), before the form closes. An invalid entry keeps the form
/// open with the reason shown in it (certErrorLabel) and focus on the field to
/// fix. The previous flow accepted any input, asked for a save path in a
/// second modal, and only then rejected an empty Common Name in a label at the
/// bottom of the Sign dialog's scroll area, after the form had gone.
///
/// accept() also writes the certificate, so a write failure is shown in the
/// form too. The written path ends in ".pfx" or ".p12" (see withPfxSuffix).
class CreateCertificateDialog : public QDialog {
    Q_OBJECT

public:
    explicit CreateCertificateDialog(QWidget* parent = nullptr);

    /// After a successful accept(): where the .pfx was written.
    QString certificatePath() const { return writtenPath_; }
    /// After a successful accept(): the passphrase that opens it, so the Sign
    /// dialog can fill its password field. Never logged or stored elsewhere.
    QString passphrase() const;

    /// The first problem with these inputs, or an empty string if they can be
    /// used. Secret-free: it never contains the passphrase.
    static QString validationError(const QString& commonName, const QString& passphrase,
                                   const QString& confirmation, const QString& outputPath);

    /// @p path with ".pfx" appended unless it already ends in .pfx or .p12
    /// (case-insensitive). Empty stays empty.
    static QString withPfxSuffix(const QString& path);

public slots:
    void accept() override;

private slots:
    void onBrowse();

private:
    void showError(const QString& message, QWidget* focus);

    QLineEdit* cnEdit_ = nullptr;
    QLineEdit* orgEdit_ = nullptr;
    QSpinBox* daysSpin_ = nullptr;
    QLineEdit* passEdit_ = nullptr;
    QLineEdit* confirmEdit_ = nullptr;
    QLineEdit* pathEdit_ = nullptr;
    QPushButton* browseButton_ = nullptr;
    QLabel* errorLabel_ = nullptr;
    QPushButton* okButton_ = nullptr;

    /// The path the Browse dialog returned, ONLY when it already carried a
    /// certificate suffix: then the save dialog asked before returning that
    /// exact file, so overwriting it is the user's choice. When the suffix had
    /// to be appended, the dialog confirmed a DIFFERENT name, so this stays
    /// empty and an existing file at the appended name is refused like a
    /// typed path.
    QString browsedPath_;
    QString writtenPath_;
};

} // namespace pdfe::ui
