#include <gtest/gtest.h>
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "core/Error.h"
#include "core/TextExtractor.h"
#include "annotations/AnnotationManager.h"
#include "annotations/AnnotationData.h"
#include "annotations/AnnotationType.h"
#include "core/CoordinateTransform.h"

#include <QRectF>
#include <QImage>
#include <QColor>
#include <cstdio>
#include <filesystem>

// Tests verifying MuPDF uses fitz-space (Y-down) coordinates
class CoordinateDiagnostic : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = std::make_shared<pdfe::core::MuPdfContext>();
        std::filesystem::path fixturesDir = TEST_FIXTURES_DIR;
        std::filesystem::path testPdf = fixturesDir / "test_single.pdf";
        doc_ = pdfe::core::Document::open(context_, testPdf);
        ASSERT_TRUE(doc_);
        ASSERT_GT(doc_->pageCount(), 0);
    }

    std::shared_ptr<pdfe::core::MuPdfContext> context_;
    std::unique_ptr<pdfe::core::Document> doc_;
};

// Verify text extraction returns fitz-space (Y-down) coordinates
TEST_F(CoordinateDiagnostic, TextExtractionIsFitzSpace) {
    auto page = doc_->page(0);
    auto pageInfo = doc_->pageInfo(0);
    float pageHeight = pageInfo.heightPt;

    pdfe::core::TextExtractor extractor(context_);
    auto blocks = extractor.extract(page.get());

    ASSERT_FALSE(blocks.empty());
    ASSERT_FALSE(blocks[0].lines.empty());
    ASSERT_FALSE(blocks[0].lines[0].chars.empty());

    auto& firstChar = blocks[0].lines[0].chars[0];

    // Text near top of page should have small Y in fitz-space (Y-DOWN)
    EXPECT_LT(firstChar.quad.y(), pageHeight / 2.0)
        << "First character Y=" << firstChar.quad.y()
        << " should be less than pageHeight/2=" << pageHeight / 2.0
        << " (fitz-space Y-DOWN)";
}

// Verify annotation renders at the correct screen position (no Y-mirror)
TEST_F(CoordinateDiagnostic, AnnotationRendersCorrectly_TopLeft) {
    using namespace pdfe::annotations;
    using namespace pdfe::core;

    auto pageInfo = doc_->pageInfo(0);
    float pageHeight = pageInfo.heightPt;
    float pageWidth = pageInfo.widthPt;

    // Create RED square annotation in the top-left area (screen y=30-80)
    float zoom = 1.0f;
    QPointF pageOrigin(0, 0);
    QPointF screenTL(30, 30);
    QPointF screenBR(80, 80);

    QPointF pdfTL = CoordinateTransform::screenToPdf(screenTL, zoom, pageOrigin, pageHeight);
    QPointF pdfBR = CoordinateTransform::screenToPdf(screenBR, zoom, pageOrigin, pageHeight);

    QRectF pdfRect(
        std::min(pdfTL.x(), pdfBR.x()),
        std::min(pdfTL.y(), pdfBR.y()),
        std::abs(pdfBR.x() - pdfTL.x()),
        std::abs(pdfBR.y() - pdfTL.y())
    );

    printf("screenToPdf TL: (%.1f, %.1f), BR: (%.1f, %.1f)\n",
           pdfTL.x(), pdfTL.y(), pdfBR.x(), pdfBR.y());
    printf("pdfRect: (%.1f, %.1f, w=%.1f, h=%.1f)\n",
           pdfRect.x(), pdfRect.y(), pdfRect.width(), pdfRect.height());

    auto page = doc_->page(0);
    AnnotationManager manager(context_);
    auto data = manager.createAnnotation(page.get(), AnnotationType::Square, pdfRect);
    manager.setColor(page.get(), data.annotIndex, QColor(255, 0, 0));

    printf("Created annot rect: (%.1f, %.1f, w=%.1f, h=%.1f)\n",
           data.rect.x(), data.rect.y(), data.rect.width(), data.rect.height());

    // Render the full page
    QRectF fullPage(0, 0, pageWidth, pageHeight);
    QImage rendered = page->render(fullPage, zoom);

    // Save for visual inspection
    rendered.save(QString::fromStdString(
        (std::filesystem::path(TEST_FIXTURES_DIR) / ".." / "rendered_annot_test.png").string()));
    printf("Saved rendered image (%dx%d)\n", rendered.width(), rendered.height());

    // Scan every pixel in the expected region for any reddish content.
    // Square annotations render as thin stroke outlines (~1-2px border),
    // so we must check every pixel, not sample at intervals.
    auto hasRedComponent = [](const QColor& px) {
        return px.red() > 150 && px.green() < 120 && px.blue() < 120;
    };

    bool foundRedExpected = false;
    for (int y = 25; y <= 85 && !foundRedExpected; ++y) {
        for (int x = 25; x <= 85; ++x) {
            if (x < rendered.width() && y < rendered.height()) {
                if (hasRedComponent(rendered.pixelColor(x, y))) {
                    printf("Found red at expected region: (%d, %d)\n", x, y);
                    foundRedExpected = true;
                    break;
                }
            }
        }
    }

    // Check for red at mirrored position (bottom-left)
    bool foundRedMirrored = false;
    int mirrorStart = static_cast<int>(pageHeight) - 85;
    int mirrorEnd = static_cast<int>(pageHeight) - 25;
    for (int y = mirrorStart; y <= mirrorEnd && !foundRedMirrored; ++y) {
        for (int x = 25; x <= 85; ++x) {
            if (x < rendered.width() && y < rendered.height() && y >= 0) {
                if (hasRedComponent(rendered.pixelColor(x, y))) {
                    printf("Found red at mirrored region: (%d, %d)\n", x, y);
                    foundRedMirrored = true;
                    break;
                }
            }
        }
    }

    manager.deleteAnnotation(page.get(), data.annotIndex);

    EXPECT_TRUE(foundRedExpected) << "Annotation should render at top-left (y=30-80)";
    EXPECT_FALSE(foundRedMirrored) << "Annotation should NOT render at mirrored bottom-left";
}

// Verify coordinate round-trip preserves position
TEST_F(CoordinateDiagnostic, AnnotationRoundTrip) {
    using namespace pdfe::annotations;
    using namespace pdfe::core;

    auto pageInfo = doc_->pageInfo(0);
    float pageHeight = pageInfo.heightPt;

    float zoom = 1.0f;
    QPointF pageOrigin(0, 0);
    QPointF screenTL(20, 20);
    QPointF screenBR(120, 70);

    QPointF pdfTL = CoordinateTransform::screenToPdf(screenTL, zoom, pageOrigin, pageHeight);
    QPointF pdfBR = CoordinateTransform::screenToPdf(screenBR, zoom, pageOrigin, pageHeight);

    QRectF pdfRect(
        std::min(pdfTL.x(), pdfBR.x()),
        std::min(pdfTL.y(), pdfBR.y()),
        std::abs(pdfBR.x() - pdfTL.x()),
        std::abs(pdfBR.y() - pdfTL.y())
    );

    auto page = doc_->page(0);
    AnnotationManager manager(context_);
    auto data = manager.createAnnotation(page.get(), AnnotationType::Square, pdfRect);

    auto readData = manager.getAnnotation(page.get(), data.annotIndex);
    QRectF screenRect = CoordinateTransform::pdfRectToScreen(readData.rect, zoom, pageOrigin, pageHeight);

    float tolerance = 10.0f;  // MuPDF border expansion
    EXPECT_NEAR(screenRect.y(), 20.0, tolerance)
        << "Round-trip should preserve screen Y position";
    EXPECT_NEAR(screenRect.x(), 20.0, tolerance)
        << "Round-trip should preserve screen X position";

    manager.deleteAnnotation(page.get(), data.annotIndex);
}
