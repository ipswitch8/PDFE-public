// -*- coding: utf-8 -*-
//
// R3-21 fidelity oracle: print-output raster vs on-screen render raster.
//
// The existing PrintTest suite (tests/test_printing.cpp) is *functional only*:
// it asserts that PrintEngine::printPage() returns true and that the target
// QImage is non-null. It never inspects the *content* of the printed raster.
// A regression that prints the wrong page, a blank page, or garbage would
// sail straight through those checks.
//
// This oracle closes that gap. It renders page 0 two independent ways:
//   (1) the on-screen path  — Page::render()
//   (2) the print path      — PrintEngine::printPage() into a QImage-backed
//                             QPainter (matched geometry)
// and asserts the two rasters depict the SAME page content, using a
// binarized normalized-cross-correlation similarity metric.
//
// kills a mutation that prints the wrong page index or a blank raster:
//   - a blank print raster has zero variance  -> binarizedNCC collapses toward
//     0, and the explicit dark-content check below fails outright.
//   - a wrong-page print raster correlates worse with the page-0 screen render
//     than the correct page-0 print raster does (PrintDiscriminatesPageIndex).
//
// IMPORTANT (CI constraint): a real QPrinter crashes headlessly on this CI
// (SEH 0xC0000005). We therefore NEVER construct a QPrinter — we drive the
// printPage(QPainter*, ...) overload against a QImage-backed QPainter, exactly
// like PrintTest.PrintPageDirectly in tests/test_printing.cpp.

#include <gtest/gtest.h>
#include "core/Document.h"
#include "core/Page.h"
#include "core/PrintEngine.h"
#include "core/MuPdfContext.h"
#include "tre/ImageSimilarity.h"

#include <QImage>
#include <QPainter>
#include <QColor>
#include <filesystem>
#include <memory>

using namespace pdfe::core;
using pdfe::tre::ImageSimilarity;

namespace {

// Flatten an image onto an opaque white background.
//
// Page::render() rasterizes via fz_new_pixmap_from_page(..., alpha=1); MuPDF
// clears an alpha pixmap to TRANSPARENT BLACK (0,0,0,0) and composites the page
// on top, so the on-screen raster's background is (0,0,0,0). Qt's grayscale
// conversion drops alpha and reads the raw RGB, turning that background BLACK —
// which both makes the screen raster near-uniform (NCC denominator -> 0) and
// inverts it relative to the print raster (which PrintEngine already draws onto
// white). Compositing both rasters over white here makes them directly
// comparable and makes darkPixelFraction() actually measure ink rather than the
// transparent background. This does not weaken the oracle: a blank print is
// still a uniform white field (NCC collapses), and a wrong-page print still
// carries different content.
QImage flattenOntoWhite(const QImage& src) {
    if (src.isNull()) return src;
    QImage out(src.size(), QImage::Format_RGB32);
    out.fill(Qt::white);
    QPainter painter(&out);
    painter.drawImage(0, 0, src);
    painter.end();
    return out;
}

// Fraction of pixels that are meaningfully dark (below a mid-grey threshold).
// A blank/white raster returns ~0.0; a page with visible text returns > 0.
double darkPixelFraction(const QImage& image) {
    QImage gray = ImageSimilarity::toGrayscale(image);
    if (gray.isNull()) return 0.0;
    const int w = gray.width();
    const int h = gray.height();
    if (w == 0 || h == 0) return 0.0;
    long dark = 0;
    for (int y = 0; y < h; ++y) {
        const uchar* row = gray.constScanLine(y);
        for (int x = 0; x < w; ++x) {
            if (row[x] < 160) ++dark;  // < ~63% grey counts as ink
        }
    }
    return static_cast<double>(dark) / (static_cast<double>(w) * h);
}

} // namespace

class FidelityR3_21_PrintVsScreen : public ::testing::Test {
protected:
    void SetUp() override {
        context = std::make_shared<MuPdfContext>();
        std::filesystem::path fixturesDir = TEST_FIXTURES_DIR;
        testPdf = fixturesDir / "test_3page.pdf";
        if (std::filesystem::exists(testPdf)) {
            document = Document::open(context, testPdf);
        }
    }

    // Render a page the on-screen way at a fixed scale (2x = crisp text).
    QImage renderScreen(int pageIndex, float scale) {
        auto page = document->page(pageIndex);
        EXPECT_TRUE(page != nullptr);
        QRectF box = page->boundingBox();
        // Flatten onto white: Page::render() returns a raster with a
        // transparent-black background (alpha pixmap), which is neither
        // comparable to the white-background print raster nor meaningful under
        // grayscale conversion. See flattenOntoWhite().
        return flattenOntoWhite(page->render(box, scale));
    }

    // Render a page the print way into a white QImage-backed QPainter, sized to
    // match the screen raster so geometry lines up. fitToPage preserves aspect
    // and fills the (already page-aspect) target rect; centering/rotation off so
    // the two rasters share an origin and orientation.
    QImage renderPrint(int pageIndex, int width, int height) {
        QImage image(width, height, QImage::Format_RGBA8888);
        image.fill(Qt::white);
        QPainter painter(&image);
        PrintOptions options;
        options.fitToPage = true;
        options.centerOnPage = false;
        options.autoRotate = false;
        QRectF targetRect(0, 0, width, height);
        PrintEngine engine(document.get());
        bool ok = engine.printPage(&painter, pageIndex, targetRect, options);
        painter.end();
        EXPECT_TRUE(ok);
        // Already an opaque white-background raster; flatten for a format match
        // with the screen raster and as a no-op safety net.
        return flattenOntoWhite(image);
    }

    std::shared_ptr<MuPdfContext> context;
    std::filesystem::path testPdf;
    std::unique_ptr<Document> document;
};

// Main oracle: the printed raster must depict the same page content as the
// on-screen render. Functional-only tests never verify this.
TEST_F(FidelityR3_21_PrintVsScreen, PrintRasterResemblesScreenRaster) {
    if (!document) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    // Screen render at 2x for a legible raster; print into the SAME dimensions
    // so geometry matches with no post-hoc rescaling required.
    const float kScale = 2.0f;
    QImage screen = renderScreen(0, kScale);
    ASSERT_FALSE(screen.isNull());
    ASSERT_GT(screen.width(), 0);
    ASSERT_GT(screen.height(), 0);

    QImage print = renderPrint(0, screen.width(), screen.height());
    ASSERT_FALSE(print.isNull());

    // 1) Both rasters must carry real ink. A blank print (a plausible
    //    regression) has near-zero dark fraction and fails here regardless of
    //    the correlation metric below.
    const double screenInk = darkPixelFraction(screen);
    const double printInk  = darkPixelFraction(print);
    EXPECT_GT(screenInk, 0.001) << "screen render has no visible content";
    EXPECT_GT(printInk,  0.001) << "print raster is blank / no visible content";

    // 2) Structural similarity. binarizedNCC binarizes both images at their own
    //    Otsu thresholds and correlates them (auto-scaling to a common size), so
    //    it is robust to mild resampling differences between the two render
    //    paths but collapses toward 0 for a blank or wrong-page raster.
    //    Same-content rasters correlate strongly; 0.70 is generous enough to
    //    absorb anti-aliasing / sub-pixel offset yet tight enough that a blank,
    //    garbage, or different-page raster cannot pass.
    const float ncc = ImageSimilarity::binarizedNCC(screen, print);
    EXPECT_GE(ncc, 0.70f)
        << "printed raster does not resemble the on-screen render "
        << "(NCC=" << ncc << ") — wrong page, blank, or garbage print output";
}

// Discrimination oracle: the page-0 screen render must correlate BETTER with
// the page-0 print raster than with a different page's print raster. This is
// what actually kills a "printPage always prints page N" or off-by-one
// page-index mutation — a similarity floor alone could be satisfied by any
// visually busy page.
TEST_F(FidelityR3_21_PrintVsScreen, PrintDiscriminatesPageIndex) {
    if (!document) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }
    if (document->pageCount() < 2) {
        GTEST_SKIP() << "fixture has fewer than 2 pages";
    }

    const float kScale = 2.0f;
    QImage screen0 = renderScreen(0, kScale);
    ASSERT_FALSE(screen0.isNull());

    QImage print0 = renderPrint(0, screen0.width(), screen0.height());
    QImage print1 = renderPrint(1, screen0.width(), screen0.height());
    ASSERT_FALSE(print0.isNull());
    ASSERT_FALSE(print1.isNull());

    const float sameNcc  = ImageSimilarity::binarizedNCC(screen0, print0);
    const float crossNcc = ImageSimilarity::binarizedNCC(screen0, print1);

    // The correct page must be the closer match. If printPage ignored the
    // pageIndex argument (or was off by one) these would be equal / inverted.
    EXPECT_GT(sameNcc, crossNcc)
        << "page-0 screen render matched page-1 print (" << crossNcc
        << ") at least as well as page-0 print (" << sameNcc
        << ") — printPage may be ignoring the page index";
}
