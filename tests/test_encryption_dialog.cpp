/**
 * @file test_encryption_dialog.cpp
 * @brief Unit tests for EncryptionDialog class.
 */

#include <gtest/gtest.h>
#include "ui/EncryptionDialog.h"
#include "encryption/EncryptionSettings.h"
#include <QApplication>
#include <QComboBox>
#include <QLineEdit>
#include <QCheckBox>
#include <QPushButton>
#include <QLabel>
#include <QProgressBar>
#include <QTest>
#include <memory>

class EncryptionDialogTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Skip tests if QApplication doesn't exist
        if (!qApp) {
            GTEST_SKIP() << "QApplication not available";
        }
    }

    void TearDown() override {
        dialog.reset();
    }

    std::unique_ptr<pdfe::ui::EncryptionDialog> dialog;
};


// Test 1: Constructor and Destructor
TEST_F(EncryptionDialogTest, ConstructDestroy) {
    dialog = std::make_unique<pdfe::ui::EncryptionDialog>(nullptr);
    ASSERT_NE(dialog, nullptr);
    EXPECT_TRUE(dialog->isEnabled());
    dialog.reset();
    SUCCEED();
}


// Test 2: Initial State - None Algorithm
TEST_F(EncryptionDialogTest, InitialState_NoneAlgorithm) {
    dialog = std::make_unique<pdfe::ui::EncryptionDialog>(nullptr);

    QComboBox* algorithmCombo = dialog->findChild<QComboBox*>("algorithmCombo");
    ASSERT_NE(algorithmCombo, nullptr) << "QComboBox with objectName 'algorithmCombo' not found";
    EXPECT_EQ(algorithmCombo->currentIndex(), 0); // None selected

    // Password fields should be disabled
    QLineEdit* userPasswordEdit = dialog->findChild<QLineEdit*>("userPasswordEdit");
    QLineEdit* userPasswordConfirmEdit = dialog->findChild<QLineEdit*>("userPasswordConfirmEdit");
    QLineEdit* ownerPasswordEdit = dialog->findChild<QLineEdit*>("ownerPasswordEdit");
    QLineEdit* ownerPasswordConfirmEdit = dialog->findChild<QLineEdit*>("ownerPasswordConfirmEdit");
    ASSERT_NE(userPasswordEdit, nullptr);
    ASSERT_NE(userPasswordConfirmEdit, nullptr);
    ASSERT_NE(ownerPasswordEdit, nullptr);
    ASSERT_NE(ownerPasswordConfirmEdit, nullptr);
    EXPECT_FALSE(userPasswordEdit->isEnabled());
    EXPECT_FALSE(userPasswordConfirmEdit->isEnabled());
    EXPECT_FALSE(ownerPasswordEdit->isEnabled());
    EXPECT_FALSE(ownerPasswordConfirmEdit->isEnabled());

    // OK button should be enabled (None = no encryption is valid)
    QPushButton* okButton = dialog->findChild<QPushButton*>("okButton");
    ASSERT_NE(okButton, nullptr);
    EXPECT_TRUE(okButton->isEnabled());
}


// Test 3: Algorithm Change Enables Fields
TEST_F(EncryptionDialogTest, AlgorithmChange_EnablesFields) {
    dialog = std::make_unique<pdfe::ui::EncryptionDialog>(nullptr);

    QComboBox* algorithmCombo = dialog->findChild<QComboBox*>("algorithmCombo");
    ASSERT_NE(algorithmCombo, nullptr);

    // Change to AES-256
    algorithmCombo->setCurrentIndex(4);

    // Password fields should be enabled
    QLineEdit* userPasswordEdit = dialog->findChild<QLineEdit*>("userPasswordEdit");
    QLineEdit* ownerPasswordEdit = dialog->findChild<QLineEdit*>("ownerPasswordEdit");
    ASSERT_NE(userPasswordEdit, nullptr);
    ASSERT_NE(ownerPasswordEdit, nullptr);
    EXPECT_TRUE(userPasswordEdit->isEnabled());
    EXPECT_TRUE(ownerPasswordEdit->isEnabled());

    // Permission checkboxes should be enabled
    QCheckBox* printCheck = dialog->findChild<QCheckBox*>("printCheck");
    QCheckBox* modifyCheck = dialog->findChild<QCheckBox*>("modifyCheck");
    ASSERT_NE(printCheck, nullptr);
    ASSERT_NE(modifyCheck, nullptr);
    EXPECT_TRUE(printCheck->isEnabled());
    EXPECT_TRUE(modifyCheck->isEnabled());

    // Metadata checkbox should be enabled
    QCheckBox* encryptMetadataCheck = dialog->findChild<QCheckBox*>("encryptMetadataCheck");
    ASSERT_NE(encryptMetadataCheck, nullptr);
    EXPECT_TRUE(encryptMetadataCheck->isEnabled());
}


// Test 4: User + Owner Password Required (P12-INFRA-1 phase-7 contract)
//
// Phase-7 (pipeline b3f19d72) tightened the EncryptionDialog
// validation contract: for non-None algorithms, BOTH the user
// password AND the owner password must be non-empty (and confirms
// must match) before OK is enabled.  algorithm=None still leaves OK
// enabled (defensible "save without encryption" path).
TEST_F(EncryptionDialogTest, OwnerPassword_Required) {
    dialog = std::make_unique<pdfe::ui::EncryptionDialog>(nullptr);

    QComboBox* algorithmCombo = dialog->findChild<QComboBox*>("algorithmCombo");
    ASSERT_NE(algorithmCombo, nullptr);
    algorithmCombo->setCurrentIndex(4); // AES-256

    QPushButton* okButton = dialog->findChild<QPushButton*>("okButton");
    ASSERT_NE(okButton, nullptr);

    // With no passwords, OK should be disabled
    EXPECT_FALSE(okButton->isEnabled());

    QLineEdit* ownerPasswordEdit = dialog->findChild<QLineEdit*>("ownerPasswordEdit");
    QLineEdit* ownerPasswordConfirmEdit = dialog->findChild<QLineEdit*>("ownerPasswordConfirmEdit");
    QLineEdit* userPasswordEdit = dialog->findChild<QLineEdit*>("userPasswordEdit");
    QLineEdit* userPasswordConfirmEdit = dialog->findChild<QLineEdit*>("userPasswordConfirmEdit");
    ASSERT_NE(ownerPasswordEdit, nullptr);
    ASSERT_NE(ownerPasswordConfirmEdit, nullptr);
    ASSERT_NE(userPasswordEdit, nullptr);
    ASSERT_NE(userPasswordConfirmEdit, nullptr);

    // Setting only the owner password is no longer sufficient under
    // the phase-7 contract.
    // Pipeline 3e9b1d7c phase-6 (E4): bumped from "test123"/"user123"
    // (7 chars) to 10-char values to satisfy the new minimum-length
    // contract enforced by EncryptionDialog::validateInput().
    ownerPasswordEdit->setText("owner12345");
    ownerPasswordConfirmEdit->setText("owner12345");
    EXPECT_FALSE(okButton->isEnabled());

    // Also set the user password — OK should now be enabled.
    userPasswordEdit->setText("user12345");
    userPasswordConfirmEdit->setText("user12345");
    EXPECT_TRUE(okButton->isEnabled());
}


// Test 5: Password Mismatch - User Password
TEST_F(EncryptionDialogTest, PasswordMismatch_UserPassword) {
    dialog = std::make_unique<pdfe::ui::EncryptionDialog>(nullptr);

    QComboBox* algorithmCombo = dialog->findChild<QComboBox*>("algorithmCombo");
    ASSERT_NE(algorithmCombo, nullptr);
    algorithmCombo->setCurrentIndex(4); // AES-256

    // Set matching owner passwords
    // Pipeline a7c4f891 phase-1 (A1): bumped to 10+ char passwords so
    // the test actually reaches the mismatch check rather than failing
    // at the 8-char minimum-length check first (added in 3e9b1d7c
    // phase-6 E4). The original test name says "PasswordMismatch" —
    // we must exercise that branch, not the length branch.
    QLineEdit* ownerPasswordEdit = dialog->findChild<QLineEdit*>("ownerPasswordEdit");
    QLineEdit* ownerPasswordConfirmEdit = dialog->findChild<QLineEdit*>("ownerPasswordConfirmEdit");
    ASSERT_NE(ownerPasswordEdit, nullptr);
    ASSERT_NE(ownerPasswordConfirmEdit, nullptr);
    ownerPasswordEdit->setText("ownerpass12");
    ownerPasswordConfirmEdit->setText("ownerpass12");

    // Set mismatched user passwords (both >= 8 chars so length check
    // passes — the failure must come from the mismatch comparison).
    QLineEdit* userPasswordEdit = dialog->findChild<QLineEdit*>("userPasswordEdit");
    QLineEdit* userPasswordConfirmEdit = dialog->findChild<QLineEdit*>("userPasswordConfirmEdit");
    ASSERT_NE(userPasswordEdit, nullptr);
    ASSERT_NE(userPasswordConfirmEdit, nullptr);
    userPasswordEdit->setText("userpass12");
    userPasswordConfirmEdit->setText("differentpw");

    QPushButton* okButton = dialog->findChild<QPushButton*>("okButton");
    ASSERT_NE(okButton, nullptr);
    EXPECT_FALSE(okButton->isEnabled());

    // Check validation message reflects the MISMATCH branch, not the
    // length branch — locks the actual scenario the test name promises.
    QLabel* validationLabel = dialog->findChild<QLabel*>("validationLabel");
    ASSERT_NE(validationLabel, nullptr);
    EXPECT_FALSE(validationLabel->text().isEmpty());
    EXPECT_TRUE(validationLabel->text().contains("do not match", Qt::CaseInsensitive))
        << "validationLabel must report the mismatch (got: "
        << validationLabel->text().toStdString() << ")";
}


// Test 6: Password Mismatch - Owner Password
TEST_F(EncryptionDialogTest, PasswordMismatch_OwnerPassword) {
    dialog = std::make_unique<pdfe::ui::EncryptionDialog>(nullptr);

    QComboBox* algorithmCombo = dialog->findChild<QComboBox*>("algorithmCombo");
    ASSERT_NE(algorithmCombo, nullptr);
    algorithmCombo->setCurrentIndex(4); // AES-256

    // Pipeline a7c4f891 phase-1 (A1): bumped to 10+ char passwords so
    // the owner password length check passes — the test must exercise
    // the mismatch comparison, not the 8-char minimum-length branch.
    // Also added a user password (matching) so user-field branches don't
    // short-circuit before owner is evaluated.
    QLineEdit* userPasswordEdit = dialog->findChild<QLineEdit*>("userPasswordEdit");
    QLineEdit* userPasswordConfirmEdit = dialog->findChild<QLineEdit*>("userPasswordConfirmEdit");
    ASSERT_NE(userPasswordEdit, nullptr);
    ASSERT_NE(userPasswordConfirmEdit, nullptr);
    userPasswordEdit->setText("userpass12");
    userPasswordConfirmEdit->setText("userpass12");

    // Set mismatched owner passwords (both >= 8 chars).
    QLineEdit* ownerPasswordEdit = dialog->findChild<QLineEdit*>("ownerPasswordEdit");
    QLineEdit* ownerPasswordConfirmEdit = dialog->findChild<QLineEdit*>("ownerPasswordConfirmEdit");
    ASSERT_NE(ownerPasswordEdit, nullptr);
    ASSERT_NE(ownerPasswordConfirmEdit, nullptr);
    ownerPasswordEdit->setText("ownerpass12");
    ownerPasswordConfirmEdit->setText("differentpw");

    QPushButton* okButton = dialog->findChild<QPushButton*>("okButton");
    ASSERT_NE(okButton, nullptr);
    EXPECT_FALSE(okButton->isEnabled());

    // Check validation message reflects the MISMATCH branch.
    QLabel* validationLabel = dialog->findChild<QLabel*>("validationLabel");
    ASSERT_NE(validationLabel, nullptr);
    EXPECT_FALSE(validationLabel->text().isEmpty());
    EXPECT_TRUE(validationLabel->text().contains("do not match", Qt::CaseInsensitive))
        << "validationLabel must report the owner-password mismatch (got: "
        << validationLabel->text().toStdString() << ")";
}


// Test 7: Valid Input - OK Enabled
TEST_F(EncryptionDialogTest, ValidInput_OkEnabled) {
    dialog = std::make_unique<pdfe::ui::EncryptionDialog>(nullptr);

    QComboBox* algorithmCombo = dialog->findChild<QComboBox*>("algorithmCombo");
    ASSERT_NE(algorithmCombo, nullptr);
    algorithmCombo->setCurrentIndex(4); // AES-256

    // Set matching user passwords
    // Pipeline 3e9b1d7c phase-6 (E4): bumped passwords to 10 chars to
    // satisfy the new minimum-length contract in validateInput().
    QLineEdit* userPasswordEdit = dialog->findChild<QLineEdit*>("userPasswordEdit");
    QLineEdit* userPasswordConfirmEdit = dialog->findChild<QLineEdit*>("userPasswordConfirmEdit");
    ASSERT_NE(userPasswordEdit, nullptr);
    ASSERT_NE(userPasswordConfirmEdit, nullptr);
    userPasswordEdit->setText("user12345");
    userPasswordConfirmEdit->setText("user12345");

    // Set matching owner passwords
    QLineEdit* ownerPasswordEdit = dialog->findChild<QLineEdit*>("ownerPasswordEdit");
    QLineEdit* ownerPasswordConfirmEdit = dialog->findChild<QLineEdit*>("ownerPasswordConfirmEdit");
    ASSERT_NE(ownerPasswordEdit, nullptr);
    ASSERT_NE(ownerPasswordConfirmEdit, nullptr);
    ownerPasswordEdit->setText("owner12345");
    ownerPasswordConfirmEdit->setText("owner12345");

    QPushButton* okButton = dialog->findChild<QPushButton*>("okButton");
    ASSERT_NE(okButton, nullptr);
    EXPECT_TRUE(okButton->isEnabled());

    // Validation label should be clear
    QLabel* validationLabel = dialog->findChild<QLabel*>("validationLabel");
    ASSERT_NE(validationLabel, nullptr);
    EXPECT_TRUE(validationLabel->text().isEmpty());
}


// Test 8: Settings Returns Correct Values
TEST_F(EncryptionDialogTest, Settings_ReturnsCorrectValues) {
    dialog = std::make_unique<pdfe::ui::EncryptionDialog>(nullptr);

    // Test None algorithm
    auto settings = dialog->settings();
    EXPECT_EQ(settings.algorithm, pdfe::encryption::EncryptionAlgorithm::None);
    EXPECT_TRUE(settings.userPassword.empty());
    EXPECT_TRUE(settings.ownerPassword.empty());

    // Switch to AES-256 and configure
    QComboBox* algorithmCombo = dialog->findChild<QComboBox*>("algorithmCombo");
    ASSERT_NE(algorithmCombo, nullptr);
    algorithmCombo->setCurrentIndex(4);

    QLineEdit* userPasswordEdit = dialog->findChild<QLineEdit*>("userPasswordEdit");
    QLineEdit* userPasswordConfirmEdit = dialog->findChild<QLineEdit*>("userPasswordConfirmEdit");
    QLineEdit* ownerPasswordEdit = dialog->findChild<QLineEdit*>("ownerPasswordEdit");
    QLineEdit* ownerPasswordConfirmEdit = dialog->findChild<QLineEdit*>("ownerPasswordConfirmEdit");
    ASSERT_NE(userPasswordEdit, nullptr);
    ASSERT_NE(userPasswordConfirmEdit, nullptr);
    ASSERT_NE(ownerPasswordEdit, nullptr);
    ASSERT_NE(ownerPasswordConfirmEdit, nullptr);

    userPasswordEdit->setText("userpass");
    userPasswordConfirmEdit->setText("userpass");
    ownerPasswordEdit->setText("ownerpass");
    ownerPasswordConfirmEdit->setText("ownerpass");

    // Uncheck some permissions
    QCheckBox* printCheck = dialog->findChild<QCheckBox*>("printCheck");
    QCheckBox* modifyCheck = dialog->findChild<QCheckBox*>("modifyCheck");
    QCheckBox* copyCheck = dialog->findChild<QCheckBox*>("copyCheck");
    ASSERT_NE(printCheck, nullptr);
    ASSERT_NE(modifyCheck, nullptr);
    ASSERT_NE(copyCheck, nullptr);

    printCheck->setChecked(false);
    modifyCheck->setChecked(false);

    // Uncheck metadata encryption
    QCheckBox* encryptMetadataCheck = dialog->findChild<QCheckBox*>("encryptMetadataCheck");
    ASSERT_NE(encryptMetadataCheck, nullptr);
    encryptMetadataCheck->setChecked(false);

    // Get settings and verify
    settings = dialog->settings();
    EXPECT_EQ(settings.algorithm, pdfe::encryption::EncryptionAlgorithm::AES_256);
    EXPECT_EQ(settings.userPassword, "userpass");
    EXPECT_EQ(settings.ownerPassword, "ownerpass");
    EXPECT_FALSE(settings.permissions.print);
    EXPECT_FALSE(settings.permissions.modify);
    EXPECT_TRUE(settings.permissions.copy); // Should still be checked
    EXPECT_TRUE(settings.permissions.printHighQuality); // Should still be checked
    EXPECT_FALSE(settings.encryptMetadata);
}


// Test 9: RC4_40 Algorithm Mapping
TEST_F(EncryptionDialogTest, AlgorithmMapping_RC4_40) {
    dialog = std::make_unique<pdfe::ui::EncryptionDialog>(nullptr);

    QComboBox* algorithmCombo = dialog->findChild<QComboBox*>("algorithmCombo");
    ASSERT_NE(algorithmCombo, nullptr);
    algorithmCombo->setCurrentIndex(1); // RC4 40-bit

    auto settings = dialog->settings();
    EXPECT_EQ(settings.algorithm, pdfe::encryption::EncryptionAlgorithm::RC4_40);
}


// Test 10: RC4_128 Algorithm Mapping
TEST_F(EncryptionDialogTest, AlgorithmMapping_RC4_128) {
    dialog = std::make_unique<pdfe::ui::EncryptionDialog>(nullptr);

    QComboBox* algorithmCombo = dialog->findChild<QComboBox*>("algorithmCombo");
    ASSERT_NE(algorithmCombo, nullptr);
    algorithmCombo->setCurrentIndex(2); // RC4 128-bit

    auto settings = dialog->settings();
    EXPECT_EQ(settings.algorithm, pdfe::encryption::EncryptionAlgorithm::RC4_128);
}


// Test 11: AES_128 Algorithm Mapping
TEST_F(EncryptionDialogTest, AlgorithmMapping_AES_128) {
    dialog = std::make_unique<pdfe::ui::EncryptionDialog>(nullptr);

    QComboBox* algorithmCombo = dialog->findChild<QComboBox*>("algorithmCombo");
    ASSERT_NE(algorithmCombo, nullptr);
    algorithmCombo->setCurrentIndex(3); // AES-128

    auto settings = dialog->settings();
    EXPECT_EQ(settings.algorithm, pdfe::encryption::EncryptionAlgorithm::AES_128);
}


// Test 12: Password Strength Thresholds
TEST_F(EncryptionDialogTest, PasswordStrength_Thresholds) {
    dialog = std::make_unique<pdfe::ui::EncryptionDialog>(nullptr);

    QComboBox* algorithmCombo = dialog->findChild<QComboBox*>("algorithmCombo");
    ASSERT_NE(algorithmCombo, nullptr);
    algorithmCombo->setCurrentIndex(4); // AES-256 to enable fields

    QLineEdit* ownerPasswordEdit = dialog->findChild<QLineEdit*>("ownerPasswordEdit");
    QProgressBar* strengthBar = dialog->findChild<QProgressBar*>("passwordStrengthBar");
    QLabel* strengthLabel = dialog->findChild<QLabel*>("passwordStrengthLabel");
    ASSERT_NE(ownerPasswordEdit, nullptr);
    ASSERT_NE(strengthBar, nullptr);
    ASSERT_NE(strengthLabel, nullptr);

    // Empty string -> 0
    ownerPasswordEdit->setText("");
    EXPECT_EQ(strengthBar->value(), 0);

    // Short, weak password -> < 20
    ownerPasswordEdit->setText("ab");
    EXPECT_LT(strengthBar->value(), 20);

    // 8 lowercase chars -> >= 20 (has length + lowercase)
    ownerPasswordEdit->setText("abcdefgh");
    EXPECT_GE(strengthBar->value(), 20);

    // Upper + digit + length >= 8 -> >= 50
    ownerPasswordEdit->setText("Abcdefgh1");
    EXPECT_GE(strengthBar->value(), 50);

    // Strong password with special chars -> >= 80
    ownerPasswordEdit->setText("MyStr0ng!P@ss");
    EXPECT_GE(strengthBar->value(), 80);
}
