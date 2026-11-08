// -*- coding: utf-8 -*-
// Pipeline E phase-7 / Bucket 3e: convert-to-grayscale.
//
// Verifies GrayscaleConverter — uses MuPDF's pdf_recolor_page to
// recolor every page to DeviceGray. Tests cover an image-bearing
// fixture, a text-only fixture, and a save+reopen round-trip with
// pixel-level R==G==B verification on the rendered output.
#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "image/ImageData.h"
#include "image/ImageManager.h"
#include "optimize/GrayscaleConverter.h"
#include "optimize/GrayscaleCommand.h"
#include "core/UndoManager.h"

#include <QImage>
#include <QRectF>
#include <QTemporaryDir>

#include <filesystem>
#include <memory>
#include <string>

using namespace pdfe::core;
using namespace pdfe::optimize;

namespace {

// QImage::Format_RGBA8888 (what Page::render produces) places pixels
// in byte order R,G,B,A. Convert to ARGB32 so qRed/qGreen/qBlue read
// channels correctly.
QImage toArgb32(const QImage& in)
{
    return in.format() == QImage::Format_ARGB32
        ? in : in.convertToFormat(QImage::Format_ARGB32);
}

// Returns true iff every sampled pixel satisfies R == G == B
// (within `tolerance`). Tolerance accommodates the JPEG re-encoding
// noise that pdf_recolor_page introduces when it rewrites color
// JPEGs as grayscale JPEGs.
bool allPixelsAreGray(const QImage& imgIn, int tolerance = 4)
{
    QImage img = toArgb32(imgIn);
    for (int y = 0; y < img.height(); y += 8) {
        const QRgb* row = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = 0; x < img.width(); x += 8) {
            QRgb p = row[x];
            const int r = qRed(p), g = qGreen(p), b = qBlue(p);
            if (std::abs(r - g) > tolerance ||
                std::abs(g - b) > tolerance ||
                std::abs(r - b) > tolerance) {
                return false;
            }
        }
    }
    return true;
}

// Fraction of sampled pixels that are LIGHT (perceived gray > 200). A white /
// light-paper background and the legitimate grayscale of light content read as
// light; the #345 blackening collapses this toward 0.
double lightFraction(const QImage& imgIn)
{
    QImage img = toArgb32(imgIn);
    long long total = 0;
    long long light = 0;
    for (int y = 0; y < img.height(); y += 4) {
        const QRgb* row = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = 0; x < img.width(); x += 4) {
            ++total;
            // The page render has a TRANSPARENT background (alpha 0, rgb 0), not
            // white paper — so composite each pixel over white before judging
            // "light", otherwise blank paper reads as black. Content drawn opaque
            // (black text) stays dark; this is what makes the blacken oracle work.
            const QRgb p = row[x];
            const int a = qAlpha(p);
            const int r = (qRed(p)   * a + 255 * (255 - a)) / 255;
            const int g = (qGreen(p) * a + 255 * (255 - a)) / 255;
            const int b = (qBlue(p)  * a + 255 * (255 - a)) / 255;
            if (qGray(r, g, b) > 200) ++light;
        }
    }
    return (total > 0) ? static_cast<double>(light) / static_cast<double>(total)
                       : 0.0;
}

// Returns true iff at least one sampled, non-transparent pixel is CHROMATIC
// (its R/G/B channels differ beyond `tolerance`). The inverse of
// allPixelsAreGray — used to prove undo restored real colour, not just that
// undo ran.
bool hasColorPixel(const QImage& imgIn, int tolerance = 24)
{
    QImage img = toArgb32(imgIn);
    for (int y = 0; y < img.height(); y += 4) {
        const QRgb* row = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = 0; x < img.width(); x += 4) {
            const QRgb p = row[x];
            if (qAlpha(p) == 0) continue;  // transparent paper carries no colour
            const int r = qRed(p), g = qGreen(p), b = qBlue(p);
            if (std::abs(r - g) > tolerance ||
                std::abs(g - b) > tolerance ||
                std::abs(r - b) > tolerance) {
                return true;
            }
        }
    }
    return false;
}

} // namespace

class GrayscaleConverterTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx = std::make_shared<MuPdfContext>();
        ASSERT_TRUE(workDir.isValid());
    }

    std::unique_ptr<Document> openFixture(const std::string& name) {
        auto fixtures = std::filesystem::path(TEST_FIXTURES_DIR);
        auto src = fixtures / name;
        if (!std::filesystem::exists(src)) return nullptr;
        return Document::open(ctx, src);
    }

    std::shared_ptr<MuPdfContext> ctx;
    QTemporaryDir workDir;
};

TEST_F(GrayscaleConverterTest, ConvertImageBearingPageProducesGrayPixels)
{
    auto doc = openFixture("test_pdf_with_images.pdf");
    if (!doc) GTEST_SKIP() << "test_pdf_with_images.pdf not available";

    // Per-page convert (covers the public single-page API).
    ASSERT_TRUE(GrayscaleConverter::convertPageToGrayscale(doc.get(), 0));

    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);
    QImage rendered = page->render(QRectF(), 1.0f);
    ASSERT_FALSE(rendered.isNull());
    EXPECT_TRUE(allPixelsAreGray(rendered))
        << "After grayscale conversion, every sampled pixel must have "
           "R==G==B (within tolerance for JPEG re-encoding noise).";
}

TEST_F(GrayscaleConverterTest, ConvertTextOnlyPageDoesNotCrashAndKeepsText)
{
    auto doc = openFixture("test_single.pdf");
    if (!doc) GTEST_SKIP() << "test_single.pdf not available";

    auto result = GrayscaleConverter::convertDocumentToGrayscale(doc.get());
    EXPECT_TRUE(result.ok)
        << "All pages should convert: " << result.errorMessage;
    EXPECT_GE(result.pagesConverted, 1);
    EXPECT_EQ(result.pagesFailed, 0);

    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);
    QImage rendered = page->render(QRectF(), 1.0f);
    ASSERT_FALSE(rendered.isNull());
    EXPECT_TRUE(allPixelsAreGray(rendered));
}

TEST_F(GrayscaleConverterTest, SaveAndReopenPreservesGrayscaleConversion)
{
    auto doc = openFixture("test_pdf_with_images.pdf");
    if (!doc) GTEST_SKIP() << "test_pdf_with_images.pdf not available";

    auto result = GrayscaleConverter::convertDocumentToGrayscale(doc.get());
    ASSERT_TRUE(result.ok);

    std::string outPath = (std::filesystem::path(
        workDir.path().toStdString()) / "grayscale.pdf").string();
    ASSERT_NO_THROW(doc->saveAs(outPath));
    doc.reset();

    auto reopened = Document::open(ctx, outPath);
    ASSERT_NE(reopened, nullptr);
    auto page = reopened->page(0);
    ASSERT_NE(page, nullptr);
    QImage rendered = page->render(QRectF(), 1.0f);
    ASSERT_FALSE(rendered.isNull());
    EXPECT_TRUE(allPixelsAreGray(rendered))
        << "Grayscale conversion must survive save+reopen.";
}

TEST_F(GrayscaleConverterTest, MultiPageDocumentConvertsAllPages)
{
    auto doc = openFixture("test_3page.pdf");
    if (!doc) GTEST_SKIP() << "test_3page.pdf not available";
    const int pageCount = doc->pageCount();
    ASSERT_GE(pageCount, 2);

    auto result = GrayscaleConverter::convertDocumentToGrayscale(doc.get());
    EXPECT_TRUE(result.ok);
    EXPECT_EQ(result.pagesConverted, pageCount);
}

// GitHub #345 — converting to grayscale AFTER inserting an image must not
// blacken the ORIGINAL (non-image) page content.
//
// Repro: a page with its own light-paper content gains an inserted image that
// carries alpha (→ a PDF /SMask) via the real ImageManager::insertImage path —
// the same path the Insert Image UI uses, and the condition the investigation
// identifies (image-dependent SMask/colorspace, e.g. ChromeSquirrel.png, which
// is not in the fixtures tree, so the triggering image is synthesized here with
// a transparency gradient). The blind pdf_recolor_page then corrupted the whole
// page to solid black.
//
// Oracle is a CONTENT-PIXEL luminance check, not "the function ran": the page
// must retain most of its light area after conversion. Pre-fix, a reproduced
// blackening collapses the light fraction toward zero; the fix's render-verify-
// and-rollback restores the untouched content, keeping it light.
TEST_F(GrayscaleConverterTest, ConvertAfterImageInsertDoesNotBlackenContent)
{
    auto doc = openFixture("test_single.pdf");
    if (!doc) GTEST_SKIP() << "test_single.pdf not available";

    // Synthesize an RGBA image WITH transparency so pdf_add_image emits an
    // /SMask — the image-dependent trigger the investigation names.
    const int iw = 96, ih = 96;
    QImage rgba(iw, ih, QImage::Format_ARGB32);
    for (int y = 0; y < ih; ++y) {
        QRgb* row = reinterpret_cast<QRgb*>(rgba.scanLine(y));
        for (int x = 0; x < iw; ++x) {
            int a = (x * 255) / (iw - 1);          // left transparent → right opaque
            row[x] = qRgba(200, 40, 40, a);        // saturated red under the alpha
        }
    }
    const std::string imgPath =
        (std::filesystem::path(workDir.path().toStdString()) / "smask_rgba.png")
            .string();
    ASSERT_TRUE(rgba.save(QString::fromStdString(imgPath), "PNG"));

    // Establish that page 0 starts out mostly light (white paper).
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);
    const double lightBefore = lightFraction(page->render(QRectF(), 1.0f));
    ASSERT_GT(lightBefore, 0.30)
        << "precondition: the fixture page must have substantial light area for "
           "the blackening oracle to be meaningful";

    // Insert the transparent image through the REAL insert path.
    pdfe::image::ImageManager imgMgr(ctx);
    pdfe::image::InsertImageOptions opts;
    opts.filePath = imgPath;
    opts.targetRect = QRectF(72, 72, 180, 180);
    opts.maintainAspectRatio = true;
    auto ins = imgMgr.insertImage(page.get(), doc.get(), opts);
    ASSERT_TRUE(ins.success) << "image insert failed: " << ins.errorMessage;

    // Convert to grayscale. The return value is intentionally NOT asserted:
    // when the safety net detects that the blind recolor would blacken the
    // content it rolls the page back and reports false (grayscale did not
    // safely apply). The safety-critical invariant is the CONTENT, below.
    (void)GrayscaleConverter::convertPageToGrayscale(doc.get(), 0);

    // The original, non-image content must NOT have gone black.
    auto pageAfter = doc->page(0);
    ASSERT_NE(pageAfter, nullptr);
    const double lightAfter = lightFraction(pageAfter->render(QRectF(), 1.0f));
    EXPECT_GE(lightAfter, lightBefore * 0.5)
        << "GitHub #345: after grayscale conversion of a page with an inserted "
           "image, the original content collapsed toward black (lightBefore="
        << lightBefore << ", lightAfter=" << lightAfter << ")";
}

// GitHub #355 — Convert to Grayscale must be undoable, and undo must restore
// the original COLOUR content (not merely pop an undo entry). The forward path
// used to call GrayscaleConverter directly with no UndoManager::execute, so
// there was no way back.
TEST_F(GrayscaleConverterTest, GrayscaleCommandUndoRestoresColor)
{
    auto doc = openFixture("test_pdf_with_images.pdf");
    if (!doc) GTEST_SKIP() << "test_pdf_with_images.pdf not available";

    // Precondition: page 0 starts with real colour, so the undo oracle below is
    // meaningful (a page that was already gray could pass vacuously).
    {
        auto page = doc->page(0);
        ASSERT_NE(page, nullptr);
        ASSERT_TRUE(hasColorPixel(page->render(QRectF(), 1.0f)))
            << "fixture page 0 must be colour for this oracle to mean anything";
    }

    pdfe::commands::UndoManager mgr;
    auto cmd = std::make_unique<pdfe::optimize::GrayscaleCommand>(doc.get());
    auto* cmdPtr = cmd.get();
    mgr.execute(std::move(cmd));
    EXPECT_TRUE(cmdPtr->ok()) << cmdPtr->errorMessage();

    // #355: the conversion must be an undoable command at all.
    ASSERT_TRUE(mgr.canUndo())
        << "Convert to Grayscale must be routed through the UndoManager";

    // After convert → every pixel is gray.
    {
        auto page = doc->page(0);
        ASSERT_NE(page, nullptr);
        EXPECT_TRUE(allPixelsAreGray(page->render(QRectF(), 1.0f)))
            << "after convert every sampled pixel must be R==G==B";
    }

    // Undo → the original colour content is restored (pixel oracle).
    mgr.undo();
    {
        auto page = doc->page(0);
        ASSERT_NE(page, nullptr);
        EXPECT_TRUE(hasColorPixel(page->render(QRectF(), 1.0f)))
            << "GitHub #355: undo must restore the original colour content, not "
               "leave the page grayscale";
    }
}

TEST_F(GrayscaleConverterTest, NullDocumentReturnsErrorWithoutCrash)
{
    auto result = GrayscaleConverter::convertDocumentToGrayscale(nullptr);
    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.errorMessage.empty());
    EXPECT_EQ(result.pagesConverted, 0);
}
