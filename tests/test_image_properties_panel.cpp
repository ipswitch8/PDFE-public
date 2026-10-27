// test_image_properties_panel.cpp - Google Test unit tests for ImagePropertiesPanel
// Copyright (c) 2025 PDFE Project

#include <gtest/gtest.h>

#include "ui/ImagePropertiesPanel.h"
#include "image/ImageData.h"

#include <QApplication>
#include <QLabel>
#include <QDoubleSpinBox>
#include <QComboBox>
#include <QPushButton>
#include <QSignalSpy>
#include <QTest>
#include <QRegularExpression>
#include <memory>

using namespace pdfe::image;

// Ensure QApplication exists for Qt widget tests
namespace {
    void ensureQApplication() {
        if (!QApplication::instance()) {
            static int argc = 0;
            static char* argv[] = { nullptr };
            static QApplication app(argc, argv);
        }
    }
}

// Test 1: Construction - no throw, objectName is "imagePropertiesPanel"
TEST(ImagePropertiesPanelTest, Construct) {
    ensureQApplication();

    std::unique_ptr<ImagePropertiesPanel> panel;
    EXPECT_NO_THROW({
        panel = std::make_unique<ImagePropertiesPanel>();
    });

    ASSERT_NE(panel, nullptr);
    EXPECT_EQ(panel->objectName().toStdString(), "imagePropertiesPanel");
}

// Test 2: HasAllWidgets - findChild for all objectNames returns non-null
TEST(ImagePropertiesPanelTest, HasAllWidgets) {
    ensureQApplication();

    auto panel = std::make_unique<ImagePropertiesPanel>();

    // Check all labels exist
    EXPECT_NE(panel->findChild<QLabel*>("imageXObjectNameLabel"), nullptr);
    EXPECT_NE(panel->findChild<QLabel*>("imagePixelDimensionsLabel"), nullptr);
    EXPECT_NE(panel->findChild<QLabel*>("imageDisplayDimensionsLabel"), nullptr);
    EXPECT_NE(panel->findChild<QLabel*>("imageDpiLabel"), nullptr);
    EXPECT_NE(panel->findChild<QLabel*>("imageColorSpaceLabel"), nullptr);
    EXPECT_NE(panel->findChild<QLabel*>("imageCompressionLabel"), nullptr);

    // Check spin boxes exist
    EXPECT_NE(panel->findChild<QDoubleSpinBox*>("imageWidthSpin"), nullptr);
    EXPECT_NE(panel->findChild<QDoubleSpinBox*>("imageHeightSpin"), nullptr);

    // Check combo box exists
    EXPECT_NE(panel->findChild<QComboBox*>("imageRotationCombo"), nullptr);

    // Check buttons exist
    EXPECT_NE(panel->findChild<QPushButton*>("imageCropButton"), nullptr);
    EXPECT_NE(panel->findChild<QPushButton*>("imageExtractButton"), nullptr);
    EXPECT_NE(panel->findChild<QPushButton*>("imageRemoveButton"), nullptr);
}

// Test 3: InitialState_ControlsDisabled - spin boxes and buttons start disabled
TEST(ImagePropertiesPanelTest, InitialState_ControlsDisabled) {
    ensureQApplication();

    auto panel = std::make_unique<ImagePropertiesPanel>();

    // Check spin boxes are disabled initially
    auto widthSpin = panel->findChild<QDoubleSpinBox*>("imageWidthSpin");
    auto heightSpin = panel->findChild<QDoubleSpinBox*>("imageHeightSpin");
    ASSERT_NE(widthSpin, nullptr);
    ASSERT_NE(heightSpin, nullptr);
    EXPECT_FALSE(widthSpin->isEnabled());
    EXPECT_FALSE(heightSpin->isEnabled());

    // Check combo box is disabled initially
    auto rotationCombo = panel->findChild<QComboBox*>("imageRotationCombo");
    ASSERT_NE(rotationCombo, nullptr);
    EXPECT_FALSE(rotationCombo->isEnabled());

    // Check buttons are disabled initially
    auto cropButton = panel->findChild<QPushButton*>("imageCropButton");
    auto extractButton = panel->findChild<QPushButton*>("imageExtractButton");
    auto removeButton = panel->findChild<QPushButton*>("imageRemoveButton");
    ASSERT_NE(cropButton, nullptr);
    ASSERT_NE(extractButton, nullptr);
    ASSERT_NE(removeButton, nullptr);
    EXPECT_FALSE(cropButton->isEnabled());
    EXPECT_FALSE(extractButton->isEnabled());
    EXPECT_FALSE(removeButton->isEnabled());
}

// Test 4: SetImageInfo_UpdatesLabels - set info with pixelWidth=100, height=80 → check label text
TEST(ImagePropertiesPanelTest, SetImageInfo_UpdatesLabels) {
    ensureQApplication();

    auto panel = std::make_unique<ImagePropertiesPanel>();

    pdfe::image::PageImageInfo info;
    info.xobjectName = "Image1";
    info.pageIndex = 0;
    info.imageIndex = 0;
    info.pixelWidth = 100;
    info.pixelHeight = 80;
    info.displayWidth = 200.0f;
    info.displayHeight = 160.0f;
    info.xDpi = 72;
    info.yDpi = 72;
    info.bitsPerComponent = 8;
    info.components = 3;
    info.isJpeg = true;
    info.isMask = false;

    panel->setImageInfo(info);

    // Check pixel dimensions label
    auto pixelDimLabel = panel->findChild<QLabel*>("imagePixelDimensionsLabel");
    ASSERT_NE(pixelDimLabel, nullptr);
    QString pixelText = pixelDimLabel->text();
    // Phase-16 fix (qt_test_gap_audit finding #17): anchor on the
    // dimension separator so "100x80", "100 x 80", "100×80", or
    // "100 px × 80 px" all match, but "10080" (no separator) does
    // not — the previous separate contains() pair would pass for
    // any label that mentions 100 anywhere and 80 anywhere.
    EXPECT_TRUE(pixelText.contains(QRegularExpression(
        QStringLiteral("100\\s*(?:[x×]|by)\\s*80"),
        QRegularExpression::CaseInsensitiveOption)))
        << "Label must show '100 × 80' (or 100x80) with a "
           "dimension separator; got: " << pixelText.toStdString();

    // Check DPI label
    auto dpiLabel = panel->findChild<QLabel*>("imageDpiLabel");
    ASSERT_NE(dpiLabel, nullptr);
    QString dpiText = dpiLabel->text();
    EXPECT_TRUE(dpiText.contains("72")) << "Label text: " << dpiText.toStdString();

    // Check xobject name label
    auto xobjectLabel = panel->findChild<QLabel*>("imageXObjectNameLabel");
    ASSERT_NE(xobjectLabel, nullptr);
    EXPECT_TRUE(xobjectLabel->text().contains("Image1"))
        << "Label text: " << xobjectLabel->text().toStdString();
}

// Test 5: SetImageInfo_UpdatesSpinValues - displayWidth/Height reflected in spin boxes
TEST(ImagePropertiesPanelTest, SetImageInfo_UpdatesSpinValues) {
    ensureQApplication();

    auto panel = std::make_unique<ImagePropertiesPanel>();

    pdfe::image::PageImageInfo info;
    info.xobjectName = "Image2";
    info.pageIndex = 0;
    info.imageIndex = 0;
    info.pixelWidth = 150;
    info.pixelHeight = 120;
    info.displayWidth = 300.5f;
    info.displayHeight = 240.75f;
    info.xDpi = 96;
    info.yDpi = 96;

    panel->setImageInfo(info);

    // Check spin box values
    auto widthSpin = panel->findChild<QDoubleSpinBox*>("imageWidthSpin");
    auto heightSpin = panel->findChild<QDoubleSpinBox*>("imageHeightSpin");
    ASSERT_NE(widthSpin, nullptr);
    ASSERT_NE(heightSpin, nullptr);

    EXPECT_NEAR(widthSpin->value(), 300.5, 0.1);
    EXPECT_NEAR(heightSpin->value(), 240.75, 0.1);
}

// Test 6: SetImageInfo_EnablesControls - after setImageInfo, buttons/spins are enabled
TEST(ImagePropertiesPanelTest, SetImageInfo_EnablesControls) {
    ensureQApplication();

    auto panel = std::make_unique<ImagePropertiesPanel>();

    pdfe::image::PageImageInfo info;
    info.xobjectName = "Image3";
    info.pageIndex = 0;
    info.imageIndex = 0;
    info.pixelWidth = 200;
    info.pixelHeight = 150;
    info.displayWidth = 400.0f;
    info.displayHeight = 300.0f;
    info.xDpi = 72;
    info.yDpi = 72;

    panel->setImageInfo(info);

    // Check spin boxes are enabled
    auto widthSpin = panel->findChild<QDoubleSpinBox*>("imageWidthSpin");
    auto heightSpin = panel->findChild<QDoubleSpinBox*>("imageHeightSpin");
    ASSERT_NE(widthSpin, nullptr);
    ASSERT_NE(heightSpin, nullptr);
    EXPECT_TRUE(widthSpin->isEnabled());
    EXPECT_TRUE(heightSpin->isEnabled());

    // Check combo box is enabled
    auto rotationCombo = panel->findChild<QComboBox*>("imageRotationCombo");
    ASSERT_NE(rotationCombo, nullptr);
    EXPECT_TRUE(rotationCombo->isEnabled());

    // Check buttons are enabled
    auto cropButton = panel->findChild<QPushButton*>("imageCropButton");
    auto extractButton = panel->findChild<QPushButton*>("imageExtractButton");
    auto removeButton = panel->findChild<QPushButton*>("imageRemoveButton");
    ASSERT_NE(cropButton, nullptr);
    ASSERT_NE(extractButton, nullptr);
    ASSERT_NE(removeButton, nullptr);
    EXPECT_TRUE(cropButton->isEnabled());
    EXPECT_TRUE(extractButton->isEnabled());
    EXPECT_TRUE(removeButton->isEnabled());
}

// Test 7: ClearInfo_DisablesControls - after clearInfo, buttons/spins are disabled
TEST(ImagePropertiesPanelTest, ClearInfo_DisablesControls) {
    ensureQApplication();

    auto panel = std::make_unique<ImagePropertiesPanel>();

    // First set image info to enable controls
    pdfe::image::PageImageInfo info;
    info.xobjectName = "Image4";
    info.pageIndex = 0;
    info.imageIndex = 0;
    info.pixelWidth = 100;
    info.pixelHeight = 100;
    info.displayWidth = 200.0f;
    info.displayHeight = 200.0f;
    info.xDpi = 72;
    info.yDpi = 72;

    panel->setImageInfo(info);

    // Now clear and verify controls are disabled
    panel->clearInfo();

    // Check spin boxes are disabled
    auto widthSpin = panel->findChild<QDoubleSpinBox*>("imageWidthSpin");
    auto heightSpin = panel->findChild<QDoubleSpinBox*>("imageHeightSpin");
    ASSERT_NE(widthSpin, nullptr);
    ASSERT_NE(heightSpin, nullptr);
    EXPECT_FALSE(widthSpin->isEnabled());
    EXPECT_FALSE(heightSpin->isEnabled());

    // Check combo box is disabled
    auto rotationCombo = panel->findChild<QComboBox*>("imageRotationCombo");
    ASSERT_NE(rotationCombo, nullptr);
    EXPECT_FALSE(rotationCombo->isEnabled());

    // Check buttons are disabled
    auto cropButton = panel->findChild<QPushButton*>("imageCropButton");
    auto extractButton = panel->findChild<QPushButton*>("imageExtractButton");
    auto removeButton = panel->findChild<QPushButton*>("imageRemoveButton");
    ASSERT_NE(cropButton, nullptr);
    ASSERT_NE(extractButton, nullptr);
    ASSERT_NE(removeButton, nullptr);
    EXPECT_FALSE(cropButton->isEnabled());
    EXPECT_FALSE(extractButton->isEnabled());
    EXPECT_FALSE(removeButton->isEnabled());
}

// Test 8: ClearInfo_ResetsLabels - labels show "-" after clearInfo
TEST(ImagePropertiesPanelTest, ClearInfo_ResetsLabels) {
    ensureQApplication();

    auto panel = std::make_unique<ImagePropertiesPanel>();

    // First set image info
    pdfe::image::PageImageInfo info;
    info.xobjectName = "Image5";
    info.pageIndex = 0;
    info.imageIndex = 0;
    info.pixelWidth = 100;
    info.pixelHeight = 100;
    info.displayWidth = 200.0f;
    info.displayHeight = 200.0f;
    info.xDpi = 72;
    info.yDpi = 72;

    panel->setImageInfo(info);

    // Clear and check labels
    panel->clearInfo();

    auto xobjectLabel = panel->findChild<QLabel*>("imageXObjectNameLabel");
    auto pixelDimLabel = panel->findChild<QLabel*>("imagePixelDimensionsLabel");
    auto displayDimLabel = panel->findChild<QLabel*>("imageDisplayDimensionsLabel");
    auto dpiLabel = panel->findChild<QLabel*>("imageDpiLabel");
    auto colorSpaceLabel = panel->findChild<QLabel*>("imageColorSpaceLabel");
    auto compressionLabel = panel->findChild<QLabel*>("imageCompressionLabel");

    ASSERT_NE(xobjectLabel, nullptr);
    ASSERT_NE(pixelDimLabel, nullptr);
    ASSERT_NE(displayDimLabel, nullptr);
    ASSERT_NE(dpiLabel, nullptr);
    ASSERT_NE(colorSpaceLabel, nullptr);
    ASSERT_NE(compressionLabel, nullptr);

    // Labels should show "-" or be empty after clear
    EXPECT_TRUE(xobjectLabel->text().contains("-") || xobjectLabel->text().isEmpty())
        << "Label text: " << xobjectLabel->text().toStdString();
    EXPECT_TRUE(pixelDimLabel->text().contains("-") || pixelDimLabel->text().isEmpty())
        << "Label text: " << pixelDimLabel->text().toStdString();
    EXPECT_TRUE(displayDimLabel->text().contains("-") || displayDimLabel->text().isEmpty())
        << "Label text: " << displayDimLabel->text().toStdString();
    EXPECT_TRUE(dpiLabel->text().contains("-") || dpiLabel->text().isEmpty())
        << "Label text: " << dpiLabel->text().toStdString();
}

// Test 9: RemoveButton_EmitsSignal - click remove button → removeRequested signal emitted
TEST(ImagePropertiesPanelTest, RemoveButton_EmitsSignal) {
    ensureQApplication();

    auto panel = std::make_unique<ImagePropertiesPanel>();

    // Set image info to enable the button
    pdfe::image::PageImageInfo info;
    info.xobjectName = "Image6";
    info.pageIndex = 0;
    info.imageIndex = 0;
    info.pixelWidth = 100;
    info.pixelHeight = 100;
    info.displayWidth = 200.0f;
    info.displayHeight = 200.0f;
    info.xDpi = 72;
    info.yDpi = 72;

    panel->setImageInfo(info);

    // Setup signal spy
    QSignalSpy spy(panel.get(), &ImagePropertiesPanel::removeRequested);

    // Click the remove button
    auto removeButton = panel->findChild<QPushButton*>("imageRemoveButton");
    ASSERT_NE(removeButton, nullptr);
    ASSERT_TRUE(removeButton->isEnabled());

    QTest::mouseClick(removeButton, Qt::LeftButton);

    // Verify signal was emitted
    EXPECT_EQ(spy.count(), 1) << "removeRequested signal should be emitted once";
}

// Test 10: ExtractButton_EmitsSignal - click extract button → extractRequested signal emitted
TEST(ImagePropertiesPanelTest, ExtractButton_EmitsSignal) {
    ensureQApplication();

    auto panel = std::make_unique<ImagePropertiesPanel>();

    // Set image info to enable the button
    pdfe::image::PageImageInfo info;
    info.xobjectName = "Image7";
    info.pageIndex = 0;
    info.imageIndex = 0;
    info.pixelWidth = 100;
    info.pixelHeight = 100;
    info.displayWidth = 200.0f;
    info.displayHeight = 200.0f;
    info.xDpi = 72;
    info.yDpi = 72;

    panel->setImageInfo(info);

    // Setup signal spy
    QSignalSpy spy(panel.get(), &ImagePropertiesPanel::extractRequested);

    // Click the extract button
    auto extractButton = panel->findChild<QPushButton*>("imageExtractButton");
    ASSERT_NE(extractButton, nullptr);
    ASSERT_TRUE(extractButton->isEnabled());

    QTest::mouseClick(extractButton, Qt::LeftButton);

    // Verify signal was emitted
    EXPECT_EQ(spy.count(), 1) << "extractRequested signal should be emitted once";
}
