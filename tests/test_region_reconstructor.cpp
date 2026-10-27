#include <gtest/gtest.h>

#include "tre/RegionReconstructor.h"
#include "tre/OcrPipeline.h"
#include "tre/CoordMapper.h"
#include "tre/BgColorDetector.h"
#include "tre/FontMatcher.h"
#include "tre/QualityAssessor.h"
#include "tre/ImageMasker.h"
#include "tre/PdfContentWriter.h"
#include "fonts/PdfFontEmbedder.h"
#include "fonts/SubstituteFontSelector.h"
#include "fonts/EmbeddedFontAnalyzer.h"
#include "fonts/FontAnalysisCache.h"
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "core/Page.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

using namespace pdfe::tre;
using namespace pdfe::core;
using namespace pdfe::fonts;

// Test fixtures directory
#ifndef TEST_FIXTURES_DIR
#define TEST_FIXTURES_DIR "test_fixtures"
#endif

class RegionReconstructorTest : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = std::make_shared<MuPdfContext>();

        // Check for scanned PDF
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

        // Initialize pipeline components
        ocrPipeline_ = std::make_unique<OcrPipeline>(context_.get(), document_.get());
        coordMapper_ = std::make_unique<CoordMapper>(context_.get(), document_.get());
        fontEmbedder_ = std::make_unique<PdfFontEmbedder>(context_.get());
        contentWriter_ = std::make_unique<PdfContentWriter>(context_.get(), fontEmbedder_.get());
        fontAnalysisCache_ = std::make_unique<FontAnalysisCache>();
        embeddedFontAnalyzer_ = std::make_unique<EmbeddedFontAnalyzer>(context_.get());
        substituteFontSelector_ = std::make_unique<SubstituteFontSelector>(
            embeddedFontAnalyzer_.get(), fontAnalysisCache_.get());
        fontMatcher_ = std::make_unique<FontMatcher>(
            substituteFontSelector_.get(), embeddedFontAnalyzer_.get());

        // Check if tessdata is available
        std::filesystem::path tessdata = std::filesystem::path(TEST_FIXTURES_DIR) / "tessdata" / "eng.traineddata";
        if (std::filesystem::exists(tessdata)) {
            OcrConfig config;
            config.tessdataPath = (std::filesystem::path(TEST_FIXTURES_DIR) / "tessdata").string();
            ocrPipeline_->setConfig(config);
            hasTessdata_ = true;
        } else {
            hasTessdata_ = false;
        }

        // Create reconstructor with all components
        reconstructor_ = std::make_unique<RegionReconstructor>(
            ocrPipeline_.get(), coordMapper_.get(), &bgDetector_,
            fontMatcher_.get(), &qualityAssessor_, &imageMasker_,
            contentWriter_.get());
    }

    std::shared_ptr<MuPdfContext> context_;
    std::unique_ptr<Document> document_;
    std::unique_ptr<Page> page_;

    std::unique_ptr<OcrPipeline> ocrPipeline_;
    std::unique_ptr<CoordMapper> coordMapper_;
    BgColorDetector bgDetector_;
    std::unique_ptr<FontMatcher> fontMatcher_;
    QualityAssessor qualityAssessor_;
    ImageMasker imageMasker_;
    std::unique_ptr<PdfFontEmbedder> fontEmbedder_;
    std::unique_ptr<PdfContentWriter> contentWriter_;
    std::unique_ptr<FontAnalysisCache> fontAnalysisCache_;
    std::unique_ptr<EmbeddedFontAnalyzer> embeddedFontAnalyzer_;
    std::unique_ptr<SubstituteFontSelector> substituteFontSelector_;

    std::unique_ptr<RegionReconstructor> reconstructor_;

    bool hasTestPdf_ = false;
    bool hasTessdata_ = false;
};

// -----------------------------------------------------------------------------
// A1-50 authored ground truth for tests/fixtures/test_scanned.pdf
// (generator: tests/fixtures/generate_test_scanned_pdf.py)
//
//   * The page image is Image.new("RGB", (850, 1100), (255, 255, 255)) — a
//     PURE WHITE 850x1100 raster — with nine lines of BLACK text drawn on it.
//     There is no gradient, no halftone screen, no paper texture: the
//     background is uniform white by construction.
//   * The text is drawn with arial.ttf (fallbacks DejaVuSans / LiberationSans /
//     FreeSans — all SANS-SERIF), at PIL size 36 for the body lines and 48 for
//     the heading.
//   * The 850x1100 image is placed full-page on a 612x792 pt MediaBox, so the
//     pixel→point scale is 612/850 = 0.72: the body text is ~25.9 pt and the
//     heading ~34.6 pt on the page.
// -----------------------------------------------------------------------------
namespace {

/// Sans-serif families that are an acceptable match for the fixture's Arial
/// text. The fixture is authored with a SANS face, so the matcher must land on
/// a sans family; the exact winner depends on which fonts the host has
/// installed and on the metric/visual ranker, hence a set rather than a single
/// name. If this list rejects a family that is genuinely sans-serif, ADD IT.
/// If a SERIF family wins, that is the defect this oracle exists to catch.
const std::vector<std::string>& acceptableSansFamilies() {
    static const std::vector<std::string> kFamilies = {
        "arial", "helvetica", "liberation sans", "dejavu sans", "freesans",
        "nimbus sans", "segoe ui", "calibri", "candara", "corbel", "tahoma",
        "verdana", "trebuchet", "franklin gothic", "century gothic",
        "microsoft sans serif", "ms sans serif", "lucida sans", "lucida grande",
        "myriad", "open sans", "noto sans", "roboto", "source sans",
        "gill sans", "futura", "optima", "univers", "geneva", "avenir",
        "ms reference sans serif", "malgun gothic", "sans serif", "sans-serif",
    };
    return kFamilies;
}

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool isAcceptableSansFamily(const std::string& family) {
    const std::string lower = toLower(family);
    return std::any_of(acceptableSansFamilies().begin(),
                       acceptableSansFamilies().end(),
                       [&lower](const std::string& known) {
                           return lower.find(known) != std::string::npos;
                       });
}

} // namespace

// -----------------------------------------------------------------------------
// Test 1: Basic construction and destruction
// -----------------------------------------------------------------------------

TEST_F(RegionReconstructorTest, ConstructAndDestroy) {
    // Setup already creates the reconstructor
    EXPECT_NE(reconstructor_, nullptr);
}

// -----------------------------------------------------------------------------
// Test 2: Move constructor
// -----------------------------------------------------------------------------

TEST_F(RegionReconstructorTest, MoveConstructor) {
    if (!hasTestPdf_ || !hasTessdata_) {
        GTEST_SKIP() << "Requires test_scanned.pdf and tessdata";
    }

    // Create a second reconstructor
    auto reconstructor2 = std::make_unique<RegionReconstructor>(
        ocrPipeline_.get(), coordMapper_.get(), &bgDetector_,
        fontMatcher_.get(), &qualityAssessor_, &imageMasker_,
        contentWriter_.get());

    // Move construct from reconstructor2
    RegionReconstructor reconstructor3(std::move(*reconstructor2));

    // The moved-to object should be usable
    PdfRect selection{50, 50, 200, 100};
    auto result = reconstructor3.analyze(0, selection);

    // Result may fail due to OCR issues, but should not crash
    EXPECT_TRUE(result.success || !result.errorMessage.empty());
}

// -----------------------------------------------------------------------------
// Test 3: Analyze returns valid quality verdict
// -----------------------------------------------------------------------------

TEST_F(RegionReconstructorTest, AnalyzeReturnsQualityVerdict) {
    if (!hasTestPdf_ || !hasTessdata_) {
        GTEST_SKIP() << "Requires test_scanned.pdf and tessdata";
    }

    // Select a region in the middle of the page
    PdfRect selection{100, 100, 300, 200};

    auto result = reconstructor_->analyze(0, selection);

    // Should complete without crashing
    if (result.success) {
        // Verdict should be populated
        EXPECT_GE(result.qualityVerdict.overallScore, 0.0f);
        EXPECT_LE(result.qualityVerdict.overallScore, 1.0f);
    } else {
        // Failure should have an error message
        EXPECT_FALSE(result.errorMessage.empty());
    }
}

// -----------------------------------------------------------------------------
// Test 4: Analyze detects background
// -----------------------------------------------------------------------------

// A1-50 / A1-11 repair. The previous oracle was
//   EXPECT_TRUE(isUniform || hasGradient || isComplex ||
//               (!isUniform && !hasGradient && !isComplex))
// — a literal tautology: the fourth disjunct is the negation of the first
// three, so the expression is `true` for every possible combination of flags.
//
// The fixture background is PURE WHITE by construction (see the authored
// ground truth block above), and BgColorDetector samples edge strips plus
// inter-line gaps, so every sample is that same white paper: dominant-colour
// uniformity is 1.0, comfortably over the 0.80 threshold, and the verdict must
// be UNIFORM — not gradient, not complex.
//
// Mutations killed: forcing isUniform=false / isComplex=true (the detector
// would then push the whole TRE pipeline onto the image-replacement fallback
// for a plain white scan); returning a zeroed/black dominantColor, which is
// what a solid-fill background repaint would paint over the region.
TEST_F(RegionReconstructorTest, AnalyzeDetectsBackground) {
    if (!hasTestPdf_ || !hasTessdata_) {
        GTEST_SKIP() << "Requires test_scanned.pdf and tessdata";
    }

    PdfRect selection{50, 50, 250, 150};

    auto result = reconstructor_->analyze(0, selection);

    if (result.success) {
        const auto& bg = result.region.bgInfo;

        EXPECT_GE(bg.confidence, 0.0f);
        EXPECT_LE(bg.confidence, 1.0f);
        EXPECT_GT(bg.confidence, 0.0f)
            << "A region full of clean white paper must yield a positive "
               "background-detection confidence.";

        EXPECT_TRUE(bg.isUniform)
            << "test_scanned.pdf's background is a solid (255,255,255) raster, "
               "so the detector must report a UNIFORM background.";
        EXPECT_FALSE(bg.hasGradient)
            << "No gradient is painted anywhere in the fixture image.";
        EXPECT_FALSE(bg.isComplex)
            << "A solid white page must not be classified as a complex "
               "background — that forces the pipeline onto the Approach C "
               "image-replacement fallback for a trivially reconstructable scan.";

        // Dominant colour must BE the authored paper white, not a default-init
        // zero (black) struct. Tolerance covers render/resample rounding.
        EXPECT_GE(static_cast<int>(bg.dominantColor.r), 240)
            << "dominantColor.r=" << static_cast<int>(bg.dominantColor.r);
        EXPECT_GE(static_cast<int>(bg.dominantColor.g), 240)
            << "dominantColor.g=" << static_cast<int>(bg.dominantColor.g);
        EXPECT_GE(static_cast<int>(bg.dominantColor.b), 240)
            << "dominantColor.b=" << static_cast<int>(bg.dominantColor.b);
    } else {
        EXPECT_FALSE(result.errorMessage.empty())
            << "analyze() must explain a failure rather than fail silently.";
    }
}

// -----------------------------------------------------------------------------
// Test 5: Analyze matches font
// -----------------------------------------------------------------------------

// A1-50 / A1-11 repair. The previous oracle was
//   EXPECT_TRUE(fontFamily.empty() || !fontFamily.empty())
// — `X || !X`, true for every possible string including the empty one, which is
// exactly the failure it was nominally guarding against.
//
// AUTHORED GROUND TRUTH: the fixture's text is rendered with arial.ttf (or a
// sans fallback — see the ground-truth block above), so the matcher must
// produce (a) a NON-EMPTY family, (b) a resolvable font FILE on disk, and (c) a
// SANS-SERIF family. Body text is PIL size 36 in an 850px-wide image on a 612pt
// page => ~25.9 pt; the heading is ~34.6 pt.
//
// Mutations killed: returning an empty/unresolvable FontMatch (the writer would
// then emit text with no font, or fall through to .notdef); hard-coding a
// default 12 pt size instead of measuring the scanned line; picking a serif
// face for sans-serif scanned text.
TEST_F(RegionReconstructorTest, AnalyzeMatchesFont) {
    if (!hasTestPdf_ || !hasTessdata_) {
        GTEST_SKIP() << "Requires test_scanned.pdf and tessdata";
    }

    PdfRect selection{100, 100, 400, 200};

    auto result = reconstructor_->analyze(0, selection);

    if (result.success) {
        const auto& fm = result.region.fontMatch;

        EXPECT_GE(fm.confidence, 0.0f);
        EXPECT_LE(fm.confidence, 1.0f);

        // The matched family must be REAL, not an empty placeholder.
        EXPECT_FALSE(fm.fontFamily.empty())
            << "A successful analyze() must name the matched font family; an "
               "empty family means the reconstructed text has no font to use.";

        EXPECT_FALSE(fm.fontFilePath.empty())
            << "The match must carry the file the substitute will be embedded "
               "from (family=" << fm.fontFamily << ").";
        if (!fm.fontFilePath.empty()) {
            EXPECT_TRUE(std::filesystem::exists(fm.fontFilePath))
                << "Matched font file does not exist on disk: "
                << fm.fontFilePath;
        }

        // The fixture text is SANS-SERIF (Arial and every fallback the
        // generator accepts). See acceptableSansFamilies() for the reason this
        // is a set rather than a single expected name.
        EXPECT_TRUE(isAcceptableSansFamily(fm.fontFamily))
            << "Matched family \"" << fm.fontFamily
            << "\" is not in the acceptable sans-serif set. test_scanned.pdf's "
               "text is drawn with arial.ttf (fallbacks DejaVu Sans / "
               "Liberation Sans / FreeSans — all sans). If the winner is a "
               "legitimate additional SANS family, add it to "
               "acceptableSansFamilies(); if it is a SERIF family, the "
               "serif/sans detection is wrong.";

        // Size, anchored to the authored 36px/48px glyphs at 612/850 = 0.72
        // points-per-pixel => ~25.9 pt body / ~34.6 pt heading. The band is
        // deliberately generous (OCR x-height estimation is approximate) but
        // still excludes a hard-coded 12 pt default.
        EXPECT_GT(fm.sizePt, 15.0f)
            << "Detected size " << fm.sizePt
            << " pt is far below the authored ~25.9 pt body text — a canned "
               "default rather than a measurement.";
        EXPECT_LT(fm.sizePt, 45.0f)
            << "Detected size " << fm.sizePt
            << " pt is far above the authored ~34.6 pt heading.";
    } else {
        EXPECT_FALSE(result.errorMessage.empty())
            << "analyze() must explain a failure rather than fail silently.";
    }
}

// -----------------------------------------------------------------------------
// Test 6: Reconstruct high quality region
// -----------------------------------------------------------------------------

TEST_F(RegionReconstructorTest, ReconstructHighQualityRegion) {
    if (!hasTestPdf_ || !hasTessdata_) {
        GTEST_SKIP() << "Requires test_scanned.pdf and tessdata";
    }

    PdfRect selection{50, 50, 300, 150};
    RegionId regionId = RegionId{1};

    auto result = reconstructor_->reconstruct(
        page_.get(), document_.get(), 0, selection, "", regionId);

    // Result may succeed or fail depending on quality, but should not crash
    if (result.success) {
        EXPECT_EQ(result.region.id, regionId);
        EXPECT_EQ(result.region.approach, Approach::B_HybridReconstruction);
        EXPECT_FALSE(result.region.originalText.empty());
    } else {
        EXPECT_FALSE(result.errorMessage.empty());
    }
}

// -----------------------------------------------------------------------------
// Test 7: Reconstruct returns region data
// -----------------------------------------------------------------------------

TEST_F(RegionReconstructorTest, ReconstructReturnsRegionData) {
    if (!hasTestPdf_ || !hasTessdata_) {
        GTEST_SKIP() << "Requires test_scanned.pdf and tessdata";
    }

    // Selection in fitz-space (Y-down, top-left origin).
    // Cover a broad region of the page to ensure OCR text is captured.
    PdfRect selection{50, 50, 400, 300};
    RegionId regionId = RegionId{42};

    auto result = reconstructor_->reconstruct(
        page_.get(), document_.get(), 0, selection, "", regionId);

    if (result.success) {
        // Region data should be populated
        EXPECT_EQ(result.region.id, regionId);
        EXPECT_GT(result.region.pdfBbox.width(), 0.0);
        EXPECT_GT(result.region.pdfBbox.height(), 0.0);
        EXPECT_GT(result.region.pixelBbox.width(), 0.0);
        EXPECT_GT(result.region.pixelBbox.height(), 0.0);

        // Text should be present (either original or edited)
        EXPECT_TRUE(!result.region.originalText.empty() ||
                    !result.region.editedText.empty());
    }
}

// -----------------------------------------------------------------------------
// Test 8: Reconstruct invalid page index
// -----------------------------------------------------------------------------

TEST_F(RegionReconstructorTest, ReconstructInvalidPage) {
    if (!hasTestPdf_) {
        GTEST_SKIP() << "Requires test_scanned.pdf";
    }

    PdfRect selection{50, 50, 200, 150};
    RegionId regionId = RegionId{1};

    // Try to reconstruct with out-of-range page index
    const int invalidPageIndex = 9999;
    auto result = reconstructor_->reconstruct(
        page_.get(), document_.get(), invalidPageIndex, selection, "", regionId);

    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.errorMessage.empty());
    EXPECT_TRUE(result.errorMessage.find("out of range") != std::string::npos ||
                result.errorMessage.find("index") != std::string::npos);
}

// -----------------------------------------------------------------------------
// Test 9: Reconstruct empty selection
// -----------------------------------------------------------------------------

TEST_F(RegionReconstructorTest, ReconstructEmptySelection) {
    if (!hasTestPdf_) {
        GTEST_SKIP() << "Requires test_scanned.pdf";
    }

    // Zero-area rectangle
    PdfRect emptySelection{100, 100, 100, 100};
    RegionId regionId = RegionId{1};

    auto result = reconstructor_->reconstruct(
        page_.get(), document_.get(), 0, emptySelection, "", regionId);

    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.errorMessage.empty());
    EXPECT_TRUE(result.errorMessage.find("zero") != std::string::npos ||
                result.errorMessage.find("area") != std::string::npos);
}

// -----------------------------------------------------------------------------
// Test 10: Reconstruct null page pointer
// -----------------------------------------------------------------------------

TEST_F(RegionReconstructorTest, ReconstructNullPage) {
    if (!hasTestPdf_) {
        GTEST_SKIP() << "Requires test_scanned.pdf";
    }

    PdfRect selection{50, 50, 200, 150};
    RegionId regionId = RegionId{1};

    // Pass null page pointer
    auto result = reconstructor_->reconstruct(
        nullptr, document_.get(), 0, selection, "", regionId);

    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.errorMessage.empty());
    EXPECT_TRUE(result.errorMessage.find("Null page") != std::string::npos ||
                result.errorMessage.find("null page") != std::string::npos);
}

// -----------------------------------------------------------------------------
// Test 11: Reconstruct with edited text
// -----------------------------------------------------------------------------

TEST_F(RegionReconstructorTest, ReconstructWithEditedText) {
    if (!hasTestPdf_ || !hasTessdata_) {
        GTEST_SKIP() << "Requires test_scanned.pdf and tessdata";
    }

    PdfRect selection{100, 100, 300, 180};
    RegionId regionId = RegionId{3};
    const std::string customText = "Custom edited text";

    auto result = reconstructor_->reconstruct(
        page_.get(), document_.get(), 0, selection, customText, regionId);

    if (result.success) {
        // Should use the custom edited text
        EXPECT_EQ(result.region.editedText, customText);
        // Original text may be different (from OCR)
        EXPECT_TRUE(result.region.originalText.empty() ||
                    result.region.originalText != customText);
    }
}

// -----------------------------------------------------------------------------
// Test 12: Analyze empty page (graceful handling)
// -----------------------------------------------------------------------------

TEST_F(RegionReconstructorTest, AnalyzeEmptyPage) {
    if (!hasTessdata_) {
        GTEST_SKIP() << "Requires tessdata";
    }

    // Open a real blank PDF (no content stream, no image XObjects)
    std::filesystem::path blankPdf = std::filesystem::path(TEST_FIXTURES_DIR) / "test_blank.pdf";
    if (!std::filesystem::exists(blankPdf)) {
        GTEST_SKIP() << "test_blank.pdf not found";
    }

    auto blankContext = std::make_shared<MuPdfContext>();
    auto blankDoc = Document::open(blankContext, blankPdf);
    ASSERT_NE(blankDoc, nullptr);
    ASSERT_GT(blankDoc->pageCount(), 0);

    // Build a pipeline wired to the blank document
    std::string tessdataPath = (std::filesystem::path(TEST_FIXTURES_DIR) / "tessdata").string();
    OcrConfig blankConfig;
    blankConfig.tessdataPath = tessdataPath;

    OcrPipeline blankOcrPipeline(blankContext.get(), blankDoc.get(), blankConfig);
    CoordMapper blankCoordMapper(blankContext.get(), blankDoc.get());
    BgColorDetector blankBgDetector;
    QualityAssessor blankQualityAssessor;
    ImageMasker blankImageMasker;

    auto blankFontEmbedder = std::make_unique<PdfFontEmbedder>(blankContext.get());
    auto blankContentWriter = std::make_unique<PdfContentWriter>(blankContext.get(), blankFontEmbedder.get());
    auto blankFontCache = std::make_unique<FontAnalysisCache>();
    auto blankFontAnalyzer = std::make_unique<EmbeddedFontAnalyzer>(blankContext.get());
    auto blankFontSelector = std::make_unique<SubstituteFontSelector>(
        blankFontAnalyzer.get(), blankFontCache.get());
    FontMatcher blankFontMatcher(blankFontSelector.get(), blankFontAnalyzer.get());

    RegionReconstructor blankReconstructor(
        &blankOcrPipeline, &blankCoordMapper, &blankBgDetector,
        &blankFontMatcher, &blankQualityAssessor, &blankImageMasker,
        blankContentWriter.get());

    PdfRect selection{50, 50, 200, 150};

    // Analyze on a blank page -- should handle gracefully (no crash)
    auto result = blankReconstructor.analyze(0, selection);

    // Either succeeds with empty/minimal data or fails with an error message
    EXPECT_TRUE(result.success || !result.errorMessage.empty());
}

// -----------------------------------------------------------------------------
// Test 13: Analyze with zero-area selection
// -----------------------------------------------------------------------------

TEST_F(RegionReconstructorTest, AnalyzeZeroAreaSelection) {
    if (!hasTestPdf_) {
        GTEST_SKIP() << "Requires test_scanned.pdf";
    }

    // Create a rectangle with zero width
    PdfRect zeroSelection{100, 100, 100, 200};

    auto result = reconstructor_->analyze(0, zeroSelection);

    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.errorMessage.empty());
    EXPECT_TRUE(result.errorMessage.find("zero") != std::string::npos ||
                result.errorMessage.find("area") != std::string::npos);
}

// -----------------------------------------------------------------------------
// Test 14: Reconstruct null document pointer
// -----------------------------------------------------------------------------

TEST_F(RegionReconstructorTest, ReconstructNullDocument) {
    if (!hasTestPdf_) {
        GTEST_SKIP() << "Requires test_scanned.pdf";
    }

    PdfRect selection{50, 50, 200, 150};
    RegionId regionId = RegionId{1};

    // Pass null document pointer
    auto result = reconstructor_->reconstruct(
        page_.get(), nullptr, 0, selection, "", regionId);

    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.errorMessage.empty());
    EXPECT_TRUE(result.errorMessage.find("Null document") != std::string::npos ||
                result.errorMessage.find("null document") != std::string::npos);
}
