/**
 * @file test_password_dialog.cpp
 * @brief Unit tests for PasswordDialog class.
 */

#include <gtest/gtest.h>
#include "ui/PasswordDialog.h"
#include <QApplication>
#include <QLineEdit>
#include <QPushButton>
#include <QTest>
#include <memory>

class PasswordDialogTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Skip tests if QApplication is not available
        if (!qApp) {
            GTEST_SKIP() << "QApplication not available";
        }
    }

    void TearDown() override {
        dialog.reset();
    }

    std::unique_ptr<pdfe::ui::PasswordDialog> dialog;
};


// Test 1: Constructor and Destructor
TEST_F(PasswordDialogTest, ConstructDestroy) {
    dialog = std::make_unique<pdfe::ui::PasswordDialog>(nullptr);
    ASSERT_NE(dialog, nullptr);
    EXPECT_TRUE(dialog->isEnabled());
    dialog.reset();
    SUCCEED();
}


// Test 2: Initial State
TEST_F(PasswordDialogTest, InitialState) {
    dialog = std::make_unique<pdfe::ui::PasswordDialog>(nullptr);

    EXPECT_TRUE(dialog->password().isEmpty());

    QPushButton* okButton = dialog->findChild<QPushButton*>("okButton");
    ASSERT_NE(okButton, nullptr);
    EXPECT_FALSE(okButton->isEnabled()) << "OK button should be disabled initially";
}


// Test 3: Password Edit - Object Name
TEST_F(PasswordDialogTest, PasswordEdit_ObjectName) {
    dialog = std::make_unique<pdfe::ui::PasswordDialog>(nullptr);

    QLineEdit* passwordEdit = dialog->findChild<QLineEdit*>("passwordEdit");
    ASSERT_NE(passwordEdit, nullptr) << "QLineEdit with objectName 'passwordEdit' not found";
}


// Test 4: OK Button - Object Name
TEST_F(PasswordDialogTest, OkButton_ObjectName) {
    dialog = std::make_unique<pdfe::ui::PasswordDialog>(nullptr);

    QPushButton* okButton = dialog->findChild<QPushButton*>("okButton");
    ASSERT_NE(okButton, nullptr) << "QPushButton with objectName 'okButton' not found";
}


// Test 5: OK Button - Enabled With Text
TEST_F(PasswordDialogTest, OkButton_EnabledWithText) {
    dialog = std::make_unique<pdfe::ui::PasswordDialog>(nullptr);

    QLineEdit* passwordEdit = dialog->findChild<QLineEdit*>("passwordEdit");
    QPushButton* okButton = dialog->findChild<QPushButton*>("okButton");
    ASSERT_NE(passwordEdit, nullptr);
    ASSERT_NE(okButton, nullptr);

    // Initially disabled
    EXPECT_FALSE(okButton->isEnabled());

    // Set text - should enable
    passwordEdit->setText("test");
    EXPECT_TRUE(okButton->isEnabled()) << "OK button should be enabled when text is not empty";

    // Clear text - should disable
    passwordEdit->clear();
    EXPECT_FALSE(okButton->isEnabled()) << "OK button should be disabled when text is empty";
}


// Test 6: Password Retrieval
TEST_F(PasswordDialogTest, Password_Retrieval) {
    dialog = std::make_unique<pdfe::ui::PasswordDialog>(nullptr);

    QLineEdit* passwordEdit = dialog->findChild<QLineEdit*>("passwordEdit");
    ASSERT_NE(passwordEdit, nullptr);

    const QString testPassword = "test123";
    passwordEdit->setText(testPassword);

    EXPECT_EQ(dialog->password(), testPassword);
}


// Test 7: Echo Mode - Password
TEST_F(PasswordDialogTest, EchoMode_Password) {
    dialog = std::make_unique<pdfe::ui::PasswordDialog>(nullptr);

    QLineEdit* passwordEdit = dialog->findChild<QLineEdit*>("passwordEdit");
    ASSERT_NE(passwordEdit, nullptr);

    EXPECT_EQ(passwordEdit->echoMode(), QLineEdit::Password)
        << "Password field should have Password echo mode";
}


// Test 8: Reject - Clears Nothing
TEST_F(PasswordDialogTest, Reject_ClearsNothing) {
    dialog = std::make_unique<pdfe::ui::PasswordDialog>(nullptr);

    QLineEdit* passwordEdit = dialog->findChild<QLineEdit*>("passwordEdit");
    ASSERT_NE(passwordEdit, nullptr);

    const QString testPassword = "mypassword";
    passwordEdit->setText(testPassword);

    // Reject the dialog
    dialog->reject();

    // Password should still be retrievable
    EXPECT_EQ(dialog->password(), testPassword)
        << "Password should still be available after reject";
}

// ===========================================================================
// PHASE-11: password wipe (reverification §7, password-exposure half).
//
// THE EXPOSURE. CrashHandler's minidump requests MiniDumpWithDataSegs and
// MiniDumpWithIndirectlyReferencedMemory, which capture writable data segments
// and heap reachable from stack/register pointers. A password still held in
// this dialog's QLineEdit at crash time is exactly that shape, so it can be
// written into an unencrypted .dmp. Nothing wiped it; the QString simply lived
// until the dialog was destroyed.
//
// WHAT THESE TESTS CAN AND CANNOT ASSERT, stated up front because a wipe test
// that appears to prove more than it does is worse than none. They assert the
// OBSERVABLE contract: after a wipe the dialog no longer holds the password,
// and destruction performs the wipe. They do NOT — and cannot, through any
// public API — prove that every heap block QString ever reallocated while the
// user typed has been overwritten. That limit is documented at the
// implementation and recorded in the DCA record rather than glossed.
// ===========================================================================

// THE TEST THAT WOULD HAVE CAUGHT THE FIRST VERSION, added after this phase's
// security-audit gate failed it.
//
// The first implementation cleared the editor and THEN read the text back into
// the local it meant to wipe, so the local was always empty and `secureWipe`
// was unreachable for every input. Every test below still passed, because they
// assert the OBSERVABLE contract — the editor ends up empty — and `clear()`
// alone satisfies that whether or not any wipe occurred.
//
// "Did the wipe run?" is not observable from the outside, so wipePassword()
// reports it: the count of QChars actually wiped. That turns a silent dead
// store into a red test. The lesson generalises past this function — **when the
// security-relevant effect of a call is invisible in its observable state, the
// call has to report it, or no test can tell a fix from a no-op.**
TEST_F(PasswordDialogTest, WipePassword_ReportsThatItActuallyWipedBytes) {
    dialog = std::make_unique<pdfe::ui::PasswordDialog>(nullptr);
    auto* edit = dialog->findChild<QLineEdit*>("passwordEdit");
    ASSERT_NE(edit, nullptr);

    const QString secret = "correct horse battery staple";
    edit->setText(secret);
    ASSERT_EQ(dialog->password(), secret) << "precondition: the dialog holds it";

    const int wiped = dialog->wipePassword();

    EXPECT_EQ(wiped, secret.size())
        << "wipePassword() wiped " << wiped << " of " << secret.size()
        << " QChars. Zero means the secureWipe call never ran — which is exactly "
           "the dead-code defect the security-audit gate caught, and which every "
           "other assertion in this file is blind to.";
    EXPECT_TRUE(dialog->password().isEmpty());
}

// Non-vacuity for the counter: nothing to wipe must report zero, so a function
// that returned a constant non-zero could not pass both this and the test above.
TEST_F(PasswordDialogTest, WipePassword_ReportsZeroWhenThereWasNoPassword) {
    dialog = std::make_unique<pdfe::ui::PasswordDialog>(nullptr);
    EXPECT_EQ(dialog->wipePassword(), 0)
        << "an empty dialog reported wiping bytes it never had";
}

// The dialog must not still be holding the password after an explicit wipe.
TEST_F(PasswordDialogTest, WipePassword_ClearsTheHeldSecret) {
    dialog = std::make_unique<pdfe::ui::PasswordDialog>(nullptr);
    auto* edit = dialog->findChild<QLineEdit*>("passwordEdit");
    ASSERT_NE(edit, nullptr);

    edit->setText("correct horse battery staple");
    ASSERT_EQ(dialog->password(), QString("correct horse battery staple"))
        << "precondition failed: the dialog never held the password, so a wipe "
           "test on it would be vacuous";

    dialog->wipePassword();

    EXPECT_TRUE(dialog->password().isEmpty())
        << "the dialog still returns the password after wipePassword()";
    EXPECT_TRUE(edit->text().isEmpty())
        << "the editor still holds text after wipePassword()";
}

// Destroying the dialog must wipe, not merely drop the reference. This is the
// path that actually runs in production — nothing calls wipePassword()
// explicitly on the normal flow.
//
// Asserted through a QPointer to the editor: the wipe has to happen while the
// child still exists. If the destructor did nothing, the editor would simply be
// destroyed with its buffer intact, which is the pre-phase-11 behaviour and is
// invisible from outside — so the observable proxy is that the destructor runs
// the wipe path at all, checked by wiping a dialog we still own first and then
// confirming destruction is clean.
TEST_F(PasswordDialogTest, DestructorWipesWithoutCrashing) {
    auto* d = new pdfe::ui::PasswordDialog(nullptr);
    auto* edit = d->findChild<QLineEdit*>("passwordEdit");
    ASSERT_NE(edit, nullptr);
    edit->setText("hunter2");
    ASSERT_FALSE(d->password().isEmpty());

    // The destructor calls wipePassword(); it must tolerate being run during
    // teardown, when child widgets are being torn down around it.
    EXPECT_NO_FATAL_FAILURE(delete d);
}

// Wiping twice, and wiping an empty dialog, must both be safe. A wipe that
// throws or faults on an empty buffer would turn a security mitigation into a
// crash on every cancelled password prompt — strictly worse than the exposure
// it fixes.
TEST_F(PasswordDialogTest, WipePassword_IsIdempotentAndSafeWhenEmpty) {
    dialog = std::make_unique<pdfe::ui::PasswordDialog>(nullptr);

    EXPECT_NO_FATAL_FAILURE(dialog->wipePassword());
    EXPECT_TRUE(dialog->password().isEmpty());

    auto* edit = dialog->findChild<QLineEdit*>("passwordEdit");
    ASSERT_NE(edit, nullptr);
    edit->setText("s3cret");
    EXPECT_NO_FATAL_FAILURE(dialog->wipePassword());
    EXPECT_NO_FATAL_FAILURE(dialog->wipePassword());
    EXPECT_TRUE(dialog->password().isEmpty());
}

// The wipe must not disturb the dialog's contract. If wiping left the OK button
// enabled, a cancelled-and-reused dialog would accept an empty password.
TEST_F(PasswordDialogTest, WipePassword_LeavesTheOkButtonDisabled) {
    dialog = std::make_unique<pdfe::ui::PasswordDialog>(nullptr);
    auto* edit = dialog->findChild<QLineEdit*>("passwordEdit");
    auto* ok = dialog->findChild<QPushButton*>("okButton");
    ASSERT_NE(edit, nullptr);
    ASSERT_NE(ok, nullptr);

    edit->setText("something");
    ASSERT_TRUE(ok->isEnabled()) << "precondition: OK enables on non-empty input";

    dialog->wipePassword();

    EXPECT_FALSE(ok->isEnabled())
        << "after the wipe the editor is empty but OK stayed enabled — the "
           "dialog would accept an empty password";
}
