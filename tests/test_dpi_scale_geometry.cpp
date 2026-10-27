// -*- coding: utf-8 -*-
//
// Phase F11 — gaps WIN-30 / WIN-32 / WIN-34: display scaling.
//
// WHAT WAS WRONG BEFORE
// ---------------------
// The only "DPI" coverage that drove the running application was
// tests/test_ui_e2e_workflows.py::test_scale_factor_launch, which launches
// PDFE at several QT_SCALE_FACTOR values and then asserts only
// ``proc.poll() is None`` — a LIVENESS poll. A build that laid every widget
// out at the wrong scale, or clipped every toolbar button to zero width,
// passes that test as long as the process does not exit. Nothing anywhere
// drove a scale CHANGE and asserted a GEOMETRY consequence of it.
//
// WHAT THIS FILE ASSERTS
// ----------------------
// The scale change is driven through PdfCanvas::setDevicePixelRatioForTest(),
// the product's own headless seam for the device-pixel ratio (it mirrors
// updateDevicePixelRatio(), the slot the real QScreen change drives), and the
// consequence asserted is geometric in two independent directions:
//
//   (1) DEVICE GEOMETRY MUST SCALE. The tile grid the canvas requests is laid
//       out in DEVICE pixels. Doubling the ratio must roughly double the tile
//       column and row extent of the visible page region. A build that stores
//       the ratio but lays tiles out at logical resolution (the R3-11 defect
//       shape) leaves the grid unchanged and REDs
//       TileGridGeometryScalesWithAnActualDprChange.
//
//   (2) LOGICAL GEOMETRY MUST NOT MOVE. Hit-testing, overlays and every
//       widget-space coordinate are DPR-independent in Qt. The page's screen
//       rect and the PDF<->canvas rect mapping must be byte-identical across
//       the same scale change. A build that leaks the ratio into coordinate
//       space REDs LogicalPageGeometryIsInvariantUnderTheSameDprChange —
//       which is the failure mode that makes a HiDPI user's clicks land in
//       the wrong place.
//
// Together those two are the "not a liveness poll" oracle: one of them fails
// for over-application of the scale, the other for under-application.
//
// WIN-34 additionally pins the process DPI-awareness mode, which is inherited
// (nothing in src/ sets it explicitly), so a regression in the Qt version or
// in an added manifest is caught rather than silently downgrading the whole
// application to system-DPI virtualisation.
//
// NOT CLOSED HERE (recorded in phase-11-dispositions.md): a genuine
// two-monitor mixed-scale drag (WIN-30's screenshot leg) and a live-UI launch
// at QT_SCALE_FACTOR=1.5 (WIN-31) both need a second physical display and a
// desktop session; neither is reachable from an in-process test.

#include <gtest/gtest.h>

#include "ui/PdfCanvas.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "rendering/TileCache.h"
#include "rendering/TileId.h"
#include "rendering/TileRenderer.h"

#include <QApplication>
#include <QCoreApplication>
#include <QImage>
#include <QPoint>
#include <QRectF>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace pdfe::ui;
using namespace pdfe::core;
using namespace pdfe::rendering;

namespace {

// Records the TileIds the canvas asks for without rasterising, so the tile
// GRID the canvas lays out is observable synchronously.
class GridSpyRenderer : public TileRenderer {
public:
    explicit GridSpyRenderer(Document* doc) : TileRenderer(doc) {}

    void requestTile(const TileId& id,
                     std::function<void(TileId, QImage)> /*cb*/) override
    {
        std::lock_guard<std::mutex> lk(mtx_);
        requested_.push_back(id);
    }

    std::vector<TileId> take()
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto copy = requested_;
        requested_.clear();
        return copy;
    }

private:
    std::mutex mtx_;
    std::vector<TileId> requested_;
};

struct GridExtent {
    int cols = 0;   // number of distinct tile columns requested
    int rows = 0;   // number of distinct tile rows requested
    int count = 0;
};

GridExtent extentOf(const std::vector<TileId>& ids)
{
    GridExtent e;
    int maxCol = -1, maxRow = -1;
    for (const auto& id : ids) {
        maxCol = (std::max)(maxCol, id.col);
        maxRow = (std::max)(maxRow, id.row);
    }
    e.cols = maxCol + 1;
    e.rows = maxRow + 1;
    e.count = static_cast<int>(ids.size());
    return e;
}

struct CanvasFixture {
    std::shared_ptr<MuPdfContext> ctx;
    std::unique_ptr<Document> doc;
    std::unique_ptr<TileCache> cache;
    std::unique_ptr<GridSpyRenderer> renderer;
    std::unique_ptr<PdfCanvas> canvas;
};

bool buildCanvas(CanvasFixture& f)
{
    std::filesystem::path pdf = std::filesystem::path(TEST_FIXTURES_DIR) / "test_single.pdf";
    if (!std::filesystem::exists(pdf)) return false;

    f.ctx = std::make_shared<MuPdfContext>();
    f.doc = Document::open(f.ctx, pdf);
    if (!f.doc || f.doc->pageCount() <= 0) return false;

    f.cache    = std::make_unique<TileCache>(64 * 1024 * 1024);
    f.renderer = std::make_unique<GridSpyRenderer>(f.doc.get());
    f.canvas   = std::make_unique<PdfCanvas>();
    f.canvas->resize(900, 700);
    f.canvas->setDocument(f.doc.get(), f.cache.get(), f.renderer.get());
    QCoreApplication::processEvents();
    return true;
}

} // namespace

// ─── (1) device geometry scales with an actual scale change ────────────────

TEST(DpiScaleGeometry, TileGridGeometryScalesWithAnActualDprChange)
{
    ASSERT_NE(qApp, nullptr) << "QApplication is created by test_main.cpp";
    CanvasFixture f;
    ASSERT_TRUE(buildCanvas(f)) << "test_single.pdf fixture is required";

    f.canvas->setZoom(2.0f);
    QCoreApplication::processEvents();
    f.renderer->take();                      // drop everything requested so far
    f.canvas->setDevicePixelRatioForTest(1.0f);
    QCoreApplication::processEvents();
    const GridExtent at1x = extentOf(f.renderer->take());
    ASSERT_GT(at1x.count, 0) << "canvas requested no tiles at dpr 1.0";

    const float zoomAt1x = f.canvas->effectiveRenderZoom();

    // THE SCALE CHANGE. Same path updateDevicePixelRatio() drives when the
    // widget is moved to a monitor with a different scale factor.
    f.canvas->setDevicePixelRatioForTest(2.0f);
    QCoreApplication::processEvents();
    const GridExtent at2x = extentOf(f.renderer->take());
    ASSERT_GT(at2x.count, 0) << "canvas requested no tiles after the dpr change";

    // GEOMETRY CONSEQUENCE: the device-space tile grid roughly doubles on each
    // axis. Tolerance of +/-1 tile absorbs the ceil() at the grid edge.
    EXPECT_GE(at2x.cols, 2 * at1x.cols - 1)
        << "tile column extent did not scale: " << at1x.cols << " -> " << at2x.cols;
    EXPECT_LE(at2x.cols, 2 * at1x.cols + 1)
        << "tile column extent over-scaled: " << at1x.cols << " -> " << at2x.cols;
    EXPECT_GE(at2x.rows, 2 * at1x.rows - 1)
        << "tile row extent did not scale: " << at1x.rows << " -> " << at2x.rows;
    EXPECT_LE(at2x.rows, 2 * at1x.rows + 1)
        << "tile row extent over-scaled: " << at1x.rows << " -> " << at2x.rows;
    EXPECT_GT(at2x.count, at1x.count)
        << "the same viewport must need strictly more device tiles at 2x";

    EXPECT_NEAR(f.canvas->effectiveRenderZoom(), 2.0f * zoomAt1x, 1e-4f);
}

TEST(DpiScaleGeometry, TileGridGeometryShrinksWhenTheScaleGoesBackDown)
{
    ASSERT_NE(qApp, nullptr);
    CanvasFixture f;
    ASSERT_TRUE(buildCanvas(f)) << "test_single.pdf fixture is required";

    f.canvas->setZoom(2.0f);
    f.canvas->setDevicePixelRatioForTest(2.0f);
    QCoreApplication::processEvents();
    f.renderer->take();
    f.canvas->setDevicePixelRatioForTest(2.0f);   // no-op: already at 2.0
    QCoreApplication::processEvents();

    f.canvas->setDevicePixelRatioForTest(3.0f);
    QCoreApplication::processEvents();
    const GridExtent at3x = extentOf(f.renderer->take());

    f.canvas->setDevicePixelRatioForTest(1.0f);
    QCoreApplication::processEvents();
    const GridExtent back1x = extentOf(f.renderer->take());

    ASSERT_GT(at3x.count, 0);
    ASSERT_GT(back1x.count, 0);
    EXPECT_LT(back1x.cols, at3x.cols)
        << "returning to 1x must shrink the device tile grid (" << at3x.cols
        << " -> " << back1x.cols << ")";
    EXPECT_LT(back1x.rows, at3x.rows);
}

// ─── (2) logical geometry is invariant across the same scale change ────────

TEST(DpiScaleGeometry, LogicalPageGeometryIsInvariantUnderTheSameDprChange)
{
    ASSERT_NE(qApp, nullptr);
    CanvasFixture f;
    ASSERT_TRUE(buildCanvas(f)) << "test_single.pdf fixture is required";

    f.canvas->setZoom(1.5f);
    f.canvas->setDevicePixelRatioForTest(1.0f);
    QCoreApplication::processEvents();

    QPoint originBefore;
    int widthBefore = 0, heightBefore = 0;
    ASSERT_TRUE(f.canvas->getPageScreenRect(0, originBefore, widthBefore, heightBefore));
    const QRectF pdfRectBefore = f.canvas->pdfRectFromCanvas(0, QRectF(20, 30, 120, 90));
    const QRectF canvasRectBefore = f.canvas->canvasRectFromPdf(0, QRectF(10, 10, 50, 40));

    f.canvas->setDevicePixelRatioForTest(2.0f);
    QCoreApplication::processEvents();

    QPoint originAfter;
    int widthAfter = 0, heightAfter = 0;
    ASSERT_TRUE(f.canvas->getPageScreenRect(0, originAfter, widthAfter, heightAfter));

    // The page occupies exactly the same LOGICAL rectangle. If the ratio leaks
    // into layout, a HiDPI user sees the page jump and every click miss.
    EXPECT_EQ(originAfter, originBefore);
    EXPECT_EQ(widthAfter, widthBefore);
    EXPECT_EQ(heightAfter, heightBefore);

    const QRectF pdfRectAfter = f.canvas->pdfRectFromCanvas(0, QRectF(20, 30, 120, 90));
    const QRectF canvasRectAfter = f.canvas->canvasRectFromPdf(0, QRectF(10, 10, 50, 40));
    EXPECT_NEAR(pdfRectAfter.x(),      pdfRectBefore.x(),      1e-3);
    EXPECT_NEAR(pdfRectAfter.y(),      pdfRectBefore.y(),      1e-3);
    EXPECT_NEAR(pdfRectAfter.width(),  pdfRectBefore.width(),  1e-3);
    EXPECT_NEAR(pdfRectAfter.height(), pdfRectBefore.height(), 1e-3);
    EXPECT_NEAR(canvasRectAfter.x(),      canvasRectBefore.x(),      1e-3);
    EXPECT_NEAR(canvasRectAfter.y(),      canvasRectBefore.y(),      1e-3);
    EXPECT_NEAR(canvasRectAfter.width(),  canvasRectBefore.width(),  1e-3);
    EXPECT_NEAR(canvasRectAfter.height(), canvasRectBefore.height(), 1e-3);
}

// ─── WIN-34: pin the inherited process DPI-awareness mode ──────────────────

#ifdef _WIN32
TEST(DpiScaleGeometry, WindowsThreadDpiAwarenessIsPerMonitorV2)
{
    ASSERT_NE(qApp, nullptr);
    const DPI_AWARENESS_CONTEXT ctx = GetThreadDpiAwarenessContext();
    ASSERT_NE(ctx, nullptr);

    const DPI_AWARENESS awareness = GetAwarenessFromDpiAwarenessContext(ctx);
    // Anything below PER_MONITOR means Windows bitmap-stretches the window on a
    // scaled monitor: the whole UI goes blurry and every mixed-DPI test below
    // becomes meaningless. PER_MONITOR_AWARE_V2 additionally gives correct
    // non-client scaling and per-monitor dialog DPI.
    EXPECT_EQ(awareness, DPI_AWARENESS_PER_MONITOR_AWARE)
        << "process DPI awareness downgraded to " << static_cast<int>(awareness);
    EXPECT_TRUE(AreDpiAwarenessContextsEqual(
        ctx, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
        << "expected PER_MONITOR_AWARE_V2 specifically (V1 lacks non-client "
           "and dialog per-monitor scaling)";
}
#endif
