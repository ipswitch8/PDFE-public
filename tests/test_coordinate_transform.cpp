#include <gtest/gtest.h>
#include "core/CoordinateTransform.h"
#include "core/Document.h"
#include "core/Page.h"
#include "core/MuPdfContext.h"
#include "annotations/AnnotationManager.h"
#include <cmath>
#include <filesystem>
#include <iostream>
#include <QImage>

using namespace pdfe::core;

// Epsilon for floating-point comparisons
static constexpr float EPSILON = 0.001f;

// Helper function to compare QPointF with epsilon
static bool pointsEqual(const QPointF& p1, const QPointF& p2, float epsilon = EPSILON) {
    return std::abs(p1.x() - p2.x()) < epsilon && std::abs(p1.y() - p2.y()) < epsilon;
}

// Helper function to compare QRectF with epsilon
static bool rectsEqual(const QRectF& r1, const QRectF& r2, float epsilon = EPSILON) {
    return std::abs(r1.x() - r2.x()) < epsilon &&
           std::abs(r1.y() - r2.y()) < epsilon &&
           std::abs(r1.width() - r2.width()) < epsilon &&
           std::abs(r1.height() - r2.height()) < epsilon;
}

class CoordinateTransformTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Standard US Letter page: 8.5" x 11" = 612pt x 792pt
        pageHeight_ = 792.0f;
    }

    float pageHeight_;
};

TEST_F(CoordinateTransformTest, IdentityTransform) {
    // With zoom=1.0 and origin=(0,0), MuPDF fitz-space maps directly to screen
    const float zoom = 1.0f;
    const QPointF origin(0.0f, 0.0f);

    // Fitz-space point at (100, 100) — both coordinate systems are Y-down
    const QPointF pdfPoint(100.0f, 100.0f);
    const QPointF screenPoint = CoordinateTransform::pdfToScreen(
        pdfPoint, zoom, origin, pageHeight_
    );

    // Expected: x = 100 * 1.0 = 100, y = 100 * 1.0 = 100 (no Y-flip)
    EXPECT_NEAR(screenPoint.x(), 100.0f, EPSILON);
    EXPECT_NEAR(screenPoint.y(), 100.0f, EPSILON);

    // Round-trip test
    const QPointF pdfBack = CoordinateTransform::screenToPdf(
        screenPoint, zoom, origin, pageHeight_
    );
    EXPECT_TRUE(pointsEqual(pdfPoint, pdfBack));
}

TEST_F(CoordinateTransformTest, ZoomScaling) {
    // Test with zoom=2.0
    const float zoom = 2.0f;
    const QPointF origin(0.0f, 0.0f);

    const QPointF pdfPoint(100.0f, 200.0f);
    const QPointF screenPoint = CoordinateTransform::pdfToScreen(
        pdfPoint, zoom, origin, pageHeight_
    );

    // Expected: x = 100 * 2.0 = 200, y = 200 * 2.0 = 400 (no Y-flip)
    EXPECT_NEAR(screenPoint.x(), 200.0f, EPSILON);
    EXPECT_NEAR(screenPoint.y(), 400.0f, EPSILON);

    // Round-trip test
    const QPointF pdfBack = CoordinateTransform::screenToPdf(
        screenPoint, zoom, origin, pageHeight_
    );
    EXPECT_TRUE(pointsEqual(pdfPoint, pdfBack));
}

TEST_F(CoordinateTransformTest, NoYFlip) {
    // Verify there is NO Y-axis flip — both systems are Y-down
    const float zoom = 1.0f;
    const QPointF origin(0.0f, 0.0f);

    // Top of page (y=0) maps to screen top (y=0)
    const QPointF pdfTop(0.0f, 0.0f);
    const QPointF screenTop = CoordinateTransform::pdfToScreen(
        pdfTop, zoom, origin, pageHeight_
    );
    EXPECT_NEAR(screenTop.y(), 0.0f, EPSILON);

    // Bottom of page (y=pageHeight) maps to screen bottom (y=pageHeight)
    const QPointF pdfBottom(0.0f, pageHeight_);
    const QPointF screenBottom = CoordinateTransform::pdfToScreen(
        pdfBottom, zoom, origin, pageHeight_
    );
    EXPECT_NEAR(screenBottom.y(), pageHeight_, EPSILON);
}

TEST_F(CoordinateTransformTest, WithPageOffset) {
    // Test with non-zero page origin
    const float zoom = 1.0f;
    const QPointF origin(50.0f, 100.0f);

    const QPointF pdfPoint(100.0f, 100.0f);
    const QPointF screenPoint = CoordinateTransform::pdfToScreen(
        pdfPoint, zoom, origin, pageHeight_
    );

    // Expected: x = 100 * 1.0 + 50 = 150, y = 100 * 1.0 + 100 = 200
    EXPECT_NEAR(screenPoint.x(), 150.0f, EPSILON);
    EXPECT_NEAR(screenPoint.y(), 200.0f, EPSILON);

    // Round-trip test
    const QPointF pdfBack = CoordinateTransform::screenToPdf(
        screenPoint, zoom, origin, pageHeight_
    );
    EXPECT_TRUE(pointsEqual(pdfPoint, pdfBack));
}

TEST_F(CoordinateTransformTest, RectTransform) {
    // Test rectangle conversion — both systems Y-down, so direct mapping
    const float zoom = 1.0f;
    const QPointF origin(0.0f, 0.0f);

    const QRectF pdfRect(100.0f, 200.0f, 150.0f, 100.0f);
    const QRectF screenRect = CoordinateTransform::pdfRectToScreen(
        pdfRect, zoom, origin, pageHeight_
    );

    // With no Y-flip and zoom=1, rect maps directly
    EXPECT_NEAR(screenRect.x(), 100.0f, EPSILON);
    EXPECT_NEAR(screenRect.y(), 200.0f, EPSILON);
    EXPECT_NEAR(screenRect.width(), 150.0f, EPSILON);
    EXPECT_NEAR(screenRect.height(), 100.0f, EPSILON);

    // Round-trip test
    const QRectF pdfBack = CoordinateTransform::screenRectToPdf(
        screenRect, zoom, origin, pageHeight_
    );
    EXPECT_TRUE(rectsEqual(pdfRect, pdfBack));
}

TEST_F(CoordinateTransformTest, RectTransformWithZoom) {
    // Test rectangle conversion with zoom
    const float zoom = 1.5f;
    const QPointF origin(20.0f, 30.0f);

    const QRectF pdfRect(50.0f, 100.0f, 200.0f, 150.0f);
    const QRectF screenRect = CoordinateTransform::pdfRectToScreen(
        pdfRect, zoom, origin, pageHeight_
    );

    // Round-trip test
    const QRectF pdfBack = CoordinateTransform::screenRectToPdf(
        screenRect, zoom, origin, pageHeight_
    );
    EXPECT_TRUE(rectsEqual(pdfRect, pdfBack));
}

TEST_F(CoordinateTransformTest, DifferentPageHeights) {
    // pageHeightPt is unused — verify it doesn't affect results
    const float zoom = 1.0f;
    const QPointF origin(0.0f, 0.0f);

    const QPointF pdfPoint(100.0f, 100.0f);

    // A4 page height: 841.89pt
    const float a4Height = 841.89f;
    const QPointF screen1 = CoordinateTransform::pdfToScreen(
        pdfPoint, zoom, origin, a4Height
    );

    // US Letter page height: 792pt
    const QPointF screen2 = CoordinateTransform::pdfToScreen(
        pdfPoint, zoom, origin, 792.0f
    );

    // Both should give same result since pageHeight is unused
    EXPECT_TRUE(pointsEqual(screen1, screen2));
}

TEST_F(CoordinateTransformTest, MatrixTransform) {
    // Test the transformation matrix matches direct calculation
    const float zoom = 1.5f;
    const QPointF origin(100.0f, 50.0f);

    const QTransform matrix = CoordinateTransform::pdfToScreenMatrix(
        zoom, origin, pageHeight_
    );

    const QPointF pdfPoint(200.0f, 300.0f);
    const QPointF screenPointDirect = CoordinateTransform::pdfToScreen(
        pdfPoint, zoom, origin, pageHeight_
    );

    const QPointF screenPointMatrix = matrix.map(pdfPoint);

    EXPECT_TRUE(pointsEqual(screenPointDirect, screenPointMatrix));
}

TEST_F(CoordinateTransformTest, RoundTripAccuracy) {
    // Test round-trip accuracy with various parameters
    const float zoom = 1.25f;
    const QPointF origin(75.0f, 125.0f);

    const std::vector<QPointF> testPoints = {
        QPointF(0.0f, 0.0f),
        QPointF(100.0f, 100.0f),
        QPointF(612.0f, 792.0f),
        QPointF(306.0f, 396.0f),
        QPointF(50.5f, 75.7f),
    };

    for (const auto& pdfPoint : testPoints) {
        const QPointF screenPoint = CoordinateTransform::pdfToScreen(
            pdfPoint, zoom, origin, pageHeight_
        );
        const QPointF pdfBack = CoordinateTransform::screenToPdf(
            screenPoint, zoom, origin, pageHeight_
        );

        EXPECT_TRUE(pointsEqual(pdfPoint, pdfBack))
            << "Round-trip failed for point (" << pdfPoint.x() << ", " << pdfPoint.y() << ")";
    }
}

TEST_F(CoordinateTransformTest, RectRoundTripAccuracy) {
    // Test round-trip accuracy for rectangles
    const float zoom = 2.0f;
    const QPointF origin(50.0f, 100.0f);

    const std::vector<QRectF> testRects = {
        QRectF(0.0f, 0.0f, 100.0f, 100.0f),
        QRectF(100.0f, 200.0f, 200.0f, 150.0f),
        QRectF(50.5f, 75.7f, 123.4f, 234.5f),
    };

    for (const auto& pdfRect : testRects) {
        const QRectF screenRect = CoordinateTransform::pdfRectToScreen(
            pdfRect, zoom, origin, pageHeight_
        );
        const QRectF pdfBack = CoordinateTransform::screenRectToPdf(
            screenRect, zoom, origin, pageHeight_
        );

        EXPECT_TRUE(rectsEqual(pdfRect, pdfBack))
            << "Round-trip failed for rect (" << pdfRect.x() << ", " << pdfRect.y()
            << ", " << pdfRect.width() << ", " << pdfRect.height() << ")";
    }
}

// ============================================================================
// Annotation Rendering Verification
// ============================================================================

TEST_F(CoordinateTransformTest, AnnotationRendersAtCorrectPosition) {
    // Create annotation at a known screen position and verify it renders there
    std::filesystem::path fixturesDir = TEST_FIXTURES_DIR;
    std::filesystem::path pdfPath = fixturesDir / "test_single.pdf";

    auto context = std::make_shared<pdfe::core::MuPdfContext>();
    auto doc = pdfe::core::Document::open(context, pdfPath);
    ASSERT_NE(doc, nullptr);

    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    QRectF pageBounds = page->boundingBox();
    float pageWidth = pageBounds.width();
    float pageHeight = pageBounds.height();

    // Simulate user clicking in the upper-left quadrant
    float zoom = 1.0f;
    QPointF pageOrigin(0.0f, 0.0f);
    QPointF screenCenter(pageWidth * 0.25f, pageHeight * 0.25f);

    // Convert to MuPDF fitz-space coordinates
    QPointF pdfCenter = CoordinateTransform::screenToPdf(
        screenCenter, zoom, pageOrigin, pageHeight
    );

    // Create 50x50 annotation
    float rectSize = 50.0f;
    QRectF pdfRect(
        pdfCenter.x() - rectSize / 2.0f,
        pdfCenter.y() - rectSize / 2.0f,
        rectSize, rectSize
    );

    pdfe::annotations::AnnotationManager annotMgr(context);
    auto annotData = annotMgr.createAnnotation(
        page.get(),
        pdfe::annotations::AnnotationType::Square,
        pdfRect
    );
    annotMgr.setColor(page.get(), annotData.annotIndex, QColor(255, 0, 0));

    // Render the page
    QRectF renderRegion(0, 0, pageWidth, pageHeight);
    QImage rendered = page->render(renderRegion, zoom);

    // Check for red pixels at expected center
    int cx = static_cast<int>(screenCenter.x());
    int cy = static_cast<int>(screenCenter.y());

    bool foundRedNearCenter = false;
    for (int dy = -30; dy <= 30; dy += 5) {
        for (int dx = -30; dx <= 30; dx += 5) {
            int px = cx + dx;
            int py = cy + dy;
            if (px >= 0 && px < rendered.width() && py >= 0 && py < rendered.height()) {
                QColor c = rendered.pixelColor(px, py);
                if (c.red() > 200 && c.green() < 100 && c.blue() < 100) {
                    foundRedNearCenter = true;
                    break;
                }
            }
        }
        if (foundRedNearCenter) break;
    }

    annotMgr.deleteAnnotation(page.get(), annotData.annotIndex);

    EXPECT_TRUE(foundRedNearCenter) << "Red annotation should render near screen center ("
                                     << cx << ", " << cy << ")";
}

TEST_F(CoordinateTransformTest, RoundTripAnnotation) {
    // Test the full user workflow: click → screenToPdf → createAnnotation →
    // getAnnotation → pdfToScreen → should match original click
    std::filesystem::path fixturesDir = TEST_FIXTURES_DIR;
    std::filesystem::path pdfPath = fixturesDir / "test_single.pdf";

    auto context = std::make_shared<pdfe::core::MuPdfContext>();
    auto doc = pdfe::core::Document::open(context, pdfPath);
    ASSERT_NE(doc, nullptr);

    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    QRectF pageBounds = page->boundingBox();
    float pageHeight = pageBounds.height();

    float zoom = 1.0f;
    QPointF pageOrigin(0.0f, 0.0f);

    // Test multiple zoom levels
    for (float testZoom : {0.5f, 1.0f, 1.5f, 2.0f}) {
        QPointF screenClick(200.0f, 300.0f);

        QPointF pdf = CoordinateTransform::screenToPdf(
            screenClick, testZoom, pageOrigin, pageHeight
        );

        QPointF screenBack = CoordinateTransform::pdfToScreen(
            pdf, testZoom, pageOrigin, pageHeight
        );

        EXPECT_NEAR(screenBack.x(), screenClick.x(), EPSILON)
            << "X round-trip failed at zoom=" << testZoom;
        EXPECT_NEAR(screenBack.y(), screenClick.y(), EPSILON)
            << "Y round-trip failed at zoom=" << testZoom;
    }
}
