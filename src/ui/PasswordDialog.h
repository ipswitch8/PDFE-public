#pragma once
#include <QDialog>
#include <QString>

class QLineEdit;
class QPushButton;

namespace pdfe::ui {

class PasswordDialog : public QDialog {
    Q_OBJECT
public:
    explicit PasswordDialog(QWidget* parent = nullptr);

    /// Wipes the entered password before the dialog goes away. See the
    /// implementation for exactly what that does and does NOT guarantee — this
    /// is a window-narrowing mitigation, not erasure.
    ~PasswordDialog() override;

    QString password() const;

    /// Wipe this dialog's copy of the password and drop the editor's reference
    /// to it. Called from the destructor; exposed so a caller that has finished
    /// with the password can shorten the exposure window without waiting for
    /// destruction, and so it is directly testable.
    ///
    /// @return the number of QChars actually wiped — 0 when there was no
    /// password. **Returned rather than void on purpose:** the first version of
    /// this function contained an unreachable `secureWipe` call, and no test
    /// could see it, because clearing the editor satisfies every observable
    /// assertion whether or not a wipe happened. A non-zero return is the
    /// evidence that the wipe ran.
    ///
    /// PARTIAL BY CONSTRUCTION — read the implementation comment before relying
    /// on this. It does NOT overwrite the editor's own internal buffer (Qt
    /// exposes no writable pointer to it), nor copies a caller took from
    /// password(). It narrows the exposure window; it does not close it.
    int wipePassword();

private slots:
    void onPasswordChanged(const QString& text);

private:
    QLineEdit* passwordEdit_ = nullptr;
    QPushButton* okButton_ = nullptr;
};

} // namespace pdfe::ui
