// -*- coding: utf-8 -*-
// Pipeline E phase-6 / Bucket 3d: crop tool + CropPageCommand.
//
// Verifies the page CropBox getter/setter round-trip, an undoable
// CropPageCommand that restores the previous /CropBox, and a crop
// applied to a real fixture page (test_3page.pdf).
#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/PageCommands.h"
#include "core/PageOperations.h"
#include "core/UndoManager.h"

#include <QRectF>
#include <QTemporaryDir>

#include <filesystem>
#include <memory>
#include <string>

using namespace pdfe::core;

class CropToolTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx = std::make_shared<MuPdfContext>();
        auto fixtures = std::filesystem::path(TEST_FIXTURES_DIR);
        auto src = fixtures / "test_3page.pdf";
        if (!std::filesystem::exists(src)) {
            GTEST_SKIP() << "test_3page.pdf not available";
        }
        doc = Document::open(ctx, src);
        ASSERT_NE(doc, nullptr);
        ASSERT_TRUE(workDir.isValid());
    }
    std::shared_ptr<MuPdfContext> ctx;
    std::unique_ptr<Document> doc;
    QTemporaryDir workDir;
};

TEST_F(CropToolTest, GetPageCropBoxReturnsMediaBoxByDefault)
{
    QRectF original = getPageCropBox(doc.get(), 0);
    EXPECT_FALSE(original.isEmpty())
        << "Default CropBox should fall back to MediaBox";
    EXPECT_GT(original.width(), 0.0);
    EXPECT_GT(original.height(), 0.0);
}

TEST_F(CropToolTest, SetAndReadCropBoxRoundTrip)
{
    // Crop test_3page.pdf page 1 to a 200x300 inset rect.
    QRectF original = getPageCropBox(doc.get(), 0);
    ASSERT_FALSE(original.isEmpty());
    QRectF requested(50.0, 50.0, 200.0, 300.0);
    setPageCropBox(doc.get(), 0, requested);

    QRectF readBack = getPageCropBox(doc.get(), 0);
    EXPECT_NEAR(readBack.left(),   requested.left(),   2.0);
    EXPECT_NEAR(readBack.top(),    requested.top(),    2.0);
    EXPECT_NEAR(readBack.width(),  requested.width(),  2.0);
    EXPECT_NEAR(readBack.height(), requested.height(), 2.0);
}

TEST_F(CropToolTest, CropPageCommandUndoRestoresOriginalCropBox)
{
    QRectF original = getPageCropBox(doc.get(), 0);
    ASSERT_FALSE(original.isEmpty());

    pdfe::commands::UndoManager mgr;
    auto cmd = std::make_unique<pdfe::commands::CropPageCommand>(
        doc.get(), 0, QRectF(50.0, 50.0, 100.0, 150.0));
    mgr.execute(std::move(cmd));

    QRectF afterCrop = getPageCropBox(doc.get(), 0);
    EXPECT_NEAR(afterCrop.width(),  100.0, 2.0);
    EXPECT_NEAR(afterCrop.height(), 150.0, 2.0);

    ASSERT_TRUE(mgr.canUndo());
    mgr.undo();

    QRectF restored = getPageCropBox(doc.get(), 0);
    EXPECT_NEAR(restored.width(),  original.width(),  2.0);
    EXPECT_NEAR(restored.height(), original.height(), 2.0);
}

TEST_F(CropToolTest, CropPageCommandRedoReappliesCrop)
{
    pdfe::commands::UndoManager mgr;
    QRectF cropTo(40.0, 40.0, 120.0, 180.0);
    auto cmd = std::make_unique<pdfe::commands::CropPageCommand>(
        doc.get(), 0, cropTo);
    mgr.execute(std::move(cmd));
    mgr.undo();
    ASSERT_TRUE(mgr.canRedo());
    mgr.redo();

    QRectF rerun = getPageCropBox(doc.get(), 0);
    EXPECT_NEAR(rerun.width(),  cropTo.width(),  2.0);
    EXPECT_NEAR(rerun.height(), cropTo.height(), 2.0);
}

TEST_F(CropToolTest, CropPersistsAcrossSaveAndReopen)
{
    QRectF cropTo(20.0, 30.0, 250.0, 350.0);
    setPageCropBox(doc.get(), 0, cropTo);

    std::string outPath = (std::filesystem::path(
        workDir.path().toStdString()) / "cropped.pdf").string();
    ASSERT_NO_THROW(doc->saveAs(outPath));

    doc.reset();
    auto reopened = Document::open(ctx, outPath);
    ASSERT_NE(reopened, nullptr);
    QRectF readBack = getPageCropBox(reopened.get(), 0);
    EXPECT_NEAR(readBack.width(),  cropTo.width(),  2.0);
    EXPECT_NEAR(readBack.height(), cropTo.height(), 2.0);
}

TEST_F(CropToolTest, OutOfRangePageIndexNoOps)
{
    // Should not crash or throw.
    QRectF empty = getPageCropBox(doc.get(), -1);
    EXPECT_TRUE(empty.isEmpty());
    QRectF empty2 = getPageCropBox(doc.get(), 99);
    EXPECT_TRUE(empty2.isEmpty());

    setPageCropBox(doc.get(), -1, QRectF(0, 0, 50, 50));
    setPageCropBox(doc.get(), 99, QRectF(0, 0, 50, 50));
    SUCCEED();
}
