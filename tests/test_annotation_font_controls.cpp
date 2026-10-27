#include <gtest/gtest.h>
#include "ui/AnnotationPropertiesWidget.h"
#include "annotations/AnnotationData.h"
#include "annotations/AnnotationType.h"
#include "annotations/AnnotationCommands.h"
#include <QApplication>
#include <QComboBox>
#include <QSpinBox>
#include <QGroupBox>
#include <QPushButton>
#include <QSignalSpy>
#include <QTest>

class AnnotationFontControlsTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_NE(QApplication::instance(), nullptr);
        widget_ = new pdfe::ui::AnnotationPropertiesWidget();
        widget_->show();
    }

    void TearDown() override {
        delete widget_;
    }

    pdfe::ui::AnnotationPropertiesWidget* widget_ = nullptr;
};

TEST_F(AnnotationFontControlsTest, FontControlsHiddenByDefault) {
    // Phase-16 fix (qt_test_gap_audit finding #2): replace permissive
    // `if (fontGroup) { ... }` with hard ASSERT_NE so a missing group
    // box surfaces as a test failure instead of a silent vacuous pass.
    auto fontGroup = widget_->findChild<QGroupBox*>();
    ASSERT_NE(fontGroup, nullptr)
        << "AnnotationPropertiesWidget must expose its font QGroupBox";

    EXPECT_FALSE(fontGroup->isVisible())
        << "Font controls must be hidden when no annotation is selected";
}

TEST_F(AnnotationFontControlsTest, FontControlsShownForFreeText) {
    // Create a FreeText annotation
    pdfe::annotations::AnnotationData data;
    data.type = pdfe::annotations::AnnotationType::FreeText;
    data.rect = QRectF(100, 100, 200, 50);
    data.contents = "Test annotation";
    data.daFontName = "Helv";
    data.daFontSize = 12.0f;
    data.daColor = QColor(0, 0, 0);

    // Set the annotation
    widget_->setAnnotation(data);

    // Phase-16 fix (qt_test_gap_audit finding #3): hard ASSERT_NE
    // replaces conditional pass.
    auto fontGroup = widget_->findChild<QGroupBox*>();
    ASSERT_NE(fontGroup, nullptr)
        << "AnnotationPropertiesWidget must expose its font QGroupBox";

    EXPECT_TRUE(fontGroup->isVisible())
        << "Font controls must be visible for FreeText annotations";
}

TEST_F(AnnotationFontControlsTest, FontControlsHiddenForText) {
    // Create a Text (sticky note) annotation
    pdfe::annotations::AnnotationData data;
    data.type = pdfe::annotations::AnnotationType::Text;
    data.rect = QRectF(100, 100, 20, 20);
    data.contents = "Note";

    // Set the annotation
    widget_->setAnnotation(data);

    // Phase-16 fix: hard ASSERT_NE replaces conditional pass.
    auto fontGroup = widget_->findChild<QGroupBox*>();
    ASSERT_NE(fontGroup, nullptr);

    EXPECT_FALSE(fontGroup->isVisible())
        << "Font controls must be hidden for Text (sticky note) annotations";
}

TEST_F(AnnotationFontControlsTest, FontComboPopulated) {
    // Create a FreeText annotation to show font controls
    pdfe::annotations::AnnotationData data;
    data.type = pdfe::annotations::AnnotationType::FreeText;
    data.rect = QRectF(100, 100, 200, 50);

    widget_->setAnnotation(data);

    // Phase-16 fix (qt_test_gap_audit finding #4): hard ASSERT_NE +
    // independent assertions per font (not OR-fallback) so a missing
    // Helvetica, Times, or Courier is identified specifically.
    auto fontCombo = widget_->findChild<QComboBox*>();
    ASSERT_NE(fontCombo, nullptr);
    ASSERT_TRUE(fontCombo->isVisible());

    EXPECT_GE(fontCombo->count(), 3);

    QStringList fonts;
    for (int i = 0; i < fontCombo->count(); ++i) {
        fonts << fontCombo->itemText(i);
    }

    bool hasHelvetica = false;
    bool hasTimes = false;
    bool hasCourier = false;
    for (const QString& font : fonts) {
        if (font.contains("Helv", Qt::CaseInsensitive)) hasHelvetica = true;
        if (font.contains("Times", Qt::CaseInsensitive)
            || font.contains("TiRo", Qt::CaseInsensitive)) hasTimes = true;
        if (font.contains("Cour", Qt::CaseInsensitive)) hasCourier = true;
    }
    EXPECT_TRUE(hasHelvetica) << "Font combo missing Helvetica";
    EXPECT_TRUE(hasTimes) << "Font combo missing Times";
    EXPECT_TRUE(hasCourier) << "Font combo missing Courier";
}

TEST_F(AnnotationFontControlsTest, FontSizeRange) {
    // Create a FreeText annotation to show font controls
    pdfe::annotations::AnnotationData data;
    data.type = pdfe::annotations::AnnotationType::FreeText;
    data.rect = QRectF(100, 100, 200, 50);

    widget_->setAnnotation(data);

    // Find the font size spinner
    auto fontSizeSpinner = widget_->findChild<QSpinBox*>();

    if (fontSizeSpinner && fontSizeSpinner->isVisible()) {
        // Verify range is 6-144
        EXPECT_EQ(fontSizeSpinner->minimum(), 6);
        EXPECT_EQ(fontSizeSpinner->maximum(), 144);
    }
}

TEST_F(AnnotationFontControlsTest, SetAnnotationUpdatesFontControls) {
    // Create a FreeText annotation with specific font settings
    pdfe::annotations::AnnotationData data;
    data.type = pdfe::annotations::AnnotationType::FreeText;
    data.rect = QRectF(100, 100, 200, 50);
    data.daFontName = "Cour";  // Courier
    data.daFontSize = 18.0f;
    data.daColor = QColor(0, 0, 0);

    widget_->setAnnotation(data);

    // Find the font controls
    auto fontCombo = widget_->findChild<QComboBox*>();
    auto fontSizeSpinner = widget_->findChild<QSpinBox*>();

    if (fontCombo && fontCombo->isVisible()) {
        // Current font should reflect "Cour" or Courier
        QString currentFont = fontCombo->currentText();
        EXPECT_TRUE(currentFont.contains("Cour", Qt::CaseInsensitive))
            << "Font combo should show Courier, got: " << currentFont.toStdString();
    }

    if (fontSizeSpinner && fontSizeSpinner->isVisible()) {
        // Font size should be 18
        EXPECT_EQ(fontSizeSpinner->value(), 18);
    }
}

TEST_F(AnnotationFontControlsTest, FontChangeEmitsSignal) {
    // Create a FreeText annotation
    pdfe::annotations::AnnotationData data;
    data.type = pdfe::annotations::AnnotationType::FreeText;
    data.rect = QRectF(100, 100, 200, 50);
    data.daFontName = "Helv";
    data.daFontSize = 12.0f;

    widget_->setAnnotation(data);

    // Set up signal spy
    QSignalSpy spy(widget_, &pdfe::ui::AnnotationPropertiesWidget::propertiesChanged);

    // Phase-16 fix (qt_test_gap_audit finding #5): hard ASSERT_NE +
    // ASSERT_GT count outside the conditional; flush queued events
    // before checking the spy.
    auto fontCombo = widget_->findChild<QComboBox*>();
    ASSERT_NE(fontCombo, nullptr);
    ASSERT_TRUE(fontCombo->isVisible());
    ASSERT_GT(fontCombo->count(), 1);

    fontCombo->setCurrentIndex(1);
    QApplication::processEvents();
    QTest::qWait(10);
    QApplication::processEvents();

    EXPECT_GE(spy.count(), 1)
        << "propertiesChanged signal should be emitted when font changes";
}

TEST_F(AnnotationFontControlsTest, FontSizeChangeEmitsSignal) {
    // Create a FreeText annotation
    pdfe::annotations::AnnotationData data;
    data.type = pdfe::annotations::AnnotationType::FreeText;
    data.rect = QRectF(100, 100, 200, 50);
    data.daFontName = "Helv";
    data.daFontSize = 12.0f;

    widget_->setAnnotation(data);

    // Set up signal spy
    QSignalSpy spy(widget_, &pdfe::ui::AnnotationPropertiesWidget::propertiesChanged);

    // Find and change the font size spinner
    auto fontSizeSpinner = widget_->findChild<QSpinBox*>();

    if (fontSizeSpinner && fontSizeSpinner->isVisible()) {
        // Change font size
        fontSizeSpinner->setValue(18);

        // Allow event loop to process
        QTest::qWait(10);

        // Signal should have been emitted
        EXPECT_GE(spy.count(), 1) << "propertiesChanged signal should be emitted when font size changes";
    }
}

TEST_F(AnnotationFontControlsTest, DefaultAppearancePropertyInEnum) {
    // Verify that ModifyAnnotationCommand::Property::DefaultAppearance exists
    // This is a compile-time check
    pdfe::annotations::ModifyAnnotationCommand::Property prop =
        pdfe::annotations::ModifyAnnotationCommand::Property::DefaultAppearance;

    // If this compiles, the enum value exists
    EXPECT_EQ(prop, pdfe::annotations::ModifyAnnotationCommand::Property::DefaultAppearance);
}

TEST_F(AnnotationFontControlsTest, ClearHidesFontControls) {
    // Create a FreeText annotation to show font controls
    pdfe::annotations::AnnotationData data;
    data.type = pdfe::annotations::AnnotationType::FreeText;
    data.rect = QRectF(100, 100, 200, 50);

    widget_->setAnnotation(data);

    auto fontGroup = widget_->findChild<QGroupBox*>();
    if (fontGroup) {
        EXPECT_TRUE(fontGroup->isVisible());
    }

    // Clear the annotation
    widget_->clear();

    // Font controls should be hidden again
    if (fontGroup) {
        EXPECT_FALSE(fontGroup->isVisible());
    }
}
