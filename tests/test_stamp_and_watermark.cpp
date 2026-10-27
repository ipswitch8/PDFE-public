// -*- coding: utf-8 -*-
#include <gtest/gtest.h>
#include "annotations/StampTool.h"
#include "annotations/AnnotationType.h"
#include "ui/WatermarkDialog.h"
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "core/Page.h"
#include <QApplication>
#include <QLineEdit>
#include <QDoubleSpinBox>
#include <QComboBox>
#include <QPushButton>
#include <filesystem>
#include <memory>

using namespace pdfe::annotations;
using namespace pdfe::core;

// ========== StampTool Tests ==========

TEST(StampToolTest, ToolTypeIsStamp) {
    StampTool tool(nullptr);
    EXPECT_EQ(tool.toolType(), AnnotationType::Stamp);
}

TEST(StampToolTest, DefaultStampName) {
    StampTool tool(nullptr);
    EXPECT_EQ(tool.stampName(), QString("Draft"));
}

TEST(StampToolTest, SetStampName) {
    StampTool tool(nullptr);
    tool.setStampName("Approved");
    EXPECT_EQ(tool.stampName(), QString("Approved"));
}

TEST(StampToolTest, SetColor) {
    StampTool tool(nullptr);
    tool.setColor(QColor(0, 100, 0));
    // No crash; setColor is virtual from base
    SUCCEED();
}

// ========== WatermarkDialog Tests ==========

class WatermarkDialogTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx = std::make_shared<MuPdfContext>();
    }
    std::shared_ptr<MuPdfContext> ctx;
};

TEST_F(WatermarkDialogTest, DialogHasObjectName) {
    pdfe::ui::WatermarkDialog dlg(ctx, nullptr);
    EXPECT_EQ(dlg.objectName(), QString("watermarkDialog"));
}

TEST_F(WatermarkDialogTest, TextEditExists) {
    pdfe::ui::WatermarkDialog dlg(ctx, nullptr);
    auto* edit = dlg.findChild<QLineEdit*>("watermarkDialogTextEdit");
    ASSERT_NE(edit, nullptr);
    EXPECT_EQ(edit->text(), QString("DRAFT"));
}

TEST_F(WatermarkDialogTest, OpacitySpinExists) {
    pdfe::ui::WatermarkDialog dlg(ctx, nullptr);
    auto* spin = dlg.findChild<QDoubleSpinBox*>("watermarkDialogOpacitySpin");
    ASSERT_NE(spin, nullptr);
    EXPECT_NEAR(spin->value(), 0.3, 0.01);
}

TEST_F(WatermarkDialogTest, PositionComboHasOptions) {
    pdfe::ui::WatermarkDialog dlg(ctx, nullptr);
    auto* combo = dlg.findChild<QComboBox*>("watermarkDialogPositionCombo");
    ASSERT_NE(combo, nullptr);
    EXPECT_GE(combo->count(), 5);
}

TEST_F(WatermarkDialogTest, ApplyAndCancelButtonsExist) {
    pdfe::ui::WatermarkDialog dlg(ctx, nullptr);
    EXPECT_NE(dlg.findChild<QPushButton*>("watermarkDialogApplyButton"), nullptr);
    EXPECT_NE(dlg.findChild<QPushButton*>("watermarkDialogCancelButton"), nullptr);
}

TEST_F(WatermarkDialogTest, AllButtonsHaveAccessibleNames) {
    pdfe::ui::WatermarkDialog dlg(ctx, nullptr);
    // Only check the user-facing buttons we created (QDoubleSpinBox contains
    // internal QLineEdit children we can't control).
    auto* apply = dlg.findChild<QPushButton*>("watermarkDialogApplyButton");
    auto* cancel = dlg.findChild<QPushButton*>("watermarkDialogCancelButton");
    ASSERT_NE(apply, nullptr);
    ASSERT_NE(cancel, nullptr);
    EXPECT_FALSE(apply->accessibleName().isEmpty());
    EXPECT_FALSE(cancel->accessibleName().isEmpty());
}

// ========== Stamp Persistence Test ==========
// Functional test: create a Stamp annotation, save to disk, reopen, verify
// the stamp annotation survived round-trip.

#include "annotations/AnnotationManager.h"

TEST(StampPersistenceTest, StampSurvivesSaveAndReload) {
    auto ctx = std::make_shared<MuPdfContext>();
    std::filesystem::path fixtures = TEST_FIXTURES_DIR;
    auto pdfPath = fixtures / "test_3page.pdf";
    if (!std::filesystem::exists(pdfPath)) {
        GTEST_SKIP() << "test_3page.pdf not available";
    }

    auto tmpPath = std::filesystem::temp_directory_path() / "pdfe_stamp_test.pdf";
    std::filesystem::remove(tmpPath);

    int createdAnnotIndex = -1;
    int stampCountBefore = 0;
    int stampCountAfter = 0;

    // Create stamp and save
    {
        auto doc = Document::open(ctx, pdfPath);
        ASSERT_NE(doc, nullptr);
        auto page = doc->page(0);
        ASSERT_NE(page, nullptr);

        AnnotationManager mgr(ctx);

        // Count stamps before
        auto annotsBefore = mgr.getAnnotations(page.get());
        for (const auto& a : annotsBefore) {
            if (a.type == AnnotationType::Stamp) stampCountBefore++;
        }

        auto data = mgr.createAnnotation(page.get(), AnnotationType::Stamp,
                                          QRectF(100, 100, 150, 50));
        createdAnnotIndex = data.annotIndex;
        mgr.setContents(page.get(), createdAnnotIndex, "TestStamp");

        page.reset();  // Release page before save
        doc->save(tmpPath);
    }

    // Reopen and verify stamp persisted
    {
        auto doc = Document::open(ctx, tmpPath);
        ASSERT_NE(doc, nullptr);
        auto page = doc->page(0);
        ASSERT_NE(page, nullptr);

        AnnotationManager mgr(ctx);
        auto annotsAfter = mgr.getAnnotations(page.get());
        for (const auto& a : annotsAfter) {
            if (a.type == AnnotationType::Stamp) stampCountAfter++;
        }
    }

    EXPECT_EQ(stampCountAfter, stampCountBefore + 1)
        << "Stamp annotation did not survive save/reload cycle";

    std::filesystem::remove(tmpPath);
}
