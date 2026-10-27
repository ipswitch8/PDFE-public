#include "ui/PasswordDialog.h"
#include "core/SecureWipe.h"
#include <QVBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QDialogButtonBox>
#include <QPushButton>
#include "ui/TestDialogSeam.h"

namespace pdfe::ui {

PasswordDialog::PasswordDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle("Password Required");
    setObjectName("passwordDialog");
    setAccessibleName(tr("Password Required Dialog"));
    setMinimumSize(350, 150);

    auto* mainLayout = new QVBoxLayout(this);

    // Label
    auto* label = new QLabel("Enter password to open document:", this);
    label->setObjectName("passwordPromptLabel");
    label->setAccessibleName("Password Prompt");
    mainLayout->addWidget(label);

    // Password input
    passwordEdit_ = new QLineEdit(this);
    passwordEdit_->setObjectName("passwordEdit");
    passwordEdit_->setAccessibleName("Password");
    passwordEdit_->setEchoMode(QLineEdit::Password);
    connect(passwordEdit_, &QLineEdit::textChanged, this, &PasswordDialog::onPasswordChanged);
    mainLayout->addWidget(passwordEdit_);

    // Button box
    auto* buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttonBox->setObjectName("buttonBox");
    buttonBox->setAccessibleName(tr("Password Dialog Buttons"));
    connect(buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    mainLayout->addWidget(buttonBox);

    // Get OK button and disable it initially
    okButton_ = buttonBox->button(QDialogButtonBox::Ok);
    okButton_->setObjectName("okButton");
    okButton_->setAccessibleName(tr("OK"));
    okButton_->setEnabled(false);

    // Register with the dialog-introspection seam so tests can observe
    // this dialog while its modal exec() loop runs (UIA cannot enumerate
    // it). MUST stay last: the seam snapshots the child inventory.
    pdfe::ui::testseam::registerDialogIntrospection(this);
}

PasswordDialog::~PasswordDialog()
{
    wipePassword();
}

// ---------------------------------------------------------------------------
// PASSWORD WIPE — phase-11, and an honest statement of its limits.
//
// THE EXPOSURE. CrashHandler's minidump requests MiniDumpWithDataSegs and
// MiniDumpWithIndirectlyReferencedMemory, which capture writable data segments
// and heap reachable from stack/register pointers. A password still living in
// this dialog's QLineEdit at crash time is exactly that shape, so it can be
// written into an unencrypted .dmp. Until now nothing wiped it and the dialog's
// QString simply lived until destruction.
//
// WHAT THIS ACTUALLY DOES, corrected after this phase's security-audit gate
// FAILED the first version. Two things were wrong and the second was worse:
//
//   1. The wipe was DEAD CODE. It cleared the editor first and then read the
//      text back into a local to wipe, so the local was always empty and
//      secureWipe never ran for any input. The tests passed anyway, because
//      they asserted the observable contract (`password().isEmpty()`), which
//      `clear()` alone satisfies.
//   2. The comment claimed that a same-length `setText()` lets "Qt reuse the
//      existing buffer", so the filler would land on the password's own bytes.
//      **QString is copy-on-write and gives no such guarantee.** That was a
//      confident statement about a mechanism that had not been verified — and
//      it is the more dangerous error, because it made an ineffective wipe read
//      as a thorough one.
//
// So, precisely:
//
// IT WIPES the copy this function holds — with `data()` forcing a detach so the
// buffer is uniquely ours to write, and `secureWipe` ensuring the store is not
// elided as dead. Without that, this function would leave a fresh plaintext
// copy in freed heap every time it ran.
//
// IT REDUCES REACHABILITY by clearing the editor. That is the part that bears
// on the actual threat: MiniDumpWithIndirectlyReferencedMemory captures heap
// reachable FROM POINTERS, so once the editor stops pointing at the password's
// buffer, that buffer is no longer part of what the dump walks.
//
// IT DOES NOT overwrite the editor's own internal buffer. No public Qt API
// hands out a writable pointer to a QLineEdit's QString, so the password's
// original bytes are freed unwiped and remain in the heap until reused. Nor
// does it reach intermediate values reallocated while the user typed, copies a
// caller took from password(), or QLineEdit's undo/input-method state.
//
// A complete guarantee needs the password never to enter a QString at all — a
// fixed secure buffer wired through the editor. That is a much larger change
// than this phase and is recorded in DCA 0030 rather than silently skipped.
// This narrows the window; it does not close it.
//
// Returns the number of QChars actually wiped, so "the wipe ran" is OBSERVABLE.
// The dead-code bug above was invisible to a test that only checked the editor
// ended up empty; a test asserting a non-zero return kills that whole class.
// ---------------------------------------------------------------------------
int PasswordDialog::wipePassword()
{
    if (passwordEdit_ == nullptr) return 0;

    // CAPTURE FIRST. The previous version cleared the editor and only THEN read
    // the text back into a local, so the local was always empty and the wipe
    // below was unreachable — dead code that read as a fix. Caught by this
    // phase's own security-audit gate; see DCA 0030.
    QString held = passwordEdit_->text();
    const int len = held.size();

    int wiped = 0;
    if (len > 0) {
        // data() detaches, so this buffer is uniquely ours and may legally be
        // written. secureWipe is optimizer-resistant, so the store cannot be
        // dropped as dead. This wipes THIS copy — which would otherwise be
        // freed with the plaintext intact when the function returns — and is
        // the only buffer in this function we are entitled to write to.
        pdfe::core::secureWipe(held.data(),
                               static_cast<std::size_t>(len) * sizeof(QChar));
        wiped = len;
    }

    // Drop the editor's own reference to the password. This is what actually
    // reduces the crash-dump exposure: MiniDumpWithIndirectlyReferencedMemory
    // captures heap reachable FROM POINTERS, so once the editor no longer
    // points at that buffer it is no longer part of what the dump walks.
    passwordEdit_->clear();
    return wiped;
}

QString PasswordDialog::password() const
{
    return passwordEdit_->text();
}

void PasswordDialog::onPasswordChanged(const QString& text)
{
    okButton_->setEnabled(!text.isEmpty());
}

} // namespace pdfe::ui
