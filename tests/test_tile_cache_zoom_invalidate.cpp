// -*- coding: utf-8 -*-
//
// Phase 3 of the thumbnail-driven zoom-hang fix: zoom-side tile cache cleanup.
//
// A zoom change produces an entirely new set of TileId keys (TileId is keyed on
// zoom). PdfCanvas::setZoom() previously never evicted the prior zoom's tiles,
// so they lingered in the 512MB LRU and churned it against the tiles the new
// zoom was about to request. TileCache::invalidateOtherZoom(keepZoom) drops
// every tile whose zoom differs from the current one; setZoom() calls it after
// zoomLevel_ updates and before requestVisibleTiles().
//
// Two oracles:
//  - direct: invalidateOtherZoom keeps only the kept zoom's tiles.
//  - via setZoom(): driving the canvas's public zoom API evicts stale-zoom
//    tiles and keeps current-zoom tiles, and repeated zoom changes do NOT
//    accumulate stale-zoom tiles (bounded retention), using contains()/
//    tileCount() rather than relying on size-based LRU eviction.

#include <gtest/gtest.h>

#include "rendering/TileCache.h"
#include "rendering/TileId.h"
#include "ui/PdfCanvas.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"

#include <QApplication>
#include <QImage>

#include <filesystem>
#include <memory>

using namespace pdfe::rendering;

namespace {

QImage tileImage()
{
    QImage img(TILE_SIZE, TILE_SIZE, QImage::Format_RGBA8888);
    img.fill(Qt::white);
    return img;
}

TileId tid(int page, int col, int row, float zoom)
{
    TileId id;
    id.pageIndex = page;
    id.col = col;
    id.row = row;
    id.zoom = zoom;
    return id;
}

std::filesystem::path fixture(const std::string& name)
{
    return std::filesystem::path(TEST_FIXTURES_DIR) / name;
}

} // namespace

// Direct oracle: invalidateOtherZoom keeps only tiles at the kept zoom.
TEST(TileCacheZoomInvalidate, KeepsOnlyKeptZoom)
{
    TileCache cache;
    cache.put(tid(0, 0, 0, 1.00f), tileImage());
    cache.put(tid(0, 1, 0, 1.00f), tileImage());
    cache.put(tid(0, 0, 0, 1.25f), tileImage());
    cache.put(tid(0, 0, 0, 2.00f), tileImage());
    ASSERT_EQ(cache.tileCount(), 4u);

    cache.invalidateOtherZoom(1.25f);

    EXPECT_TRUE(cache.contains(tid(0, 0, 0, 1.25f)));
    EXPECT_FALSE(cache.contains(tid(0, 0, 0, 1.00f)));
    EXPECT_FALSE(cache.contains(tid(0, 1, 0, 1.00f)));
    EXPECT_FALSE(cache.contains(tid(0, 0, 0, 2.00f)));
    EXPECT_EQ(cache.tileCount(), 1u) << "exactly the kept-zoom tile remains";
}

// setZoom()-driven oracle: a zoom change evicts the prior zoom's tiles and
// keeps the new zoom's; repeated zoom changes do not accumulate stale tiles.
TEST(TileCacheZoomInvalidate, SetZoomEvictsStaleZoomBounded)
{
    const auto pdf = fixture("test_3page.pdf");
    if (!std::filesystem::exists(pdf)) {
        GTEST_SKIP() << "test_3page.pdf fixture not available";
    }
    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    auto doc = pdfe::core::Document::open(ctx, pdf);
    ASSERT_NE(doc, nullptr);

    TileCache cache;
    pdfe::ui::PdfCanvas canvas;
    // Real cache, null renderer: setZoom() still calls invalidateOtherZoom()
    // before requestVisibleTiles(), and requestVisibleTiles() returns early
    // without a renderer, so no async tiles perturb the assertions.
    canvas.setDocument(doc.get(), &cache, nullptr);
    QApplication::processEvents();

    const float dpr = canvas.devicePixelRatio();
    auto renderTid = [&](float level, int col) {
        return tid(0, col, 0, level * dpr);
    };

    // Seed a tile at the zoom we will switch TO (the "current" tile) plus stale
    // tiles from the initial zoom level (1.0).
    cache.put(renderTid(1.0f, 0), tileImage());
    cache.put(renderTid(1.0f, 1), tileImage());
    cache.put(renderTid(2.0f, 0), tileImage());
    ASSERT_EQ(cache.tileCount(), 3u);

    canvas.setZoom(2.0f);

    EXPECT_TRUE(cache.contains(renderTid(2.0f, 0)))
        << "a tile already at the new zoom must be retained";
    EXPECT_FALSE(cache.contains(renderTid(1.0f, 0)))
        << "prior-zoom tile must be evicted on zoom change";
    EXPECT_FALSE(cache.contains(renderTid(1.0f, 1)));

    // Repeated zoom in/out over the same page range: before each change, seed
    // two stale tiles at a zoom OTHER than the target, then zoom. After each
    // setZoom, the cache must retain ZERO tiles at any non-current zoom —
    // stale-zoom retention is bounded at 0, not merely trimmed by LRU size.
    const float levels[] = {1.0f, 3.0f, 1.5f, 2.5f};
    float prev = 2.0f;
    for (float lvl : levels) {
        // stale tiles at the previous zoom (distinct from lvl)
        cache.put(renderTid(prev, 7), tileImage());
        cache.put(renderTid(prev, 8), tileImage());
        canvas.setZoom(lvl);

        // No tile at any zoom != lvl*dpr may survive.
        EXPECT_FALSE(cache.contains(renderTid(prev, 7)))
            << "stale tile at prior zoom survived setZoom(" << lvl << ")";
        EXPECT_FALSE(cache.contains(renderTid(prev, 8)));
        EXPECT_LE(cache.tileCount(), 4u)
            << "stale-zoom tiles accumulated across repeated zoom changes";
        prev = lvl;
    }
}
