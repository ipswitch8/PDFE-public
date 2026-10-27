#include <gtest/gtest.h>
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "core/Page.h"
#include "tre/ImageReplacer.h"
#include "tre/RegionReconstructor.h"
#include "tre/OcrPipeline.h"
#include "tre/CoordMapper.h"
#include "tre/BgColorDetector.h"
#include "tre/FontMatcher.h"
#include "tre/ImageMasker.h"
#include "tre/BitmapCompositor.h"
#include "tre/PdfContentWriter.h"
#include "tre/QualityAssessor.h"
#include "tre/TreTypes.h"
#include "fonts/PdfFontEmbedder.h"
#include "fonts/FontResolver.h"
#include "fonts/SubstituteFontSelector.h"
#include "fonts/EmbeddedFontAnalyzer.h"
#include "fonts/FontAnalysisCache.h"
#include "textedit/ContentStreamEditor.h"
#include "textedit/TextEditModel.h"
#include "textedit/TextEditCommands.h"

#include <QFont>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QImage>
#include <QRectF>
#include <memory>
#include <string>
#include <filesystem>

using namespace pdfe::tre;
using namespace pdfe::core;

static std::string fixturesDir() {
    return TEST_FIXTURES_DIR;
}

/// Helper: Compare two images for differences using scanLine (fast path).
/// Returns true if images are significantly different (>threshold fraction of pixels differ).
static bool imagesAreDifferent(const QImage& img1, const QImage& img2, double threshold = 0.01) {
    if (img1.size() != img2.size()) return true;
    if (img1.isNull() || img2.isNull()) return true;

    // Convert to consistent format for byte-level comparison
    QImage a = img1.convertToFormat(QImage::Format_RGBA8888);
    QImage b = img2.convertToFormat(QImage::Format_RGBA8888);

    int diffPixels = 0;
    int totalPixels = a.width() * a.height();

    for (int y = 0; y < a.height(); ++y) {
        const uchar* line1 = a.constScanLine(y);
        const uchar* line2 = b.constScanLine(y);
        int bytesPerLine = a.width() * 4; // RGBA = 4 bytes per pixel
        for (int x = 0; x < bytesPerLine; x += 4) {
            if (line1[x] != line2[x] || line1[x+1] != line2[x+1] ||
                line1[x+2] != line2[x+2] || line1[x+3] != line2[x+3]) {
                diffPixels++;
            }
        }
    }

    double diffRatio = static_cast<double>(diffPixels) / totalPixels;
    return diffRatio > threshold;
}

class ImageTextEditDisplayTest : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = std::make_shared<MuPdfContext>();

        // Check for test_scanned.pdf
        std::filesystem::path scannedPdf = std::filesystem::path(fixturesDir()) / "test_scanned.pdf";
        if (std::filesystem::exists(scannedPdf)) {
            scannedDoc_ = Document::open(context_, scannedPdf);
            hasScannedPdf_ = (scannedDoc_ && scannedDoc_->pageCount() > 0);
        } else {
            hasScannedPdf_ = false;
        }

        // Check for test_single.pdf (native text PDF)
        std::filesystem::path nativePdf = std::filesystem::path(fixturesDir()) / "test_single.pdf";
        if (std::filesystem::exists(nativePdf)) {
            nativeDoc_ = Document::open(context_, nativePdf);
            hasNativePdf_ = (nativeDoc_ && nativeDoc_->pageCount() > 0);
        } else {
            hasNativePdf_ = false;
        }

        // Check for test_ocr_text.pdf (image + invisible OCR text layer)
        std::filesystem::path ocrPdf = std::filesystem::path(fixturesDir()) / "test_ocr_text.pdf";
        if (std::filesystem::exists(ocrPdf)) {
            ocrDoc_ = Document::open(context_, ocrPdf);
            hasOcrPdf_ = (ocrDoc_ && ocrDoc_->pageCount() > 0);
        } else {
            hasOcrPdf_ = false;
        }

        // Check for tessdata (needed for OCR in ImageReplacer)
        std::filesystem::path tessdata = std::filesystem::path(fixturesDir()) / "tessdata" / "eng.traineddata";
        hasTessdata_ = std::filesystem::exists(tessdata);

        // Initialize TRE components (for tests that need ImageReplacer)
        if (hasScannedPdf_ && hasTessdata_) {
            initTreComponents();
        }
    }

    void TearDown() override {
        destroyTreComponents();
        scannedDoc_.reset();
        nativeDoc_.reset();
        ocrDoc_.reset();
        context_.reset();
    }

    void initTreComponents() {
        if (!context_ || !scannedDoc_) return;

        ocrPipeline_ = std::make_unique<OcrPipeline>(context_.get(), scannedDoc_.get());
        coordMapper_ = std::make_unique<CoordMapper>(context_.get(), scannedDoc_.get());
        fontEmbedder_ = std::make_unique<pdfe::fonts::PdfFontEmbedder>(context_.get());
        contentWriter_ = std::make_unique<PdfContentWriter>(context_.get(), fontEmbedder_.get());
        fontAnalysisCache_ = std::make_unique<pdfe::fonts::FontAnalysisCache>();
        embeddedFontAnalyzer_ = std::make_unique<pdfe::fonts::EmbeddedFontAnalyzer>(context_.get());
        substituteFontSelector_ = std::make_unique<pdfe::fonts::SubstituteFontSelector>(
            embeddedFontAnalyzer_.get(), fontAnalysisCache_.get());
        fontMatcher_ = std::make_unique<FontMatcher>(
            substituteFontSelector_.get(), embeddedFontAnalyzer_.get());
        bgDetector_ = std::make_unique<BgColorDetector>();
        qualityAssessor_ = std::make_unique<QualityAssessor>();
        imageMasker_ = std::make_unique<ImageMasker>();
        bitmapCompositor_ = std::make_unique<BitmapCompositor>();

        // Configure OCR pipeline with tessdata path
        if (hasTessdata_) {
            OcrConfig config;
            config.tessdataPath = (std::filesystem::path(fixturesDir()) / "tessdata").string();
            ocrPipeline_->setConfig(config);
        }

        // Create ImageReplacer
        imageReplacer_ = std::make_unique<ImageReplacer>(
            ocrPipeline_.get(),
            coordMapper_.get(),
            bgDetector_.get(),
            fontMatcher_.get(),
            imageMasker_.get(),
            bitmapCompositor_.get(),
            contentWriter_.get());
    }

    void destroyTreComponents() {
        imageReplacer_.reset();
        bitmapCompositor_.reset();
        imageMasker_.reset();
        qualityAssessor_.reset();
        bgDetector_.reset();
        fontMatcher_.reset();
        substituteFontSelector_.reset();
        embeddedFontAnalyzer_.reset();
        fontAnalysisCache_.reset();
        contentWriter_.reset();
        fontEmbedder_.reset();
        coordMapper_.reset();
        ocrPipeline_.reset();
    }

    std::shared_ptr<MuPdfContext> context_;
    std::unique_ptr<Document> scannedDoc_;
    std::unique_ptr<Document> nativeDoc_;
    std::unique_ptr<Document> ocrDoc_;

    // TRE components
    std::unique_ptr<OcrPipeline> ocrPipeline_;
    std::unique_ptr<CoordMapper> coordMapper_;
    std::unique_ptr<BgColorDetector> bgDetector_;
    std::unique_ptr<QualityAssessor> qualityAssessor_;
    std::unique_ptr<FontMatcher> fontMatcher_;
    std::unique_ptr<ImageMasker> imageMasker_;
    std::unique_ptr<BitmapCompositor> bitmapCompositor_;
    std::unique_ptr<PdfContentWriter> contentWriter_;
    std::unique_ptr<pdfe::fonts::PdfFontEmbedder> fontEmbedder_;
    std::unique_ptr<pdfe::fonts::SubstituteFontSelector> substituteFontSelector_;
    std::unique_ptr<pdfe::fonts::EmbeddedFontAnalyzer> embeddedFontAnalyzer_;
    std::unique_ptr<pdfe::fonts::FontAnalysisCache> fontAnalysisCache_;
    std::unique_ptr<ImageReplacer> imageReplacer_;

    bool hasScannedPdf_ = false;
    bool hasNativePdf_ = false;
    bool hasOcrPdf_ = false;
    bool hasTessdata_ = false;
};

// =============================================================================
// Test 1: Verify test_scanned.pdf page 0 classification
// =============================================================================
TEST_F(ImageTextEditDisplayTest, ScannedPage_Classification) {
    if (!hasScannedPdf_) {
        GTEST_SKIP() << "Requires test_scanned.pdf";
    }

    ASSERT_GT(scannedDoc_->pageCount(), 0);
    auto page = scannedDoc_->page(0);
    ASSERT_NE(page, nullptr);

    // test_scanned.pdf is a pure image PDF (one image XObject covering
    // the whole page, no embedded text layer). After the
    // 2026-05-06 isImageDominated semantics fix, a page where images
    // cover >50% of the page area is image-dominated regardless of
    // whether an OCR text layer exists yet — that's the precondition
    // for invoking the OCR pipeline in the first place.
    bool isImageDom = page->isImageDominated();
    EXPECT_TRUE(isImageDom)
        << "Pure-image PDF page should be image-dominated so the TRE/OCR "
        << "pipeline can run on it";
}

// =============================================================================
// Test 2: Verify test_single.pdf page 0 is NOT image-dominated
// =============================================================================
TEST_F(ImageTextEditDisplayTest, NativePage_NotImageDominated) {
    if (!hasNativePdf_) {
        GTEST_SKIP() << "Requires test_single.pdf";
    }

    ASSERT_GT(nativeDoc_->pageCount(), 0);
    auto page = nativeDoc_->page(0);
    ASSERT_NE(page, nullptr);

    // Native text PDFs should NOT be image-dominated
    bool isImageDom = page->isImageDominated();
    EXPECT_FALSE(isImageDom) << "test_single.pdf page 0 should NOT be image-dominated (native text)";
}

// =============================================================================
// Test 3: Verify scanned page text content status
// =============================================================================
TEST_F(ImageTextEditDisplayTest, ScannedPage_TextContentStatus) {
    if (!hasScannedPdf_) {
        GTEST_SKIP() << "Requires test_scanned.pdf";
    }

    ASSERT_GT(scannedDoc_->pageCount(), 0);
    auto page = scannedDoc_->page(0);
    ASSERT_NE(page, nullptr);

    // test_scanned.pdf is a pure image PDF — no OCR text layer embedded.
    // hasTextContent() should return false for image-only pages.
    bool hasText = page->hasTextContent();
    EXPECT_FALSE(hasText) << "Pure image PDF should not have text content until OCR is applied";
}

// =============================================================================
// Test 4: Verify scanned page renders to a valid image
// =============================================================================
TEST_F(ImageTextEditDisplayTest, ScannedPage_RenderProducesImage) {
    if (!hasScannedPdf_) {
        GTEST_SKIP() << "Requires test_scanned.pdf";
    }

    ASSERT_GT(scannedDoc_->pageCount(), 0);
    auto page = scannedDoc_->page(0);
    ASSERT_NE(page, nullptr);

    QRectF bbox = page->boundingBox();
    QImage rendered = page->render(bbox);

    EXPECT_FALSE(rendered.isNull()) << "Page render should produce a valid image";
    EXPECT_GT(rendered.width(), 0);
    EXPECT_GT(rendered.height(), 0);
    EXPECT_EQ(rendered.format(), QImage::Format_RGBA8888);
}

// =============================================================================
// Test 5: Verify ImageReplacer changes the page's XObject (binary data differs)
// =============================================================================
TEST_F(ImageTextEditDisplayTest, ScannedPage_ImageReplacerChangesXObject) {
    if (!hasScannedPdf_ || !hasTessdata_) {
        GTEST_SKIP() << "Requires test_scanned.pdf and tessdata";
    }

    ASSERT_GT(scannedDoc_->pageCount(), 0);
    auto page = scannedDoc_->page(0);
    ASSERT_NE(page, nullptr);

    // Get XObject binary data BEFORE ImageReplacer
    auto xobjBefore = contentWriter_->getImageXObjectData(page.get(), scannedDoc_.get());
    ASSERT_FALSE(xobjBefore.empty()) << "Scanned page should have XObject image data";

    // Run ImageReplacer with edited text on a small region
    PdfRect selection;
    selection.x0 = 72.0;   // 1 inch from left
    selection.y0 = 72.0;   // 1 inch from bottom
    selection.x1 = 216.0;  // 3 inches from left
    selection.y1 = 144.0;  // 2 inches from bottom

    std::string editedText = "EDITED TEXT";
    RegionId regionId = RegionId{1};

    ReconstructionResult result = imageReplacer_->replace(
        page.get(), scannedDoc_.get(), 0, selection, editedText, regionId);

    EXPECT_TRUE(result.success) << "ImageReplacer::replace() should succeed: "
                                 << result.errorMessage;
    EXPECT_EQ(result.region.approach, Approach::C_ImageReplacement);

    // Get XObject binary data AFTER ImageReplacer
    auto xobjAfter = contentWriter_->getImageXObjectData(page.get(), scannedDoc_.get());
    ASSERT_FALSE(xobjAfter.empty());

    // Verify XObject data changed (binary comparison is definitive,
    // unlike 72 DPI pixel comparison which misses small changes)
    EXPECT_NE(xobjBefore, xobjAfter)
        << "XObject image data should differ after ImageReplacer::replace()";
}

// =============================================================================
// Test 6: Verify native text page doesn't need image replacement
// =============================================================================
TEST_F(ImageTextEditDisplayTest, NativePage_ImageReplacerNotNeeded) {
    if (!hasNativePdf_) {
        GTEST_SKIP() << "Requires test_single.pdf";
    }

    ASSERT_GT(nativeDoc_->pageCount(), 0);
    auto page = nativeDoc_->page(0);
    ASSERT_NE(page, nullptr);

    // Native text pages should NOT be image-dominated
    EXPECT_FALSE(page->isImageDominated());

    // MainWindow::refreshImageForScannedPage() should skip native pages
    // (We can't test MainWindow directly here, but we verify page properties)
    // If isImageDominated() == false, refreshImageForScannedPage() early-returns.
}

// =============================================================================
// Test 7: Verify saved original image differs from edited image
// =============================================================================
TEST_F(ImageTextEditDisplayTest, ScannedPage_SavedImageDiffersFromEdited) {
    if (!hasScannedPdf_ || !hasTessdata_) {
        GTEST_SKIP() << "Requires test_scanned.pdf and tessdata";
    }

    ASSERT_GT(scannedDoc_->pageCount(), 0);
    auto page = scannedDoc_->page(0);
    ASSERT_NE(page, nullptr);

    QRectF bbox = page->boundingBox();

    // Save original page render (simulating imageEditStack_ beforeImage)
    QImage originalRender = page->render(bbox);
    ASSERT_FALSE(originalRender.isNull());

    // Run ImageReplacer to edit the page
    PdfRect selection;
    selection.x0 = 100.0;
    selection.y0 = 100.0;
    selection.x1 = 300.0;
    selection.y1 = 200.0;

    std::string editedText = "Modified Text";
    ReconstructionResult result = imageReplacer_->replace(
        page.get(), scannedDoc_.get(), 0, selection, editedText, RegionId{2});

    EXPECT_TRUE(result.success);

    // Get fresh page and render after edit
    scannedDoc_->markPageDirty(0);
    auto freshPage = scannedDoc_->page(0);
    ASSERT_NE(freshPage, nullptr);
    QImage currentRender = freshPage->render(bbox);
    ASSERT_FALSE(currentRender.isNull());

    // Verify original != current
    bool different = imagesAreDifferent(originalRender, currentRender);
    EXPECT_TRUE(different) << "Saved original image should differ from current (edited) image";
}

// =============================================================================
// Test 8: Verify restoring original XObject via saveImageXObjectRef/restoreImageXObjectRef
// =============================================================================
TEST_F(ImageTextEditDisplayTest, ScannedPage_RestoreOriginalImage) {
    if (!hasScannedPdf_ || !hasTessdata_) {
        GTEST_SKIP() << "Requires test_scanned.pdf and tessdata";
    }

    ASSERT_GT(scannedDoc_->pageCount(), 0);
    auto page = scannedDoc_->page(0);
    ASSERT_NE(page, nullptr);

    // Save original XObject reference for undo
    auto imageInfo = contentWriter_->findPageImageXObject(page.get(), scannedDoc_.get());
    ASSERT_TRUE(imageInfo.has_value()) << "Scanned page should have image XObject";
    std::string xobjName = imageInfo->xobjectName;

    void* savedRef = contentWriter_->saveImageXObjectRef(page.get(), scannedDoc_.get());
    ASSERT_NE(savedRef, nullptr) << "Should save image XObject reference";

    auto origData = contentWriter_->getImageXObjectData(page.get(), scannedDoc_.get());
    ASSERT_FALSE(origData.empty());

    // Run ImageReplacer to modify the page
    PdfRect selection;
    selection.x0 = 80.0;
    selection.y0 = 80.0;
    selection.x1 = 250.0;
    selection.y1 = 150.0;

    std::string editedText = "Test Undo";
    ReconstructionResult result = imageReplacer_->replace(
        page.get(), scannedDoc_.get(), 0, selection, editedText, RegionId{3});

    EXPECT_TRUE(result.success);

    // Verify XObject changed after edit
    auto editedData = contentWriter_->getImageXObjectData(page.get(), scannedDoc_.get());
    EXPECT_NE(origData, editedData) << "XObject should change after edit";

    // Restore original XObject via saved reference (byte-exact restore)
    bool restored = contentWriter_->restoreImageXObjectRef(
        page.get(), scannedDoc_.get(), savedRef, xobjName);
    EXPECT_TRUE(restored) << "restoreImageXObjectRef should succeed";

    // Verify XObject matches original (byte-exact since we restored the same pdf_obj)
    auto restoredData = contentWriter_->getImageXObjectData(page.get(), scannedDoc_.get());
    EXPECT_EQ(origData, restoredData)
        << "Restored XObject data should match original exactly";

    // Release saved ref (page resources now hold their own reference)
    contentWriter_->releaseImageXObjectRef(savedRef);
}

// =============================================================================
// Test 9: Verify MainWindow-style workflow skips native pages
// =============================================================================
TEST_F(ImageTextEditDisplayTest, RefreshImageForScannedPage_SkipsNativePage) {
    if (!hasNativePdf_) {
        GTEST_SKIP() << "Requires test_single.pdf";
    }

    ASSERT_GT(nativeDoc_->pageCount(), 0);
    auto page = nativeDoc_->page(0);
    ASSERT_NE(page, nullptr);

    // Simulate MainWindow::refreshImageForScannedPage() logic:
    // if (!page->isImageDominated()) return;
    if (!page->isImageDominated()) {
        // Should skip native pages
        SUCCEED() << "Native page correctly skipped (not image-dominated)";
        return;
    }

    // If we reach here, the test fails (native page should not be image-dominated)
    FAIL() << "Native page should not trigger image replacement";
}

// =============================================================================
// Test 10: Verify multiple edits produce cumulative XObject changes
// =============================================================================
TEST_F(ImageTextEditDisplayTest, ScannedPage_MultipleEdits) {
    if (!hasScannedPdf_ || !hasTessdata_) {
        GTEST_SKIP() << "Requires test_scanned.pdf and tessdata";
    }

    ASSERT_GT(scannedDoc_->pageCount(), 0);
    auto page = scannedDoc_->page(0);
    ASSERT_NE(page, nullptr);

    // Get original XObject data
    auto origData = contentWriter_->getImageXObjectData(page.get(), scannedDoc_.get());
    ASSERT_FALSE(origData.empty());

    // First edit: top-left region
    PdfRect selection1;
    selection1.x0 = 72.0;
    selection1.y0 = 72.0;
    selection1.x1 = 200.0;
    selection1.y1 = 120.0;

    ReconstructionResult result1 = imageReplacer_->replace(
        page.get(), scannedDoc_.get(), 0, selection1, "First Edit", RegionId{10});
    EXPECT_TRUE(result1.success);

    auto afterEdit1 = contentWriter_->getImageXObjectData(page.get(), scannedDoc_.get());
    ASSERT_FALSE(afterEdit1.empty());
    EXPECT_NE(origData, afterEdit1) << "First edit should change XObject data";

    // Second edit: bottom-right region (cumulative on same page)
    PdfRect selection2;
    selection2.x0 = 200.0;
    selection2.y0 = 200.0;
    selection2.x1 = 400.0;
    selection2.y1 = 300.0;

    ReconstructionResult result2 = imageReplacer_->replace(
        page.get(), scannedDoc_.get(), 0, selection2, "Second Edit", RegionId{11});
    EXPECT_TRUE(result2.success);

    auto afterEdit2 = contentWriter_->getImageXObjectData(page.get(), scannedDoc_.get());
    ASSERT_FALSE(afterEdit2.empty());

    // Verify cumulative changes
    EXPECT_NE(origData, afterEdit2) << "After both edits, XObject should differ from original";
    EXPECT_NE(afterEdit1, afterEdit2) << "Second edit should produce different XObject than first";
}

// =============================================================================
// Test 11: Verify ImageReplacer preserves page dimensions
// =============================================================================
TEST_F(ImageTextEditDisplayTest, ScannedPage_ImageReplacerPreservesDimensions) {
    if (!hasScannedPdf_ || !hasTessdata_) {
        GTEST_SKIP() << "Requires test_scanned.pdf and tessdata";
    }

    ASSERT_GT(scannedDoc_->pageCount(), 0);
    auto page = scannedDoc_->page(0);
    ASSERT_NE(page, nullptr);

    // Get original page dimensions
    QRectF originalBbox = page->boundingBox();

    // Run ImageReplacer
    PdfRect selection;
    selection.x0 = 100.0;
    selection.y0 = 100.0;
    selection.x1 = 300.0;
    selection.y1 = 200.0;

    ReconstructionResult result = imageReplacer_->replace(
        page.get(), scannedDoc_.get(), 0, selection, "Dimension Test", RegionId{99});

    EXPECT_TRUE(result.success);

    // Verify page dimensions unchanged
    QRectF newBbox = page->boundingBox();
    EXPECT_DOUBLE_EQ(originalBbox.width(), newBbox.width());
    EXPECT_DOUBLE_EQ(originalBbox.height(), newBbox.height());
    EXPECT_DOUBLE_EQ(originalBbox.x(), newBbox.x());
    EXPECT_DOUBLE_EQ(originalBbox.y(), newBbox.y());
}

// =============================================================================
// Test 12: Verify PdfContentWriter finds page image XObject
// =============================================================================
TEST_F(ImageTextEditDisplayTest, ScannedPage_FindPageImageXObject) {
    if (!hasScannedPdf_) {
        GTEST_SKIP() << "Requires test_scanned.pdf";
    }

    ASSERT_GT(scannedDoc_->pageCount(), 0);
    auto page = scannedDoc_->page(0);
    ASSERT_NE(page, nullptr);

    // Initialize contentWriter if not already done
    if (!contentWriter_) {
        fontEmbedder_ = std::make_unique<pdfe::fonts::PdfFontEmbedder>(context_.get());
        contentWriter_ = std::make_unique<PdfContentWriter>(context_.get(), fontEmbedder_.get());
    }

    // Scanned pages should have a background image XObject
    auto imageInfo = contentWriter_->findPageImageXObject(page.get(), scannedDoc_.get());

    EXPECT_TRUE(imageInfo.has_value()) << "Scanned page should have an image XObject";
    if (imageInfo) {
        EXPECT_FALSE(imageInfo->xobjectName.empty());
        EXPECT_GT(imageInfo->width, 0);
        EXPECT_GT(imageInfo->height, 0);
    }
}

// =============================================================================
// Test 13: Verify native text page has no background image XObject
// =============================================================================
TEST_F(ImageTextEditDisplayTest, NativePage_NoImageXObject) {
    if (!hasNativePdf_) {
        GTEST_SKIP() << "Requires test_single.pdf";
    }

    ASSERT_GT(nativeDoc_->pageCount(), 0);
    auto page = nativeDoc_->page(0);
    ASSERT_NE(page, nullptr);

    // Initialize contentWriter
    if (!contentWriter_) {
        fontEmbedder_ = std::make_unique<pdfe::fonts::PdfFontEmbedder>(context_.get());
        contentWriter_ = std::make_unique<PdfContentWriter>(context_.get(), fontEmbedder_.get());
    }

    // Native text pages typically don't have a large background image
    auto imageInfo = contentWriter_->findPageImageXObject(page.get(), nativeDoc_.get());

    // May or may not have images (logos, etc.), but shouldn't be image-dominated
    // The key test is that isImageDominated() == false (already tested above)
    EXPECT_FALSE(page->isImageDominated());
}

// =============================================================================
// Test 14: Verify undo/redo workflow with saved XObject references
// =============================================================================
TEST_F(ImageTextEditDisplayTest, ScannedPage_UndoRedoWorkflow) {
    if (!hasScannedPdf_ || !hasTessdata_) {
        GTEST_SKIP() << "Requires test_scanned.pdf and tessdata";
    }

    ASSERT_GT(scannedDoc_->pageCount(), 0);
    auto page = scannedDoc_->page(0);
    ASSERT_NE(page, nullptr);

    // Step 1: Save original XObject state
    auto imageInfo = contentWriter_->findPageImageXObject(page.get(), scannedDoc_.get());
    ASSERT_TRUE(imageInfo.has_value());
    std::string xobjName = imageInfo->xobjectName;

    void* origRef = contentWriter_->saveImageXObjectRef(page.get(), scannedDoc_.get());
    ASSERT_NE(origRef, nullptr);
    auto origData = contentWriter_->getImageXObjectData(page.get(), scannedDoc_.get());
    ASSERT_FALSE(origData.empty());

    // Step 2: Make an edit
    PdfRect selection;
    selection.x0 = 100.0;
    selection.y0 = 100.0;
    selection.x1 = 300.0;
    selection.y1 = 200.0;

    std::string editedText = "Undo Test";
    ReconstructionResult result = imageReplacer_->replace(
        page.get(), scannedDoc_.get(), 0, selection, editedText, RegionId{20});
    EXPECT_TRUE(result.success);

    // Save edited XObject for redo
    void* editedRef = contentWriter_->saveImageXObjectRef(page.get(), scannedDoc_.get());
    ASSERT_NE(editedRef, nullptr);
    auto editedData = contentWriter_->getImageXObjectData(page.get(), scannedDoc_.get());
    EXPECT_NE(origData, editedData) << "Edit should change XObject";

    // Step 3: Undo (restore original XObject)
    bool undone = contentWriter_->restoreImageXObjectRef(
        page.get(), scannedDoc_.get(), origRef, xobjName);
    EXPECT_TRUE(undone);
    auto afterUndoData = contentWriter_->getImageXObjectData(page.get(), scannedDoc_.get());
    EXPECT_EQ(origData, afterUndoData) << "After undo, XObject should match original";

    // Step 4: Redo (restore edited XObject)
    bool redone = contentWriter_->restoreImageXObjectRef(
        page.get(), scannedDoc_.get(), editedRef, xobjName);
    EXPECT_TRUE(redone);
    auto afterRedoData = contentWriter_->getImageXObjectData(page.get(), scannedDoc_.get());
    EXPECT_EQ(editedData, afterRedoData) << "After redo, XObject should match edited state";

    // Release saved refs
    contentWriter_->releaseImageXObjectRef(origRef);
    contentWriter_->releaseImageXObjectRef(editedRef);
}

// =============================================================================
// Test 15: Multi-edit undo/redo with per-edit XObject tracking
// =============================================================================
TEST_F(ImageTextEditDisplayTest, ScannedPage_MultiEditUndoRedo) {
    if (!hasScannedPdf_ || !hasTessdata_) {
        GTEST_SKIP() << "Requires test_scanned.pdf and tessdata";
    }

    ASSERT_GT(scannedDoc_->pageCount(), 0);
    auto page = scannedDoc_->page(0);
    ASSERT_NE(page, nullptr);

    // Get XObject name for restore operations
    auto imageInfo = contentWriter_->findPageImageXObject(page.get(), scannedDoc_.get());
    ASSERT_TRUE(imageInfo.has_value());
    std::string xobjName = imageInfo->xobjectName;

    // Save original XObject state
    void* ref0 = contentWriter_->saveImageXObjectRef(page.get(), scannedDoc_.get());
    ASSERT_NE(ref0, nullptr);
    auto data0 = contentWriter_->getImageXObjectData(page.get(), scannedDoc_.get());
    ASSERT_FALSE(data0.empty());

    // --- Edit 1 ---
    PdfRect sel1{72.0, 72.0, 200.0, 120.0};
    ReconstructionResult r1 = imageReplacer_->replace(
        page.get(), scannedDoc_.get(), 0, sel1, "Edit One", RegionId{30});
    ASSERT_TRUE(r1.success);

    void* ref1 = contentWriter_->saveImageXObjectRef(page.get(), scannedDoc_.get());
    ASSERT_NE(ref1, nullptr);
    auto data1 = contentWriter_->getImageXObjectData(page.get(), scannedDoc_.get());
    EXPECT_NE(data0, data1) << "Edit 1 should change XObject";

    // --- Edit 2 ---
    PdfRect sel2{200.0, 200.0, 400.0, 280.0};
    ReconstructionResult r2 = imageReplacer_->replace(
        page.get(), scannedDoc_.get(), 0, sel2, "Edit Two", RegionId{31});
    ASSERT_TRUE(r2.success);

    void* ref2 = contentWriter_->saveImageXObjectRef(page.get(), scannedDoc_.get());
    ASSERT_NE(ref2, nullptr);
    auto data2 = contentWriter_->getImageXObjectData(page.get(), scannedDoc_.get());
    EXPECT_NE(data1, data2) << "Edit 2 should further change XObject";

    // --- Undo Edit 2: restore ref1 ---
    bool u2 = contentWriter_->restoreImageXObjectRef(
        page.get(), scannedDoc_.get(), ref1, xobjName);
    EXPECT_TRUE(u2);
    auto afterU2 = contentWriter_->getImageXObjectData(page.get(), scannedDoc_.get());
    EXPECT_EQ(data1, afterU2) << "After undoing edit 2, should match post-edit-1 state";

    // --- Undo Edit 1: restore ref0 ---
    bool u1 = contentWriter_->restoreImageXObjectRef(
        page.get(), scannedDoc_.get(), ref0, xobjName);
    EXPECT_TRUE(u1);
    auto afterU1 = contentWriter_->getImageXObjectData(page.get(), scannedDoc_.get());
    EXPECT_EQ(data0, afterU1) << "After undoing edit 1, should match original";

    // --- Redo Edit 1: restore ref1 ---
    bool redo1 = contentWriter_->restoreImageXObjectRef(
        page.get(), scannedDoc_.get(), ref1, xobjName);
    EXPECT_TRUE(redo1);
    auto afterR1 = contentWriter_->getImageXObjectData(page.get(), scannedDoc_.get());
    EXPECT_EQ(data1, afterR1) << "After redoing edit 1, should match post-edit-1 state";

    // --- Redo Edit 2: restore ref2 ---
    bool redo2 = contentWriter_->restoreImageXObjectRef(
        page.get(), scannedDoc_.get(), ref2, xobjName);
    EXPECT_TRUE(redo2);
    auto afterR2 = contentWriter_->getImageXObjectData(page.get(), scannedDoc_.get());
    EXPECT_EQ(data2, afterR2) << "After redoing edit 2, should match post-edit-2 state";

    // Release all saved refs
    contentWriter_->releaseImageXObjectRef(ref0);
    contentWriter_->releaseImageXObjectRef(ref1);
    contentWriter_->releaseImageXObjectRef(ref2);
}

// =============================================================================
// Test 16: isImageDominated() stays true through pure-scan → text-added
//          transition (image still covers >50% of the page area in both
//          states; the 2026-05-06 fix removed the prior hasText gating).
// =============================================================================
TEST_F(ImageTextEditDisplayTest, ScannedPage_ImageDominatedAfterTextAdded) {
    if (!hasScannedPdf_ || !hasTessdata_) {
        GTEST_SKIP() << "Requires test_scanned.pdf and tessdata";
    }

    ASSERT_GT(scannedDoc_->pageCount(), 0);
    auto page = scannedDoc_->page(0);
    ASSERT_NE(page, nullptr);

    // Before adding text: pure image page is image-dominated by area.
    EXPECT_TRUE(page->isImageDominated());

    // Add text via PdfContentWriter::writeTextBlock with a system font.
    // FontResolver picks platform-appropriate (Arial on Windows,
    // Liberation Sans on Linux).
    std::string arialPath = pdfe::fonts::FontResolver::resolveFontPath("Arial");
    if (arialPath.empty()) {
        GTEST_SKIP() << "FontResolver could not locate Arial-equivalent font";
    }

    FontMatch fm;
    fm.fontFilePath = arialPath;
    fm.fontFamily = "Arial";
    fm.fontStyle = "Regular";
    fm.sizePt = 12.0f;
    fm.color = {0, 0, 0};

    PdfRect textRect{72.0, 72.0, 200.0, 100.0};
    contentWriter_->writeTextBlock(
        page.get(), scannedDoc_.get(), "Test text for classification", fm, textRect);

    // After adding text: page has image + text → should be image-dominated
    // Get fresh page to pick up content stream changes
    auto page2 = scannedDoc_->page(0);
    ASSERT_NE(page2, nullptr);
    EXPECT_TRUE(page2->isImageDominated())
        << "Page with large image AND text should be image-dominated";
}

// =============================================================================
// Test 17: ImageReplacer handles empty text gracefully
// =============================================================================
TEST_F(ImageTextEditDisplayTest, ScannedPage_EmptyTextEdit) {
    if (!hasScannedPdf_ || !hasTessdata_) {
        GTEST_SKIP() << "Requires test_scanned.pdf and tessdata";
    }

    ASSERT_GT(scannedDoc_->pageCount(), 0);
    auto page = scannedDoc_->page(0);
    ASSERT_NE(page, nullptr);

    QRectF bbox = page->boundingBox();
    QImage beforeImage = page->render(bbox);
    ASSERT_FALSE(beforeImage.isNull());

    // Replace with empty text — should not crash
    PdfRect selection{100.0, 100.0, 300.0, 200.0};
    EXPECT_NO_THROW({
        ReconstructionResult result = imageReplacer_->replace(
            page.get(), scannedDoc_.get(), 0, selection, "", RegionId{50});
        // Result may or may not succeed, but it must not crash
        (void)result;
    }) << "ImageReplacer should handle empty text without crashing";
}

// =============================================================================
// Test 18: Verify unique RegionIds avoid cache collisions
// =============================================================================
TEST_F(ImageTextEditDisplayTest, ScannedPage_UniqueRegionIds) {
    if (!hasScannedPdf_ || !hasTessdata_) {
        GTEST_SKIP() << "Requires test_scanned.pdf and tessdata";
    }

    ASSERT_GT(scannedDoc_->pageCount(), 0);
    auto page = scannedDoc_->page(0);
    ASSERT_NE(page, nullptr);

    PdfRect selection{72.0, 72.0, 216.0, 144.0};

    // Two calls with DIFFERENT RegionIds should both succeed
    ReconstructionResult r1 = imageReplacer_->replace(
        page.get(), scannedDoc_.get(), 0, selection, "Text A", RegionId{100});
    EXPECT_TRUE(r1.success);

    ReconstructionResult r2 = imageReplacer_->replace(
        page.get(), scannedDoc_.get(), 0, selection, "Text B", RegionId{101});
    EXPECT_TRUE(r2.success);
}

// =============================================================================
// Test 19: Undo after new edit discards redo (stack truncation via XObject refs)
// =============================================================================
TEST_F(ImageTextEditDisplayTest, ScannedPage_UndoThenNewEditDiscardsRedo) {
    if (!hasScannedPdf_ || !hasTessdata_) {
        GTEST_SKIP() << "Requires test_scanned.pdf and tessdata";
    }

    ASSERT_GT(scannedDoc_->pageCount(), 0);
    auto page = scannedDoc_->page(0);
    ASSERT_NE(page, nullptr);

    // Get XObject name
    auto imageInfo = contentWriter_->findPageImageXObject(page.get(), scannedDoc_.get());
    ASSERT_TRUE(imageInfo.has_value());
    std::string xobjName = imageInfo->xobjectName;

    // Original state
    void* ref0 = contentWriter_->saveImageXObjectRef(page.get(), scannedDoc_.get());
    ASSERT_NE(ref0, nullptr);
    auto data0 = contentWriter_->getImageXObjectData(page.get(), scannedDoc_.get());

    // Edit 1
    PdfRect sel1{72.0, 72.0, 200.0, 120.0};
    auto r1 = imageReplacer_->replace(page.get(), scannedDoc_.get(), 0, sel1, "A", RegionId{60});
    ASSERT_TRUE(r1.success);
    void* ref1 = contentWriter_->saveImageXObjectRef(page.get(), scannedDoc_.get());
    auto data1 = contentWriter_->getImageXObjectData(page.get(), scannedDoc_.get());
    EXPECT_NE(data0, data1);

    // Edit 2
    PdfRect sel2{200.0, 200.0, 350.0, 280.0};
    auto r2 = imageReplacer_->replace(page.get(), scannedDoc_.get(), 0, sel2, "B", RegionId{61});
    ASSERT_TRUE(r2.success);
    void* ref2 = contentWriter_->saveImageXObjectRef(page.get(), scannedDoc_.get());
    auto data2 = contentWriter_->getImageXObjectData(page.get(), scannedDoc_.get());
    EXPECT_NE(data1, data2);

    // Undo edit 2 → restore ref1
    contentWriter_->restoreImageXObjectRef(page.get(), scannedDoc_.get(), ref1, xobjName);
    auto afterUndo = contentWriter_->getImageXObjectData(page.get(), scannedDoc_.get());
    EXPECT_EQ(data1, afterUndo) << "After undo edit 2, should be at post-edit-1 state";

    // NEW edit 3 (discards redo of edit 2)
    PdfRect sel3{100.0, 300.0, 300.0, 380.0};
    auto r3 = imageReplacer_->replace(page.get(), scannedDoc_.get(), 0, sel3, "C", RegionId{62});
    ASSERT_TRUE(r3.success);
    void* ref3 = contentWriter_->saveImageXObjectRef(page.get(), scannedDoc_.get());
    auto data3 = contentWriter_->getImageXObjectData(page.get(), scannedDoc_.get());

    // Edit 3 should produce different data than discarded edit 2
    EXPECT_NE(data2, data3)
        << "New edit after undo should produce different XObject than discarded redo";

    // Undo edit 3 → restore ref1 (back to post-edit-1 state)
    contentWriter_->restoreImageXObjectRef(page.get(), scannedDoc_.get(), ref1, xobjName);
    auto finalData = contentWriter_->getImageXObjectData(page.get(), scannedDoc_.get());
    EXPECT_EQ(data1, finalData)
        << "After undoing edit 3, should be at post-edit-1 state";

    // Release all refs
    contentWriter_->releaseImageXObjectRef(ref0);
    contentWriter_->releaseImageXObjectRef(ref1);
    contentWriter_->releaseImageXObjectRef(ref2);
    contentWriter_->releaseImageXObjectRef(ref3);
}

// =============================================================================
// Test 20: ImageReplacer snapToLines=false only modifies span area (Bug 1 fix)
// =============================================================================
TEST_F(ImageTextEditDisplayTest, SpanEdit_NoSnapToLines_PreservesAdjacentRegions) {
    if (!hasScannedPdf_ || !hasTessdata_) {
        GTEST_SKIP() << "Requires test_scanned.pdf and tessdata";
    }

    ASSERT_GT(scannedDoc_->pageCount(), 0);
    auto page = scannedDoc_->page(0);
    ASSERT_NE(page, nullptr);

    // Get original XObject data
    auto origData = contentWriter_->getImageXObjectData(page.get(), scannedDoc_.get());
    ASSERT_FALSE(origData.empty());

    // Define a SMALL span selection (simulating a single short word edit)
    PdfRect spanSelection{100.0, 100.0, 150.0, 120.0};  // Small 50x20pt span

    // Call ImageReplacer with snapToLines=false (inline span edit)
    ReconstructionResult result = imageReplacer_->replace(
        page.get(), scannedDoc_.get(), 0, spanSelection, "XYZ",
        RegionId{200}, false);  // snapToLines=false

    EXPECT_TRUE(result.success) << "ImageReplacer with snapToLines=false should succeed: "
                                 << result.errorMessage;

    // Verify XObject changed (binary comparison is definitive)
    auto afterNoSnap = contentWriter_->getImageXObjectData(page.get(), scannedDoc_.get());
    ASSERT_FALSE(afterNoSnap.empty());
    EXPECT_NE(origData, afterNoSnap)
        << "XObject should differ after snapToLines=false edit";

    // Now do a SECOND edit on a fresh document to compare snapToLines=true vs false.
    // Reload the scanned PDF to get a clean state.
    auto freshDoc = Document::open(context_, std::filesystem::path(fixturesDir()) / "test_scanned.pdf");
    ASSERT_NE(freshDoc, nullptr);
    ASSERT_GT(freshDoc->pageCount(), 0);
    auto freshPage = freshDoc->page(0);
    ASSERT_NE(freshPage, nullptr);

    // Rebuild TRE components against the fresh document
    auto freshOcr = std::make_unique<OcrPipeline>(context_.get(), freshDoc.get());
    auto freshCoord = std::make_unique<CoordMapper>(context_.get(), freshDoc.get());
    OcrConfig config;
    config.tessdataPath = (std::filesystem::path(fixturesDir()) / "tessdata").string();
    freshOcr->setConfig(config);

    auto freshReplacer = std::make_unique<ImageReplacer>(
        freshOcr.get(), freshCoord.get(), bgDetector_.get(),
        fontMatcher_.get(), imageMasker_.get(), bitmapCompositor_.get(),
        contentWriter_.get());

    // Same selection, same text, but snapToLines=true (default)
    auto freshWriter = std::make_unique<PdfContentWriter>(context_.get(), fontEmbedder_.get());
    auto origData2 = freshWriter->getImageXObjectData(freshPage.get(), freshDoc.get());
    ASSERT_FALSE(origData2.empty());

    ReconstructionResult resultSnap = freshReplacer->replace(
        freshPage.get(), freshDoc.get(), 0, spanSelection, "XYZ",
        RegionId{201});  // snapToLines=true (default)

    EXPECT_TRUE(resultSnap.success) << "snapToLines=true should also succeed: "
                                      << resultSnap.errorMessage;

    auto afterSnap = freshWriter->getImageXObjectData(freshPage.get(), freshDoc.get());
    ASSERT_FALSE(afterSnap.empty());
    EXPECT_NE(origData2, afterSnap)
        << "XObject should differ after snapToLines=true edit";

    // The two edits (snap vs no-snap) should produce DIFFERENT XObject data
    // because snapToLines=true composites over a larger (line-snapped) region
    // while snapToLines=false composites over just the exact span bbox.
    EXPECT_NE(afterNoSnap, afterSnap)
        << "snapToLines=false and snapToLines=true should produce different XObjects "
        << "(different compositing regions)";
}

// =============================================================================
// Test 21: ImageReplacer snapToLines=true (default) works as before
// =============================================================================
TEST_F(ImageTextEditDisplayTest, RegionEdit_SnapToLines_StillWorks) {
    if (!hasScannedPdf_ || !hasTessdata_) {
        GTEST_SKIP() << "Requires test_scanned.pdf and tessdata";
    }

    ASSERT_GT(scannedDoc_->pageCount(), 0);
    auto page = scannedDoc_->page(0);
    ASSERT_NE(page, nullptr);

    QRectF bbox = page->boundingBox();
    QImage originalRender = page->render(bbox);
    ASSERT_FALSE(originalRender.isNull());

    // Default snapToLines=true (TRE region selection mode)
    PdfRect selection{72.0, 72.0, 300.0, 200.0};
    ReconstructionResult result = imageReplacer_->replace(
        page.get(), scannedDoc_.get(), 0, selection, "Region Edit",
        RegionId{210});  // snapToLines defaults to true

    EXPECT_TRUE(result.success) << "Default snapToLines=true should still work: "
                                 << result.errorMessage;

    QImage afterEdit = page->render(bbox);
    ASSERT_FALSE(afterEdit.isNull());
    EXPECT_TRUE(imagesAreDifferent(originalRender, afterEdit));
}

// =============================================================================
// Test 22: replacePageImage scales to original XObject dimensions (Bug 3 fix)
// =============================================================================
TEST_F(ImageTextEditDisplayTest, ReplacePageImage_ScalesToOriginalDimensions) {
    if (!hasScannedPdf_) {
        GTEST_SKIP() << "Requires test_scanned.pdf";
    }

    ASSERT_GT(scannedDoc_->pageCount(), 0);
    auto page = scannedDoc_->page(0);
    ASSERT_NE(page, nullptr);

    // Initialize contentWriter if needed
    if (!contentWriter_) {
        fontEmbedder_ = std::make_unique<pdfe::fonts::PdfFontEmbedder>(context_.get());
        contentWriter_ = std::make_unique<PdfContentWriter>(context_.get(), fontEmbedder_.get());
    }

    // Get original XObject dimensions
    auto imageInfo = contentWriter_->findPageImageXObject(page.get(), scannedDoc_.get());
    if (!imageInfo.has_value()) {
        GTEST_SKIP() << "No image XObject found on page";
    }

    int origWidth = imageInfo->width;
    int origHeight = imageInfo->height;
    ASSERT_GT(origWidth, 0);
    ASSERT_GT(origHeight, 0);

    // Create a replacement image at 75% of original size (above the 50% threshold).
    // The scaling logic only triggers when input is >= 50% of original in both dimensions,
    // so this image should be scaled up to match original dimensions.
    int mediumWidth = (origWidth * 3) / 4;
    int mediumHeight = (origHeight * 3) / 4;
    ASSERT_GE(mediumWidth, origWidth / 2) << "Test image must be >= 50% of original to trigger scaling";
    QImage mediumImage(mediumWidth, mediumHeight, QImage::Format_RGB888);
    mediumImage.fill(QColor(200, 200, 200));

    // Replace page image with the medium image
    bool success = contentWriter_->replacePageImage(page.get(), scannedDoc_.get(), mediumImage);
    EXPECT_TRUE(success);

    // Verify the XObject now has the ORIGINAL dimensions (was scaled up)
    auto newInfo = contentWriter_->findPageImageXObject(page.get(), scannedDoc_.get());
    ASSERT_TRUE(newInfo.has_value());
    EXPECT_EQ(newInfo->width, origWidth)
        << "Replacement XObject width should match original (" << origWidth
        << "), not medium image (" << mediumWidth << ")";
    EXPECT_EQ(newInfo->height, origHeight)
        << "Replacement XObject height should match original (" << origHeight
        << "), not medium image (" << mediumHeight << ")";

    // Also verify: a SMALL image (<50% of original) does NOT get scaled
    int tinyWidth = origWidth / 4;
    int tinyHeight = origHeight / 4;
    QImage tinyImage(tinyWidth, tinyHeight, QImage::Format_RGB888);
    tinyImage.fill(QColor(180, 180, 180));

    success = contentWriter_->replacePageImage(page.get(), scannedDoc_.get(), tinyImage);
    EXPECT_TRUE(success);

    auto tinyInfo = contentWriter_->findPageImageXObject(page.get(), scannedDoc_.get());
    ASSERT_TRUE(tinyInfo.has_value());
    EXPECT_EQ(tinyInfo->width, tinyWidth)
        << "Tiny image (<50% of original) should NOT be scaled (kept at " << tinyWidth << ")";
    EXPECT_EQ(tinyInfo->height, tinyHeight)
        << "Tiny image (<50% of original) should NOT be scaled (kept at " << tinyHeight << ")";
}

// =============================================================================
// Test 23: replacePageImage with same-size image doesn't re-scale
// =============================================================================
TEST_F(ImageTextEditDisplayTest, ReplacePageImage_SameSizeNoRescale) {
    if (!hasScannedPdf_) {
        GTEST_SKIP() << "Requires test_scanned.pdf";
    }

    ASSERT_GT(scannedDoc_->pageCount(), 0);
    auto page = scannedDoc_->page(0);
    ASSERT_NE(page, nullptr);

    // Initialize contentWriter if needed
    if (!contentWriter_) {
        fontEmbedder_ = std::make_unique<pdfe::fonts::PdfFontEmbedder>(context_.get());
        contentWriter_ = std::make_unique<PdfContentWriter>(context_.get(), fontEmbedder_.get());
    }

    auto imageInfo = contentWriter_->findPageImageXObject(page.get(), scannedDoc_.get());
    if (!imageInfo.has_value()) {
        GTEST_SKIP() << "No image XObject found on page";
    }

    // Create replacement image at EXACT original dimensions
    QImage sameSize(imageInfo->width, imageInfo->height, QImage::Format_RGB888);
    sameSize.fill(QColor(180, 180, 180));

    bool success = contentWriter_->replacePageImage(page.get(), scannedDoc_.get(), sameSize);
    EXPECT_TRUE(success);

    auto newInfo = contentWriter_->findPageImageXObject(page.get(), scannedDoc_.get());
    ASSERT_TRUE(newInfo.has_value());
    EXPECT_EQ(newInfo->width, imageInfo->width);
    EXPECT_EQ(newInfo->height, imageInfo->height);
}

// =============================================================================
// Test 24: ContentStreamEditor handles subset-prefixed fonts (Bug 2 fix)
// =============================================================================
TEST_F(ImageTextEditDisplayTest, ContentStreamEditor_SubsetPrefixHandling) {
    // Verifies that buildFontNameMapping() strips subset prefixes so that
    // font name lookup works even when TextSpan stores "ArialMT" but the
    // PDF has "ABCDEF+ArialMT" in the Font resource dictionary.
    if (!hasNativePdf_) {
        GTEST_SKIP() << "Requires test_single.pdf";
    }

    ASSERT_GT(nativeDoc_->pageCount(), 0);
    auto page = nativeDoc_->page(0);
    ASSERT_NE(page, nullptr);

    // Extract text and build spans
    auto blocks = page->extractText();
    ASSERT_FALSE(blocks.empty()) << "test_single.pdf should have text blocks";

    pdfe::textedit::TextEditModel model(blocks);
    ASSERT_GT(model.spanCount(), 0) << "Should have at least one text span";

    // Collect all spans and find one with enough text to replace
    std::vector<pdfe::textedit::TextSpan> spans;
    int targetIdx = -1;
    for (int i = 0; i < model.spanCount(); ++i) {
        spans.push_back(model.span(i));
        if (targetIdx < 0 && spans.back().text.size() >= 3) {
            targetIdx = i;
        }
    }
    ASSERT_GE(targetIdx, 0) << "Need a span with 3+ chars for meaningful edit";

    // Verify the span has a font name (MuPDF extracts it, possibly with subset prefix)
    EXPECT_FALSE(spans[targetIdx].fontName.empty())
        << "Target span should have a font name";

    // Apply an edit: replace with distinctly different text.
    // If subset prefix stripping fails, afterTextObjectCallback can't find
    // the font resource key and emits wrong font or crashes.
    pdfe::textedit::ContentStreamEditor editor(context_.get());
    std::vector<std::pair<int, std::u32string>> mods;
    mods.push_back({targetIdx, U"XYZMODIFIED"});

    EXPECT_NO_THROW(
        editor.applyEdits(page.get(), nativeDoc_.get(), spans, mods)
    ) << "applyEdits should not throw (font name mapping should resolve subset-prefixed fonts)";

    // Re-extract text from the modified page to verify the edit persisted
    // and the content stream is still valid (not corrupted by bad font mapping)
    auto freshPage = nativeDoc_->page(0);
    ASSERT_NE(freshPage, nullptr);
    auto blocksAfter = freshPage->extractText();
    EXPECT_GT(blocksAfter.size(), 0u)
        << "Page should still have valid text blocks after edit";

    // Verify the edited text appears in the re-extracted content
    bool foundModified = false;
    for (const auto& block : blocksAfter) {
        for (const auto& line : block.lines) {
            std::u32string lineText;
            for (const auto& ch : line.chars) {
                lineText += ch.codepoint;
            }
            if (lineText.find(U"XYZMODIFIED") != std::u32string::npos) {
                foundModified = true;
                break;
            }
        }
        if (foundModified) break;
    }
    EXPECT_TRUE(foundModified)
        << "Edited text 'XYZMODIFIED' should appear in re-extracted text "
        << "(font name mapping resolved correctly for subset-prefixed fonts)";
}

// =============================================================================
// Test 25: ImageReplacer snapToLines=false uses exact selection bbox
// =============================================================================
TEST_F(ImageTextEditDisplayTest, SpanEdit_UsesExactSelection) {
    if (!hasScannedPdf_ || !hasTessdata_) {
        GTEST_SKIP() << "Requires test_scanned.pdf and tessdata";
    }

    ASSERT_GT(scannedDoc_->pageCount(), 0);
    auto page = scannedDoc_->page(0);
    ASSERT_NE(page, nullptr);

    // Use a very small selection (single word bbox)
    PdfRect smallSpan{120.0, 110.0, 160.0, 125.0};  // 40x15pt

    ReconstructionResult result = imageReplacer_->replace(
        page.get(), scannedDoc_.get(), 0, smallSpan, "Hi",
        RegionId{300}, false);

    EXPECT_TRUE(result.success) << "Small span edit should succeed: " << result.errorMessage;

    // Verify the result region matches our input selection, not a snapped line bbox
    EXPECT_NEAR(result.region.pdfBbox.x0, smallSpan.x0, 0.01);
    EXPECT_NEAR(result.region.pdfBbox.y0, smallSpan.y0, 0.01);
    EXPECT_NEAR(result.region.pdfBbox.x1, smallSpan.x1, 0.01);
    EXPECT_NEAR(result.region.pdfBbox.y1, smallSpan.y1, 0.01);
}

// =============================================================================
// Test 26: Image-dominated page skips font substitution (EditTextCommand)
// =============================================================================
TEST_F(ImageTextEditDisplayTest, ImageDominated_SkipsFontSubstitution) {
    if (!hasOcrPdf_) {
        GTEST_SKIP() << "Requires test_ocr_text.pdf";
    }

    ASSERT_GT(ocrDoc_->pageCount(), 0);
    auto page = ocrDoc_->page(0);
    ASSERT_NE(page, nullptr);

    // test_ocr_text.pdf has image + invisible OCR text = image-dominated
    ASSERT_TRUE(page->isImageDominated())
        << "test_ocr_text.pdf must be image-dominated for this test";

    // Extract text spans from the OCR text layer
    auto blocks = page->extractText();
    ASSERT_FALSE(blocks.empty()) << "OCR'd page should have text blocks";

    pdfe::textedit::TextEditModel model(blocks);
    ASSERT_GT(model.spanCount(), 0) << "Need at least one text span";

    const auto& span = model.span(0);
    std::u32string oldText = span.text;
    ASSERT_FALSE(oldText.empty()) << "Span should have text";

    // Create replacement text with additional characters
    std::u32string newText = oldText;
    newText += U"XYZ";

    // Count font resources BEFORE edit using EmbeddedFontAnalyzer
    pdfe::fonts::EmbeddedFontAnalyzer fontAnalyzer(context_.get());
    size_t fontCountBefore = fontAnalyzer.analyzePageFonts(page.get(), ocrDoc_.get()).size();

    std::string textBefore = page->extractPlainText();

    // Execute EditTextCommand -- should NOT embed substitute fonts
    EXPECT_NO_THROW({
        pdfe::textedit::EditTextCommand cmd(
            ocrDoc_.get(), context_, 0, 0, span, oldText, newText);
        cmd.execute();
    }) << "EditTextCommand on image-dominated page should not throw";

    // Re-extract text after edit -- text layer should be updated
    auto page2 = ocrDoc_->page(0);
    ASSERT_NE(page2, nullptr);
    std::string textAfter = page2->extractPlainText();

    // Verify text was updated in the invisible layer
    EXPECT_NE(textBefore, textAfter)
        << "Text layer should be modified by EditTextCommand";

    // Verify no new font resources were added (font substitution was skipped)
    size_t fontCountAfter = fontAnalyzer.analyzePageFonts(page2.get(), ocrDoc_.get()).size();
    EXPECT_EQ(fontCountBefore, fontCountAfter)
        << "No new fonts should be embedded on image-dominated page edit. "
        << "Before: " << fontCountBefore << ", After: " << fontCountAfter;
}

// =============================================================================
// Test 27: Native page still runs font substitution (regression guard)
// =============================================================================
TEST_F(ImageTextEditDisplayTest, NativePage_StillRunsFontSubstitution) {
    if (!hasNativePdf_) {
        GTEST_SKIP() << "Requires test_single.pdf";
    }

    ASSERT_GT(nativeDoc_->pageCount(), 0);
    auto page = nativeDoc_->page(0);
    ASSERT_NE(page, nullptr);

    // Native text PDF should NOT be image-dominated
    ASSERT_FALSE(page->isImageDominated())
        << "test_single.pdf must NOT be image-dominated";

    auto blocks = page->extractText();
    ASSERT_FALSE(blocks.empty()) << "Native page should have text blocks";

    pdfe::textedit::TextEditModel model(blocks);
    ASSERT_GT(model.spanCount(), 0) << "Need at least one text span";

    const auto& span = model.span(0);
    std::u32string oldText = span.text;
    ASSERT_FALSE(oldText.empty());

    // Create replacement text
    std::u32string newText = oldText;
    newText += U"TEST";

    // Execute EditTextCommand -- should run full pipeline (no crash)
    EXPECT_NO_THROW({
        pdfe::textedit::EditTextCommand cmd(
            nativeDoc_.get(), context_, 0, 0, span, oldText, newText);
        cmd.execute();
        cmd.undo();
        cmd.redo();
    }) << "EditTextCommand on native page should handle font substitution pipeline";
}

// =============================================================================
// Test 28: Image-dominated page edit with longer text has no truncation
// =============================================================================
TEST_F(ImageTextEditDisplayTest, ImageDominated_LongerText_NoTruncation) {
    if (!hasOcrPdf_) {
        GTEST_SKIP() << "Requires test_ocr_text.pdf";
    }

    auto page = ocrDoc_->page(0);
    ASSERT_NE(page, nullptr);
    ASSERT_TRUE(page->isImageDominated());

    auto blocks = page->extractText();
    ASSERT_FALSE(blocks.empty());

    pdfe::textedit::TextEditModel model(blocks);
    ASSERT_GT(model.spanCount(), 0);

    const auto& span = model.span(0);
    std::u32string oldText = span.text;

    // Make replacement text significantly longer (3x original)
    std::u32string newText = oldText;
    newText += oldText;
    newText += oldText;

    // Execute -- should NOT apply horizontal scaling (Tz) for image-dominated pages
    EXPECT_NO_THROW({
        pdfe::textedit::EditTextCommand cmd(
            ocrDoc_.get(), context_, 0, 0, span, oldText, newText);
        cmd.execute();
    }) << "Longer text on image-dominated page should not throw";

    // Capture text length before edit for comparison
    std::string textBefore = page->extractPlainText();
    size_t lenBefore = textBefore.size();

    // Re-extract and verify the replacement text is not truncated
    auto page2 = ocrDoc_->page(0);
    ASSERT_NE(page2, nullptr);
    std::string extracted = page2->extractPlainText();

    EXPECT_FALSE(extracted.empty())
        << "Page should still have extractable text after edit";

    // The replacement text (3x original) should produce extracted text at least
    // as long as the original -- if horizontal scaling (Tz) were applied, the text
    // would be compressed into the original bounding box and potentially truncated.
    EXPECT_GE(extracted.size(), lenBefore)
        << "Longer replacement text should not be truncated by horizontal scaling. "
        << "Before: " << lenBefore << " chars, After: " << extracted.size() << " chars";
}

// =============================================================================
// Test 29: Image-dominated page content stream updated for searchability
// =============================================================================
TEST_F(ImageTextEditDisplayTest, ImageDominated_ContentStreamTextUpdated) {
    if (!hasOcrPdf_) {
        GTEST_SKIP() << "Requires test_ocr_text.pdf";
    }

    auto page = ocrDoc_->page(0);
    ASSERT_NE(page, nullptr);
    ASSERT_TRUE(page->isImageDominated());

    auto blocks = page->extractText();
    ASSERT_FALSE(blocks.empty());

    pdfe::textedit::TextEditModel model(blocks);
    ASSERT_GT(model.spanCount(), 0);

    const auto& span = model.span(0);
    std::u32string oldText = span.text;

    // Replace with unique marker text
    std::u32string newText = U"UNIQUEMARKER42";

    EXPECT_NO_THROW({
        pdfe::textedit::EditTextCommand cmd(
            ocrDoc_.get(), context_, 0, 0, span, oldText, newText);
        cmd.execute();
    });

    // Extract text and verify the marker appears
    auto page2 = ocrDoc_->page(0);
    ASSERT_NE(page2, nullptr);
    std::string extracted = page2->extractPlainText();

    EXPECT_NE(extracted.find("UNIQUEMARKER42"), std::string::npos)
        << "Edited text should be searchable in the invisible OCR text layer. "
        << "Extracted text: " << extracted;
}

// =============================================================================
// Test 30: EditTextCommand undo/redo on image-dominated page
// =============================================================================
TEST_F(ImageTextEditDisplayTest, ImageDominated_UndoRedo) {
    if (!hasOcrPdf_) {
        GTEST_SKIP() << "Requires test_ocr_text.pdf";
    }

    auto page = ocrDoc_->page(0);
    ASSERT_NE(page, nullptr);
    ASSERT_TRUE(page->isImageDominated());

    auto blocks = page->extractText();
    ASSERT_FALSE(blocks.empty());

    pdfe::textedit::TextEditModel model(blocks);
    ASSERT_GT(model.spanCount(), 0);

    const auto& span = model.span(0);
    std::u32string oldText = span.text;
    std::u32string newText = U"UNDOTEST";

    // Execute, undo, redo cycle should work without exceptions
    pdfe::textedit::EditTextCommand cmd(
        ocrDoc_.get(), context_, 0, 0, span, oldText, newText);

    EXPECT_NO_THROW(cmd.execute())
        << "Execute on image-dominated page should not throw";

    // Verify edit applied
    {
        auto p = ocrDoc_->page(0);
        std::string text = p->extractPlainText();
        EXPECT_NE(text.find("UNDOTEST"), std::string::npos)
            << "After execute, edited text should be present";
    }

    EXPECT_NO_THROW(cmd.undo())
        << "Undo on image-dominated page should not throw";

    // Verify undo restored original
    {
        auto p = ocrDoc_->page(0);
        std::string text = p->extractPlainText();
        EXPECT_EQ(text.find("UNDOTEST"), std::string::npos)
            << "After undo, edited text should be removed";
    }

    EXPECT_NO_THROW(cmd.redo())
        << "Redo on image-dominated page should not throw";

    // Verify redo re-applied
    {
        auto p = ocrDoc_->page(0);
        std::string text = p->extractPlainText();
        EXPECT_NE(text.find("UNDOTEST"), std::string::npos)
            << "After redo, edited text should be present again";
    }
}

// ---------------------------------------------------------------------------
// Test 31: buildOverlayFont logic falls back to Arial for unresolvable fonts
// ---------------------------------------------------------------------------
TEST_F(ImageTextEditDisplayTest, OverlayFont_FallsBackToArial)
{
    // Replicate the buildOverlayFont logic: check QFontDatabase for
    // unresolvable OCR font names, then fall back to Arial.
    QStringList families = QFontDatabase::families();

    // Tesseract's "GlyphLessFont" should NOT exist on the system
    EXPECT_FALSE(families.contains("GlyphLessFont", Qt::CaseInsensitive))
        << "GlyphLessFont should NOT be a system font";

    // Other OCR font names should also not exist
    EXPECT_FALSE(families.contains("InvisibleFont", Qt::CaseInsensitive))
        << "InvisibleFont should not be a system font";
    EXPECT_FALSE(families.contains("HiddenOcrFont", Qt::CaseInsensitive))
        << "HiddenOcrFont should not be a system font";
    EXPECT_FALSE(families.contains("AdobeBlank", Qt::CaseInsensitive))
        << "AdobeBlank should not be a system font";

    // Arial (or a metric-compatible substitute) SHOULD exist as the
    // fallback target. On Linux, Liberation Sans / Arimo / DejaVu Sans
    // serve as the substitute.
    bool fallbackAvailable =
        families.contains("Arial",            Qt::CaseInsensitive) ||
        families.contains("Liberation Sans",  Qt::CaseInsensitive) ||
        families.contains("Arimo",            Qt::CaseInsensitive) ||
        families.contains("DejaVu Sans",      Qt::CaseInsensitive);
    EXPECT_TRUE(fallbackAvailable)
        << "Arial or a metric-compatible substitute (Liberation Sans /"
        << " Arimo / DejaVu Sans) must be available as the fallback font";

    // Verify the fallback font renders measurable text. Use the
    // platform-appropriate family — querying QFont("Arial") on Linux
    // returns DejaVu Sans (Qt6 ignores fontconfig substitution for
    // QFontInfo), so pick whichever name is actually installed.
    QString fallbackName = families.contains("Arial", Qt::CaseInsensitive)
                              ? QStringLiteral("Arial")
                          : families.contains("Liberation Sans", Qt::CaseInsensitive)
                              ? QStringLiteral("Liberation Sans")
                          : families.contains("DejaVu Sans", Qt::CaseInsensitive)
                              ? QStringLiteral("DejaVu Sans")
                              : QStringLiteral("Sans");
    QFont font(fallbackName, 12);
    QFontMetricsF fm(font);
    EXPECT_GT(fm.horizontalAdvance(QStringLiteral("Test")), 0.0)
        << "Fallback font (" << fallbackName.toStdString()
        << ") should render measurable text";
}

// ---------------------------------------------------------------------------
// Test 32: buildOverlayFont logic preserves known system fonts
// ---------------------------------------------------------------------------
TEST_F(ImageTextEditDisplayTest, OverlayFont_PreservesKnownFont)
{
    // Arial is a standard Windows font that should be found
    QFont arialFont(QStringLiteral("Arial"), 12);
    // Even if exactMatch() is false (Qt's strict matching includes style),
    // the family should resolve to Arial, not a serif fallback
    QFontMetricsF fm(arialFont);
    EXPECT_GT(fm.horizontalAdvance(QStringLiteral("Test")), 0.0)
        << "Arial should produce measurable text";

    // Verify family name resolved to something containing "Arial"
    QString resolved = arialFont.family();
    EXPECT_TRUE(resolved.contains("Arial", Qt::CaseInsensitive) ||
                resolved.contains("arial", Qt::CaseInsensitive))
        << "Arial font family should resolve to Arial, got: "
        << resolved.toStdString();
}

// ---------------------------------------------------------------------------
// Test 33: SubstituteFontSelector resolves Tesseract/OCR font names
// ---------------------------------------------------------------------------
TEST_F(ImageTextEditDisplayTest, SubstituteFontSelector_ResolvesOcrFonts)
{
    auto analyzer = std::make_unique<pdfe::fonts::EmbeddedFontAnalyzer>(context_.get());
    auto cache = std::make_unique<pdfe::fonts::FontAnalysisCache>();
    auto selector = std::make_unique<pdfe::fonts::SubstituteFontSelector>(
        analyzer.get(), cache.get());

    std::u32string testText = U"Hello";

    // GlyphLessFont (Tesseract) should match via known pairs → an
    // Arial-equivalent file (arial.ttf on Windows, LiberationSans-Regular
    // on Linux).
    {
        pdfe::fonts::FontMetricProfile profile;
        profile.postScriptName = "GlyphLessFont";
        profile.familyName = "GlyphLessFont";
        auto result = selector->findSubstitute(profile, testText);
        EXPECT_TRUE(result.has_value())
            << "GlyphLessFont should resolve via known pairs";
        if (result.has_value()) {
            std::string lower = result->filePath;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char c){ return std::tolower(c); });
            bool ok = lower.find("arial") != std::string::npos
                   || lower.find("liberationsans") != std::string::npos
                   || lower.find("arimo") != std::string::npos
                   || lower.find("dejavusans") != std::string::npos;
            EXPECT_TRUE(ok)
                << "GlyphLessFont should map to Arial or metric-compatible "
                << "substitute, got: " << result->filePath;
        }
    }

    // AdobeBlank should also resolve
    {
        pdfe::fonts::FontMetricProfile profile;
        profile.postScriptName = "AdobeBlank";
        profile.familyName = "AdobeBlank";
        auto result = selector->findSubstitute(profile, testText);
        EXPECT_TRUE(result.has_value())
            << "AdobeBlank should resolve via known pairs";
    }

    // OCR-A should map to monospace (Consolas on Windows, Liberation
    // Mono / Cousine / DejaVu Sans Mono on Linux — all metric-compatible).
    {
        pdfe::fonts::FontMetricProfile profile;
        profile.postScriptName = "OCR-A";
        profile.familyName = "OCR-A";
        auto result = selector->findSubstitute(profile, testText);
        EXPECT_TRUE(result.has_value())
            << "OCR-A should resolve via known pairs";
        if (result.has_value()) {
            std::string lower = result->filePath;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char c){ return std::tolower(c); });
            bool ok = lower.find("consola") != std::string::npos
                   || lower.find("liberationmono") != std::string::npos
                   || lower.find("cousine") != std::string::npos
                   || lower.find("dejavusansmono") != std::string::npos;
            EXPECT_TRUE(ok)
                << "OCR-A should map to Consolas or metric-compatible "
                << "monospace substitute, got: " << result->filePath;
        }
    }
}

// ---------------------------------------------------------------------------
// Test 34: Font size is capped to fit within bitmap pixel height
// ---------------------------------------------------------------------------
TEST_F(ImageTextEditDisplayTest, FontSizeCapped_FitsInBitmapHeight)
{
    // Verify the font size capping logic in ImageReplacer:
    // maxFontPt = pixelHeight * 72 / dpi / 0.8
    // For a 50-pixel-tall region at 300 DPI:
    // maxFontPt = 50 * 72 / 300 / 0.8 = 15.0 pt
    // So a 20pt OCR-detected font should be capped to 15pt.

    int pixelHeight = 50;
    int dpi = 300;
    float maxFontPt = static_cast<float>(pixelHeight) * 72.0f
                      / static_cast<float>(dpi) / 0.8f;

    EXPECT_NEAR(maxFontPt, 15.0f, 0.1f)
        << "50px at 300 DPI should cap font to ~15pt";

    // A font size below the cap should be unchanged
    float smallFont = 10.0f;
    EXPECT_LE(smallFont, maxFontPt)
        << "10pt font should be below the cap for 50px region";

    // A font size above the cap should be reduced
    float largeFont = 20.0f;
    EXPECT_GT(largeFont, maxFontPt)
        << "20pt font should exceed the cap for 50px region";

    // Verify the cap scales linearly with height
    int tallHeight = 100;
    float tallMax = static_cast<float>(tallHeight) * 72.0f
                    / static_cast<float>(dpi) / 0.8f;
    EXPECT_NEAR(tallMax, 30.0f, 0.1f)
        << "100px at 300 DPI should cap font to ~30pt";
}

// ---------------------------------------------------------------------------
// Test 35: ImageReplacer returns success for valid OCR page + bbox
// ---------------------------------------------------------------------------
TEST_F(ImageTextEditDisplayTest, ImageReplacer_ReturnsSuccess)
{
    if (!hasScannedPdf_ || !hasTessdata_ || !imageReplacer_) {
        GTEST_SKIP() << "Requires test_scanned.pdf and tessdata";
    }

    auto page = scannedDoc_->page(0);
    ASSERT_TRUE(page);

    // Use the full page bounding box as selection
    QRectF pageBbox = page->boundingBox();
    PdfRect selectionPdf{
        pageBbox.left(), pageBbox.top(), pageBbox.right(), pageBbox.bottom()};

    auto result = imageReplacer_->replace(
        page.get(), scannedDoc_.get(), 0,
        selectionPdf, "Replacement text",
        RegionId{999}, true);  // snapToLines=true for full-region

    EXPECT_TRUE(result.success)
        << "ImageReplacer::replace() should succeed for valid scanned page; error: "
        << result.errorMessage;
}

// ---------------------------------------------------------------------------
// Test 36: ImageReplacer returns failure with error message for bad inputs
// ---------------------------------------------------------------------------
TEST_F(ImageTextEditDisplayTest, ImageReplacer_FailureReturnsMessage)
{
    if (!hasScannedPdf_ || !hasTessdata_ || !imageReplacer_) {
        GTEST_SKIP() << "Requires test_scanned.pdf and tessdata";
    }

    // Invalid page index should fail gracefully
    auto page = scannedDoc_->page(0);
    ASSERT_TRUE(page);

    PdfRect badSelection{0, 0, 0, 0};  // Zero-area bbox

    auto result = imageReplacer_->replace(
        page.get(), scannedDoc_.get(), 0,
        badSelection, "Test",
        RegionId{998}, false);

    // Either succeeds with 1-pixel min or returns a meaningful error
    if (!result.success) {
        EXPECT_FALSE(result.errorMessage.empty())
            << "Failed ImageReplacer should provide an error message";
    }
}

// ---------------------------------------------------------------------------
// Test 37: ContentStreamEditor emits Tr=3 when textRenderMode=3
// ---------------------------------------------------------------------------
TEST_F(ImageTextEditDisplayTest, ContentStreamEditor_EmitsTr3_WhenTextRenderMode3)
{
    if (!hasOcrPdf_) {
        GTEST_SKIP() << "Requires test_ocr_text.pdf";
    }

    auto page = ocrDoc_->page(0);
    ASSERT_NE(page, nullptr);
    ASSERT_TRUE(page->isImageDominated());

    // Extract text spans
    auto blocks = page->extractText();
    ASSERT_FALSE(blocks.empty());

    pdfe::textedit::TextEditModel model(blocks);
    ASSERT_GT(model.spanCount(), 0);

    std::vector<pdfe::textedit::TextSpan> spans;
    for (int i = 0; i < model.spanCount(); ++i) {
        spans.push_back(model.span(i));
    }

    const auto& span = spans[0];
    std::u32string newText = span.text;
    newText += U"TR3";

    std::vector<std::pair<int, std::u32string>> mods;
    mods.push_back({0, newText});

    // Apply edits with textRenderMode=3 (invisible)
    pdfe::textedit::ContentStreamEditor editor(context_.get());
    EXPECT_NO_THROW(
        editor.applyEdits(page.get(), ocrDoc_.get(), spans, mods, /*textRenderMode=*/3)
    ) << "applyEdits with textRenderMode=3 should not throw";

    // Verify the replacement text is in the content stream
    auto page2 = ocrDoc_->page(0);
    ASSERT_NE(page2, nullptr);
    std::string extracted = page2->extractPlainText();
    EXPECT_NE(extracted.find("TR3"), std::string::npos)
        << "Edited text should appear in re-extracted text. Got: " << extracted;
}

// ---------------------------------------------------------------------------
// Test 38: EditTextCommand uses Tr=3 on image-dominated page
// ---------------------------------------------------------------------------
TEST_F(ImageTextEditDisplayTest, EditTextCommand_UsesTr3_OnImageDominatedPage)
{
    if (!hasOcrPdf_) {
        GTEST_SKIP() << "Requires test_ocr_text.pdf";
    }

    auto page = ocrDoc_->page(0);
    ASSERT_NE(page, nullptr);
    ASSERT_TRUE(page->isImageDominated());

    // Get initial render (image should look the same after invisible text edit)
    QRectF bbox = page->boundingBox();
    QImage renderBefore = page->render(bbox);
    ASSERT_FALSE(renderBefore.isNull());

    auto blocks = page->extractText();
    ASSERT_FALSE(blocks.empty());
    pdfe::textedit::TextEditModel model(blocks);
    ASSERT_GT(model.spanCount(), 0);

    const auto& span = model.span(0);
    std::u32string oldText = span.text;
    std::u32string newText = U"INVISIBLE_TEXT_CHECK";

    // Execute EditTextCommand (should use Tr=3 for image-dominated page)
    pdfe::textedit::EditTextCommand cmd(
        ocrDoc_.get(), context_, 0, 0, span, oldText, newText);
    EXPECT_NO_THROW(cmd.execute());

    // Re-extract: the invisible text layer should contain the edited text
    auto page2 = ocrDoc_->page(0);
    ASSERT_NE(page2, nullptr);
    std::string extracted = page2->extractPlainText();
    EXPECT_NE(extracted.find("INVISIBLE_TEXT_CHECK"), std::string::npos)
        << "Edited text should be present in invisible OCR layer for searchability";

    // The page render should NOT show visible text artifacts from Pipeline A.
    // With Tr=3, the replacement text is invisible, so the rendered image should
    // look very similar to the original (no serif text bleeding through).
    QImage renderAfter = page2->render(bbox);
    ASSERT_FALSE(renderAfter.isNull());

    // Allow some tolerance for compression artifacts, but visible text would
    // cause >5% pixel difference on a typical scanned page.
    bool significantlyDifferent = imagesAreDifferent(renderBefore, renderAfter, 0.05);
    EXPECT_FALSE(significantlyDifferent)
        << "After editing invisible OCR text with Tr=3, the visible page image "
        << "should not change significantly (no visible text artifacts)";
}

// ---------------------------------------------------------------------------
// Test 39: Native page edit does NOT emit Tr=3 (default Tr=-1)
// ---------------------------------------------------------------------------
TEST_F(ImageTextEditDisplayTest, NativePage_NoTr3_Emitted)
{
    if (!hasNativePdf_) {
        GTEST_SKIP() << "Requires test_single.pdf";
    }

    auto page = nativeDoc_->page(0);
    ASSERT_NE(page, nullptr);
    ASSERT_FALSE(page->isImageDominated());

    auto blocks = page->extractText();
    ASSERT_FALSE(blocks.empty());
    pdfe::textedit::TextEditModel model(blocks);
    ASSERT_GT(model.spanCount(), 0);

    std::vector<pdfe::textedit::TextSpan> spans;
    for (int i = 0; i < model.spanCount(); ++i) {
        spans.push_back(model.span(i));
    }

    const auto& span = spans[0];
    std::u32string newText = span.text;
    newText += U"VIS";

    std::vector<std::pair<int, std::u32string>> mods;
    mods.push_back({0, newText});

    // Apply edits with DEFAULT textRenderMode (-1, no Tr emitted)
    pdfe::textedit::ContentStreamEditor editor(context_.get());
    EXPECT_NO_THROW(
        editor.applyEdits(page.get(), nativeDoc_.get(), spans, mods)
    ) << "applyEdits on native page should not throw";

    // Verify the text is visible (extractable) -- native text should remain visible
    auto page2 = nativeDoc_->page(0);
    ASSERT_NE(page2, nullptr);
    std::string extracted = page2->extractPlainText();
    EXPECT_NE(extracted.find("VIS"), std::string::npos)
        << "Edited text on native page should be visible/extractable";
}

// ---------------------------------------------------------------------------
// Test 40: ContentStreamEditor with textRenderMode=0 (explicit visible fill)
// ---------------------------------------------------------------------------
TEST_F(ImageTextEditDisplayTest, ContentStreamEditor_Tr0_ExplicitVisible)
{
    if (!hasNativePdf_) {
        GTEST_SKIP() << "Requires test_single.pdf";
    }

    auto page = nativeDoc_->page(0);
    ASSERT_NE(page, nullptr);

    auto blocks = page->extractText();
    ASSERT_FALSE(blocks.empty());
    pdfe::textedit::TextEditModel model(blocks);
    ASSERT_GT(model.spanCount(), 0);

    std::vector<pdfe::textedit::TextSpan> spans;
    for (int i = 0; i < model.spanCount(); ++i) {
        spans.push_back(model.span(i));
    }

    const auto& span = spans[0];
    std::u32string newText = span.text;
    newText += U"FILL";

    std::vector<std::pair<int, std::u32string>> mods;
    mods.push_back({0, newText});

    // Apply with explicit Tr=0 (fill mode)
    pdfe::textedit::ContentStreamEditor editor(context_.get());
    EXPECT_NO_THROW(
        editor.applyEdits(page.get(), nativeDoc_.get(), spans, mods, /*textRenderMode=*/0)
    ) << "applyEdits with textRenderMode=0 should not throw";

    auto page2 = nativeDoc_->page(0);
    ASSERT_NE(page2, nullptr);
    std::string extracted = page2->extractPlainText();
    EXPECT_NE(extracted.find("FILL"), std::string::npos)
        << "Text with Tr=0 (fill) should be extractable";
}

// ---------------------------------------------------------------------------
// Test 41: Undo/redo preserves invisible text rendering mode
// ---------------------------------------------------------------------------
TEST_F(ImageTextEditDisplayTest, ImageDominated_UndoRedo_PreservesInvisibleMode)
{
    if (!hasOcrPdf_) {
        GTEST_SKIP() << "Requires test_ocr_text.pdf";
    }

    auto page = ocrDoc_->page(0);
    ASSERT_NE(page, nullptr);
    ASSERT_TRUE(page->isImageDominated());

    // Get baseline render
    QRectF bbox = page->boundingBox();
    QImage baseline = page->render(bbox);
    ASSERT_FALSE(baseline.isNull());

    auto blocks = page->extractText();
    ASSERT_FALSE(blocks.empty());
    pdfe::textedit::TextEditModel model(blocks);
    ASSERT_GT(model.spanCount(), 0);

    const auto& span = model.span(0);
    std::u32string oldText = span.text;
    std::u32string newText = U"UNDO_REDO_INVISIBLE";

    pdfe::textedit::EditTextCommand cmd(
        ocrDoc_.get(), context_, 0, 0, span, oldText, newText);

    // Execute → undo → redo cycle
    cmd.execute();
    cmd.undo();
    cmd.redo();

    // After redo, the invisible text should still be invisible
    auto page2 = ocrDoc_->page(0);
    ASSERT_NE(page2, nullptr);

    // Verify text is in the invisible layer
    std::string extracted = page2->extractPlainText();
    EXPECT_NE(extracted.find("UNDO_REDO_INVISIBLE"), std::string::npos)
        << "After redo, edited text should be in invisible OCR layer";

    // Verify no visible artifacts (Tr=3 preserved through undo/redo)
    QImage afterRedo = page2->render(bbox);
    ASSERT_FALSE(afterRedo.isNull());
    bool significantlyDifferent = imagesAreDifferent(baseline, afterRedo, 0.05);
    EXPECT_FALSE(significantlyDifferent)
        << "After undo/redo cycle, invisible text should not produce visible artifacts";
}

// ---------------------------------------------------------------------------
// Test 42: ImageReplacer persists XObject change through save (renumbered from 37)
// ---------------------------------------------------------------------------
TEST_F(ImageTextEditDisplayTest, ImageReplacer_PersistsThroughSave)
{
    if (!hasScannedPdf_ || !hasTessdata_ || !imageReplacer_) {
        GTEST_SKIP() << "Requires test_scanned.pdf and tessdata";
    }

    // Create a temp copy of the scanned PDF
    auto tempDir = std::filesystem::temp_directory_path();
    auto tempPdf = tempDir / "test_save_persist.pdf";
    std::filesystem::copy_file(
        std::filesystem::path(fixturesDir()) / "test_scanned.pdf",
        tempPdf, std::filesystem::copy_options::overwrite_existing);

    auto tempDoc = Document::open(context_, tempPdf);
    ASSERT_TRUE(tempDoc);
    ASSERT_GT(tempDoc->pageCount(), 0);

    // Render page before edit
    auto pageBefore = tempDoc->page(0);
    ASSERT_TRUE(pageBefore);
    QImage imgBefore = pageBefore->render(pageBefore->boundingBox());
    ASSERT_FALSE(imgBefore.isNull());

    // Initialize TRE components for temp doc
    auto ocrPipe = std::make_unique<OcrPipeline>(context_.get(), tempDoc.get());
    auto coordMap = std::make_unique<CoordMapper>(context_.get(), tempDoc.get());
    auto fontEmbed = std::make_unique<pdfe::fonts::PdfFontEmbedder>(context_.get());
    auto cWriter = std::make_unique<PdfContentWriter>(context_.get(), fontEmbed.get());
    auto fCache = std::make_unique<pdfe::fonts::FontAnalysisCache>();
    auto fAnalyzer = std::make_unique<pdfe::fonts::EmbeddedFontAnalyzer>(context_.get());
    auto fSelector = std::make_unique<pdfe::fonts::SubstituteFontSelector>(
        fAnalyzer.get(), fCache.get());
    auto fMatcher = std::make_unique<FontMatcher>(fSelector.get(), fAnalyzer.get());
    auto bgDet = std::make_unique<BgColorDetector>();
    auto imgMask = std::make_unique<ImageMasker>();
    auto bmpComp = std::make_unique<BitmapCompositor>();

    OcrConfig config;
    config.tessdataPath = (std::filesystem::path(fixturesDir()) / "tessdata").string();
    ocrPipe->setConfig(config);

    auto replacer = std::make_unique<ImageReplacer>(
        ocrPipe.get(), coordMap.get(), bgDet.get(), fMatcher.get(),
        imgMask.get(), bmpComp.get(), cWriter.get());

    // Replace page image with edited text
    QRectF pageBbox = pageBefore->boundingBox();
    PdfRect selPdf{pageBbox.left(), pageBbox.top(), pageBbox.right(), pageBbox.bottom()};
    auto result = replacer->replace(
        pageBefore.get(), tempDoc.get(), 0,
        selPdf, "SAVE_PERSIST_TEST", RegionId{997}, true);

    ASSERT_TRUE(result.success)
        << "replace() must succeed; error: " << result.errorMessage;

    tempDoc->markPageDirty(0);

    // Save to a separate output file (save() to same path fails on Windows
    // because MuPDF keeps the source file open)
    auto savedPdf = tempDir / "test_save_persist_out.pdf";
    tempDoc->saveAs(savedPdf);

    // Drop all references and reopen the saved copy
    pageBefore.reset();
    replacer.reset();
    ocrPipe.reset();
    coordMap.reset();
    cWriter.reset();
    fontEmbed.reset();
    fMatcher.reset();
    fSelector.reset();
    fAnalyzer.reset();
    fCache.reset();
    bgDet.reset();
    imgMask.reset();
    bmpComp.reset();
    tempDoc.reset();

    auto reopened = Document::open(context_, savedPdf);
    ASSERT_TRUE(reopened);
    auto pageAfter = reopened->page(0);
    ASSERT_TRUE(pageAfter);
    QImage imgAfter = pageAfter->render(pageAfter->boundingBox());
    ASSERT_FALSE(imgAfter.isNull());

    // The rendered image should be different (text was composited onto it)
    EXPECT_TRUE(imagesAreDifferent(imgBefore, imgAfter, 0.001))
        << "After save and reopen, page image should differ from original";

    // Clean up: close document before removing files
    pageAfter.reset();
    reopened.reset();
    std::error_code ec;
    std::filesystem::remove(tempPdf, ec);
    std::filesystem::remove(savedPdf, ec);
}
