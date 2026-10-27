// -*- coding: utf-8 -*-
//
// R3-11 (Medium): real high-DPI rendering.
//
// The defect: PdfCanvas STORED a device-pixel ratio but never applied it to
// render resolution. Tiles were keyed/requested at LOGICAL zoom only
// (TileId{page,col,row, zoomLevel_}), so on a HiDPI display pages rendered at
// logical resolution and Qt upscaled them → blurry. The pre-existing
// tests/test_hidpi.cpp only asserts the DESIRED math in the abstract
// (effectiveZoom == zoom*DPR) and never drives the product, so it passed even
// though the product ignored DPR entirely.
//
// These tests drive a real PdfCanvas with a real document and prove:
//
//   Oracle 1 (DEVICE-RESOLUTION RENDER): after forcing a non-1.0 DPR through
//   the headless test seam, the tiles the canvas actually REQUESTS carry an
//   effective render zoom of zoomLevel_ * dpr (device pixels) — observed via a
//   TileRenderer spy that records the exact TileId.zoom requested. Reverting
//   the "* devicePixelRatio_" render scaling re-REDs this oracle.
//
//   Oracle 2 (COORDINATE MAPPING INVARIANT): logical hit-test / overlay
//   coordinate mapping (pdfRectFromCanvas / canvasRectFromPdf) is UNCHANGED by
//   DPR. Qt mouse/overlay coordinates are logical (DPR-independent), so the
//   DPR render change must not shift any coordinate translation.

#include <gtest/gtest.h>

#include "ui/PdfCanvas.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "rendering/TileCache.h"
#include "rendering/TileRenderer.h"
#include "rendering/TileId.h"

#include <QApplication>
#include <QCoreApplication>
#include <QImage>
#include <QRectF>

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

using namespace pdfe::ui;
using namespace pdfe::core;
using namespace pdfe::rendering;

namespace {

// TileRenderer spy: records the exact TileId of every requested tile WITHOUT
// rasterizing (no async thread-pool work), so the effective render zoom the
// canvas asks for is observable synchronously and deterministically.
class SpyTileRenderer : public TileRenderer {
public:
    explicit SpyTileRenderer(Document* doc) : TileRenderer(doc) {}

    void requestTile(const TileId& id,
                     std::function<void(TileId, QImage)> /*cb*/) override
    {
        std::lock_guard<std::mutex> lk(mtx_);
        requested_.push_back(id);
        // Intentionally do NOT render: this test only observes the request.
    }

    std::vector<TileId> requested()
    {
        std::lock_guard<std::mutex> lk(mtx_);
        return requested_;
    }
    void clear()
    {
        std::lock_guard<std::mutex> lk(mtx_);
        requested_.clear();
    }

private:
    std::mutex mtx_;
    std::vector<TileId> requested_;
};

struct SpyFixture {
    std::shared_ptr<MuPdfContext> ctx;
    std::unique_ptr<Document> doc;
    std::unique_ptr<TileCache> cache;
    std::unique_ptr<SpyTileRenderer> renderer;
    std::unique_ptr<PdfCanvas> canvas;
};

bool makeSpyFixture(SpyFixture& f, const std::string& pdfName = "test_single.pdf")
{
    std::filesystem::path fixtures = TEST_FIXTURES_DIR;
    auto pdf = fixtures / pdfName;
    if (!std::filesystem::exists(pdf)) return false;

    f.ctx = std::make_shared<MuPdfContext>();
    f.doc = Document::open(f.ctx, pdf);
    if (!f.doc || f.doc->pageCount() <= 0) return false;

    f.cache = std::make_unique<TileCache>(64 * 1024 * 1024);
    f.renderer = std::make_unique<SpyTileRenderer>(f.doc.get());
    f.canvas = std::make_unique<PdfCanvas>();
    f.canvas->resize(800, 600);
    f.canvas->setDocument(f.doc.get(), f.cache.get(), f.renderer.get());
    QCoreApplication::processEvents();  // flush deferred (queued) re-request
    return true;
}

} // namespace

// ─── Oracle 1: device-resolution render (TileId.zoom == zoom * dpr) ─────────

TEST(HiDPIRender, TilesRequestedAtDeviceResolution)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    SpyFixture f;
    if (!makeSpyFixture(f)) GTEST_SKIP() << "test_single.pdf fixture unavailable";

    const float logicalZoom = f.canvas->zoom();
    ASSERT_GT(logicalZoom, 0.0f);

    for (float dpr : {1.25f, 1.5f, 2.0f}) {
        f.renderer->clear();
        // Force the DPR through the headless test seam. This invalidates the
        // tile cache and re-requests at the new device resolution.
        f.canvas->setDevicePixelRatioForTest(dpr);
        QCoreApplication::processEvents();

        const float expectedRenderZoom = logicalZoom * dpr;
        ASSERT_FLOAT_EQ(f.canvas->effectiveRenderZoom(), expectedRenderZoom)
            << "effectiveRenderZoom() must be zoom*dpr for dpr=" << dpr;

        auto ids = f.renderer->requested();
        ASSERT_FALSE(ids.empty())
            << "canvas must request visible tiles after a DPR change (dpr=" << dpr << ")";

        for (const auto& id : ids) {
            // The load-bearing oracle: tiles are rasterized at DEVICE
            // resolution. Reverting the "* devicePixelRatio_" scaling makes
            // this request come in at the logical zoom and this fails.
            EXPECT_NEAR(id.zoom, expectedRenderZoom, 1e-4f)
                << "tile requested at zoom " << id.zoom << " but expected device "
                   "zoom " << expectedRenderZoom << " (logical " << logicalZoom
                << " * dpr " << dpr << ")";
            // Explicitly reject the un-fixed behavior (logical zoom).
            EXPECT_GT(id.zoom, logicalZoom + 1e-4f)
                << "tile requested at LOGICAL zoom " << id.zoom
                << " — DPR render scaling not applied (dpr=" << dpr << ")";
        }
    }
}

// Proves the multiplier is zoom*dpr (not dpr alone): at a non-1.0 zoom the
// effective render zoom must scale with BOTH factors.
TEST(HiDPIRender, DeviceZoomScalesWithBothZoomAndDpr)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    SpyFixture f;
    if (!makeSpyFixture(f)) GTEST_SKIP() << "test_single.pdf fixture unavailable";

    f.canvas->setZoom(2.0f);
    QCoreApplication::processEvents();
    ASSERT_FLOAT_EQ(f.canvas->zoom(), 2.0f);

    f.renderer->clear();
    f.canvas->setDevicePixelRatioForTest(1.5f);
    QCoreApplication::processEvents();

    const float expected = 2.0f * 1.5f;  // 3.0
    ASSERT_FLOAT_EQ(f.canvas->effectiveRenderZoom(), expected);

    auto ids = f.renderer->requested();
    ASSERT_FALSE(ids.empty());
    for (const auto& id : ids) {
        EXPECT_NEAR(id.zoom, expected, 1e-4f)
            << "device render zoom must be zoom(2.0)*dpr(1.5)=3.0, got " << id.zoom;
    }
}

// A stamped tile carries the DPR so it paints back at the correct logical size.
// Exercises the request→deliver path with a real (non-spy) renderer so the
// delivered QImage is observable in the cache.
TEST(HiDPIRender, DeliveredTileCarriesDevicePixelRatio)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";

    std::filesystem::path fixtures = TEST_FIXTURES_DIR;
    auto pdf = fixtures / "test_single.pdf";
    if (!std::filesystem::exists(pdf)) GTEST_SKIP() << "fixture unavailable";

    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, pdf);
    if (!doc || doc->pageCount() <= 0) GTEST_SKIP() << "doc open failed";

    auto cache = std::make_unique<TileCache>(64 * 1024 * 1024);
    auto renderer = std::make_unique<TileRenderer>(doc.get());
    auto canvas = std::make_unique<PdfCanvas>();
    canvas->resize(800, 600);
    canvas->setDocument(doc.get(), cache.get(), renderer.get());
    QCoreApplication::processEvents();

    const float dpr = 2.0f;
    canvas->setDevicePixelRatioForTest(dpr);

    // Let the real renderer rasterize and deliver at least one tile.
    renderer->waitForDone();
    for (int i = 0; i < 50; ++i) {
        QCoreApplication::processEvents();
    }

    const float renderZoom = canvas->effectiveRenderZoom();
    ASSERT_FLOAT_EQ(renderZoom, 2.0f);

    // The top-left tile at the device render zoom must be present and stamped
    // with the DPR, and be a full device-sized (TILE_SIZE) tile.
    TileId topLeft{0, 0, 0, renderZoom};
    auto tile = cache->get(topLeft);
    ASSERT_TRUE(tile.has_value()) << "top-left device-resolution tile was not rendered";
    ASSERT_FALSE(tile->image.isNull());
    EXPECT_FLOAT_EQ(static_cast<float>(tile->image.devicePixelRatio()), dpr)
        << "delivered tile must carry the device-pixel ratio so it paints at "
           "the correct logical size";
    EXPECT_EQ(tile->image.width(), TILE_SIZE);
    EXPECT_EQ(tile->image.height(), TILE_SIZE);

    canvas->clearDocument();
    renderer->waitForDone();
}

// ─── Oracle 2: coordinate mapping is INVARIANT under DPR ────────────────────

TEST(HiDPIRender, CoordinateMappingUnchangedByDpr)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    SpyFixture f;
    if (!makeSpyFixture(f)) GTEST_SKIP() << "test_single.pdf fixture unavailable";

    // Baseline at dpr = 1.0.
    f.canvas->setDevicePixelRatioForTest(1.0f);
    QCoreApplication::processEvents();

    const QRectF pdfRect(72.0, 72.0, 100.0, 50.0);
    const QRectF canvasAt1 = f.canvas->canvasRectFromPdf(0, pdfRect);
    ASSERT_FALSE(canvasAt1.isNull());

    // A known canvas rect → PDF, at dpr = 1.0.
    const QRectF canvasProbe(200.0, 150.0, 120.0, 80.0);
    const QRectF pdfAt1 = f.canvas->pdfRectFromCanvas(0, canvasProbe);
    ASSERT_FALSE(pdfAt1.isNull());

    // Now force dpr = 2.0. Render resolution changes; LOGICAL coordinate
    // mapping must NOT (Qt mouse/overlay coords are DPR-independent).
    f.canvas->setDevicePixelRatioForTest(2.0f);
    QCoreApplication::processEvents();
    ASSERT_FLOAT_EQ(f.canvas->effectiveRenderZoom(), f.canvas->zoom() * 2.0f);

    const QRectF canvasAt2 = f.canvas->canvasRectFromPdf(0, pdfRect);
    ASSERT_FALSE(canvasAt2.isNull());
    EXPECT_NEAR(canvasAt2.x(),      canvasAt1.x(),      1e-6);
    EXPECT_NEAR(canvasAt2.y(),      canvasAt1.y(),      1e-6);
    EXPECT_NEAR(canvasAt2.width(),  canvasAt1.width(),  1e-6);
    EXPECT_NEAR(canvasAt2.height(), canvasAt1.height(), 1e-6);

    const QRectF pdfAt2 = f.canvas->pdfRectFromCanvas(0, canvasProbe);
    ASSERT_FALSE(pdfAt2.isNull());
    EXPECT_NEAR(pdfAt2.x(),      pdfAt1.x(),      1e-6);
    EXPECT_NEAR(pdfAt2.y(),      pdfAt1.y(),      1e-6);
    EXPECT_NEAR(pdfAt2.width(),  pdfAt1.width(),  1e-6);
    EXPECT_NEAR(pdfAt2.height(), pdfAt1.height(), 1e-6);
}
