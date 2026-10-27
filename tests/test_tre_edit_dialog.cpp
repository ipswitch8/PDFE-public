/**
 * @file test_tre_edit_dialog.cpp
 * @brief Unit tests for TreEditDialog class.
 */

#include <gtest/gtest.h>
#include "tre/TreEditDialog.h"
#include "tre/RegionReconstructor.h"
#include "tre/TreTypes.h"
#include <QTextEdit>
#include <QRadioButton>
#include <QDialogButtonBox>
#include <QLabel>
#include <QApplication>
#include <memory>

namespace {

// Helper to build a synthetic ReconstructionResult for testing.
pdfe::tre::ReconstructionResult makeTestResult(float qualityScore, const std::string& ocrText) {
    pdfe::tre::ReconstructionResult result;
    result.success = true;
    result.qualityVerdict.overallScore = qualityScore;
    result.qualityVerdict.recommendedApproach =
        qualityScore >= 0.65f
            ? pdfe::tre::Approach::B_HybridReconstruction
            : pdfe::tre::Approach::C_ImageReplacement;
    result.region.originalText = ocrText;
    result.region.fontMatch.fontFamily = "Arial";
    result.region.fontMatch.confidence = 0.85f;
    return result;
}

} // anonymous namespace


class TreEditDialogTest : public ::testing::Test {
protected:
    void TearDown() override {
        dialog.reset();
    }

    std::unique_ptr<pdfe::tre::TreEditDialog> dialog;
};


// Test 1: Constructor and Destructor
TEST_F(TreEditDialogTest, ConstructAndDestroy) {
    auto result = makeTestResult(0.8f, "Hello World");
    dialog = std::make_unique<pdfe::tre::TreEditDialog>(result, nullptr);
    ASSERT_NE(dialog, nullptr);
    EXPECT_TRUE(dialog->isEnabled());
    dialog.reset();
    SUCCEED();
}


// Test 2: Displays OCR Text
TEST_F(TreEditDialogTest, DisplaysOcrText) {
    const std::string expectedText = "Test OCR text";
    auto result = makeTestResult(0.8f, expectedText);
    dialog = std::make_unique<pdfe::tre::TreEditDialog>(result, nullptr);

    QTextEdit* textEdit = dialog->findChild<QTextEdit*>("treTextEdit");
    ASSERT_NE(textEdit, nullptr) << "QTextEdit with objectName 'treTextEdit' not found";
    EXPECT_EQ(textEdit->toPlainText(), QString::fromStdString(expectedText));
}


// Test 3: Editable Text
TEST_F(TreEditDialogTest, EditableText) {
    auto result = makeTestResult(0.8f, "Original text");
    dialog = std::make_unique<pdfe::tre::TreEditDialog>(result, nullptr);

    QTextEdit* textEdit = dialog->findChild<QTextEdit*>("treTextEdit");
    ASSERT_NE(textEdit, nullptr);

    const QString modifiedText = "Modified text";
    textEdit->setPlainText(modifiedText);

    EXPECT_EQ(dialog->editedText(), modifiedText);
    EXPECT_EQ(textEdit->toPlainText(), modifiedText);
}


// Test 4: Approach B Selected For High Quality
TEST_F(TreEditDialogTest, ApproachBSelectedForHighQuality) {
    auto result = makeTestResult(0.8f, "High quality text");
    dialog = std::make_unique<pdfe::tre::TreEditDialog>(result, nullptr);

    QRadioButton* approachBRadio = dialog->findChild<QRadioButton*>("approachBRadio");
    ASSERT_NE(approachBRadio, nullptr);
    EXPECT_TRUE(approachBRadio->isChecked());
    EXPECT_EQ(dialog->selectedApproach(), pdfe::tre::Approach::B_HybridReconstruction);
}


// Test 5: Approach C Selected For Low Quality
TEST_F(TreEditDialogTest, ApproachCSelectedForLowQuality) {
    auto result = makeTestResult(0.3f, "Low quality text");
    dialog = std::make_unique<pdfe::tre::TreEditDialog>(result, nullptr);

    QRadioButton* approachCRadio = dialog->findChild<QRadioButton*>("approachCRadio");
    ASSERT_NE(approachCRadio, nullptr);
    EXPECT_TRUE(approachCRadio->isChecked());
    EXPECT_EQ(dialog->selectedApproach(), pdfe::tre::Approach::C_ImageReplacement);
}


// Test 6: Accept Returns Edited Text
TEST_F(TreEditDialogTest, AcceptReturnsEditedText) {
    auto result = makeTestResult(0.7f, "Original");
    dialog = std::make_unique<pdfe::tre::TreEditDialog>(result, nullptr);

    QTextEdit* textEdit = dialog->findChild<QTextEdit*>("treTextEdit");
    ASSERT_NE(textEdit, nullptr);

    const QString editedContent = "Edited";
    textEdit->setPlainText(editedContent);

    EXPECT_EQ(dialog->editedText(), editedContent);
    // Verify consistency on repeated calls
    EXPECT_EQ(dialog->editedText(), editedContent);
}


// Test 7: Boundary Quality Score - Just Above Threshold
TEST_F(TreEditDialogTest, BoundaryQualityScoreAboveThreshold) {
    auto result = makeTestResult(0.66f, "Boundary test");
    dialog = std::make_unique<pdfe::tre::TreEditDialog>(result, nullptr);

    QRadioButton* approachBRadio = dialog->findChild<QRadioButton*>("approachBRadio");
    ASSERT_NE(approachBRadio, nullptr);
    EXPECT_TRUE(approachBRadio->isChecked());
    EXPECT_EQ(dialog->selectedApproach(), pdfe::tre::Approach::B_HybridReconstruction);
}


// Test 8: Boundary Quality Score - Just Below Threshold
TEST_F(TreEditDialogTest, BoundaryQualityScoreBelowThreshold) {
    auto result = makeTestResult(0.64f, "Boundary test");
    dialog = std::make_unique<pdfe::tre::TreEditDialog>(result, nullptr);

    QRadioButton* approachCRadio = dialog->findChild<QRadioButton*>("approachCRadio");
    ASSERT_NE(approachCRadio, nullptr);
    EXPECT_TRUE(approachCRadio->isChecked());
    EXPECT_EQ(dialog->selectedApproach(), pdfe::tre::Approach::C_ImageReplacement);
}


// Test 9: Empty OCR Text
TEST_F(TreEditDialogTest, EmptyOcrText) {
    auto result = makeTestResult(0.5f, "");
    dialog = std::make_unique<pdfe::tre::TreEditDialog>(result, nullptr);

    QTextEdit* textEdit = dialog->findChild<QTextEdit*>("treTextEdit");
    ASSERT_NE(textEdit, nullptr);
    EXPECT_TRUE(textEdit->toPlainText().isEmpty());
    EXPECT_TRUE(dialog->editedText().isEmpty());
}


// Test 10: Special Characters in OCR Text
TEST_F(TreEditDialogTest, SpecialCharactersInOcrText) {
    const std::string specialText = "Special: \xe2\x82\xac, \xc3\xb1, \xe4\xb8\xad\xe6\x96\x87, \n\t\"quotes\"";
    auto result = makeTestResult(0.75f, specialText);
    dialog = std::make_unique<pdfe::tre::TreEditDialog>(result, nullptr);

    QTextEdit* textEdit = dialog->findChild<QTextEdit*>("treTextEdit");
    ASSERT_NE(textEdit, nullptr);
    EXPECT_EQ(textEdit->toPlainText(), QString::fromStdString(specialText));
    EXPECT_EQ(dialog->editedText(), QString::fromStdString(specialText));
}


// Test 11: Multiple Edits
TEST_F(TreEditDialogTest, MultipleEdits) {
    auto result = makeTestResult(0.7f, "Original");
    dialog = std::make_unique<pdfe::tre::TreEditDialog>(result, nullptr);

    QTextEdit* textEdit = dialog->findChild<QTextEdit*>("treTextEdit");
    ASSERT_NE(textEdit, nullptr);

    textEdit->setPlainText("First edit");
    EXPECT_EQ(dialog->editedText(), QString("First edit"));

    textEdit->setPlainText("Second edit");
    EXPECT_EQ(dialog->editedText(), QString("Second edit"));

    textEdit->setPlainText("Final edit");
    EXPECT_EQ(dialog->editedText(), QString("Final edit"));
}


// Test 12: User Can Switch Approach Selection
TEST_F(TreEditDialogTest, UserCanSwitchApproachSelection) {
    auto result = makeTestResult(0.8f, "Test"); // Defaults to Approach B
    dialog = std::make_unique<pdfe::tre::TreEditDialog>(result, nullptr);

    QRadioButton* approachBRadio = dialog->findChild<QRadioButton*>("approachBRadio");
    QRadioButton* approachCRadio = dialog->findChild<QRadioButton*>("approachCRadio");
    ASSERT_NE(approachBRadio, nullptr);
    ASSERT_NE(approachCRadio, nullptr);

    // Verify initial state
    EXPECT_TRUE(approachBRadio->isChecked());
    EXPECT_FALSE(approachCRadio->isChecked());

    // Switch to Approach C
    approachCRadio->setChecked(true);

    EXPECT_FALSE(approachBRadio->isChecked());
    EXPECT_TRUE(approachCRadio->isChecked());
    EXPECT_EQ(dialog->selectedApproach(), pdfe::tre::Approach::C_ImageReplacement);
}
