// -*- coding: utf-8 -*-
// Phase 4e: WatermarkExtConfig — page-range filtering and image source.
// Tests go through BatchEngine::applyWatermarkExt; the legacy
// applyWatermark (regression-guarded) continues to use WatermarkConfig
// as before.
#include <gtest/gtest.h>
#include "batch/BatchEngine.h"
#include "batch/BatchTypes.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"

#include <QImage>
#include <QPainter>
#include <QRectF>

#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace pdfe::batch;
using namespace pdfe::core;

namespace {

std::filesystem::path fixturesDir()
{
    return std::filesystem::path(TEST_FIXTURES_DIR);
}

std::filesystem::path tempOut(const char* name)
{
    auto dir = std::filesystem::temp_directory_path() / "pdfe_wm_tests";
    std::filesystem::create_directories(dir);
    return dir / name;
}

bool pageContains(Document* doc, int pageIdx, const std::string& needle)
{
    auto p = doc->page(pageIdx);
    if (!p) return false;
    return p->extractPlainText().find(needle) != std::string::npos;
}

// ─────────────────────────────────────────────────────────────────────
// A1-57 / A1-32 render oracles.
//
// Every watermark test above reads EXTRACTED TEXT. Text extraction sees
// the `(TEXT) Tj` operator regardless of whether a single pixel of ink
// reaches the page — a wrong CTM, a zero font size, an off-page text
// matrix, a fully-transparent ExtGState or a clipped-away marked-content
// section all extract identically and all ship an invisible watermark.
// (That exact class of bug is documented in BatchEngine.cpp: the Phase-4
// implementation emitted the rotation angle as a text RENDERING MODE, so
// the watermark extracted fine and rendered nothing.)
//
// The helpers below turn the suite into a REALIZATION oracle: rasterize
// the page before and after the watermark run and compare pixels.
// ─────────────────────────────────────────────────────────────────────

// Rasterize a page and composite it over opaque white.
//
// Page::render() rasterizes through fz_new_pixmap_from_page(..., alpha=1),
// and MuPDF clears an alpha pixmap to TRANSPARENT BLACK before compositing
// the page onto it. Reading the raw RGB of that buffer would report the
// untouched background as pure black and make every "is there ink here"
// measurement meaningless, so flatten onto white first.
QImage renderPageOverWhite(Document* doc, int pageIdx, float scale)
{
    if (!doc) return QImage();
    auto p = doc->page(pageIdx);
    if (!p) return QImage();
    QImage raw = p->render(p->boundingBox(), scale);
    if (raw.isNull()) return raw;
    QImage out(raw.size(), QImage::Format_RGB32);
    out.fill(Qt::white);
    QPainter painter(&out);
    painter.drawImage(0, 0, raw);
    painter.end();
    return out;
}

// Fraction of pixels that carry ink (below a mid-grey threshold). A blank
// white raster returns ~0.0.
double darkFraction(const QImage& img)
{
    if (img.isNull()) return 0.0;
    const int w = img.width(), h = img.height();
    if (w == 0 || h == 0) return 0.0;
    long dark = 0;
    for (int y = 0; y < h; ++y) {
        const QRgb* row = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = 0; x < w; ++x) {
            const QRgb c = row[x];
            const int lum = (qRed(c) * 299 + qGreen(c) * 587 + qBlue(c) * 114) / 1000;
            if (lum < 160) ++dark;
        }
    }
    return static_cast<double>(dark) / (static_cast<double>(w) * h);
}

struct PixelDiff {
    double changedFraction = 0.0;  // 0..1 of the page area
    double centroidX = -1.0;       // 0..1 across the page, -1 when no change
    double centroidY = -1.0;
    bool valid = false;
};

// Per-pixel comparison of two same-size rasters. `changedFraction` counts
// pixels whose luminance moved by more than `tolerance` (8/255 absorbs
// nothing more than rounding — the two rasters come from the same renderer
// on the same content, so an untouched page diffs to exactly zero).
PixelDiff diffPixels(const QImage& before, const QImage& after, int tolerance = 8)
{
    PixelDiff d;
    if (before.isNull() || after.isNull()) return d;
    if (before.size() != after.size()) return d;
    const int w = before.width(), h = before.height();
    if (w == 0 || h == 0) return d;
    long changed = 0;
    double sx = 0.0, sy = 0.0;
    for (int y = 0; y < h; ++y) {
        const QRgb* rb = reinterpret_cast<const QRgb*>(before.constScanLine(y));
        const QRgb* ra = reinterpret_cast<const QRgb*>(after.constScanLine(y));
        for (int x = 0; x < w; ++x) {
            const int lb = (qRed(rb[x]) * 299 + qGreen(rb[x]) * 587 + qBlue(rb[x]) * 114) / 1000;
            const int la = (qRed(ra[x]) * 299 + qGreen(ra[x]) * 587 + qBlue(ra[x]) * 114) / 1000;
            if (std::abs(la - lb) > tolerance) {
                ++changed;
                sx += x;
                sy += y;
            }
        }
    }
    d.valid = true;
    d.changedFraction = static_cast<double>(changed) / (static_cast<double>(w) * h);
    if (changed > 0) {
        d.centroidX = (sx / changed) / w;
        d.centroidY = (sy / changed) / h;
    }
    return d;
}

// A watermark that actually reaches the raster covers far more than this
// (the 48pt diagonal string measures in the thousands of pixels on an A4
// page); an untouched page diffs to exactly 0.
constexpr double kInkedMin = 0.0015;   // >= 0.15% of the page changed
constexpr double kUntouchedMax = 1e-5; // effectively "not one glyph"

} // namespace

class WatermarkExtendedTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx = std::make_shared<MuPdfContext>();
        src = fixturesDir() / "test_3page.pdf";
        imageFixture = fixturesDir() / "test_image_rgb.png";
        if (!std::filesystem::exists(src)) {
            GTEST_SKIP() << "test_3page.pdf not available";
        }
    }
    std::shared_ptr<MuPdfContext> ctx;
    std::filesystem::path src;
    std::filesystem::path imageFixture;
};

// -------- Regression: legacy applyWatermark behaves like Phase 4 --------

TEST_F(WatermarkExtendedTest, LegacyApplyWatermarkAppliesToAllPages)
{
    WatermarkConfig cfg;
    cfg.text = "PHASE4E-DRAFT";
    auto out = tempOut("legacy_all.pdf");
    std::filesystem::remove(out);

    BatchEngine eng(ctx);
    ASSERT_NO_THROW(eng.applyWatermark(src, out, cfg));

    auto doc = Document::open(ctx, out);
    ASSERT_NE(doc, nullptr);
    ASSERT_EQ(doc->pageCount(), 3);
    for (int i = 0; i < 3; ++i) {
        EXPECT_TRUE(pageContains(doc.get(), i, "PHASE4E-DRAFT"))
            << "Legacy applyWatermark should stamp every page";
    }
}

// -------- Page-range filtering via applyWatermarkExt --------

TEST_F(WatermarkExtendedTest, OddRangeSkipsEvenPages)
{
    WatermarkConfig base;
    base.text = "ODDMARK";
    WatermarkExtConfig ext;
    ext.rangeMode = "odd";
    auto out = tempOut("odd_range.pdf");
    std::filesystem::remove(out);

    BatchEngine eng(ctx);
    ASSERT_NO_THROW(eng.applyWatermarkExt(src, out, base, ext));

    auto doc = Document::open(ctx, out);
    ASSERT_NE(doc, nullptr);
    EXPECT_TRUE (pageContains(doc.get(), 0, "ODDMARK"));
    EXPECT_FALSE(pageContains(doc.get(), 1, "ODDMARK"));
    EXPECT_TRUE (pageContains(doc.get(), 2, "ODDMARK"));
}

TEST_F(WatermarkExtendedTest, EvenRangeSkipsOddPages)
{
    WatermarkConfig base;
    base.text = "EVENMARK";
    WatermarkExtConfig ext;
    ext.rangeMode = "even";
    auto out = tempOut("even_range.pdf");
    std::filesystem::remove(out);

    BatchEngine eng(ctx);
    ASSERT_NO_THROW(eng.applyWatermarkExt(src, out, base, ext));

    auto doc = Document::open(ctx, out);
    ASSERT_NE(doc, nullptr);
    EXPECT_FALSE(pageContains(doc.get(), 0, "EVENMARK"));
    EXPECT_TRUE (pageContains(doc.get(), 1, "EVENMARK"));
    EXPECT_FALSE(pageContains(doc.get(), 2, "EVENMARK"));
}

TEST_F(WatermarkExtendedTest, ExplicitRangeOnlyTouchesListedPages)
{
    WatermarkConfig base;
    base.text = "EXPLICITMARK";
    WatermarkExtConfig ext;
    ext.rangeMode = "explicit";
    ext.explicitPages = {0, 2};
    auto out = tempOut("explicit_range.pdf");
    std::filesystem::remove(out);

    BatchEngine eng(ctx);
    ASSERT_NO_THROW(eng.applyWatermarkExt(src, out, base, ext));

    auto doc = Document::open(ctx, out);
    ASSERT_NE(doc, nullptr);
    EXPECT_TRUE (pageContains(doc.get(), 0, "EXPLICITMARK"));
    EXPECT_FALSE(pageContains(doc.get(), 1, "EXPLICITMARK"));
    EXPECT_TRUE (pageContains(doc.get(), 2, "EXPLICITMARK"));
}

TEST_F(WatermarkExtendedTest, AllRangeEquivalentToLegacy)
{
    WatermarkConfig base;
    base.text = "ALLMARK";
    WatermarkExtConfig ext;
    ext.rangeMode = "all";
    auto out = tempOut("ext_all.pdf");
    std::filesystem::remove(out);

    BatchEngine eng(ctx);
    ASSERT_NO_THROW(eng.applyWatermarkExt(src, out, base, ext));

    auto doc = Document::open(ctx, out);
    ASSERT_NE(doc, nullptr);
    for (int i = 0; i < 3; ++i) {
        EXPECT_TRUE(pageContains(doc.get(), i, "ALLMARK"));
    }
}

// -------- Image watermark --------

TEST_F(WatermarkExtendedTest, ImageWatermarkOpensCleanly)
{
    if (!std::filesystem::exists(imageFixture)) {
        GTEST_SKIP() << "test_image_rgb.png fixture not available";
    }

    WatermarkConfig base;
    base.opacity = 0.4f;
    WatermarkExtConfig ext;
    ext.mode = "image";
    ext.imagePath = imageFixture.string();
    auto out = tempOut("image_watermark.pdf");
    std::filesystem::remove(out);

    BatchEngine eng(ctx);
    ASSERT_NO_THROW(eng.applyWatermarkExt(src, out, base, ext));
    ASSERT_TRUE(std::filesystem::exists(out));

    auto doc = Document::open(ctx, out);
    ASSERT_NE(doc, nullptr);
    EXPECT_EQ(doc->pageCount(), 3);
    for (int i = 0; i < 3; ++i) {
        EXPECT_NE(doc->page(i), nullptr);
    }
}

TEST_F(WatermarkExtendedTest, ImageWatermarkRespectsPageRange)
{
    if (!std::filesystem::exists(imageFixture)) {
        GTEST_SKIP() << "test_image_rgb.png fixture not available";
    }

    WatermarkConfig base;
    WatermarkExtConfig ext;
    ext.mode = "image";
    ext.imagePath = imageFixture.string();
    ext.rangeMode = "even";
    auto out = tempOut("image_even_range.pdf");
    std::filesystem::remove(out);

    BatchEngine eng(ctx);
    ASSERT_NO_THROW(eng.applyWatermarkExt(src, out, base, ext));

    auto doc = Document::open(ctx, out);
    ASSERT_NE(doc, nullptr);
    EXPECT_EQ(doc->pageCount(), 3);
}

TEST_F(WatermarkExtendedTest, EmptyImagePathWithImageModeFallsBackToText)
{
    // applyWatermarkExt with mode="image" but no imagePath should fall
    // back to text rendering (per the useImage guard).
    WatermarkConfig base;
    base.text = "FALLBACK";
    WatermarkExtConfig ext;
    ext.mode = "image";
    ext.imagePath = "";
    auto out = tempOut("image_fallback.pdf");
    std::filesystem::remove(out);

    BatchEngine eng(ctx);
    ASSERT_NO_THROW(eng.applyWatermarkExt(src, out, base, ext));

    auto doc = Document::open(ctx, out);
    ASSERT_NE(doc, nullptr);
    EXPECT_TRUE(pageContains(doc.get(), 0, "FALLBACK"));
}

// ═════════════════════════════════════════════════════════════════════
// A1-57 — the watermark is RENDERED, not merely extractable.
// A1-32 — page-range boundaries (first page, last page, out-of-range)
//         are exercised, and asserted at the pixel level.
//
// Every test below rasterizes the SOURCE document and the WATERMARKED
// document at the same scale and compares them page by page. The
// unwatermarked pages must diff to nothing; the watermarked pages must
// gain real ink.
//
// MUTATIONS KILLED (each is a one-line change to
// BatchEngine.cpp:applyWatermarkImpl):
//   * make the whole content-append a no-op (drop the
//     appendContentIsolatingAmbientCtm call)  -> every "ink present"
//     assertion fails: changedFraction == 0.
//   * revert the Tr/Tm bug (emit the rotation as a text rendering mode)
//     -> the watermark still EXTRACTS, so every pre-existing test in this
//     file stays green, but nothing rasterizes -> these fail.
//   * shift the placement off the page (e.g. drop the `cx`/`cy` centring
//     or the MediaBox-origin correction) -> the centroid assertion fails.
//   * off-by-one in the explicit page range (compare `oneBased` instead of
//     `pageIndex`) -> FirstPageOnly inks page 1, LastPageOnly inks nothing,
//     OutOfRange inks page 2. All three fail.
//   * off-by-one in odd/even (drop the +1 that makes the range 1-based)
//     -> OddRange inks page 1 instead of pages 0 and 2.
// ═════════════════════════════════════════════════════════════════════

class WatermarkRenderTest : public WatermarkExtendedTest {
protected:
    void SetUp() override {
        WatermarkExtendedTest::SetUp();
        if (::testing::Test::HasFatalFailure() || ::testing::Test::IsSkipped()) return;
        source = Document::open(ctx, src);
        ASSERT_NE(source, nullptr);
        ASSERT_EQ(source->pageCount(), 3);
    }

    // Opaque so the 0.5-grey watermark lands well below the ink threshold;
    // the default 0.3 alpha would render as light grey and make "is it
    // visible" a judgement call rather than a measurement.
    static WatermarkConfig opaqueConfig(const char* text) {
        WatermarkConfig cfg;
        cfg.text = text;
        cfg.opacity = 1.0f;
        cfg.fontSize = 48.0f;
        return cfg;
    }

    // Diff every page of `watermarked` against the pristine source.
    std::vector<PixelDiff> diffAllPages(const std::filesystem::path& watermarked) {
        std::vector<PixelDiff> out;
        auto doc = Document::open(ctx, watermarked);
        EXPECT_NE(doc, nullptr);
        if (!doc) return out;
        EXPECT_EQ(doc->pageCount(), 3);
        for (int i = 0; i < 3; ++i) {
            QImage before = renderPageOverWhite(source.get(), i, kScale);
            QImage after = renderPageOverWhite(doc.get(), i, kScale);
            EXPECT_FALSE(before.isNull()) << "source page " << i << " did not rasterize";
            EXPECT_FALSE(after.isNull()) << "output page " << i << " did not rasterize";
            out.push_back(diffPixels(before, after));
        }
        return out;
    }

    static constexpr float kScale = 1.0f;
    std::unique_ptr<Document> source;
};

TEST_F(WatermarkRenderTest, LegacyWatermarkPutsRealInkOnEveryPage)
{
    auto cfg = opaqueConfig("RENDERMARK");
    auto out = tempOut("render_legacy_all.pdf");
    std::filesystem::remove(out);

    BatchEngine eng(ctx);
    ASSERT_NO_THROW(eng.applyWatermark(src, out, cfg));

    auto doc = Document::open(ctx, out);
    ASSERT_NE(doc, nullptr);

    for (int i = 0; i < 3; ++i) {
        QImage before = renderPageOverWhite(source.get(), i, kScale);
        QImage after = renderPageOverWhite(doc.get(), i, kScale);
        ASSERT_FALSE(before.isNull());
        ASSERT_FALSE(after.isNull());

        // (1) The raster CHANGED — the watermark is not a no-op.
        auto d = diffPixels(before, after);
        ASSERT_TRUE(d.valid) << "page " << i << ": rasters not comparable";
        EXPECT_GT(d.changedFraction, kInkedMin)
            << "page " << i << ": watermark produced no visible ink "
               "(changed " << d.changedFraction << " of the page). The text "
               "still EXTRACTS, so the text-based tests above cannot see this.";

        // (2) The change is INK, not erasure — dark coverage went up.
        EXPECT_GT(darkFraction(after), darkFraction(before))
            << "page " << i << ": watermark did not darken the page";

        // (3) The ink landed on the page CENTRE, which is where a
        //     "center"-positioned watermark belongs. A watermark drawn at
        //     the MediaBox origin, or offset by an uncorrected origin,
        //     drifts out of this band or off the page entirely.
        ASSERT_GE(d.centroidX, 0.0) << "page " << i << ": no changed pixels";
        EXPECT_GT(d.centroidX, 0.15) << "page " << i << ": ink centroid too far left";
        EXPECT_LT(d.centroidX, 0.85) << "page " << i << ": ink centroid too far right";
        EXPECT_GT(d.centroidY, 0.15) << "page " << i << ": ink centroid too high";
        EXPECT_LT(d.centroidY, 0.85) << "page " << i << ": ink centroid too low";
    }
}

TEST_F(WatermarkRenderTest, ExplicitFirstPageOnlyInksPageZero)
{
    auto base = opaqueConfig("FIRSTONLY");
    WatermarkExtConfig ext;
    ext.rangeMode = "explicit";
    ext.explicitPages = {0};  // FIRST page, 0-based
    auto out = tempOut("render_first_only.pdf");
    std::filesystem::remove(out);

    BatchEngine eng(ctx);
    ASSERT_NO_THROW(eng.applyWatermarkExt(src, out, base, ext));

    auto diffs = diffAllPages(out);
    ASSERT_EQ(diffs.size(), 3u);
    EXPECT_GT(diffs[0].changedFraction, kInkedMin)
        << "first page was in range and must carry watermark ink";
    EXPECT_LT(diffs[1].changedFraction, kUntouchedMax)
        << "page 1 is out of range and must be pixel-identical to the source";
    EXPECT_LT(diffs[2].changedFraction, kUntouchedMax)
        << "page 2 is out of range and must be pixel-identical to the source";
}

TEST_F(WatermarkRenderTest, ExplicitLastPageOnlyInksFinalPage)
{
    auto base = opaqueConfig("LASTONLY");
    WatermarkExtConfig ext;
    ext.rangeMode = "explicit";
    ext.explicitPages = {2};  // LAST page of the 3-page fixture, 0-based
    auto out = tempOut("render_last_only.pdf");
    std::filesystem::remove(out);

    BatchEngine eng(ctx);
    ASSERT_NO_THROW(eng.applyWatermarkExt(src, out, base, ext));

    auto diffs = diffAllPages(out);
    ASSERT_EQ(diffs.size(), 3u);
    EXPECT_LT(diffs[0].changedFraction, kUntouchedMax);
    EXPECT_LT(diffs[1].changedFraction, kUntouchedMax);
    EXPECT_GT(diffs[2].changedFraction, kInkedMin)
        << "last page was in range and must carry watermark ink";
}

TEST_F(WatermarkRenderTest, OutOfRangeExplicitPagesLeaveEveryPageUntouched)
{
    auto base = opaqueConfig("NOWHEREMARK");
    WatermarkExtConfig ext;
    ext.rangeMode = "explicit";
    // One index past the end, one negative, one far beyond: none of these
    // addresses a real page of the 3-page fixture.
    ext.explicitPages = {3, -1, 99};
    auto out = tempOut("render_out_of_range.pdf");
    std::filesystem::remove(out);

    BatchEngine eng(ctx);
    ASSERT_NO_THROW(eng.applyWatermarkExt(src, out, base, ext));

    // The text oracle agrees: nothing was stamped anywhere.
    auto doc = Document::open(ctx, out);
    ASSERT_NE(doc, nullptr);
    for (int i = 0; i < 3; ++i)
        EXPECT_FALSE(pageContains(doc.get(), i, "NOWHEREMARK"));

    auto diffs = diffAllPages(out);
    ASSERT_EQ(diffs.size(), 3u);
    for (int i = 0; i < 3; ++i) {
        EXPECT_LT(diffs[i].changedFraction, kUntouchedMax)
            << "page " << i << " must be untouched: no requested index names it. "
               "An off-by-one that reads the range as 1-based would ink page 2.";
    }
}

TEST_F(WatermarkRenderTest, OddRangeInksOnlyTheOddPagesAtPixelLevel)
{
    auto base = opaqueConfig("ODDRENDER");
    WatermarkExtConfig ext;
    ext.rangeMode = "odd";  // 1-based odd => document pages 0 and 2
    auto out = tempOut("render_odd_range.pdf");
    std::filesystem::remove(out);

    BatchEngine eng(ctx);
    ASSERT_NO_THROW(eng.applyWatermarkExt(src, out, base, ext));

    auto diffs = diffAllPages(out);
    ASSERT_EQ(diffs.size(), 3u);
    EXPECT_GT(diffs[0].changedFraction, kInkedMin);
    EXPECT_LT(diffs[1].changedFraction, kUntouchedMax);
    EXPECT_GT(diffs[2].changedFraction, kInkedMin);
}

TEST_F(WatermarkRenderTest, ImageWatermarkPutsRealInkOnThePage)
{
    if (!std::filesystem::exists(imageFixture)) {
        GTEST_SKIP() << "test_image_rgb.png fixture not available";
    }

    WatermarkConfig base;
    base.opacity = 1.0f;
    WatermarkExtConfig ext;
    ext.mode = "image";
    ext.imagePath = imageFixture.string();
    ext.rangeMode = "explicit";
    ext.explicitPages = {1};  // middle page only
    auto out = tempOut("render_image_mid.pdf");
    std::filesystem::remove(out);

    BatchEngine eng(ctx);
    ASSERT_NO_THROW(eng.applyWatermarkExt(src, out, base, ext));

    auto diffs = diffAllPages(out);
    ASSERT_EQ(diffs.size(), 3u);
    EXPECT_LT(diffs[0].changedFraction, kUntouchedMax);
    EXPECT_GT(diffs[1].changedFraction, kInkedMin)
        << "the image XObject must actually rasterize onto the page; an "
           "unhooked /XObject resource or a dropped `Do` extracts nothing "
           "and previously produced a fully green test";
    EXPECT_LT(diffs[2].changedFraction, kUntouchedMax);

    // The image is placed at 40% of page width, centred — so its ink must
    // sit around the middle of the page, not at the origin.
    EXPECT_GT(diffs[1].centroidX, 0.25);
    EXPECT_LT(diffs[1].centroidX, 0.75);
    EXPECT_GT(diffs[1].centroidY, 0.25);
    EXPECT_LT(diffs[1].centroidY, 0.75);
}
