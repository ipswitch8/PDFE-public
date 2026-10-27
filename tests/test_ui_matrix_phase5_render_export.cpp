// -*- coding: utf-8 -*-
//
// Pipeline p86-followups phase-5 — turn the seven `tool=gtest` UI-matrix
// `planned` cells into tests that genuinely assert an outcome.
//
// Matrix rows covered by this file:
//   render_multi_page            RenderMultiPageTest.EachPageRendersItsOwnInkWhereItsTextLayerSaysItIs
//   render_tile_cache            RenderTileCacheTest.ScrollingNeverReRequestsAnAlreadyCachedTile
//   render_zoom                  RenderZoomTest.DoublingZoomDoublesPixelsAndLeavesPagePointsUnchanged
//   export_image                 ExportImageTest.PngPixelDimensionsTrackThePageSizeAtTheChosenDpi
//   export_extract_pages         ExportExtractPagesTest.ExtractedPdfHoldsExactlyTheSelectedPagesInOrder
//   redaction_panel_widget_audit RedactionPanelMatrixAuditTest.AuditedWidgetsAreReachableAndLive
//   redaction_panel_apply_state  RedactionPanelMatrixAuditTest.ApplyButtonTracksMarkPresence
//
// Oracle discipline (MEMORY: "vacuous_oracle_three_shapes",
// "assertion_sharing_the_defect"):
//
//   * Every assertion compares against an INPUT — the literal text authored
//     into the fixture's content stream, the page size the document reports,
//     the DPI handed to the exporter, the marks handed to setMarks() — or
//     against an INDEPENDENT code path (the stext text layer versus the
//     rasteriser). Never against a second read of the thing under test.
//
//   * Every test first guards that the MEASUREMENT HAPPENED. A rendered image
//     is checked non-null and checked to contain ink BEFORE any property of
//     that ink is asserted, and the ink is checked to cover less than half the
//     page BEFORE that — otherwise a fully-black or fully-transparent buffer
//     would satisfy an ink assertion for the wrong reason (the "zero-vs-zero"
//     and "one-against-one" shapes).
//
// NOTE on the `render_zoom` row. Its `expected` column used to read "page
// re-renders without crash or artifacts". "Without crash" is the
// permanently-false-guard shape this project documents: a render function that
// returned an empty QImage, or returned nothing at all, satisfies it. Phase 4
// hit the identical problem on `font_substitute`. The row's `expected` has been
// rewritten to the property this test actually asserts — see the CSV notes.

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "core/PageInfo.h"
#include "core/PageOperations.h"
#include "core/TextBlock.h"
#include "exports/ImageExporter.h"
#include "redaction/RedactionData.h"
#include "rendering/TileCache.h"
#include "rendering/TileId.h"
#include "rendering/TileRenderer.h"
#include "ui/PdfCanvas.h"
#include "ui/RedactionPanel.h"

#include <QApplication>
#include <QCoreApplication>
#include <QImage>
#include <QLabel>
#include <QPushButton>
#include <QRectF>
#include <QTreeWidget>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#ifndef TEST_FIXTURES_DIR
#define TEST_FIXTURES_DIR "."
#endif

using pdfe::core::Document;
using pdfe::core::MuPdfContext;
using pdfe::core::PageInfo;
using pdfe::core::TextBlock;
using pdfe::exports::ImageExporter;
using pdfe::exports::ImageExportOptions;
using pdfe::exports::ImageFormat;
using pdfe::redaction::RedactionData;
using pdfe::ui::PdfCanvas;
using pdfe::ui::RedactionPanel;
using namespace pdfe::rendering;

namespace {

std::filesystem::path fixturesDir()
{
    return std::filesystem::path(TEST_FIXTURES_DIR);
}

std::filesystem::path freshOutDir(const char* suffix)
{
    auto d = std::filesystem::temp_directory_path()
           / "pdfe_matrix_phase5" / suffix;
    std::error_code ec;
    std::filesystem::remove_all(d, ec);
    std::filesystem::create_directories(d, ec);
    return d;
}

// ---------------------------------------------------------------------------
// Ink measurement
// ---------------------------------------------------------------------------
//
// "Ink" is a pixel that is neither fully transparent nor near-white. Both
// conventions have to be handled because they both occur in this codebase:
// Page::render() goes through fz_new_pixmap_from_page(..., alpha=1), which
// clears to TRANSPARENT, while the tile path clears to opaque WHITE
// (fz_clear_pixmap_with_value(ctx, pixmap, 255)). A test that only knew one
// convention would report "the whole page is ink" on the other and pass its
// ink assertions for entirely the wrong reason.

struct Ink {
    bool any = false;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;   // inclusive, device pixels
    long long count = 0;
    long long total = 0;

    [[nodiscard]] int widthPx() const { return any ? (x1 - x0 + 1) : 0; }
    [[nodiscard]] int heightPx() const { return any ? (y1 - y0 + 1) : 0; }
    /// Ink bounding box expressed in PAGE POINTS, given the render scale.
    /// This is the scale-invariant quantity: the same page rendered at any
    /// zoom must place its ink at the same page coordinates.
    [[nodiscard]] QRectF boxPt(double scale) const
    {
        return QRectF(x0 / scale, y0 / scale,
                      widthPx() / scale, heightPx() / scale);
    }
};

Ink measureInk(const QImage& src)
{
    Ink ink;
    if (src.isNull()) {
        return ink;
    }
    const QImage img = src.convertToFormat(QImage::Format_ARGB32);
    ink.total = static_cast<long long>(img.width()) * img.height();
    for (int y = 0; y < img.height(); ++y) {
        const auto* line = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = 0; x < img.width(); ++x) {
            const QRgb p = line[x];
            if (qAlpha(p) == 0) {
                continue;   // transparent background (Page::render convention)
            }
            if (qRed(p) >= 200 && qGreen(p) >= 200 && qBlue(p) >= 200) {
                continue;   // white-ish background (tile convention)
            }
            if (!ink.any) {
                ink.any = true;
                ink.x0 = ink.x1 = x;
                ink.y0 = ink.y1 = y;
            } else {
                ink.x0 = std::min(ink.x0, x);
                ink.x1 = std::max(ink.x1, x);
                ink.y0 = std::min(ink.y0, y);
                ink.y1 = std::max(ink.y1, y);
            }
            ++ink.count;
        }
    }
    return ink;
}

/// Guard that a rendered image really was MEASURED before anything is asserted
/// about its ink. Fails (rather than silently reporting an empty Ink) on a null
/// image, on a blank image, and on a saturated image — the three ways the
/// measurement can be meaningless while an ink assertion still "passes".
::testing::AssertionResult inkIsMeasurable(const QImage& img, const Ink& ink,
                                           const char* what)
{
    if (img.isNull()) {
        return ::testing::AssertionFailure() << what << ": image is null";
    }
    if (img.width() <= 0 || img.height() <= 0) {
        return ::testing::AssertionFailure()
               << what << ": image has no pixels (" << img.width() << "x"
               << img.height() << ")";
    }
    if (!ink.any || ink.count == 0) {
        return ::testing::AssertionFailure()
               << what << ": image is blank — no non-background pixel in "
               << img.width() << "x" << img.height();
    }
    if (ink.count * 2 >= ink.total) {
        return ::testing::AssertionFailure()
               << what << ": " << ink.count << " of " << ink.total
               << " pixels counted as ink — that is a saturated buffer, not a "
                  "rendered page, and every ink assertion below would pass for "
                  "the wrong reason";
    }
    return ::testing::AssertionSuccess();
}

/// Union of the text layer's block bounding boxes. This comes out of
/// fz_stext_page — a code path entirely independent of the rasteriser — and is
/// in the same top-down device space as Page::render() at scale 1.0, so the two
/// are directly comparable.
QRectF textLayerBox(pdfe::core::Page* page, int* blockCount)
{
    QRectF box;
    const auto blocks = page->extractText();
    if (blockCount) {
        *blockCount = static_cast<int>(blocks.size());
    }
    for (const auto& b : blocks) {
        if (b.boundingBox.isEmpty()) {
            continue;
        }
        box = box.isNull() ? b.boundingBox : box.united(b.boundingBox);
    }
    return box;
}

struct DocFixture {
    std::shared_ptr<MuPdfContext> ctx;
    std::unique_ptr<Document> doc;
};

bool openFixture(DocFixture& f, const char* name)
{
    const auto p = fixturesDir() / name;
    if (!std::filesystem::exists(p)) {
        return false;
    }
    f.ctx = std::make_shared<MuPdfContext>();
    f.doc = Document::open(f.ctx, p);
    return f.doc != nullptr && f.doc->pageCount() > 0;
}

} // namespace

// ===========================================================================
// render_zoom
// ===========================================================================
//
// Row `render_zoom` (fixture test_single.pdf).
//
// Asserted property: a 2x render is the SAME PAGE at twice the linear size.
//   (a) pixel dimensions double (a renderer that ignores `scale` fails here);
//   (b) the ink bounding box expressed in PAGE POINTS is unchanged, i.e. the
//       zoom magnified the content rather than moving, cropping or re-laying
//       it out — this is what "no artifacts" can actually be made to mean;
//   (c) the ink pixel count grows roughly as the square of the linear scale.
//
// The page size used for (a) comes from Document::pageInfo(), which reads
// /MediaBox — an input to the renderer, not another render.

TEST(RenderZoomTest, DoublingZoomDoublesPixelsAndLeavesPagePointsUnchanged)
{
    DocFixture f;
    if (!openFixture(f, "test_single.pdf")) {
        GTEST_SKIP() << "test_single.pdf fixture unavailable";
    }

    const PageInfo info = f.doc->pageInfo(0);
    ASSERT_GT(info.widthPt, 1.0f) << "fixture reports a degenerate page width";
    ASSERT_GT(info.heightPt, 1.0f) << "fixture reports a degenerate page height";

    auto page = f.doc->page(0);
    ASSERT_NE(page, nullptr);

    const QImage at1x = page->render(QRectF(), 1.0f);
    const QImage at2x = page->render(QRectF(), 2.0f);

    const Ink ink1 = measureInk(at1x);
    const Ink ink2 = measureInk(at2x);
    ASSERT_TRUE(inkIsMeasurable(at1x, ink1, "100% render"));
    ASSERT_TRUE(inkIsMeasurable(at2x, ink2, "200% render"));

    // (a) Pixel dimensions follow the page size at the requested scale.
    EXPECT_NEAR(at1x.width(), info.widthPt, 1.5)
        << "100% render is not the page's own width";
    EXPECT_NEAR(at1x.height(), info.heightPt, 1.5)
        << "100% render is not the page's own height";
    EXPECT_NEAR(at2x.width(), 2.0 * info.widthPt, 2.5)
        << "200% render did not double the width";
    EXPECT_NEAR(at2x.height(), 2.0 * info.heightPt, 2.5)
        << "200% render did not double the height";

    // (b) Same content, same place on the page.
    const QRectF box1 = ink1.boxPt(1.0);
    const QRectF box2 = ink2.boxPt(2.0);
    EXPECT_NEAR(box1.left(),   box2.left(),   2.0);
    EXPECT_NEAR(box1.top(),    box2.top(),    2.0);
    EXPECT_NEAR(box1.width(),  box2.width(),  2.0);
    EXPECT_NEAR(box1.height(), box2.height(), 2.0);

    // (c) Linear doubling => area roughly quadruples. The band is wide because
    // antialiasing of thin glyph stems is not exactly scale-linear; it is still
    // nowhere near 1.0, which is what a zoom-ignoring renderer produces.
    const double areaRatio = static_cast<double>(ink2.count)
                           / static_cast<double>(ink1.count);
    EXPECT_GE(areaRatio, 2.5) << "ink area ratio " << areaRatio
                              << " — 200% barely produced more ink than 100%";
    EXPECT_LE(areaRatio, 6.0) << "ink area ratio " << areaRatio
                              << " — implausibly more ink than a 2x scale";

    // The ink's own pixel extent doubles too.
    EXPECT_NEAR(static_cast<double>(ink2.widthPx()),
                2.0 * ink1.widthPx(), 0.12 * 2.0 * ink1.widthPx());
    EXPECT_NEAR(static_cast<double>(ink2.heightPx()),
                2.0 * ink1.heightPx(), 0.12 * 2.0 * ink1.heightPx());
}

// ===========================================================================
// render_multi_page
// ===========================================================================
//
// Row `render_multi_page` (fixture test_3page.pdf).
//
// "Each page renders correctly" is made assertable by cross-checking the
// RASTERISER against the TEXT LAYER, which is a different code path
// (fz_stext_page) over the same page:
//
//   * every page renders non-blank ink;
//   * each page's ink bounding box CONTAINS that page's own text-layer
//     bounding box — the glyphs the text layer says are at those coordinates
//     really got painted there;
//   * the three renders are pairwise different, and page p's ink box matches
//     page p's text box rather than some other page's. A renderer that served
//     page 0's raster for every index fails this: the fixture's three pages
//     have visibly different extents (authored content streams: page 1 is a
//     three-line title page, page 2 is full-width Lorem ipsum paragraphs,
//     page 3 is a short list plus a stroked rectangle).
//   * page 3's INK extends well below its TEXT box, because of the `re S`
//     stroked rectangle that carries no text — a vector mark that only the
//     rasteriser can see.

TEST(RenderMultiPageTest, EachPageRendersItsOwnInkWhereItsTextLayerSaysItIs)
{
    DocFixture f;
    if (!openFixture(f, "test_3page.pdf")) {
        GTEST_SKIP() << "test_3page.pdf fixture unavailable";
    }
    // Sidecar test_3page.expected.json authors pageCount: 3.
    ASSERT_EQ(f.doc->pageCount(), 3);

    std::vector<QImage> images;
    std::vector<Ink> inks;
    std::vector<QRectF> textBoxes;

    for (int p = 0; p < 3; ++p) {
        auto page = f.doc->page(p);
        ASSERT_NE(page, nullptr) << "page " << p;

        const QImage img = page->render(QRectF(), 1.0f);
        const Ink ink = measureInk(img);
        ASSERT_TRUE(inkIsMeasurable(img, ink,
                                    ("render of page " + std::to_string(p)).c_str()));

        int blocks = 0;
        const QRectF tbox = textLayerBox(page.get(), &blocks);
        ASSERT_GT(blocks, 0) << "page " << p
                             << ": text layer is empty, so it cannot be used as "
                                "an independent oracle for this page's raster";
        ASSERT_FALSE(tbox.isNull()) << "page " << p << ": text layer has no extent";

        images.push_back(img);
        inks.push_back(ink);
        textBoxes.push_back(tbox);
    }

    // The horizontal extent is the clean comparison: a glyph's painted ink and
    // its stext bbox agree on left/right to within antialias slack. The TOP
    // edge deliberately is not compared tightly — a stext line bbox spans the
    // font's full ascent, which for these fixtures sits 5-8 pt above the
    // highest pixel any glyph actually paints, so "ink covers the text box" is
    // false at the top for CORRECT output. Measured on this fixture:
    //   page 0  ink 193,46,404,134   text 191.60,38.21,403.69,134.75
    //   page 1  ink  31,41,564,137   text  31.18,35.75,564.10,136.98
    //   page 2  ink  32,41,426,454   text  31.18,35.75,329.93,360.92
    // (page 2's ink runs past its text on the right and the bottom: that is
    // the stroked rectangle, asserted against its authored operator below.)
    for (int p = 0; p < 3; ++p) {
        const QRectF inkBox = inks[p].boxPt(1.0);
        const QRectF want = textBoxes[p];
        std::cout << "[ page " << p << " ] ink " << inkBox.left() << ","
                  << inkBox.top() << "," << inkBox.right() << ","
                  << inkBox.bottom() << "   text " << want.left() << ","
                  << want.top() << "," << want.right() << "," << want.bottom()
                  << std::endl;
        EXPECT_NEAR(inkBox.left(), want.left(), 4.0)
            << "page " << p << ": ink does not start where its own text does";
        EXPECT_GE(inkBox.top(), want.top() - 2.0)
            << "page " << p << ": ink is painted ABOVE its own text layer";
        EXPECT_LE(inkBox.top(), want.top() + 12.0)
            << "page " << p << ": ink starts far below its own text layer";
    }

    // Pages 1 and 2 (indices 0, 1) are text-only, so their ink must end where
    // their text ends on the right. Page 3 is handled separately below: it
    // carries a stroked rectangle that reaches beyond the text on both the
    // right and the bottom.
    for (int p : {0, 1}) {
        const QRectF inkBox = inks[p].boxPt(1.0);
        const QRectF want = textBoxes[p];
        EXPECT_NEAR(inkBox.right(), want.right(), 4.0)
            << "page " << p << ": text-only page's ink does not end where its "
                              "text does";
    }

    // The three pages are genuinely distinct rasters.
    EXPECT_NE(images[0], images[1]);
    EXPECT_NE(images[1], images[2]);
    EXPECT_NE(images[0], images[2]);

    // ...and each raster matches ITS OWN page's text layer better than any
    // other page's. This is the assertion that "page p rendered page p": a
    // renderer that served page 0's raster for every index satisfies every
    // check above for p == 0 and fails here for p == 1 and p == 2.
    auto boxError = [](const QRectF& a, const QRectF& b) {
        return std::abs(a.left() - b.left()) + std::abs(a.right() - b.right())
             + std::abs(a.top() - b.top()) + std::abs(a.bottom() - b.bottom());
    };
    for (int p = 0; p < 3; ++p) {
        const QRectF inkBox = inks[p].boxPt(1.0);
        const double own = boxError(inkBox, textBoxes[p]);
        for (int q = 0; q < 3; ++q) {
            if (q == p) {
                continue;
            }
            const double other = boxError(inkBox, textBoxes[q]);
            EXPECT_LT(own, other)
                << "page " << p << "'s raster fits page " << q
                << "'s text layer at least as well as its own (" << own
                << " vs " << other << ") — it may not be page " << p
                << " that was rendered";
        }
    }

    // The fixture's pages have distinguishable extents, so the cross-check
    // above is discriminating rather than trivially true. Page 2 (index 1) is
    // the tallest text block of the three.
    EXPECT_GT(textBoxes[1].height(), textBoxes[0].height())
        << "fixture assumption broken: page 2 is no longer taller than page 1, "
           "so the per-page cross-check above stops discriminating";

    // Page 3 carries a stroked rectangle that has no text at all. Only the
    // rasteriser can see it, so page 3's ink must reach beyond its text layer
    // on the right and the bottom — and it must reach EXACTLY as far as the
    // authored operator says:
    //
    //     141.73 558.43 283.46 -170.08 re S
    //
    // right edge  = 141.73 + 283.46      = 425.19 in PDF user space
    // bottom edge = 558.43 - 170.08      = 388.35 in PDF user space
    //
    // Both converted to the top-down device space the raster is measured in.
    // These are literals from the fixture's content stream, i.e. an INPUT to
    // the renderer, not a second reading of its output.
    {
        const QRectF ink2 = inks[2].boxPt(1.0);
        const double pageHeightPt = f.doc->pageInfo(2).heightPt;
        const double rectRightPt = 141.73 + 283.46;
        const double rectBottomDeviceY = pageHeightPt - (558.43 - 170.08);

        EXPECT_GT(ink2.right(), textBoxes[2].right() + 50.0)
            << "page 3's stroked rectangle did not reach the raster (right)";
        EXPECT_GT(ink2.bottom(), textBoxes[2].bottom() + 50.0)
            << "page 3's stroked rectangle did not reach the raster (bottom)";
        EXPECT_NEAR(ink2.right(), rectRightPt, 3.0)
            << "page 3's rightmost ink is not the authored rectangle's right edge";
        EXPECT_NEAR(ink2.bottom(), rectBottomDeviceY, 3.0)
            << "page 3's lowest ink is not the authored rectangle's bottom edge";
    }
}

// ===========================================================================
// render_tile_cache
// ===========================================================================
//
// Row `render_tile_cache` (fixture test_3page.pdf).
//
// The mechanism under test is PdfCanvas::requestVisibleTiles()'s
// `if (!tileCache_->contains(tileId))` guard — the only thing standing between
// a scroll and a re-render of tiles that are already in hand.
//
// The spy renderer subclasses TileRenderer (declared virtual for exactly this
// purpose) and records every requested TileId, then FULFILS the request
// synchronously by invoking the canvas's own callback. That callback is
// production code: it is what calls TileCache::put(). So the cache is populated
// by the product, not by the test.
//
// Oracle: across a full scroll through the document and back to the top, no
// TileId is EVER requested twice, and the cache holds exactly the tiles that
// were requested. Page 0's tiles are visible at the start and again at the end;
// the second visit must produce no new work.

namespace {

class RecordingTileRenderer : public TileRenderer {
public:
    explicit RecordingTileRenderer(Document* doc) : TileRenderer(doc) {}

    void requestTile(const TileId& id,
                     std::function<void(TileId, QImage)> cb) override
    {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            requested_.push_back(id);
        }
        // Stand in for a completed rasterization: hand the canvas a real,
        // correctly-sized tile through its own completion callback, so the
        // PRODUCTION lambda is what populates the TileCache.
        if (cb) {
            QImage img(TILE_SIZE, TILE_SIZE, QImage::Format_RGBA8888);
            img.fill(Qt::white);
            cb(id, img);
        }
    }

    std::vector<TileId> requested() const
    {
        std::lock_guard<std::mutex> lk(mtx_);
        return requested_;
    }

private:
    mutable std::mutex mtx_;
    std::vector<TileId> requested_;
};

struct TileKey {
    int page, col, row;
    int zoomQ;   // zoom quantised, so float equality is never relied on
    bool operator<(const TileKey& o) const
    {
        return std::tie(page, col, row, zoomQ)
             < std::tie(o.page, o.col, o.row, o.zoomQ);
    }
};

TileKey keyOf(const TileId& id)
{
    return TileKey{id.pageIndex, id.col, id.row,
                   static_cast<int>(std::lround(id.zoom * 10000.0f))};
}

} // namespace

TEST(RenderTileCacheTest, ScrollingNeverReRequestsAnAlreadyCachedTile)
{
    if (!qApp) {
        GTEST_SKIP() << "QApplication not available";
    }
    DocFixture f;
    if (!openFixture(f, "test_3page.pdf")) {
        GTEST_SKIP() << "test_3page.pdf fixture unavailable";
    }
    ASSERT_EQ(f.doc->pageCount(), 3);

    TileCache cache(64ull * 1024 * 1024);
    RecordingTileRenderer renderer(f.doc.get());
    PdfCanvas canvas;
    canvas.resize(800, 600);
    canvas.setDocument(f.doc.get(), &cache, &renderer);
    QCoreApplication::processEvents();   // flush the deferred re-request

    // Scroll through the document and back to the top. goToPage() is the public
    // scroll entry point; it moves scrollOffset_ to the page top and calls
    // requestVisibleTiles(), which is the loop under test.
    std::set<int> scrollPositions{canvas.scrollOffsetY()};
    for (int p : {1, 2, 1, 0}) {
        canvas.goToPage(p);
        QCoreApplication::processEvents();
        scrollPositions.insert(canvas.scrollOffsetY());
    }
    ASSERT_GE(scrollPositions.size(), 3u)
        << "the viewport only ever sat at " << scrollPositions.size()
        << " distinct scroll offset(s) — nothing scrolled, so this test would "
           "be asserting 'no duplicate requests' over a viewport that never "
           "moved";

    const std::vector<TileId> requested = renderer.requested();

    // --- guard: the measurement happened --------------------------------
    ASSERT_GE(requested.size(), 4u)
        << "the canvas requested " << requested.size()
        << " tiles — too few for any page of this fixture at 800x600, so the "
           "duplicate check below would be comparing nothing to nothing";
    std::set<TileKey> unique;
    std::set<int> pagesSeen;
    for (const auto& id : requested) {
        unique.insert(keyOf(id));
        pagesSeen.insert(id.pageIndex);
    }
    ASSERT_GE(pagesSeen.size(), 2u)
        << "only page(s) " << pagesSeen.size()
        << " were ever requested — the scroll did not move the viewport, so "
           "'revisiting a page costs nothing' is untested";
    ASSERT_TRUE(pagesSeen.count(0) == 1)
        << "page 0 was never requested, so the revisit at the end proves nothing";

    // --- the property ----------------------------------------------------
    EXPECT_EQ(requested.size(), unique.size())
        << requested.size() - unique.size()
        << " tile(s) were requested more than once: a tile already held by the "
           "cache was re-rendered. The `!tileCache_->contains(tileId)` guard in "
           "PdfCanvas::requestVisibleTiles() is what prevents this.";

    // Every requested tile ended up in the cache, put there by the canvas's own
    // completion callback — "tiles cached", asserted against the request log
    // rather than against a second cache query.
    EXPECT_EQ(cache.tileCount(), unique.size());
    EXPECT_EQ(cache.stats().putCount, static_cast<uint64_t>(unique.size()));
    for (const auto& id : requested) {
        EXPECT_TRUE(cache.contains(id))
            << "tile page=" << id.pageIndex << " col=" << id.col
            << " row=" << id.row << " was rendered but is not cached";
    }

    canvas.setDocument(nullptr, nullptr, nullptr);
}

// ===========================================================================
// export_image
// ===========================================================================
//
// Row `export_image` (fixture test_single.pdf).
//
// "PNG image created matching page" is made assertable as: the PNG's pixel
// dimensions are the page's own /MediaBox size scaled by the requested DPI
// (72 dpi == 1:1 with PDF points), the PNG is non-blank, and the ink lands at
// the same PAGE-POINT coordinates at two different DPIs. The DPI and the page
// size are both INPUTS; the second DPI exists so that a hard-coded scale
// factor cannot satisfy the test.

TEST(ExportImageTest, PngPixelDimensionsTrackThePageSizeAtTheChosenDpi)
{
    DocFixture f;
    if (!openFixture(f, "test_single.pdf")) {
        GTEST_SKIP() << "test_single.pdf fixture unavailable";
    }
    // Sidecar test_single.expected.json authors pageCount: 1.
    ASSERT_EQ(f.doc->pageCount(), 1);

    const PageInfo info = f.doc->pageInfo(0);
    ASSERT_GT(info.widthPt, 1.0f);
    ASSERT_GT(info.heightPt, 1.0f);

    struct Run {
        int dpi;
        QImage png;
        Ink ink;
    };
    std::vector<Run> runs{{72, {}, {}}, {144, {}, {}}};

    for (auto& run : runs) {
        auto outDir = freshOutDir(run.dpi == 72 ? "png_72" : "png_144");
        ImageExporter ex(f.doc.get());
        ImageExportOptions opts;
        opts.format = ImageFormat::Png;
        opts.dpi = run.dpi;
        const auto result = ex.exportToFolder(outDir, opts);

        ASSERT_EQ(result.totalRequested, 1) << "dpi=" << run.dpi;
        ASSERT_EQ(result.successCount, 1) << "dpi=" << run.dpi;
        ASSERT_EQ(result.errorCount, 0) << "dpi=" << run.dpi;
        ASSERT_EQ(result.fileResults.size(), 1u) << "dpi=" << run.dpi;

        const auto& fr = result.fileResults.front();
        ASSERT_TRUE(fr.success) << "dpi=" << run.dpi << ": " << fr.errorMessage;
        ASSERT_EQ(fr.pageIndex, 0);
        ASSERT_TRUE(std::filesystem::exists(fr.outputPath))
            << "dpi=" << run.dpi << ": export reported success but wrote no file";
        ASSERT_EQ(fr.outputPath.extension().string(), ".png");

        run.png = QImage(QString::fromStdString(fr.outputPath.string()));
        run.ink = measureInk(run.png);
        ASSERT_TRUE(inkIsMeasurable(run.png, run.ink,
                                    ("exported PNG at dpi=" + std::to_string(run.dpi)).c_str()));

        // The page, at this DPI. 72 dpi is 1 pixel per PDF point.
        const double scale = run.dpi / 72.0;
        EXPECT_NEAR(run.png.width(), info.widthPt * scale, 2.0)
            << "dpi=" << run.dpi << ": PNG width does not match the page width";
        EXPECT_NEAR(run.png.height(), info.heightPt * scale, 2.0)
            << "dpi=" << run.dpi << ": PNG height does not match the page height";
    }

    // Same page, same ink coordinates, two different DPIs.
    const QRectF a = runs[0].ink.boxPt(72 / 72.0);
    const QRectF b = runs[1].ink.boxPt(144 / 72.0);
    EXPECT_NEAR(a.left(),   b.left(),   2.0);
    EXPECT_NEAR(a.top(),    b.top(),    2.0);
    EXPECT_NEAR(a.width(),  b.width(),  2.0);
    EXPECT_NEAR(a.height(), b.height(), 2.0);
}

// ===========================================================================
// export_extract_pages
// ===========================================================================
//
// Row `export_extract_pages` (fixture test_3page.pdf).
//
// "New PDF created with selected pages" is asserted against the literal strings
// authored into the fixture's three content streams — an INPUT, not another
// read of the extractor. Extracting {0, 2} must yield a two-page PDF whose
// pages carry page 1's and page 3's text IN THAT ORDER, and must NOT carry
// page 2's. The negative half is what catches an off-by-one or an
// extract-everything implementation; the count alone would not.

TEST(ExportExtractPagesTest, ExtractedPdfHoldsExactlyTheSelectedPagesInOrder)
{
    DocFixture f;
    if (!openFixture(f, "test_3page.pdf")) {
        GTEST_SKIP() << "test_3page.pdf fixture unavailable";
    }
    ASSERT_EQ(f.doc->pageCount(), 3);

    // Authored by tests/fixtures generate_fixtures.py into the three content
    // streams; verified present in the SOURCE document first, so the assertions
    // on the EXTRACT below cannot pass vacuously against a string the fixture
    // never had.
    const std::vector<std::string> marker{
        "Page 1 - Title Page",
        "Page 2 - Body Content",
        "Page 3 - Final Page",
    };
    for (int p = 0; p < 3; ++p) {
        auto page = f.doc->page(p);
        ASSERT_NE(page, nullptr);
        const std::string text = page->extractPlainText();
        ASSERT_NE(text.find(marker[p]), std::string::npos)
            << "source page " << p << " does not contain its own marker \""
            << marker[p] << "\" — the fixture changed and this oracle is void";
    }

    auto outDir = freshOutDir("extract");
    const auto outPath = outDir / "extracted.pdf";
    const std::vector<int> selected{0, 2};

    ASSERT_NO_THROW(
        pdfe::core::extractPagesToFile(f.ctx, f.doc.get(), selected, outPath));
    ASSERT_TRUE(std::filesystem::exists(outPath))
        << "extractPagesToFile returned without writing a file";
    ASSERT_GT(std::filesystem::file_size(outPath), 0u);

    auto outCtx = std::make_shared<MuPdfContext>();
    auto outDoc = Document::open(outCtx, outPath);
    ASSERT_NE(outDoc, nullptr) << "the extracted PDF does not open";
    ASSERT_EQ(outDoc->pageCount(), static_cast<int>(selected.size()))
        << "extracted page count does not match the selection";

    std::vector<std::string> extracted;
    for (int p = 0; p < outDoc->pageCount(); ++p) {
        auto page = outDoc->page(p);
        ASSERT_NE(page, nullptr);
        extracted.push_back(page->extractPlainText());
        ASSERT_FALSE(extracted.back().empty())
            << "extracted page " << p << " has no text at all";
    }

    // Positive: the selected pages, in the order they were selected.
    EXPECT_NE(extracted[0].find(marker[0]), std::string::npos)
        << "extracted page 0 is not source page 0";
    EXPECT_NE(extracted[1].find(marker[2]), std::string::npos)
        << "extracted page 1 is not source page 2";

    // Negative: the unselected page is nowhere in the output.
    for (int p = 0; p < outDoc->pageCount(); ++p) {
        EXPECT_EQ(extracted[p].find(marker[1]), std::string::npos)
            << "extracted page " << p << " carries UNSELECTED source page 1";
    }

    // Order matters: the reverse assignment must not also hold.
    EXPECT_EQ(extracted[0].find(marker[2]), std::string::npos)
        << "extracted pages came out in the wrong order";
}

// ===========================================================================
// redaction_panel_widget_audit / redaction_panel_apply_state
// ===========================================================================

namespace {

RedactionData makeMark(int pageIndex, int annotIndex, const QRectF& rect,
                       const QColor& color, std::string overlay)
{
    RedactionData d;
    d.pageIndex = pageIndex;
    d.annotIndex = annotIndex;
    d.rect = rect;
    d.fillColor = color;
    d.overlayText = std::move(overlay);
    d.enabled = true;
    return d;
}

} // namespace

// Row `redaction_panel_widget_audit`.
//
// An audit that only checks `findChild(...) != nullptr` proves the widget was
// constructed, not that it is WIRED — the same trap phase 4 hit on
// DefaultsLogPanel. So each audited widget is additionally required to be LIVE:
// its observable state must change in response to marks handed to setMarks()
// and return to its empty state on clear(). The expected values come from the
// marks the test supplies.
TEST(RedactionPanelMatrixAuditTest, AuditedWidgetsAreReachableAndLive)
{
    if (!qApp) {
        GTEST_SKIP() << "QApplication not available";
    }
    RedactionPanel panel;

    auto* list = panel.findChild<QTreeWidget*>("redactionMarksList");
    auto* deleteButton = panel.findChild<QPushButton*>("deleteRedactionButton");
    auto* applyButton = panel.findChild<QPushButton*>("applyRedactionsButton");
    auto* countLabel = panel.findChild<QLabel*>("markCountLabel");

    ASSERT_NE(list, nullptr) << "redactionMarksList not reachable by objectName";
    ASSERT_NE(deleteButton, nullptr) << "deleteRedactionButton not reachable by objectName";
    ASSERT_NE(applyButton, nullptr) << "applyRedactionsButton not reachable by objectName";
    ASSERT_NE(countLabel, nullptr) << "markCountLabel not reachable by objectName";

    // Each audited widget must be identifiable to UIA / a screen reader too.
    EXPECT_FALSE(list->accessibleName().isEmpty());
    EXPECT_FALSE(deleteButton->accessibleName().isEmpty());
    EXPECT_FALSE(applyButton->accessibleName().isEmpty());
    EXPECT_FALSE(countLabel->accessibleName().isEmpty());

    const QString emptyCaption = countLabel->text();
    ASSERT_FALSE(emptyCaption.isEmpty())
        << "markCountLabel has no empty-state text, so 'the caption changed' "
           "below cannot distinguish live from blank";

    // --- live: the list reflects the marks it was given ------------------
    const std::vector<RedactionData> marks{
        makeMark(0, 0, QRectF(10.0, 20.0, 30.0, 40.0), Qt::black, "REDACTED"),
        makeMark(2, 1, QRectF(50.5, 60.5, 70.5, 80.5), Qt::red, "SECRET"),
    };
    panel.setMarks(marks);

    ASSERT_EQ(list->topLevelItemCount(), static_cast<int>(marks.size()))
        << "the list does not hold one row per supplied mark";
    EXPECT_EQ(panel.topLevelItemCount(), static_cast<int>(marks.size()));

    for (int i = 0; i < list->topLevelItemCount(); ++i) {
        auto* item = list->topLevelItem(i);
        ASSERT_NE(item, nullptr);
        // Column 0 is the 1-based page number of the mark handed in.
        EXPECT_EQ(item->text(0),
                  QString::number(marks[static_cast<size_t>(i)].pageIndex + 1))
            << "row " << i << " does not name its mark's page";
        // The overlay text supplied by the caller is displayed verbatim.
        EXPECT_EQ(item->text(3),
                  QString::fromStdString(marks[static_cast<size_t>(i)].overlayText))
            << "row " << i << " does not show its mark's overlay text";
        // UserRole carries the identity the deleteMarkRequested signal uses.
        EXPECT_EQ(item->data(0, Qt::UserRole).toInt(),
                  marks[static_cast<size_t>(i)].pageIndex);
        EXPECT_EQ(item->data(0, Qt::UserRole + 1).toInt(),
                  marks[static_cast<size_t>(i)].annotIndex);
    }

    // --- live: the count label reports the supplied count -----------------
    EXPECT_NE(countLabel->text(), emptyCaption)
        << "markCountLabel did not change when marks arrived — it is a static "
           "caption, not a live readout";
    EXPECT_TRUE(countLabel->text().contains(QString::number(marks.size())))
        << "markCountLabel reads \"" << countLabel->text().toStdString()
        << "\" and does not mention the " << marks.size()
        << " marks it was given";

    // --- live: clear() returns every audited widget to its empty state ----
    panel.clear();
    EXPECT_EQ(list->topLevelItemCount(), 0);
    EXPECT_EQ(panel.topLevelItemCount(), 0);
    EXPECT_EQ(countLabel->text(), emptyCaption)
        << "markCountLabel did not return to its empty-state text after clear()";
    EXPECT_FALSE(applyButton->isEnabled());
    EXPECT_FALSE(deleteButton->isEnabled());
}

// Row `redaction_panel_apply_state`.
//
// Asserted against the INPUT count: the apply button is disabled with no marks,
// enabled for one mark, enabled for several, and disabled again after clear().
// The 1-mark case is included deliberately — RedactionPanel::updateMarkCount()
// has a separate `count == 1` branch, and a test that only ever passed two
// marks would never execute it.
TEST(RedactionPanelMatrixAuditTest, ApplyButtonTracksMarkPresence)
{
    if (!qApp) {
        GTEST_SKIP() << "QApplication not available";
    }
    RedactionPanel panel;
    auto* applyButton = panel.findChild<QPushButton*>("applyRedactionsButton");
    ASSERT_NE(applyButton, nullptr);

    // Freshly constructed: nothing to apply.
    ASSERT_EQ(panel.topLevelItemCount(), 0);
    EXPECT_FALSE(applyButton->isEnabled())
        << "apply is enabled on an empty panel";

    // One mark — the count == 1 branch.
    const std::vector<RedactionData> one{
        makeMark(0, 0, QRectF(1.0, 2.0, 3.0, 4.0), Qt::black, "one"),
    };
    panel.setMarks(one);
    ASSERT_EQ(panel.topLevelItemCount(), 1)
        << "the panel did not take the single mark, so the button state below "
           "would be asserted over an empty panel";
    EXPECT_TRUE(applyButton->isEnabled())
        << "apply is disabled with 1 mark pending";

    // Three marks.
    const std::vector<RedactionData> three{
        makeMark(0, 0, QRectF(1.0, 2.0, 3.0, 4.0), Qt::black, "a"),
        makeMark(1, 0, QRectF(5.0, 6.0, 7.0, 8.0), Qt::red, "b"),
        makeMark(2, 0, QRectF(9.0, 10.0, 11.0, 12.0), Qt::blue, "c"),
    };
    panel.setMarks(three);
    ASSERT_EQ(panel.topLevelItemCount(), 3);
    EXPECT_TRUE(applyButton->isEnabled())
        << "apply is disabled with 3 marks pending";

    // Cleared — back to nothing to apply.
    panel.clear();
    ASSERT_EQ(panel.topLevelItemCount(), 0);
    EXPECT_FALSE(applyButton->isEnabled())
        << "apply stayed enabled after the marks were cleared";

    // A mark with enabled=false is not pending, so it must not arm the button.
    // This is the discriminating case: a panel that armed on "setMarks was
    // called" rather than on "there is something to apply" passes everything
    // above and fails here.
    std::vector<RedactionData> disabled{
        makeMark(0, 0, QRectF(1.0, 2.0, 3.0, 4.0), Qt::black, "skipped"),
    };
    disabled[0].enabled = false;
    panel.setMarks(disabled);
    EXPECT_EQ(panel.topLevelItemCount(), 0)
        << "a disabled mark was listed as pending";
    EXPECT_FALSE(applyButton->isEnabled())
        << "apply armed for a mark that is not enabled";
}
