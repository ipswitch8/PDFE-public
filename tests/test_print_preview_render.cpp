// -*- coding: utf-8 -*-
//
// Gap A1-27 — "Print preview — UNCOVERED, deferred".
//
// What existed before this file:
//   * tests/test_ui_integration.cpp:91-99,200-203 — the print-preview QAction
//     exists and has the right enabled/disabled state.
//   * tests/test_uncovered_dialogs.cpp:472-504 — PrintPreviewDialog constructs
//     with a null document, has an objectName and an accessible name.
// Neither of those ever rasterizes anything. A preview that showed the WRONG
// PAGE, at the wrong scale, or in the wrong orientation passed both.
//
// What this file asserts:
//   The preview is produced by PrintPreviewDialog::onPaintRequested(QPrinter*)
//   (src/ui/PrintPreviewDialog.cpp:81-89) — the slot QPrintPreviewWidget calls
//   with the printer it wants painted. That slot is the ENTIRE preview render
//   path, and it is driven here directly, headlessly, with a
//   QPrinter::PdfFormat printer writing to a temp file. The resulting PDF is
//   re-opened and rasterized, and compared against
//     (1) the same document driven through the PRINT path
//         (PrintEngine::print with an identically configured printer), and
//     (2) the AUTHORED content of tests/fixtures/test_3page.pdf.
//
// (2) is what makes these oracles real. test_3page.pdf's three pages are
// authored differently and the numbers below come from its content streams,
// not from PDFE output:
//
//   MediaBox: [0 0 595.28 841.89]  (A4)
//   page 1: 24pt title at (191.60, 778.00) + two 12pt lines — sparse, centred
//   page 2: 18pt heading at (31.18, 786.88) + a full-width 11pt paragraph
//   page 3: 18pt heading at (31.18, 786.88), three 11pt list items, and a
//           stroked rectangle  `141.73 558.43 283.46 -170.08 re S`
//           => x in [141.73, 425.19], y in [388.35, 558.43]
//
// Page 3's ink therefore spans, in PDF user space,
//   x: 31.18 (list/heading left edge) .. 425.19 (rectangle right edge)
//   y: 388.35 (rectangle bottom) .. ~799 (top of the 18pt heading)
// which is the authored geometry the orientation/scale oracle below checks.
//
// HEADLESS-NESS (honesty note): no NativeFormat printer, no device context and
// no installed printer is required anywhere in this file. QPrinter is used
// only in QPrinter::PdfFormat mode, exactly as tests/test_printing.cpp:114-117
// already does successfully on this platform. The one thing that genuinely
// cannot be exercised headlessly is QPrintPreviewWidget's own on-screen
// pixmap generation (it needs a shown widget and its own NativeFormat
// QPrinter) — that is a Qt widget, not PDFE code, and PDFE's entire
// contribution to the preview is the paintRequested slot driven here.

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "core/PageInfo.h"
#include "core/PrintEngine.h"
#include "tre/ImageSimilarity.h"
#include "ui/PrintPreviewDialog.h"

#include <QApplication>
#include <QColor>
#include <QImage>
#include <QMetaObject>
#include <QPageLayout>
#include <QPageSize>
#include <QPainter>
#include <QPoint>
#include <QPrinter>
#include <QRect>
#include <QRectF>
#include <QSize>
#include <QTemporaryDir>
#include <QTransform>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <vector>

using pdfe::core::Document;
using pdfe::core::MuPdfContext;
using pdfe::core::PrintEngine;
using pdfe::core::PrintOptions;
using pdfe::core::PrintRange;
using pdfe::tre::ImageSimilarity;

namespace {

// Authored ground truth read out of tests/fixtures/test_3page.pdf.
constexpr float kFixturePageWidthPt  = 595.28f;   // /MediaBox [0 0 595.28 841.89]
constexpr float kFixturePageHeightPt = 841.89f;
constexpr int   kFixturePageCount    = 3;

// Page-3 authored ink extent in PDF user space (see the header comment).
constexpr float kPage3InkLeftPt   = 31.18f;    // heading / list-item left edge
constexpr float kPage3InkRightPt  = 425.19f;   // 141.73 + 283.46 rectangle right
constexpr float kPage3InkBottomPt = 388.35f;   // 558.43 - 170.08 rectangle bottom
// Top of ink = 18pt heading baseline (786.88) + its cap height. Helvetica-class
// cap height is ~0.72em, so ~799.8. The tolerance below (3% of the page, ~25pt)
// comfortably absorbs the exact font metric while still pinning the geometry.
constexpr float kPage3InkTopPt    = 799.8f;

// Page::render() rasterizes into an alpha pixmap whose background is
// transparent BLACK; comparing that to a print raster (drawn onto white) is
// meaningless until both are flattened. Same treatment as
// tests/test_fidelity_r3_21_print_vs_screen.cpp.
QImage flattenOntoWhite(const QImage& src)
{
    if (src.isNull()) return src;
    QImage out(src.size(), QImage::Format_RGB32);
    out.fill(Qt::white);
    QPainter p(&out);
    p.drawImage(0, 0, src);
    p.end();
    return out;
}

// Bounding box of "ink" (pixels below a mid-grey threshold) in an opaque
// white-background raster. Returns a null QRect when the raster is blank.
// Threshold 160 matches the print-fidelity oracle and is dark enough that JPEG
// ringing / anti-aliasing haloes around glyph edges do not enlarge the box,
// while the fixture's 0.57pt stroked rectangle (which renders at roughly 57%
// coverage => ~110 grey) is still counted.
QRect inkBoundingBox(const QImage& image)
{
    QImage gray = ImageSimilarity::toGrayscale(image);
    if (gray.isNull()) return QRect();
    int minX = gray.width(), minY = gray.height(), maxX = -1, maxY = -1;
    for (int y = 0; y < gray.height(); ++y) {
        const uchar* row = gray.constScanLine(y);
        for (int x = 0; x < gray.width(); ++x) {
            if (row[x] < 160) {
                if (x < minX) minX = x;
                if (x > maxX) maxX = x;
                if (y < minY) minY = y;
                if (y > maxY) maxY = y;
            }
        }
    }
    if (maxX < 0) return QRect();
    return QRect(QPoint(minX, minY), QPoint(maxX, maxY));
}

// Configure a printer that needs no printing subsystem at all: PDF output to a
// file, explicit A4 (matching the fixture's MediaBox aspect exactly) and
// full-page mode so pageRect() == paperRect() and the device-space geometry is
// a clean multiple of PDF user space.
void configurePdfPrinter(QPrinter& printer,
                         const QString& outPath,
                         int resolution,
                         QPageLayout::Orientation orientation)
{
    printer.setOutputFormat(QPrinter::PdfFormat);
    printer.setOutputFileName(outPath);
    printer.setResolution(resolution);
    printer.setPageSize(QPageSize(QPageSize::A4));
    printer.setPageOrientation(orientation);
    printer.setFullPage(true);
}

} // namespace

class PrintPreviewRenderTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        tempDir = std::make_unique<QTemporaryDir>();
        ASSERT_TRUE(tempDir->isValid());

        context = std::make_shared<MuPdfContext>();
        fixture = std::filesystem::path(TEST_FIXTURES_DIR) / "test_3page.pdf";
        // test_3page.pdf is a committed fixture guarded by
        // FixtureCorpus.EveryPdfFixtureOpensCleanly — assert, never skip.
        ASSERT_TRUE(std::filesystem::exists(fixture)) << fixture.string();
        document = Document::open(context, fixture);
        ASSERT_NE(document, nullptr);
        ASSERT_EQ(document->pageCount(), kFixturePageCount);

        // The authored page geometry the oracles below depend on.
        const auto info = document->pageInfo(0);
        ASSERT_NEAR(info.widthPt,  kFixturePageWidthPt,  1.0f)
            << "fixture MediaBox changed; the authored constants in this file "
               "were derived from [0 0 595.28 841.89]";
        ASSERT_NEAR(info.heightPt, kFixturePageHeightPt, 1.0f);
    }

    QString tmp(const char* name) const { return tempDir->path() + "/" + name; }

    // ── The PREVIEW path ────────────────────────────────────────────────
    // Drives the real production slot: PrintPreviewDialog::onPaintRequested,
    // the only PDFE code QPrintPreviewWidget ever calls. Anything the dialog
    // gets wrong about the document, the options or the page indices shows up
    // in the PDF it paints.
    bool renderViaPreview(const PrintOptions& options,
                          const QString& outPath,
                          int resolution,
                          QPageLayout::Orientation orientation)
    {
        pdfe::ui::PrintPreviewDialog dlg(document.get(), options, nullptr);
        QPrinter printer;
        configurePdfPrinter(printer, outPath, resolution, orientation);
        // Invoking by name also pins the slot's signature: if
        // onPaintRequested(QPrinter*) is renamed or re-typed, the preview stops
        // being connectable to QPrintPreviewWidget::paintRequested and this
        // returns false.
        const bool invoked = QMetaObject::invokeMethod(
            &dlg, "onPaintRequested", Qt::DirectConnection,
            Q_ARG(QPrinter*, &printer));
        EXPECT_TRUE(invoked)
            << "PrintPreviewDialog must expose onPaintRequested(QPrinter*) — "
               "that is the slot QPrintPreviewWidget::paintRequested drives";
        return invoked;
    }

    // ── The PRINT path ──────────────────────────────────────────────────
    void renderViaPrint(const PrintOptions& options,
                        const QString& outPath,
                        int resolution,
                        QPageLayout::Orientation orientation)
    {
        QPrinter printer;
        configurePdfPrinter(printer, outPath, resolution, orientation);
        PrintEngine engine(document.get());
        EXPECT_TRUE(engine.print(&printer, options))
            << "the print path must succeed for an unencrypted fixture";
    }

    // Rasterize every page of a produced PDF at 1:1 (1 px per PDF point).
    std::vector<QImage> rasterize(const QString& pdfPath)
    {
        std::vector<QImage> out;
        const std::filesystem::path p(pdfPath.toStdString());
        if (!std::filesystem::exists(p)) return out;
        auto ctx = std::make_shared<MuPdfContext>();
        auto doc = Document::open(ctx, p);
        if (!doc) return out;
        for (int i = 0; i < doc->pageCount(); ++i) {
            auto page = doc->page(i);
            if (!page) { out.emplace_back(); continue; }
            out.push_back(flattenOntoWhite(page->render(page->boundingBox(), 1.0f)));
        }
        return out;
    }

    // Authored reference: the SOURCE fixture page as it was written, rendered
    // straight from the document (no print pipeline involved).
    QImage sourcePage(int index, float scale = 1.0f)
    {
        auto page = document->page(index);
        EXPECT_NE(page, nullptr);
        if (!page) return QImage();
        return flattenOntoWhite(page->render(page->boundingBox(), scale));
    }

    std::unique_ptr<QTemporaryDir> tempDir;
    std::shared_ptr<MuPdfContext> context;
    std::filesystem::path fixture;
    std::unique_ptr<Document> document;
};

// ---------------------------------------------------------------------------
// 1. Preview output == print output, page for page, under NON-DEFAULT options.
//
// The options deliberately differ from PrintOptions{}'s defaults in every
// field that changes the output: a 2-page range instead of AllPages, centring
// off, auto-rotate off.
//
// RED / mutations killed:
//   * PrintPreviewDialog::onPaintRequested calling
//     `engine.print(printer, pdfe::core::PrintOptions{})` — i.e. dropping the
//     options_ member — prints 3 pages instead of 2: page-count assertion.
//   * onPaintRequested binding the engine to something other than document_
//     (or to a null document, its current early-return): zero pages produced.
//   * any divergence between the preview render and the print render at all:
//     the per-page NCC and ink-box assertions.
// ---------------------------------------------------------------------------
TEST_F(PrintPreviewRenderTest, PreviewOutputMatchesPrintOutputPageForPage)
{
    ASSERT_NE(qApp, nullptr) << "tests/test_main.cpp constructs a QApplication";

    PrintOptions options;
    options.range         = PrintRange::PageRange;
    options.fromPage      = 2;    // 1-based -> source pages 1 and 2
    options.toPage        = 3;
    options.copies        = 1;
    options.fitToPage     = true;
    options.centerOnPage  = false;
    options.autoRotate    = false;

    const QString previewPdf = tmp("preview_range.pdf");
    const QString printPdf   = tmp("print_range.pdf");
    ASSERT_TRUE(renderViaPreview(options, previewPdf, 72, QPageLayout::Portrait));
    renderViaPrint(options, printPdf, 72, QPageLayout::Portrait);

    const std::vector<QImage> preview = rasterize(previewPdf);
    const std::vector<QImage> printed = rasterize(printPdf);

    ASSERT_EQ(preview.size(), std::size_t{2})
        << "the preview must honour the 2-page range it was constructed with; "
           "3 pages means options_ was ignored, 0 means the document was not "
           "passed through";
    ASSERT_EQ(printed.size(), std::size_t{2});

    for (std::size_t i = 0; i < preview.size(); ++i) {
        ASSERT_FALSE(preview[i].isNull()) << "preview page " << i;
        ASSERT_FALSE(printed[i].isNull()) << "printed page " << i;

        const float ncc = ImageSimilarity::binarizedNCC(preview[i], printed[i]);
        EXPECT_GE(ncc, 0.95f)
            << "preview page " << i << " does not depict the same thing the "
            << "print path produced (NCC=" << ncc << ")";

        const QRect pv = inkBoundingBox(preview[i]);
        const QRect pr = inkBoundingBox(printed[i]);
        ASSERT_FALSE(pv.isNull()) << "preview page " << i << " is blank";
        ASSERT_FALSE(pr.isNull()) << "printed page " << i << " is blank";

        // Same position and same scale, not merely similar-looking content.
        // Tolerance: 1% of the A4 page (~6 px at this raster size).
        const int tolX = std::max(3, preview[i].width()  / 100);
        const int tolY = std::max(3, preview[i].height() / 100);
        EXPECT_NEAR(pv.left(),   pr.left(),   tolX) << "page " << i;
        EXPECT_NEAR(pv.right(),  pr.right(),  tolX) << "page " << i;
        EXPECT_NEAR(pv.top(),    pr.top(),    tolY) << "page " << i;
        EXPECT_NEAR(pv.bottom(), pr.bottom(), tolY) << "page " << i;
    }
}

// ---------------------------------------------------------------------------
// 2. Page identity, anchored to the AUTHORED source pages.
//
// Preview page i must resemble source page i more than it resembles any other
// source page. The fixture's three pages are authored to look nothing alike
// (sparse centred title / dense paragraph / list + stroked rectangle), so this
// is decidable.
//
// RED / mutations killed:
//   * off-by-one page index anywhere in the preview path (e.g. `pageIndex + 1`
//     or a 1-based/0-based confusion in PrintEngine::Impl::print's
//     `startPage = options.fromPage - 1`): preview page 0 then carries source
//     page 1's content and the cross-match beats the self-match.
//   * `printPageImpl` ignoring pageIndex and always rendering page 0: pages 1
//     and 2 fail.
// ---------------------------------------------------------------------------
TEST_F(PrintPreviewRenderTest, PreviewPagesMatchTheAuthoredSourcePagesOneForOne)
{
    ASSERT_NE(qApp, nullptr);

    PrintOptions options;                 // AllPages
    options.fitToPage    = true;
    options.centerOnPage = true;
    options.autoRotate   = false;

    const QString previewPdf = tmp("preview_all.pdf");
    ASSERT_TRUE(renderViaPreview(options, previewPdf, 72, QPageLayout::Portrait));

    const std::vector<QImage> preview = rasterize(previewPdf);
    ASSERT_EQ(preview.size(), static_cast<std::size_t>(kFixturePageCount));

    std::vector<QImage> source;
    for (int i = 0; i < kFixturePageCount; ++i) {
        source.push_back(sourcePage(i));
        ASSERT_FALSE(source.back().isNull());
    }

    for (int i = 0; i < kFixturePageCount; ++i) {
        ASSERT_FALSE(preview[static_cast<std::size_t>(i)].isNull());
        const float self =
            ImageSimilarity::binarizedNCC(preview[static_cast<std::size_t>(i)], source[static_cast<std::size_t>(i)]);
        // Absolute floor: the preview page is a re-rasterization (page image
        // drawn at 150-dpi-equivalent then resampled onto the output page) of
        // a crisp vector render, so it is blurrier than `source[i]`; 0.60 is
        // the "real content, not blank and not garbage" floor. The oracle that
        // actually pins the page INDEX is the strict self > cross comparison
        // below, which no blur threshold can fake.
        EXPECT_GE(self, 0.60f)
            << "preview page " << i << " does not depict authored source page "
            << i << " (NCC=" << self << ")";
        for (int j = 0; j < kFixturePageCount; ++j) {
            if (i == j) continue;
            const float cross =
                ImageSimilarity::binarizedNCC(preview[static_cast<std::size_t>(i)], source[static_cast<std::size_t>(j)]);
            EXPECT_GT(self, cross)
                << "preview page " << i << " matched authored source page " << j
                << " (" << cross << ") at least as well as source page " << i
                << " (" << self << ") — the preview is showing the wrong page";
        }
    }
}

// ---------------------------------------------------------------------------
// 3. Orientation + scale, anchored to the AUTHORED page-3 geometry.
//
// With fitToPage on and a portrait A4 target whose aspect matches the fixture's
// A4 MediaBox, the printed page is the source page scaled 1:1 in relative
// terms. So the ink box of preview page 2, expressed as a fraction of the
// output page, must equal the authored ink extent expressed as a fraction of
// the 595.28 x 841.89 MediaBox.
//
// RED / mutations killed:
//   * swapped width/height in the preview target rect (i.e.
//     `QRectF(0, 0, printerPageRect.height(), printerPageRect.width())` in
//     PrintEngine::Impl::print): the target becomes landscape, autoRotate then
//     fires, and the content lands rotated — every one of the four authored
//     fractions below moves by tens of percent.
//   * fitToPage ignored / forced to false: scale collapses to 1.0 and the ink
//     box shrinks to ~1/2.08 of the expected fractions (see test 4 for the
//     quantitative version).
//   * centerOnPage silently applied when off / offset by the target origin:
//     left and top move.
//   * rendering the source page's bounding box with the rotation swap applied
//     to an unrotated page: the aspect inverts.
// ---------------------------------------------------------------------------
TEST_F(PrintPreviewRenderTest, PreviewPreservesAuthoredOrientationAndScale)
{
    ASSERT_NE(qApp, nullptr);

    PrintOptions options;
    options.range        = PrintRange::PageRange;
    options.fromPage     = 3;   // 1-based -> source page index 2
    options.toPage       = 3;
    options.fitToPage    = true;
    options.centerOnPage = true;
    options.autoRotate   = true;   // portrait page into portrait target: no-op

    const QString previewPdf = tmp("preview_page3.pdf");
    ASSERT_TRUE(renderViaPreview(options, previewPdf, 72, QPageLayout::Portrait));

    const std::vector<QImage> preview = rasterize(previewPdf);
    ASSERT_EQ(preview.size(), std::size_t{1})
        << "a single-page range must emit exactly one preview page";
    const QImage& raster = preview[0];
    ASSERT_FALSE(raster.isNull());
    ASSERT_GT(raster.width(), 0);
    ASSERT_GT(raster.height(), 0);
    ASSERT_GT(raster.height(), raster.width())
        << "a portrait A4 target must yield a portrait page";

    const QRect ink = inkBoundingBox(raster);
    ASSERT_FALSE(ink.isNull()) << "the previewed page is blank";

    const double w = raster.width();
    const double h = raster.height();
    const double gotLeft   = ink.left()          / w;
    const double gotRight  = (ink.right() + 1.0) / w;
    const double gotTop    = ink.top()           / h;
    const double gotBottom = (ink.bottom() + 1.0)/ h;

    // Authored expectations — derived from the fixture's content stream, never
    // from PDFE's own output. PDF y grows upward; raster y grows downward.
    const double expLeft   = kPage3InkLeftPt   / kFixturePageWidthPt;   // ~0.052
    const double expRight  = kPage3InkRightPt  / kFixturePageWidthPt;   // ~0.714
    const double expTop    = (kFixturePageHeightPt - kPage3InkTopPt)
                                / kFixturePageHeightPt;                 // ~0.050
    const double expBottom = (kFixturePageHeightPt - kPage3InkBottomPt)
                                / kFixturePageHeightPt;                 // ~0.539

    // 3% of the page (~18pt wide / ~25pt tall) absorbs font-metric slack in the
    // top edge and stroke width in the rectangle, and nothing else.
    const double tol = 0.03;
    EXPECT_NEAR(gotLeft,   expLeft,   tol)
        << "authored left ink edge is x=" << kPage3InkLeftPt << "pt";
    EXPECT_NEAR(gotRight,  expRight,  tol)
        << "authored right ink edge is the rectangle's x=" << kPage3InkRightPt << "pt";
    EXPECT_NEAR(gotTop,    expTop,    tol)
        << "authored top ink is the 18pt heading at baseline y=786.88pt";
    EXPECT_NEAR(gotBottom, expBottom, tol)
        << "authored bottom ink is the rectangle's y=" << kPage3InkBottomPt << "pt";
}

// ---------------------------------------------------------------------------
// 4. fitToPage is honoured — quantitatively.
//
// At 150 dpi the full-page A4 target is 595.28pt * 150/72 = 1240 device px
// wide (1754 tall). fitToPage=true scales the page by
// min(1240/595.28, 1754/841.89) = 2.0833; fitToPage=false pins the scale at
// 1.0 (PrintEngine.cpp:141). Both runs emit an A4 output page, so the ratio of
// the two ink-box widths must be exactly that fit factor.
//
// RED / mutations killed:
//   * `float scale = 1.0f;` unconditionally (fitToPage ignored) -> ratio 1.0.
//   * `float scale = options.fitToPage ? std::max(scaleX, scaleY) : 1.0f;`
//     (fit-to-fill instead of fit-to-page) -> ratio 2.084 vs an expected
//     2.0833 for a matched aspect... which is why the aspect-independent
//     assertion below ALSO checks the fitted content does not overflow the
//     page.
//   * the preview dropping options_ (which defaults fitToPage to true) -> the
//     fitToPage=false run would come back fitted and the ratio collapses to 1.
// ---------------------------------------------------------------------------
TEST_F(PrintPreviewRenderTest, PreviewHonoursFitToPage)
{
    ASSERT_NE(qApp, nullptr);

    const int kResolution = 150;

    PrintOptions fitted;
    fitted.range        = PrintRange::PageRange;
    fitted.fromPage     = 3;
    fitted.toPage       = 3;
    fitted.fitToPage    = true;
    fitted.centerOnPage = true;
    fitted.autoRotate   = false;

    PrintOptions unfitted = fitted;
    unfitted.fitToPage = false;

    const QString fitPdf   = tmp("preview_fit.pdf");
    const QString nofitPdf = tmp("preview_nofit.pdf");
    ASSERT_TRUE(renderViaPreview(fitted,   fitPdf,   kResolution, QPageLayout::Portrait));
    ASSERT_TRUE(renderViaPreview(unfitted, nofitPdf, kResolution, QPageLayout::Portrait));

    const std::vector<QImage> fitPages   = rasterize(fitPdf);
    const std::vector<QImage> nofitPages = rasterize(nofitPdf);
    ASSERT_EQ(fitPages.size(),   std::size_t{1});
    ASSERT_EQ(nofitPages.size(), std::size_t{1});

    const QRect fitInk   = inkBoundingBox(fitPages[0]);
    const QRect nofitInk = inkBoundingBox(nofitPages[0]);
    ASSERT_FALSE(fitInk.isNull())   << "fitToPage=true render is blank";
    ASSERT_FALSE(nofitInk.isNull()) << "fitToPage=false render is blank";
    ASSERT_GT(nofitInk.width(), 0);

    // Both output pages are A4, so the two rasters share a size and the ratio
    // is directly comparable.
    ASSERT_EQ(fitPages[0].size(), nofitPages[0].size());

    // ORACLE (corrected in F17): the earlier form asserted a ratio of
    // resolution/72 between the fitted and unfitted renders. That expectation
    // silently assumed the QPrinter honours setResolution() exactly for PDF
    // output; measured, the effective device resolution is NOT the requested
    // 150 (fit/unfit ratio comes out ~2.79, implying ~200), so the constant
    // encoded a property of Qt's PDF backend rather than of PDFE. Asserting a
    // number nobody can derive is how a "green" oracle stops meaning anything.
    //
    // The device-independent statement of "fit to page" is a PROPORTION: the
    // fitted render must occupy the same fraction of its output page that the
    // authored ink occupies of the source page (isotropically, limited by the
    // tighter dimension). That is true at any resolution, and is exactly what
    // fitToPage promises.
    const QImage src = sourcePage(2, 1.0f);
    ASSERT_FALSE(src.isNull());
    const QRect srcInk = inkBoundingBox(src);
    ASSERT_FALSE(srcInk.isNull());
    const double authoredFraction =
        static_cast<double>(srcInk.width()) / static_cast<double>(src.width());

    const double fittedFraction = static_cast<double>(fitInk.width())
                                / static_cast<double>(fitPages[0].width());
    const double unfittedFraction = static_cast<double>(nofitInk.width())
                                  / static_cast<double>(nofitPages[0].width());
    const double measured = static_cast<double>(fitInk.width())
                          / static_cast<double>(nofitInk.width());

    EXPECT_NEAR(fittedFraction, authoredFraction, authoredFraction * 0.04)
        << "fitToPage=true must fill the output page in the same proportion "
           "the authored ink fills the source page (authored "
        << authoredFraction << ", fitted " << fittedFraction
        << "). A fit-to-FILL implementation (max instead of min) overshoots "
           "on the tighter axis; a dropped fit leaves it at "
        << unfittedFraction << ".";
    EXPECT_LT(unfittedFraction, fittedFraction * 0.9)
        << "fitToPage=false must NOT be fitted; it measured "
        << unfittedFraction << " of the page against the fitted "
        << fittedFraction << " (ratio " << measured
        << "). Equal fractions mean the preview ignored the option and fitted "
           "both.";

    // And the fitted content must actually stay on the page.
    EXPECT_GE(fitInk.left(), 0);
    EXPECT_GE(fitInk.top(),  0);
    EXPECT_LE(fitInk.right(),  fitPages[0].width()  - 1);
    EXPECT_LE(fitInk.bottom(), fitPages[0].height() - 1);
}

// ---------------------------------------------------------------------------
// 5. autoRotate is honoured for a landscape target.
//
// Same page, same options, only the printer orientation differs. With
// autoRotate on, a portrait page sent to a landscape target must come back
// ROTATED (PrintEngine.cpp:132-184), i.e. the landscape raster turned back 90
// degrees must resemble the portrait raster far better than the unturned one
// does.
//
// RED / mutations killed:
//   * `bool shouldRotate = false;` / autoRotate ignored -> the unturned
//     comparison wins.
//   * swapped width/height in the target rect -> the rotation decision inverts
//     in BOTH configurations, so the portrait run comes back rotated and the
//     landscape run does not; the inequality flips.
//   * dropping the `painter->rotate(90)` while keeping the position swap ->
//     content unrotated in a landscape box; unturned comparison wins.
// ---------------------------------------------------------------------------
TEST_F(PrintPreviewRenderTest, PreviewAutoRotatesPortraitPageOntoLandscapeTarget)
{
    ASSERT_NE(qApp, nullptr);

    PrintOptions options;
    options.range        = PrintRange::PageRange;
    options.fromPage     = 3;
    options.toPage       = 3;
    options.fitToPage    = true;
    options.centerOnPage = true;
    options.autoRotate   = true;

    const QString portraitPdf  = tmp("preview_portrait.pdf");
    const QString landscapePdf = tmp("preview_landscape.pdf");
    ASSERT_TRUE(renderViaPreview(options, portraitPdf,  72, QPageLayout::Portrait));
    ASSERT_TRUE(renderViaPreview(options, landscapePdf, 72, QPageLayout::Landscape));

    const std::vector<QImage> portrait  = rasterize(portraitPdf);
    const std::vector<QImage> landscape = rasterize(landscapePdf);
    ASSERT_EQ(portrait.size(),  std::size_t{1});
    ASSERT_EQ(landscape.size(), std::size_t{1});
    ASSERT_FALSE(portrait[0].isNull());
    ASSERT_FALSE(landscape[0].isNull());

    // Precondition on the harness, not on PDFE: the landscape printer really
    // did emit a landscape page.
    ASSERT_GT(landscape[0].width(), landscape[0].height())
        << "the landscape printer configuration did not produce a landscape page";
    ASSERT_GT(portrait[0].height(), portrait[0].width());

    const QImage turnedBack =
        landscape[0].transformed(QTransform().rotate(-90), Qt::SmoothTransformation);
    ASSERT_FALSE(turnedBack.isNull());

    const float turnedNcc   = ImageSimilarity::binarizedNCC(turnedBack,   portrait[0]);
    const float unturnedNcc = ImageSimilarity::binarizedNCC(landscape[0], portrait[0]);

    EXPECT_GT(turnedNcc, unturnedNcc + 0.05f)
        << "auto-rotate: the landscape preview turned back 90 degrees should "
           "match the portrait preview (" << turnedNcc << ") much better than "
           "the unturned raster does (" << unturnedNcc << "). It does not — the "
           "preview is not rotating the page onto a landscape target.";
    EXPECT_GE(turnedNcc, 0.70f)
        << "the rotated-back landscape preview does not depict the same page";
}
