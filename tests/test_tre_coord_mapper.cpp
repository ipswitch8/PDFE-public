#include <gtest/gtest.h>
#include "tre/CoordMapper.h"
#include "tre/TreTypes.h"
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "core/Error.h"
#include <cmath>
#include <filesystem>
#include <memory>

using namespace pdfe::tre;
using namespace pdfe::core;

// ============================================================================
// Test Fixture for Pure Math Tests (no document needed)
// ============================================================================

class CoordMapperMathTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create a standard affine transform for testing
        // Simulates 300 DPI scan of US Letter page (8.5" x 11" = 612pt x 792pt)
        tf.scaleX = 72.0 / 300.0;  // 0.24 pt/px
        tf.scaleY = 72.0 / 300.0;
        tf.translateX = 0.0;
        tf.translateY = 0.0;
        tf.flipY = false;  // Both pixel and fitz-space are Y-down
        tf.rotation = 0;
        tf.imageWidth = 2550.0;   // 612pt * 300/72 = 2550px
        tf.imageHeight = 3300.0;  // 792pt * 300/72 = 3300px
        tf.pageWidth = 612.0;
        tf.pageHeight = 792.0;

        // A real CoordMapper instance so the tests exercise the PRODUCT mapper
        // rather than re-deriving its formula in-test. pixelToPdf/pdfToPixel are
        // pure functions of the passed tf, so any valid mapper suffices.
        ctx_ = std::make_shared<MuPdfContext>();
        std::filesystem::path testPdf =
            std::filesystem::path(TEST_FIXTURES_DIR) / "test_single.pdf";
        ASSERT_TRUE(std::filesystem::exists(testPdf))
            << "test_single.pdf required to construct CoordMapper";
        doc_ = Document::open(ctx_, testPdf);
        ASSERT_NE(doc_, nullptr);
        mapper_ = std::make_unique<CoordMapper>(ctx_.get(), doc_.get());
    }

    AffineTransform tf;
    std::shared_ptr<MuPdfContext> ctx_;
    std::unique_ptr<Document> doc_;
    std::unique_ptr<CoordMapper> mapper_;
};

// ============================================================================
// Test Fixture for Document-Based Tests
// ============================================================================

class CoordMapperDocumentTest : public ::testing::Test {
protected:
    void SetUp() override {
        context = std::make_shared<MuPdfContext>();

        std::filesystem::path fixturesDir = TEST_FIXTURES_DIR;
        std::filesystem::path testPdf = fixturesDir / "test_single.pdf";

        if (!std::filesystem::exists(testPdf)) {
            GTEST_SKIP() << "Test fixture not available: " << testPdf;
        }

        document = Document::open(context, testPdf);
        if (!document || document->pageCount() == 0) {
            GTEST_SKIP() << "Failed to open test document or document has no pages";
        }
    }

    std::shared_ptr<MuPdfContext> context;
    std::shared_ptr<Document> document;
};

// ============================================================================
// Size Conversion Tests (Static Methods)
// ============================================================================

TEST_F(CoordMapperMathTest, PixelSizeToPdfPt_300DPI) {
    // 300 pixels at 300 DPI should equal 72 points (1 inch)
    double result = CoordMapper::pixelSizeToPdfPt(300.0, 300.0);
    EXPECT_NEAR(result, 72.0, 0.01);
}

TEST_F(CoordMapperMathTest, PixelSizeToPdfPt_SmallSize) {
    // 14 pixels at 300 DPI should equal approximately 3.36 points
    double result = CoordMapper::pixelSizeToPdfPt(14.0, 300.0);
    EXPECT_NEAR(result, 3.36, 0.01);
}

TEST_F(CoordMapperMathTest, PdfPtToPixelSize_300DPI) {
    // 72 points at 300 DPI should equal 300 pixels (inverse of above)
    double result = CoordMapper::pdfPtToPixelSize(72.0, 300.0);
    EXPECT_NEAR(result, 300.0, 0.01);
}

TEST_F(CoordMapperMathTest, PdfPtToPixelSize_SmallSize) {
    // 3.36 points at 300 DPI should equal approximately 14 pixels
    double result = CoordMapper::pdfPtToPixelSize(3.36, 300.0);
    EXPECT_NEAR(result, 14.0, 0.01);
}

TEST_F(CoordMapperMathTest, RoundTripPixelSize) {
    // Converting px -> pt -> px should return original value
    double originalPx = 123.45;
    double dpi = 300.0;

    double pt = CoordMapper::pixelSizeToPdfPt(originalPx, dpi);
    double backToPx = CoordMapper::pdfPtToPixelSize(pt, dpi);

    EXPECT_NEAR(backToPx, originalPx, 0.01);
}

// ============================================================================
// Point Conversion Tests - Rotation 0 (No Y-flip: both spaces Y-down)
// ============================================================================

TEST_F(CoordMapperMathTest, PixelToPdf_Rotation0_TopLeft) {
    // Pixel (0,0) top-left -> fitz-space top-left (0,0). Calls the PRODUCT mapper.
    PdfPoint got = mapper_->pixelToPdf(PixelPoint{0.0, 0.0}, tf);
    EXPECT_NEAR(got.x, 0.0, 0.01);
    EXPECT_NEAR(got.y, 0.0, 0.01);
}

TEST_F(CoordMapperMathTest, PixelToPdf_Rotation0_Center) {
    // Pixel center (1275,1650) -> fitz-space center (306,396).
    PdfPoint got = mapper_->pixelToPdf(PixelPoint{1275.0, 1650.0}, tf);
    EXPECT_NEAR(got.x, 306.0, 0.01);
    EXPECT_NEAR(got.y, 396.0, 0.01);
}

TEST_F(CoordMapperMathTest, PixelToPdf_Rotation0_BottomRight) {
    // Pixel (2550,3300) bottom-right -> fitz-space (612,792).
    PdfPoint got = mapper_->pixelToPdf(PixelPoint{2550.0, 3300.0}, tf);
    EXPECT_NEAR(got.x, 612.0, 0.01);
    EXPECT_NEAR(got.y, 792.0, 0.01);
}

TEST_F(CoordMapperMathTest, PdfToPixel_Rotation0_TopLeft) {
    // Fitz-space (0,0) -> pixel (0,0).
    PixelPoint got = mapper_->pdfToPixel(PdfPoint{0.0, 0.0}, tf);
    EXPECT_NEAR(got.x, 0.0, 0.01);
    EXPECT_NEAR(got.y, 0.0, 0.01);
}

TEST_F(CoordMapperMathTest, PdfToPixel_Rotation0_BottomRight) {
    // Fitz-space (612,792) -> pixel (2550,3300).
    PixelPoint got = mapper_->pdfToPixel(PdfPoint{612.0, 792.0}, tf);
    EXPECT_NEAR(got.x, 2550.0, 0.01);
    EXPECT_NEAR(got.y, 3300.0, 0.01);
}

// ============================================================================
// No Y-flip verification: top maps to top, bottom maps to bottom
// ============================================================================

TEST_F(CoordMapperMathTest, NoYFlip_TopStaysTop) {
    // R-14 de-tautologized: this used to RE-DERIVE the mapping with the test's
    // own arithmetic ((y - translateY)/scaleY) and never touch the product — so
    // a Y-inversion regression in CoordMapper::pdfToPixel could not fail it. Now
    // it calls the PRODUCT mapper. A fitz-space point near the TOP (small Y) must
    // map to a pixel near the top (small pixel Y), NOT flipped to the bottom.
    PdfPoint fitzTopLeft{10.0, 10.0};
    PixelPoint px = mapper_->pdfToPixel(fitzTopLeft, tf);

    // Small fitz Y -> small pixel Y (top of image). Invert the product Y-mapping
    // and this assertion fails.
    EXPECT_LT(px.y, tf.imageHeight / 2.0)
        << "Top of fitz-space should map to top of pixel space (no Y-flip)";
    EXPECT_NEAR(px.x, 10.0 / tf.scaleX, 0.01);
    EXPECT_NEAR(px.y, 10.0 / tf.scaleY, 0.01);
}

TEST_F(CoordMapperMathTest, NoYFlip_BottomStaysBottom) {
    // R-14 de-tautologized: calls the PRODUCT mapper instead of recomputing the
    // formula. A fitz-space point near the BOTTOM (large Y) must map to a pixel
    // near the bottom (large pixel Y), NOT flipped to the top.
    PdfPoint fitzBottom{100.0, 780.0};
    PixelPoint px = mapper_->pdfToPixel(fitzBottom, tf);

    // Large fitz Y -> large pixel Y (bottom of image).
    EXPECT_GT(px.y, tf.imageHeight / 2.0)
        << "Bottom of fitz-space should map to bottom of pixel space (no Y-flip)";
    EXPECT_NEAR(px.y, 780.0 / tf.scaleY, 0.01);
}

// ============================================================================
// Rectangle Conversion Tests (updated for no Y-flip)
// ============================================================================

TEST_F(CoordMapperMathTest, PixelRectToPdf_SimpleRect) {
    // R-14 de-tautologized: this used to compute `100 * tf.scaleX` itself and
    // assert the LITERAL equalled 24 — a tautology that never exercised
    // CoordMapper::pixelRectToPdf (which transforms all four corners and returns
    // their AABB). Now it calls the PRODUCT mapper. Both spaces Y-down at
    // rotation 0, so pixel (100,100)-(200,200) → fitz (24,24)-(48,48).
    PdfRect got = mapper_->pixelRectToPdf(PixelRect{100.0, 100.0, 200.0, 200.0}, tf);

    EXPECT_NEAR(got.x0, 24.0, 0.01);
    EXPECT_NEAR(got.y0, 24.0, 0.01);
    EXPECT_NEAR(got.x1, 48.0, 0.01);
    EXPECT_NEAR(got.y1, 48.0, 0.01);
}

TEST_F(CoordMapperMathTest, PdfRectToPixel_SimpleRect) {
    // R-14 de-tautologized: calls the PRODUCT rect mapper instead of recomputing
    // (pdf - translate)/scale in-test. Fitz rect (24,24)-(48,48) → pixel
    // (100,100)-(200,200). A Y-inversion in pdfToPixel (which pdfRectToPixel
    // drives per corner) would move y0/y1 and fail this.
    PixelRect got = mapper_->pdfRectToPixel(PdfRect{24.0, 24.0, 48.0, 48.0}, tf);

    EXPECT_NEAR(got.x0, 100.0, 0.01);
    EXPECT_NEAR(got.y0, 100.0, 0.01);
    EXPECT_NEAR(got.x1, 200.0, 0.01);
    EXPECT_NEAR(got.y1, 200.0, 0.01);
}

// ============================================================================
// Round-Trip Tests (Using Document-Based Mapper)
// ============================================================================

TEST_F(CoordMapperDocumentTest, RoundTripPoint_Rotation0) {
    CoordMapper mapper(context.get(), document.get());

    // Get transform for page 0
    const AffineTransform& tf = mapper.getTransform(0);

    // Test round-trip conversion of a point
    PixelPoint originalPx{500.0, 1000.0};

    PdfPoint pdfPt = mapper.pixelToPdf(originalPx, tf);
    PixelPoint backToPx = mapper.pdfToPixel(pdfPt, tf);

    EXPECT_NEAR(backToPx.x, originalPx.x, 0.01);
    EXPECT_NEAR(backToPx.y, originalPx.y, 0.01);
}

TEST_F(CoordMapperDocumentTest, RoundTripRect_Rotation0) {
    CoordMapper mapper(context.get(), document.get());

    const AffineTransform& tf = mapper.getTransform(0);

    // Test round-trip conversion of a rectangle
    PixelRect originalRect{300.0, 400.0, 800.0, 900.0};

    PdfRect pdfRect = mapper.pixelRectToPdf(originalRect, tf);
    PixelRect backToPixel = mapper.pdfRectToPixel(pdfRect, tf);

    EXPECT_NEAR(backToPixel.x0, originalRect.x0, 0.01);
    EXPECT_NEAR(backToPixel.y0, originalRect.y0, 0.01);
    EXPECT_NEAR(backToPixel.x1, originalRect.x1, 0.01);
    EXPECT_NEAR(backToPixel.y1, originalRect.y1, 0.01);
}

// ============================================================================
// Rotation Tests (updated for new formulas)
// ============================================================================

TEST_F(CoordMapperMathTest, PixelToPdf_Rotation90) {
    tf.rotation = 90;
    // Calls the PRODUCT mapper; expected values derived independently:
    //   x = px.y(200) * 0.24 = 48; y = (imageWidth 2550 - px.x 100) * 0.24 = 588.
    // A mapper that swapped the axes or dropped the (imageWidth - x) reflection
    // would produce different numbers and fail here.
    PdfPoint got = mapper_->pixelToPdf(PixelPoint{100.0, 200.0}, tf);
    EXPECT_NEAR(got.x, 48.0, 0.01);
    EXPECT_NEAR(got.y, 588.0, 0.01);
}

TEST_F(CoordMapperMathTest, PixelToPdf_Rotation180) {
    tf.rotation = 180;
    // x = (2550 - 100) * 0.24 = 588; y = (3300 - 200) * 0.24 = 744.
    PdfPoint got = mapper_->pixelToPdf(PixelPoint{100.0, 200.0}, tf);
    EXPECT_NEAR(got.x, 588.0, 0.01);
    EXPECT_NEAR(got.y, 744.0, 0.01);
}

TEST_F(CoordMapperMathTest, PixelToPdf_Rotation270) {
    tf.rotation = 270;
    // x = (3300 - 200) * 0.24 = 744; y = 100 * 0.24 = 24.
    PdfPoint got = mapper_->pixelToPdf(PixelPoint{100.0, 200.0}, tf);
    EXPECT_NEAR(got.x, 744.0, 0.01);
    EXPECT_NEAR(got.y, 24.0, 0.01);
}

// ============================================================================
// Rotation round-trip tests
// ============================================================================

TEST_F(CoordMapperMathTest, RoundTrip_Rotation90) {
    tf.rotation = 90;
    auto ctx = std::make_shared<MuPdfContext>();
    auto fixturesDir = std::filesystem::path(TEST_FIXTURES_DIR);
    auto testPdf = fixturesDir / "test_single.pdf";
    if (!std::filesystem::exists(testPdf)) GTEST_SKIP();
    auto doc = Document::open(ctx, testPdf);
    CoordMapper mapper(ctx.get(), doc.get());

    PixelPoint original{300.0, 500.0};
    PdfPoint pdf = mapper.pixelToPdf(original, tf);
    PixelPoint back = mapper.pdfToPixel(pdf, tf);
    EXPECT_NEAR(back.x, original.x, 0.01);
    EXPECT_NEAR(back.y, original.y, 0.01);
}

TEST_F(CoordMapperMathTest, RoundTrip_Rotation180) {
    tf.rotation = 180;
    auto ctx = std::make_shared<MuPdfContext>();
    auto fixturesDir = std::filesystem::path(TEST_FIXTURES_DIR);
    auto testPdf = fixturesDir / "test_single.pdf";
    if (!std::filesystem::exists(testPdf)) GTEST_SKIP();
    auto doc = Document::open(ctx, testPdf);
    CoordMapper mapper(ctx.get(), doc.get());

    PixelPoint original{300.0, 500.0};
    PdfPoint pdf = mapper.pixelToPdf(original, tf);
    PixelPoint back = mapper.pdfToPixel(pdf, tf);
    EXPECT_NEAR(back.x, original.x, 0.01);
    EXPECT_NEAR(back.y, original.y, 0.01);
}

TEST_F(CoordMapperMathTest, RoundTrip_Rotation270) {
    tf.rotation = 270;
    auto ctx = std::make_shared<MuPdfContext>();
    auto fixturesDir = std::filesystem::path(TEST_FIXTURES_DIR);
    auto testPdf = fixturesDir / "test_single.pdf";
    if (!std::filesystem::exists(testPdf)) GTEST_SKIP();
    auto doc = Document::open(ctx, testPdf);
    CoordMapper mapper(ctx.get(), doc.get());

    PixelPoint original{300.0, 500.0};
    PdfPoint pdf = mapper.pixelToPdf(original, tf);
    PixelPoint back = mapper.pdfToPixel(pdf, tf);
    EXPECT_NEAR(back.x, original.x, 0.01);
    EXPECT_NEAR(back.y, original.y, 0.01);
}

// ============================================================================
// Cache Tests
// ============================================================================

TEST_F(CoordMapperDocumentTest, CacheHit_SamePage) {
    CoordMapper mapper(context.get(), document.get());

    // Get transform for page 0 twice
    const AffineTransform& tf1 = mapper.getTransform(0);
    const AffineTransform& tf2 = mapper.getTransform(0);

    // Should return reference to same cached object
    EXPECT_EQ(&tf1, &tf2);

    // Verify values are identical
    EXPECT_EQ(tf1.scaleX, tf2.scaleX);
    EXPECT_EQ(tf1.scaleY, tf2.scaleY);
    EXPECT_EQ(tf1.rotation, tf2.rotation);
    EXPECT_EQ(tf1.imageWidth, tf2.imageWidth);
    EXPECT_EQ(tf1.imageHeight, tf2.imageHeight);
}

TEST_F(CoordMapperDocumentTest, CacheInvalidation_SinglePage) {
    CoordMapper mapper(context.get(), document.get());

    // Get transform for page 0
    const AffineTransform& tf1 = mapper.getTransform(0);
    double originalScaleX = tf1.scaleX;

    // Invalidate cache for page 0
    mapper.invalidateCache(0);

    // Get transform again - should recompute
    const AffineTransform& tf2 = mapper.getTransform(0);

    // Address might be different (recomputed), but values should be same
    EXPECT_NEAR(tf2.scaleX, originalScaleX, 0.01);
}

TEST_F(CoordMapperDocumentTest, CacheInvalidation_AllPages) {
    CoordMapper mapper(context.get(), document.get());

    // Get transform for page 0
    const AffineTransform& tf1 = mapper.getTransform(0);
    double originalScaleX = tf1.scaleX;

    // Invalidate all caches
    mapper.invalidateAllCaches();

    // Get transform again - should recompute
    const AffineTransform& tf2 = mapper.getTransform(0);

    // Values should be the same after recomputation
    EXPECT_NEAR(tf2.scaleX, originalScaleX, 0.01);
}

// ============================================================================
// Edge Cases and Error Handling
// ============================================================================

TEST_F(CoordMapperDocumentTest, GetTransform_InvalidPageIndex) {
    CoordMapper mapper(context.get(), document.get());

    int pageCount = document->pageCount();

    // Test with out-of-range page index
    EXPECT_THROW(
        mapper.getTransform(pageCount + 10),
        PdfeException
    );
}

TEST(CoordMapperConstructorTest, NullContextThrows) {
    auto context = std::make_shared<MuPdfContext>();

    std::filesystem::path fixturesDir = TEST_FIXTURES_DIR;
    std::filesystem::path testPdf = fixturesDir / "test_single.pdf";

    if (!std::filesystem::exists(testPdf)) {
        GTEST_SKIP() << "Test fixture not available";
    }

    auto doc = Document::open(context, testPdf);

    // Null context should throw
    EXPECT_THROW(
        CoordMapper(nullptr, doc.get()),
        PdfeException
    );
}

TEST(CoordMapperConstructorTest, NullDocumentThrows) {
    auto context = std::make_shared<MuPdfContext>();

    // Null document should throw
    EXPECT_THROW(
        CoordMapper(context.get(), nullptr),
        PdfeException
    );
}

// ============================================================================
// Integration Test - Full Workflow
// ============================================================================

TEST_F(CoordMapperDocumentTest, FullWorkflow_ConvertAndBack) {
    CoordMapper mapper(context.get(), document.get());

    // Get transform for first page
    const AffineTransform& tf = mapper.getTransform(0);

    // Verify transform has reasonable values
    EXPECT_GT(tf.scaleX, 0.0);
    EXPECT_GT(tf.scaleY, 0.0);
    EXPECT_GT(tf.imageWidth, 0.0);
    EXPECT_GT(tf.imageHeight, 0.0);
    EXPECT_GT(tf.pageWidth, 0.0);
    EXPECT_GT(tf.pageHeight, 0.0);
    EXPECT_FALSE(tf.flipY);  // Both spaces Y-down, no flip needed

    // Test multiple points
    std::vector<PixelPoint> testPoints = {
        {0.0, 0.0},
        {100.0, 100.0},
        {500.0, 500.0},
        {1000.0, 1000.0}
    };

    for (const auto& originalPx : testPoints) {
        PdfPoint pdfPt = mapper.pixelToPdf(originalPx, tf);
        PixelPoint backToPx = mapper.pdfToPixel(pdfPt, tf);

        EXPECT_NEAR(backToPx.x, originalPx.x, 0.01);
        EXPECT_NEAR(backToPx.y, originalPx.y, 0.01);
    }
}

// ============================================================================
// Test: pixelToPdf preserves Y-direction (no inversion)
// ============================================================================

TEST_F(CoordMapperDocumentTest, PixelToPdf_PreservesYDirection) {
    CoordMapper mapper(context.get(), document.get());
    const AffineTransform& tf = mapper.getTransform(0);

    // Two pixel points: one near top, one near bottom
    PixelPoint topPixel{500.0, 100.0};
    PixelPoint bottomPixel{500.0, 2000.0};

    PdfPoint topPdf = mapper.pixelToPdf(topPixel, tf);
    PdfPoint bottomPdf = mapper.pixelToPdf(bottomPixel, tf);

    // Since both spaces are Y-down, top (small Y) should map to small PDF Y
    // and bottom (large Y) should map to large PDF Y.
    EXPECT_LT(topPdf.y, bottomPdf.y)
        << "Top pixel should map to smaller fitz Y than bottom pixel (both Y-down)";
}

TEST_F(CoordMapperDocumentTest, PdfToPixel_PreservesYDirection) {
    CoordMapper mapper(context.get(), document.get());
    const AffineTransform& tf = mapper.getTransform(0);

    // Two fitz points: one near top, one near bottom
    PdfPoint topFitz{100.0, 50.0};
    PdfPoint bottomFitz{100.0, 700.0};

    PixelPoint topPixel = mapper.pdfToPixel(topFitz, tf);
    PixelPoint bottomPixel = mapper.pdfToPixel(bottomFitz, tf);

    // Since both spaces are Y-down, small fitz Y -> small pixel Y
    EXPECT_LT(topPixel.y, bottomPixel.y)
        << "Top fitz-space point should map to smaller pixel Y (both Y-down)";
}
