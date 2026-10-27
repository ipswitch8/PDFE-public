/**
 * @file test_tre_commands.cpp
 * @brief Unit tests for TreCommands (ReconstructRegionCommand, ImageReplaceCommand)
 *        and PdfContentWriter undo support methods.
 */

#include <gtest/gtest.h>
#include "tre/TreCommands.h"
#include "tre/PdfContentWriter.h"
#include "tre/RegionReconstructor.h"
#include "tre/ImageReplacer.h"
#include "tre/OcrPipeline.h"
#include "tre/CoordMapper.h"
#include "tre/BgColorDetector.h"
#include "tre/FontMatcher.h"
#include "tre/QualityAssessor.h"
#include "tre/ImageMasker.h"
#include "tre/BitmapCompositor.h"
#include "core/Document.h"
#include "core/Page.h"
#include "core/MuPdfContext.h"
#include "core/Error.h"
#include "fonts/EmbeddedFontAnalyzer.h"
#include "fonts/SubstituteFontSelector.h"
#include "fonts/FontAnalysisCache.h"
#include "fonts/PdfFontEmbedder.h"
#include <cstdint>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string getFixturePath(const std::string& filename) {
    return std::string(TEST_FIXTURES_DIR) + "/" + filename;
}

// ---------------------------------------------------------------------------
// A1-51 support: byte-level comparison of two observable-state snapshots.
//
// The *ExecuteAndUndo cases used to assert only EXPECT_NO_THROW(execute()) /
// EXPECT_NO_THROW(undo()) plus a SIZE comparison of the image XObject data.
// Both TRE approaches replace the page image IN PLACE at the same pixel
// dimensions, so the decompressed stream keeps its byte COUNT (850*1100*3) even
// when every pixel changes — a size-only oracle is blind to the edit, and a
// command whose execute()/undo() are no-ops passes it outright.
// ---------------------------------------------------------------------------
std::string describeDiff(const std::vector<uint8_t>& a,
                         const std::vector<uint8_t>& b) {
    std::ostringstream os;
    os << "sizes " << a.size() << " vs " << b.size();
    if (a.size() != b.size())
        return os.str();
    size_t differing = 0;
    size_t firstOffset = a.size();
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i] != b[i]) {
            if (differing == 0) firstOffset = i;
            ++differing;
        }
    }
    os << ", " << differing << " differing byte(s)";
    if (differing > 0) os << ", first at offset " << firstOffset;
    return os.str();
}

// ============================================================================
// Lightweight fixture for null-safety tests (no PDF or OCR needed)
// ============================================================================

class PdfContentWriterNullTest : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = std::make_shared<pdfe::core::MuPdfContext>();
        writer_ = std::make_unique<pdfe::tre::PdfContentWriter>(context_.get(), nullptr);
    }

    std::shared_ptr<pdfe::core::MuPdfContext> context_;
    std::unique_ptr<pdfe::tre::PdfContentWriter> writer_;
};

// ============================================================================
// Full fixture requiring real PDF and OCR infrastructure
// ============================================================================

class TreCommandsTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::string scannedPath = getFixturePath("test_scanned.pdf");
        if (!std::filesystem::exists(scannedPath)) {
            GTEST_SKIP() << "test_scanned.pdf not found at " << scannedPath;
        }

        std::string tessdataPath = getFixturePath("tessdata");
        if (!std::filesystem::exists(tessdataPath + "/eng.traineddata")) {
            GTEST_SKIP() << "tessdata/eng.traineddata not found";
        }

        context_ = std::make_shared<pdfe::core::MuPdfContext>();
        document_ = pdfe::core::Document::open(context_, scannedPath);
        ASSERT_NE(document_, nullptr) << "Failed to open " << scannedPath;

        // Font infrastructure
        analyzer_ = std::make_unique<pdfe::fonts::EmbeddedFontAnalyzer>(context_.get());
        fontCache_ = std::make_unique<pdfe::fonts::FontAnalysisCache>();
        selector_ = std::make_unique<pdfe::fonts::SubstituteFontSelector>(
            analyzer_.get(), fontCache_.get());
        embedder_ = std::make_unique<pdfe::fonts::PdfFontEmbedder>(context_.get());

        // TRE pipeline components
        pdfe::tre::OcrConfig ocrConfig;
        ocrConfig.tessdataPath = tessdataPath;
        ocrPipeline_ = std::make_unique<pdfe::tre::OcrPipeline>(
            context_.get(), document_.get(), ocrConfig);
        coordMapper_ = std::make_unique<pdfe::tre::CoordMapper>(
            context_.get(), document_.get());
        bgDetector_ = std::make_unique<pdfe::tre::BgColorDetector>();
        fontMatcher_ = std::make_unique<pdfe::tre::FontMatcher>(
            selector_.get(), analyzer_.get());
        qualityAssessor_ = std::make_unique<pdfe::tre::QualityAssessor>();
        imageMasker_ = std::make_unique<pdfe::tre::ImageMasker>();
        bitmapCompositor_ = std::make_unique<pdfe::tre::BitmapCompositor>();
        contentWriter_ = std::make_unique<pdfe::tre::PdfContentWriter>(
            context_.get(), embedder_.get());
        reconstructor_ = std::make_unique<pdfe::tre::RegionReconstructor>(
            ocrPipeline_.get(), coordMapper_.get(), bgDetector_.get(),
            fontMatcher_.get(), qualityAssessor_.get(), imageMasker_.get(),
            contentWriter_.get());
        imageReplacer_ = std::make_unique<pdfe::tre::ImageReplacer>(
            ocrPipeline_.get(), coordMapper_.get(), bgDetector_.get(),
            fontMatcher_.get(), imageMasker_.get(), bitmapCompositor_.get(),
            contentWriter_.get());
    }

    std::shared_ptr<pdfe::core::MuPdfContext> context_;
    std::unique_ptr<pdfe::core::Document> document_;
    std::unique_ptr<pdfe::fonts::EmbeddedFontAnalyzer> analyzer_;
    std::unique_ptr<pdfe::fonts::FontAnalysisCache> fontCache_;
    std::unique_ptr<pdfe::fonts::SubstituteFontSelector> selector_;
    std::unique_ptr<pdfe::fonts::PdfFontEmbedder> embedder_;
    std::unique_ptr<pdfe::tre::OcrPipeline> ocrPipeline_;
    std::unique_ptr<pdfe::tre::CoordMapper> coordMapper_;
    std::unique_ptr<pdfe::tre::BgColorDetector> bgDetector_;
    std::unique_ptr<pdfe::tre::FontMatcher> fontMatcher_;
    std::unique_ptr<pdfe::tre::QualityAssessor> qualityAssessor_;
    std::unique_ptr<pdfe::tre::ImageMasker> imageMasker_;
    std::unique_ptr<pdfe::tre::BitmapCompositor> bitmapCompositor_;
    std::unique_ptr<pdfe::tre::PdfContentWriter> contentWriter_;
    std::unique_ptr<pdfe::tre::RegionReconstructor> reconstructor_;
    std::unique_ptr<pdfe::tre::ImageReplacer> imageReplacer_;
};

// ============================================================================
// PdfContentWriter Undo Support -- Null Safety
// ============================================================================

TEST_F(PdfContentWriterNullTest, SaveImageXObjectRef_NullPage) {
    auto ref = writer_->saveImageXObjectRef(nullptr, nullptr);
    EXPECT_EQ(ref, nullptr);
}

TEST_F(PdfContentWriterNullTest, SaveContentsRef_NullPage) {
    auto ref = writer_->saveContentsRef(nullptr, nullptr);
    EXPECT_EQ(ref, nullptr);
}

TEST_F(PdfContentWriterNullTest, ReleaseImageXObjectRef_NullSafe) {
    EXPECT_NO_THROW(writer_->releaseImageXObjectRef(nullptr));
}

TEST_F(PdfContentWriterNullTest, ReleaseContentsRef_NullSafe) {
    EXPECT_NO_THROW(writer_->releaseContentsRef(nullptr));
}

TEST_F(PdfContentWriterNullTest, RestoreImageXObjectRef_NullSafe) {
    // All null args -- should return false, not crash
    bool result = writer_->restoreImageXObjectRef(nullptr, nullptr, nullptr, "Im0");
    EXPECT_FALSE(result);
}

TEST_F(PdfContentWriterNullTest, RestoreContentsRef_NullSafe) {
    // Null page/document -- should return false, not crash
    bool result = writer_->restoreContentsRef(nullptr, nullptr, nullptr);
    EXPECT_FALSE(result);
}

// ============================================================================
// PdfContentWriter Undo Support -- Real PDF
// ============================================================================

TEST_F(TreCommandsTest, SaveImageXObjectRef_ReturnsValue) {
    auto page = document_->page(0);
    ASSERT_NE(page, nullptr);

    auto ref = contentWriter_->saveImageXObjectRef(page.get(), document_.get());
    // For a scanned PDF, expect a non-null image reference
    // Release it either way (safe for nullptr)
    contentWriter_->releaseImageXObjectRef(ref);
}

TEST_F(TreCommandsTest, SaveContentsRef_ReturnsValue) {
    auto page = document_->page(0);
    ASSERT_NE(page, nullptr);

    auto ref = contentWriter_->saveContentsRef(page.get(), document_.get());
    contentWriter_->releaseContentsRef(ref);
}

TEST_F(TreCommandsTest, SaveAndRestoreImageRoundtrip) {
    auto page = document_->page(0);
    ASSERT_NE(page, nullptr);

    // Find image name first
    auto imageInfo = contentWriter_->findPageImageXObject(page.get(), document_.get());
    if (!imageInfo.has_value()) {
        GTEST_SKIP() << "No image XObject on page";
    }
    std::string imageName = imageInfo->xobjectName;

    // Save reference
    auto ref = contentWriter_->saveImageXObjectRef(page.get(), document_.get());
    ASSERT_NE(ref, nullptr) << "Scanned page should have image XObject";

    // Get original image data for comparison
    auto originalData = contentWriter_->getImageXObjectData(page.get(), document_.get());

    // Restore the same reference back
    bool restored = contentWriter_->restoreImageXObjectRef(
        page.get(), document_.get(), ref, imageName);
    EXPECT_TRUE(restored);

    // Verify data is the same after roundtrip
    auto restoredData = contentWriter_->getImageXObjectData(page.get(), document_.get());
    EXPECT_EQ(originalData.size(), restoredData.size());
    if (!originalData.empty() && originalData.size() == restoredData.size()) {
        EXPECT_EQ(originalData, restoredData);
    }
}

TEST_F(TreCommandsTest, SaveAndRestoreContentsRoundtrip) {
    auto page = document_->page(0);
    ASSERT_NE(page, nullptr);

    auto ref = contentWriter_->saveContentsRef(page.get(), document_.get());

    // Get original content data
    auto originalData = contentWriter_->getContentStreamData(page.get(), document_.get());

    // Restore
    bool restored = contentWriter_->restoreContentsRef(page.get(), document_.get(), ref);
    EXPECT_TRUE(restored);

    // Verify roundtrip
    auto restoredData = contentWriter_->getContentStreamData(page.get(), document_.get());
    EXPECT_EQ(originalData.size(), restoredData.size());
    if (!originalData.empty() && originalData.size() == restoredData.size()) {
        EXPECT_EQ(originalData, restoredData);
    }
}

// ============================================================================
// ReconstructRegionCommand Tests
// ============================================================================

TEST_F(TreCommandsTest, ReconstructCommand_ConstructAndDestroy) {
    pdfe::tre::PdfRect selection{50, 50, 500, 700};

    EXPECT_NO_THROW({
        auto command = std::make_unique<pdfe::tre::ReconstructRegionCommand>(
            document_.get(), context_,
            reconstructor_.get(), imageReplacer_.get(), contentWriter_.get(),
            0, selection, "Test text", pdfe::tre::RegionId{1});
    });
}

TEST_F(TreCommandsTest, ReconstructCommand_Description) {
    pdfe::tre::PdfRect selection{50, 50, 500, 700};

    auto command = std::make_unique<pdfe::tre::ReconstructRegionCommand>(
        document_.get(), context_,
        reconstructor_.get(), imageReplacer_.get(), contentWriter_.get(),
        0, selection, "Test text", pdfe::tre::RegionId{1});

    EXPECT_EQ(command->description(), "Reconstruct Scanned Text");
}

// A1-51: execute() must CHANGE the page, undo() must restore it EXACTLY.
//
// AUTHORED GROUND TRUTH: test_scanned.pdf (generate_test_scanned_pdf.py) is a
// single page whose only content is a full-page 850x1100 RGB image XObject
// /Im0 drawn by a one-operator content stream. Reconstructing a region over
// that page must repaint the page image (Approach B masks the region before
// writing real text over it; the Approach C fallback composites a text bitmap
// into it) — so the decompressed /Im0 stream MUST differ afterwards. undo()
// grafts the saved pre-edit XObject and content stream back, so both streams
// must come back byte-for-byte identical.
//
// Mutations killed:
//   * a no-op execute() (the previous EXPECT_NO_THROW passed for one);
//   * an execute() that fails silently on both approaches and leaves the page
//     untouched;
//   * a no-op undo(), or an undo() that restores only the image and leaks the
//     appended text operators into the content stream (or vice versa);
//   * an undo() that restores a same-SIZE but different-CONTENT image — which
//     the old size-only comparison could not see, since the replacement image
//     is written at the original pixel dimensions.
TEST_F(TreCommandsTest, ReconstructCommand_ExecuteAndUndo) {
    auto page = document_->page(0);
    ASSERT_NE(page, nullptr);

    // Observable state BEFORE execute.
    auto originalImage = contentWriter_->getImageXObjectData(page.get(), document_.get());
    if (originalImage.empty()) {
        GTEST_SKIP() << "No image XObject data on page";
    }
    auto originalContents =
        contentWriter_->getContentStreamData(page.get(), document_.get());
    ASSERT_FALSE(originalContents.empty())
        << "Fixture page must have a content stream to compare against";

    pdfe::tre::PdfRect selection{50, 50, 500, 700};

    auto command = std::make_unique<pdfe::tre::ReconstructRegionCommand>(
        document_.get(), context_,
        reconstructor_.get(), imageReplacer_.get(), contentWriter_.get(),
        0, selection, "Hello World", pdfe::tre::RegionId{1});

    ASSERT_NO_THROW(command->execute());

    // Observable state AFTER execute: the page image must actually change.
    page = document_->page(0);
    ASSERT_NE(page, nullptr);
    auto executedImage = contentWriter_->getImageXObjectData(page.get(), document_.get());
    EXPECT_NE(executedImage, originalImage)
        << "ReconstructRegionCommand::execute() left the page image byte-identical "
           "— neither Approach B (mask + text) nor the Approach C fallback "
           "modified the page, i.e. the command was a no-op. "
        << describeDiff(originalImage, executedImage);

    ASSERT_NO_THROW(command->undo());

    // Re-get page (undo may have invalidated the old one)
    page = document_->page(0);
    ASSERT_NE(page, nullptr);

    // Deep restore check — every byte, not just the length.
    auto restoredImage = contentWriter_->getImageXObjectData(page.get(), document_.get());
    EXPECT_EQ(restoredImage, originalImage)
        << "undo() did not restore the page image exactly: "
        << describeDiff(originalImage, restoredImage);

    auto restoredContents =
        contentWriter_->getContentStreamData(page.get(), document_.get());
    EXPECT_EQ(restoredContents, originalContents)
        << "undo() did not restore the page content stream exactly (appended "
           "text operators leaked past the undo): "
        << describeDiff(originalContents, restoredContents);
}

TEST_F(TreCommandsTest, ReconstructCommand_InvalidPage) {
    pdfe::tre::PdfRect selection{50, 50, 500, 700};

    auto command = std::make_unique<pdfe::tre::ReconstructRegionCommand>(
        document_.get(), context_,
        reconstructor_.get(), imageReplacer_.get(), contentWriter_.get(),
        9999, selection, "Test text", pdfe::tre::RegionId{1});

    // Execute on invalid page should throw PdfeException (page index out of range)
    EXPECT_THROW(command->execute(), pdfe::core::PdfeException);
}

// ============================================================================
// ImageReplaceCommand Tests
// ============================================================================

TEST_F(TreCommandsTest, ImageReplaceCommand_ConstructAndDestroy) {
    pdfe::tre::PdfRect selection{50, 50, 500, 700};

    EXPECT_NO_THROW({
        auto command = std::make_unique<pdfe::tre::ImageReplaceCommand>(
            document_.get(), context_,
            imageReplacer_.get(), contentWriter_.get(),
            0, selection, "Test text", pdfe::tre::RegionId{1});
    });
}

TEST_F(TreCommandsTest, ImageReplaceCommand_Description) {
    pdfe::tre::PdfRect selection{50, 50, 500, 700};

    auto command = std::make_unique<pdfe::tre::ImageReplaceCommand>(
        document_.get(), context_,
        imageReplacer_.get(), contentWriter_.get(),
        0, selection, "Test text", pdfe::tre::RegionId{1});

    EXPECT_EQ(command->description(), "Replace Scanned Text Image");
}

// A1-51: same contract for Approach C. ImageReplaceCommand composites a text
// bitmap into the scanned page image, so /Im0's decompressed bytes must change
// on execute() and come back byte-identical on undo(). Same mutations killed as
// ReconstructCommand_ExecuteAndUndo above.
TEST_F(TreCommandsTest, ImageReplaceCommand_ExecuteAndUndo) {
    auto page = document_->page(0);
    ASSERT_NE(page, nullptr);

    auto originalImage = contentWriter_->getImageXObjectData(page.get(), document_.get());
    if (originalImage.empty()) {
        GTEST_SKIP() << "No image XObject data on page";
    }
    auto originalContents =
        contentWriter_->getContentStreamData(page.get(), document_.get());
    ASSERT_FALSE(originalContents.empty())
        << "Fixture page must have a content stream to compare against";

    pdfe::tre::PdfRect selection{50, 50, 500, 700};

    auto command = std::make_unique<pdfe::tre::ImageReplaceCommand>(
        document_.get(), context_,
        imageReplacer_.get(), contentWriter_.get(),
        0, selection, "Hello World", pdfe::tre::RegionId{1});

    ASSERT_NO_THROW(command->execute());

    page = document_->page(0);
    ASSERT_NE(page, nullptr);
    auto executedImage = contentWriter_->getImageXObjectData(page.get(), document_.get());
    EXPECT_NE(executedImage, originalImage)
        << "ImageReplaceCommand::execute() left the page image byte-identical — "
           "the composited replacement text never reached the page, i.e. the "
           "command was a no-op. "
        << describeDiff(originalImage, executedImage);

    ASSERT_NO_THROW(command->undo());

    page = document_->page(0);
    ASSERT_NE(page, nullptr);

    auto restoredImage = contentWriter_->getImageXObjectData(page.get(), document_.get());
    EXPECT_EQ(restoredImage, originalImage)
        << "undo() did not restore the page image exactly: "
        << describeDiff(originalImage, restoredImage);

    auto restoredContents =
        contentWriter_->getContentStreamData(page.get(), document_.get());
    EXPECT_EQ(restoredContents, originalContents)
        << "undo() did not restore the page content stream exactly: "
        << describeDiff(originalContents, restoredContents);
}

TEST_F(TreCommandsTest, ImageReplaceCommand_InvalidPage) {
    pdfe::tre::PdfRect selection{50, 50, 500, 700};

    auto command = std::make_unique<pdfe::tre::ImageReplaceCommand>(
        document_.get(), context_,
        imageReplacer_.get(), contentWriter_.get(),
        9999, selection, "Test text", pdfe::tre::RegionId{1});

    // Execute on invalid page should throw PdfeException (page index out of range)
    EXPECT_THROW(command->execute(), pdfe::core::PdfeException);
}

} // namespace
