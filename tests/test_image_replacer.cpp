#include <gtest/gtest.h>
#include "tre/ImageReplacer.h"
#include "tre/RegionReconstructor.h"
#include "tre/OcrPipeline.h"
#include "tre/CoordMapper.h"
#include "tre/BgColorDetector.h"
#include "tre/FontMatcher.h"
#include "tre/ImageMasker.h"
#include "fonts/FontResolver.h"
#include "tre/BitmapCompositor.h"
#include "tre/PdfContentWriter.h"
#include "fonts/PdfFontEmbedder.h"
#include "fonts/SubstituteFontSelector.h"
#include "fonts/EmbeddedFontAnalyzer.h"
#include "fonts/FontAnalysisCache.h"
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "core/Page.h"

#include <QImage>
#include <memory>
#include <string>
#include <filesystem>

using namespace pdfe::tre;
using namespace pdfe::core;

class ImageReplacerTest : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = std::make_shared<MuPdfContext>();

        // Check for test_scanned.pdf
        std::filesystem::path scannedPdf = std::filesystem::path(TEST_FIXTURES_DIR) / "test_scanned.pdf";
        if (!std::filesystem::exists(scannedPdf)) {
            hasTestPdf_ = false;
            return;
        }

        document_ = Document::open(context_, scannedPdf);
        if (!document_ || document_->pageCount() == 0) {
            hasTestPdf_ = false;
            return;
        }

        page_ = document_->page(0);
        hasTestPdf_ = true;

        // Initialize components
        ocrPipeline_ = std::make_unique<OcrPipeline>(context_.get(), document_.get());
        coordMapper_ = std::make_unique<CoordMapper>(context_.get(), document_.get());
        fontEmbedder_ = std::make_unique<pdfe::fonts::PdfFontEmbedder>(context_.get());
        contentWriter_ = std::make_unique<PdfContentWriter>(context_.get(), fontEmbedder_.get());
        fontAnalysisCache_ = std::make_unique<pdfe::fonts::FontAnalysisCache>();
        embeddedFontAnalyzer_ = std::make_unique<pdfe::fonts::EmbeddedFontAnalyzer>(context_.get());
        substituteFontSelector_ = std::make_unique<pdfe::fonts::SubstituteFontSelector>(
            embeddedFontAnalyzer_.get(), fontAnalysisCache_.get());
        fontMatcher_ = std::make_unique<FontMatcher>(
            substituteFontSelector_.get(), embeddedFontAnalyzer_.get());

        bgDetector_ = std::make_unique<BgColorDetector>();
        imageMasker_ = std::make_unique<ImageMasker>();
        bitmapCompositor_ = std::make_unique<BitmapCompositor>();

        // Check for tessdata
        std::filesystem::path tessdata = std::filesystem::path(TEST_FIXTURES_DIR) / "tessdata" / "eng.traineddata";
        if (std::filesystem::exists(tessdata)) {
            OcrConfig config;
            config.tessdataPath = (std::filesystem::path(TEST_FIXTURES_DIR) / "tessdata").string();
            ocrPipeline_->setConfig(config);
            hasTessdata_ = true;
        } else {
            hasTessdata_ = false;
        }

        // Create ImageReplacer
        replacer_ = std::make_unique<ImageReplacer>(
            ocrPipeline_.get(),
            coordMapper_.get(),
            bgDetector_.get(),
            fontMatcher_.get(),
            imageMasker_.get(),
            bitmapCompositor_.get(),
            contentWriter_.get());
    }

    void TearDown() override {
        replacer_.reset();
        bitmapCompositor_.reset();
        imageMasker_.reset();
        bgDetector_.reset();
        fontMatcher_.reset();
        substituteFontSelector_.reset();
        embeddedFontAnalyzer_.reset();
        fontAnalysisCache_.reset();
        contentWriter_.reset();
        fontEmbedder_.reset();
        coordMapper_.reset();
        ocrPipeline_.reset();
        page_.reset();
        document_.reset();
        context_.reset();
    }

    std::shared_ptr<MuPdfContext> context_;
    std::unique_ptr<Document> document_;
    std::unique_ptr<Page> page_;
    std::unique_ptr<OcrPipeline> ocrPipeline_;
    std::unique_ptr<CoordMapper> coordMapper_;
    std::unique_ptr<BgColorDetector> bgDetector_;
    std::unique_ptr<FontMatcher> fontMatcher_;
    std::unique_ptr<ImageMasker> imageMasker_;
    std::unique_ptr<BitmapCompositor> bitmapCompositor_;
    std::unique_ptr<PdfContentWriter> contentWriter_;
    std::unique_ptr<pdfe::fonts::PdfFontEmbedder> fontEmbedder_;
    std::unique_ptr<pdfe::fonts::SubstituteFontSelector> substituteFontSelector_;
    std::unique_ptr<pdfe::fonts::EmbeddedFontAnalyzer> embeddedFontAnalyzer_;
    std::unique_ptr<pdfe::fonts::FontAnalysisCache> fontAnalysisCache_;
    std::unique_ptr<ImageReplacer> replacer_;

    bool hasTestPdf_ = false;
    bool hasTessdata_ = false;
};

TEST_F(ImageReplacerTest, ConstructAndDestroy) {
    // Construction happens in SetUp, destruction in TearDown
    EXPECT_NE(replacer_, nullptr);
}

TEST_F(ImageReplacerTest, MoveConstructor) {
    if (!hasTestPdf_) {
        GTEST_SKIP() << "Requires test_scanned.pdf";
    }

    // Create another replacer and move it
    ImageReplacer replacer2(
        ocrPipeline_.get(),
        coordMapper_.get(),
        bgDetector_.get(),
        fontMatcher_.get(),
        imageMasker_.get(),
        bitmapCompositor_.get(),
        contentWriter_.get());

    ImageReplacer moved(std::move(replacer2));
    EXPECT_NO_THROW({
        // The moved-to object should be usable
        // We can't test much without running OCR, but at least verify it doesn't crash
    });
}

TEST_F(ImageReplacerTest, ReplaceBasicRegion) {
    if (!hasTestPdf_ || !hasTessdata_) {
        GTEST_SKIP() << "Requires test_scanned.pdf and tessdata";
    }

    // Define a small region in PDF coordinates
    PdfRect selection;
    selection.x0 = 72.0;  // 1 inch from left
    selection.y0 = 72.0;  // 1 inch from bottom
    selection.x1 = 216.0; // 3 inches from left
    selection.y1 = 144.0; // 2 inches from bottom

    std::string editedText = "Test Text";
    RegionId regionId = RegionId{1};

    ReconstructionResult result = replacer_->replace(
        page_.get(), document_.get(), 0, selection, editedText, regionId);

    // The result should succeed (Approach C always proceeds)
    EXPECT_TRUE(result.success) << "Error: " << result.errorMessage;
    EXPECT_EQ(result.region.approach, Approach::C_ImageReplacement);
    EXPECT_EQ(result.region.editedText, editedText);
}

TEST_F(ImageReplacerTest, ReplaceReturnsApproachC) {
    if (!hasTestPdf_ || !hasTessdata_) {
        GTEST_SKIP() << "Requires test_scanned.pdf and tessdata";
    }

    PdfRect selection;
    selection.x0 = 72.0;
    selection.y0 = 72.0;
    selection.x1 = 216.0;
    selection.y1 = 144.0;

    ReconstructionResult result = replacer_->replace(
        page_.get(), document_.get(), 0, selection, "Text", RegionId{1});

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.region.approach, Approach::C_ImageReplacement);
    EXPECT_EQ(result.qualityVerdict.recommendedApproach, Approach::C_ImageReplacement);
}

TEST_F(ImageReplacerTest, ReplacePopulatesRegionData) {
    if (!hasTestPdf_ || !hasTessdata_) {
        GTEST_SKIP() << "Requires test_scanned.pdf and tessdata";
    }

    PdfRect selection;
    selection.x0 = 72.0;
    selection.y0 = 72.0;
    selection.x1 = 216.0;
    selection.y1 = 144.0;

    std::string editedText = "Populated Test";
    RegionId regionId = RegionId{42};

    ReconstructionResult result = replacer_->replace(
        page_.get(), document_.get(), 0, selection, editedText, regionId);

    EXPECT_TRUE(result.success) << "Error: " << result.errorMessage;
    EXPECT_EQ(result.region.id, regionId);
    EXPECT_EQ(result.region.editedText, editedText);
    // originalText may be empty if OCR finds no text in the selected region
    EXPECT_GT(result.region.fontMatch.sizePt, 0.0f); // Font match should have size
    EXPECT_GE(result.qualityVerdict.overallScore, 0.0f);
}

TEST_F(ImageReplacerTest, ReplaceWithFontMatchSkipsAnalysis) {
    if (!hasTestPdf_ || !hasTessdata_) {
        GTEST_SKIP() << "Requires test_scanned.pdf and tessdata";
    }

    // Create pre-computed font match and background info.
    // FontResolver picks a platform-appropriate font path
    // (Arial on Windows, Liberation Sans on Linux).
    FontMatch fontMatch;
    fontMatch.fontFamily = "Arial";
    fontMatch.fontStyle = "Regular";
    fontMatch.fontFilePath = pdfe::fonts::FontResolver::resolveFontPath("Arial");
    if (fontMatch.fontFilePath.empty()) {
        GTEST_SKIP() << "FontResolver could not locate Arial-equivalent font";
    }
    fontMatch.sizePt = 12.0f;
    fontMatch.color = RgbColor{0, 0, 0};
    fontMatch.confidence = 0.8f;

    BackgroundInfo bgInfo;
    bgInfo.dominantColor = RgbColor{255, 255, 255};
    bgInfo.isUniform = true;
    bgInfo.confidence = 0.9f;

    PdfRect selection;
    selection.x0 = 72.0;
    selection.y0 = 72.0;
    selection.x1 = 216.0;
    selection.y1 = 144.0;

    std::string editedText = "Pre-analyzed Text";
    RegionId regionId = RegionId{99};

    ReconstructionResult result = replacer_->replaceWithFontMatch(
        page_.get(), document_.get(), 0, selection, editedText, fontMatch, bgInfo, regionId);

    EXPECT_TRUE(result.success) << "Error: " << result.errorMessage;
    EXPECT_EQ(result.region.approach, Approach::C_ImageReplacement);
    EXPECT_EQ(result.region.editedText, editedText);
    EXPECT_EQ(result.region.fontMatch.fontFamily, "Arial");
    EXPECT_EQ(result.region.bgInfo.dominantColor.r, 255);
}

TEST_F(ImageReplacerTest, ReplacePreservesImageSize) {
    if (!hasTestPdf_ || !hasTessdata_) {
        GTEST_SKIP() << "Requires test_scanned.pdf and tessdata";
    }

    // Get original page dimensions
    QRectF originalBbox = page_->boundingBox();

    PdfRect selection;
    selection.x0 = 72.0;
    selection.y0 = 72.0;
    selection.x1 = 216.0;
    selection.y1 = 144.0;

    ReconstructionResult result = replacer_->replace(
        page_.get(), document_.get(), 0, selection, "Size Test", RegionId{1});

    EXPECT_TRUE(result.success);

    // Check that page dimensions haven't changed
    QRectF newBbox = page_->boundingBox();
    EXPECT_DOUBLE_EQ(originalBbox.width(), newBbox.width());
    EXPECT_DOUBLE_EQ(originalBbox.height(), newBbox.height());
}

TEST_F(ImageReplacerTest, ReplaceInvalidPage) {
    if (!hasTestPdf_) {
        GTEST_SKIP() << "Requires test_scanned.pdf";
    }

    PdfRect selection;
    selection.x0 = 72.0;
    selection.y0 = 72.0;
    selection.x1 = 216.0;
    selection.y1 = 144.0;

    // Invalid page index
    ReconstructionResult result = replacer_->replace(
        page_.get(), document_.get(), 999, selection, "Invalid", RegionId{1});

    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.errorMessage.empty());
    EXPECT_NE(result.errorMessage.find("out of range"), std::string::npos);
}

TEST_F(ImageReplacerTest, ReplaceEmptyText) {
    if (!hasTestPdf_ || !hasTessdata_) {
        GTEST_SKIP() << "Requires test_scanned.pdf and tessdata";
    }

    PdfRect selection;
    selection.x0 = 72.0;
    selection.y0 = 72.0;
    selection.x1 = 216.0;
    selection.y1 = 144.0;

    // Empty edited text should fall back to OCR text
    ReconstructionResult result = replacer_->replace(
        page_.get(), document_.get(), 0, selection, "", RegionId{1});

    // Should succeed if OCR finds text in the region
    // If the region is blank, it might fail with "No text to render"
    if (result.success) {
        EXPECT_FALSE(result.region.editedText.empty());
    } else {
        // It's acceptable to fail if the region contains no text
        EXPECT_NE(result.errorMessage.find("No text"), std::string::npos);
    }
}

TEST_F(ImageReplacerTest, ReplaceNullPage) {
    if (!hasTestPdf_) {
        GTEST_SKIP() << "Requires test_scanned.pdf";
    }

    PdfRect selection;
    selection.x0 = 72.0;
    selection.y0 = 72.0;
    selection.x1 = 216.0;
    selection.y1 = 144.0;

    // Null page pointer
    ReconstructionResult result = replacer_->replace(
        nullptr, document_.get(), 0, selection, "Null test", RegionId{1});

    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.errorMessage.empty());
    EXPECT_NE(result.errorMessage.find("Null"), std::string::npos);

    // Null document pointer
    result = replacer_->replace(
        page_.get(), nullptr, 0, selection, "Null test", RegionId{1});

    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.errorMessage.empty());
    EXPECT_NE(result.errorMessage.find("Null"), std::string::npos);
}
