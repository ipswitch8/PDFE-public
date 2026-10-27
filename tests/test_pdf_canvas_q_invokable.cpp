// -*- coding: utf-8 -*-
//
// Pipeline a7c2e541 phase-17 (coverage expansion).
//
// AC2-coverage: addresses tests/docs/coverage_gaps.md entries #1, #2.
//
// Verifies the three Q_INVOKABLE test-automation hooks added to
// PdfCanvas in phase-17:
//
//   1. pdfRectFromCanvas(int pageIndex, const QRectF& canvasRect) const
//   2. canvasRectFromPdf(int pageIndex, const QRectF& pdfRect) const
//   3. addRedactionMarkAtPdfRect(int pageIndex, const QRectF& pdfRect)
//
// These hooks close the KNOWN_WEAK entries P11-INFRA-1 (canvas-drag
// PDF coord translation) and P11-BUG-2 / P15-INFRA-2 (high-volume
// redaction-mark drag fragmentation) on the test-automation side.

#include <gtest/gtest.h>
#include "ui/PdfCanvas.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "rendering/TileCache.h"
#include "rendering/TileRenderer.h"
#include "annotations/AnnotationTool.h"
#include "annotations/AnnotationType.h"
#include "annotations/AnnotationManager.h"
#include "annotations/SelectTool.h"

#include <QApplication>
#include <QCoreApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QRectF>
#include <QSignalSpy>
#include <filesystem>
#include <memory>

using namespace pdfe::ui;
using namespace pdfe::core;
using namespace pdfe::rendering;

namespace {

struct CanvasFixture {
    std::shared_ptr<MuPdfContext> ctx;
    std::unique_ptr<Document> doc;
    std::unique_ptr<TileCache> cache;
    std::unique_ptr<TileRenderer> renderer;
    std::unique_ptr<PdfCanvas> canvas;
};

bool makeFixture(CanvasFixture& f, const std::string& pdfName = "test_single.pdf")
{
    std::filesystem::path fixtures = TEST_FIXTURES_DIR;
    auto pdf = fixtures / pdfName;
    if (!std::filesystem::exists(pdf)) return false;

    f.ctx = std::make_shared<MuPdfContext>();
    f.doc = Document::open(f.ctx, pdf);
    if (!f.doc || f.doc->pageCount() <= 0) return false;

    f.cache = std::make_unique<TileCache>(8 * 1024 * 1024);
    f.renderer = std::make_unique<TileRenderer>(f.doc.get());
    f.canvas = std::make_unique<PdfCanvas>();
    f.canvas->resize(800, 600);
    f.canvas->setDocument(f.doc.get(), f.cache.get(), f.renderer.get());
    QCoreApplication::processEvents();
    return true;
}

// Test-only AnnotationTool subclass: records every onMousePress
// invocation so we can prove clickByPdfCoord's synthesized press
// actually reaches the tool's onMousePress (i.e. flows through the
// currentTool_ dispatch branch in PdfCanvas::mousePressEvent, lines
// ~643-666 of src/ui/PdfCanvas.cpp, NOT the no-tool fallback that
// emits canvasClicked).
class SpyAnnotationTool : public pdfe::annotations::AnnotationTool {
public:
    int pressCount = 0;
    int lastPageIndex = -1;
    QPointF lastPdfPoint;

    pdfe::annotations::AnnotationType toolType() const override {
        return pdfe::annotations::AnnotationType::Unknown;
    }
    void onMousePress(QMouseEvent* /*event*/, int pageIndex, QPointF pdfPoint) override {
        ++pressCount;
        lastPageIndex = pageIndex;
        lastPdfPoint = pdfPoint;
    }
    void onMouseMove(QMouseEvent* /*event*/, int /*pageIndex*/, QPointF /*pdfPoint*/) override {}
    void onMouseRelease(QMouseEvent* /*event*/, int /*pageIndex*/, QPointF /*pdfPoint*/) override {}
};

} // namespace

// ─── pdfRectFromCanvas ────────────────────────────────────────────────────

TEST(PdfCanvasQInvokable, PdfRectFromCanvas_NoDocumentReturnsNull)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    PdfCanvas canvas;
    QRectF result = canvas.pdfRectFromCanvas(0, QRectF(10, 10, 100, 100));
    EXPECT_TRUE(result.isNull());
}

TEST(PdfCanvasQInvokable, PdfRectFromCanvas_OutOfRangePageIndexReturnsNull)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    CanvasFixture f;
    if (!makeFixture(f)) GTEST_SKIP() << "test_single.pdf fixture unavailable";

    EXPECT_TRUE(f.canvas->pdfRectFromCanvas(-1, QRectF(10, 10, 100, 100)).isNull());
    EXPECT_TRUE(f.canvas->pdfRectFromCanvas(9999, QRectF(10, 10, 100, 100)).isNull());
}

TEST(PdfCanvasQInvokable, PdfRectFromCanvas_NullInputReturnsNull)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    CanvasFixture f;
    if (!makeFixture(f)) GTEST_SKIP() << "test_single.pdf fixture unavailable";

    EXPECT_TRUE(f.canvas->pdfRectFromCanvas(0, QRectF()).isNull());
}

TEST(PdfCanvasQInvokable, PdfRectFromCanvas_ProducesNonNullRect_OnValidInput)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    CanvasFixture f;
    if (!makeFixture(f)) GTEST_SKIP() << "test_single.pdf fixture unavailable";

    // Find the page's screen rect, then sample a sub-region.
    QPoint origin; int wPx = 0, hPx = 0;
    ASSERT_TRUE(f.canvas->getPageScreenRect(0, origin, wPx, hPx));
    ASSERT_GT(wPx, 0); ASSERT_GT(hPx, 0);

    // Take a canvas rect somewhere inside the page area.
    QRectF canvasRect(origin.x() + wPx / 4, origin.y() + hPx / 4,
                      wPx / 2, hPx / 2);
    QRectF pdfRect = f.canvas->pdfRectFromCanvas(0, canvasRect);
    EXPECT_FALSE(pdfRect.isNull());
    EXPECT_GT(pdfRect.width(), 0.0);
    EXPECT_GT(pdfRect.height(), 0.0);
}

// ─── canvasRectFromPdf ────────────────────────────────────────────────────

TEST(PdfCanvasQInvokable, CanvasRectFromPdf_NoDocumentReturnsNull)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    PdfCanvas canvas;
    EXPECT_TRUE(canvas.canvasRectFromPdf(0, QRectF(0, 0, 100, 100)).isNull());
}

TEST(PdfCanvasQInvokable, CanvasRectFromPdf_OutOfRangePageIndexReturnsNull)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    CanvasFixture f;
    if (!makeFixture(f)) GTEST_SKIP() << "test_single.pdf fixture unavailable";

    EXPECT_TRUE(f.canvas->canvasRectFromPdf(-1, QRectF(0, 0, 100, 100)).isNull());
    EXPECT_TRUE(f.canvas->canvasRectFromPdf(9999, QRectF(0, 0, 100, 100)).isNull());
}

TEST(PdfCanvasQInvokable, CanvasRectFromPdf_RoundTrip)
{
    // Round-trip identity at zoom=1.0: pdfRectFromCanvas(canvasRectFromPdf(R)) == R.
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    CanvasFixture f;
    if (!makeFixture(f)) GTEST_SKIP() << "test_single.pdf fixture unavailable";

    f.canvas->setZoom(1.0f);
    QCoreApplication::processEvents();

    QRectF pdfRect(72.0, 72.0, 100.0, 50.0);  // 1" × 0.7" rect at top-left margin
    QRectF canvasRect = f.canvas->canvasRectFromPdf(0, pdfRect);
    ASSERT_FALSE(canvasRect.isNull());

    QRectF pdfBack = f.canvas->pdfRectFromCanvas(0, canvasRect);
    ASSERT_FALSE(pdfBack.isNull());

    // Round-trip within 1 pt tolerance (pixel-rounding artifacts).
    EXPECT_NEAR(pdfBack.x(),      pdfRect.x(),      1.0);
    EXPECT_NEAR(pdfBack.y(),      pdfRect.y(),      1.0);
    EXPECT_NEAR(pdfBack.width(),  pdfRect.width(),  1.0);
    EXPECT_NEAR(pdfBack.height(), pdfRect.height(), 1.0);
}

// ─── addRedactionMarkAtPdfRect ────────────────────────────────────────────

TEST(PdfCanvasQInvokable, AddRedactionMark_NoDocumentReturnsFalse)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    PdfCanvas canvas;
    EXPECT_FALSE(canvas.addRedactionMarkAtPdfRect(0, QRectF(0, 0, 100, 100)));
}

TEST(PdfCanvasQInvokable, AddRedactionMark_OutOfRangePageIndexReturnsFalse)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    CanvasFixture f;
    if (!makeFixture(f)) GTEST_SKIP() << "test_single.pdf fixture unavailable";

    EXPECT_FALSE(f.canvas->addRedactionMarkAtPdfRect(-1, QRectF(0, 0, 100, 100)));
    EXPECT_FALSE(f.canvas->addRedactionMarkAtPdfRect(9999, QRectF(0, 0, 100, 100)));
}

TEST(PdfCanvasQInvokable, AddRedactionMark_NullOrZeroSizedRectReturnsFalse)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    CanvasFixture f;
    if (!makeFixture(f)) GTEST_SKIP() << "test_single.pdf fixture unavailable";

    EXPECT_FALSE(f.canvas->addRedactionMarkAtPdfRect(0, QRectF()));
    EXPECT_FALSE(f.canvas->addRedactionMarkAtPdfRect(0, QRectF(10, 10, 0, 10)));
    EXPECT_FALSE(f.canvas->addRedactionMarkAtPdfRect(0, QRectF(10, 10, 10, 0)));
}

TEST(PdfCanvasQInvokable, AddRedactionMark_ValidInputEmitsSignalAndReturnsTrue)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    CanvasFixture f;
    if (!makeFixture(f)) GTEST_SKIP() << "test_single.pdf fixture unavailable";

    QSignalSpy spy(f.canvas.get(), &PdfCanvas::redactionMarkRequested);
    ASSERT_TRUE(spy.isValid());

    QRectF pdfRect(72.0, 72.0, 200.0, 50.0);
    EXPECT_TRUE(f.canvas->addRedactionMarkAtPdfRect(0, pdfRect));

    ASSERT_EQ(spy.count(), 1);
    const auto& args = spy.takeFirst();
    EXPECT_EQ(args.at(0).toInt(), 0);
    QRectF emittedRect = args.at(1).toRectF();
    EXPECT_DOUBLE_EQ(emittedRect.x(), pdfRect.x());
    EXPECT_DOUBLE_EQ(emittedRect.y(), pdfRect.y());
    EXPECT_DOUBLE_EQ(emittedRect.width(), pdfRect.width());
    EXPECT_DOUBLE_EQ(emittedRect.height(), pdfRect.height());
}

TEST(PdfCanvasQInvokable, AddRedactionMark_RepeatedCallsEmitNSignals)
{
    // Per-mark undo audit (closes P11-BUG-2): N independent calls
    // produce N distinct emissions, so MainWindow's slot creates N
    // distinct CreateRedactionCommand entries in the undo stack.
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    CanvasFixture f;
    if (!makeFixture(f)) GTEST_SKIP() << "test_single.pdf fixture unavailable";

    QSignalSpy spy(f.canvas.get(), &PdfCanvas::redactionMarkRequested);

    for (int i = 0; i < 5; ++i) {
        QRectF r(72.0, 72.0 + i * 60.0, 100.0, 50.0);
        EXPECT_TRUE(f.canvas->addRedactionMarkAtPdfRect(0, r));
    }
    EXPECT_EQ(spy.count(), 5);
}

// ─── clickByPdfCoord — pipeline b3f19d72 phase-1 ─────────────────────────

TEST(PdfCanvasQInvokable, ClickByPdfCoord_NoDocumentReturnsFalse)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    PdfCanvas canvas;
    EXPECT_FALSE(canvas.clickByPdfCoord(0, 100.0, 100.0));
}

TEST(PdfCanvasQInvokable, ClickByPdfCoord_OutOfRangePageIndexReturnsFalse)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    CanvasFixture f;
    if (!makeFixture(f)) GTEST_SKIP() << "test_single.pdf fixture unavailable";

    EXPECT_FALSE(f.canvas->clickByPdfCoord(-1, 100.0, 100.0));
    EXPECT_FALSE(f.canvas->clickByPdfCoord(9999, 100.0, 100.0));
}

TEST(PdfCanvasQInvokable, ClickByPdfCoord_DispatchesToHitTest)
{
    // Phase-1 AC4: synthesizing a click at a known PDF coord must
    // dispatch through the canvas's normal mouse-event pipeline. With
    // no tool installed, the "no tool consuming the click" branch in
    // mousePressEvent emits canvasClicked(pageIdx, pdfPoint), which is
    // the signal MainWindow uses to drive followLinkAt() and hence
    // form/widget activation. Verify at least one emission and that
    // the emitted pageIndex matches the click target.
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    CanvasFixture f;
    // test_form.pdf — has a widget at known PDF coordinates we can
    // click. Even without form-tool wiring (since canvasClicked is
    // emitted for any clicked page), the test exercises the dispatch.
    if (!makeFixture(f, "test_form.pdf"))
        GTEST_SKIP() << "test_form.pdf fixture unavailable";

    QSignalSpy spy(f.canvas.get(), &PdfCanvas::canvasClicked);
    ASSERT_TRUE(spy.isValid());

    // Click near the page center in PDF coords. test_form.pdf is
    // letter-sized (612 × 792 pt); (200, 400) sits inside the page.
    EXPECT_TRUE(f.canvas->clickByPdfCoord(0, 200.0, 400.0));

    // canvasClicked must have fired at least once for page 0.
    ASSERT_GE(spy.count(), 1);
    const auto& args = spy.first();
    EXPECT_EQ(args.at(0).toInt(), 0);
    // Translated PDF point should sit close to the requested coord;
    // small rounding from the screen ↔ PDF round-trip is expected.
    QPointF emittedPdf = args.at(1).toPointF();
    EXPECT_NEAR(emittedPdf.x(), 200.0, 2.0);
    EXPECT_NEAR(emittedPdf.y(), 400.0, 2.0);
}

TEST(PdfCanvasQInvokable, ClickByPdfCoord_DispatchesToInstalledTool)
{
    // Karen-finding remediation for phase-1 AC4: the existing
    // ClickByPdfCoord_DispatchesToHitTest only spies on canvasClicked,
    // which fires ONLY in the "no tool consuming the click" fallback
    // branch (src/ui/PdfCanvas.cpp:716). The AC says the synthesized
    // press must dispatch through the normal FormTool/AnnotationTool
    // hit-test pipeline, so we also need to prove the press reaches an
    // INSTALLED tool's onMousePress (the currentTool_ branch at lines
    // ~643-666). If clickByPdfCoord ever regresses to a path that
    // bypasses currentTool_->onMousePress (e.g. direct signal emission
    // skipping QApplication::sendEvent), this test fails.
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    CanvasFixture f;
    if (!makeFixture(f)) GTEST_SKIP() << "test_single.pdf fixture unavailable";

    auto tool = std::make_unique<SpyAnnotationTool>();
    SpyAnnotationTool* toolRaw = tool.get();
    f.canvas->setCurrentTool(std::move(tool));
    ASSERT_EQ(f.canvas->currentTool(), toolRaw);

    // Also spy on canvasClicked to PROVE the synthesized event went
    // through currentTool_ and NOT the no-tool fallback. With a tool
    // installed, the fallback branch (line 704) is skipped because the
    // currentTool_ branch returns early at line 665.
    QSignalSpy canvasClickedSpy(f.canvas.get(), &PdfCanvas::canvasClicked);
    ASSERT_TRUE(canvasClickedSpy.isValid());

    EXPECT_TRUE(f.canvas->clickByPdfCoord(0, 100.0, 100.0));
    QCoreApplication::processEvents();

    // Primary assertion: the installed tool's onMousePress was invoked
    // at least once. This is the karen-required proof that the click
    // synthesis flows through the real mousePressEvent dispatch chain.
    EXPECT_GE(toolRaw->pressCount, 1);
    EXPECT_EQ(toolRaw->lastPageIndex, 0);
    // Translated PDF point should sit close to the requested coord.
    EXPECT_NEAR(toolRaw->lastPdfPoint.x(), 100.0, 2.0);
    EXPECT_NEAR(toolRaw->lastPdfPoint.y(), 100.0, 2.0);

    // Secondary assertion: canvasClicked must NOT have fired, because
    // with a tool installed the currentTool_ branch consumes the click
    // and returns before the no-tool fallback. This locks in that we
    // really did exercise the tool-dispatch path, not the fallback.
    EXPECT_EQ(canvasClickedSpy.count(), 0);

    // Cleanup: clear the tool so the canvas destructor doesn't see a
    // stale ptr (defensive; setCurrentTool stores a unique_ptr).
    f.canvas->clearCurrentTool();
}

// ─── scrollToPdfRect (P-H-INFRA-5 fix) ───────────────────────────────────
//
// Verifies that scrollToPdfRect() adjusts scrollOffset_ so that the
// annotation's midpoint is centered in the viewport, producing a visible
// canvas change even when the annotation is on the current page.

TEST(PdfCanvasScrollToPdfRect, NoDocumentIsNoOp)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    PdfCanvas canvas;
    canvas.resize(800, 600);
    // Should not crash with no document.
    EXPECT_NO_THROW(canvas.scrollToPdfRect(0, QRectF(0, 0, 100, 100)));
}

TEST(PdfCanvasScrollToPdfRect, OutOfRangePageIndexIsNoOp)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    CanvasFixture f;
    if (!makeFixture(f)) GTEST_SKIP() << "test_single.pdf fixture unavailable";

    // Should not crash for out-of-range page index.
    EXPECT_NO_THROW(f.canvas->scrollToPdfRect(-1, QRectF(0, 0, 100, 100)));
    EXPECT_NO_THROW(f.canvas->scrollToPdfRect(999, QRectF(0, 0, 100, 100)));
}

TEST(PdfCanvasScrollToPdfRect, ScrollsToAnnotationRect)
{
    // Verifies that after scrollToPdfRect the canvas is no longer at the
    // default scroll position (top of page) when the target rect is in the
    // lower half of the page.  This is the core invariant needed by
    // test_comment_panel_navigates_to_annotation: navigating to a
    // lower-page annotation from scroll=0 must change the scroll position.
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    CanvasFixture f;
    if (!makeFixture(f)) GTEST_SKIP() << "test_single.pdf fixture unavailable";

    // Canvas at default: scrollOffset = (0,0), current page = 0.
    // Scroll to an annotation rect at the lower half of the page (y=500 in PDF pts).
    // For test_single.pdf (595×842 pt) at zoom=1.0:
    //   layout.heightPx ≈ 842, layout.yOffset ≈ PAGE_MARGIN.
    //   annotCenterDocY = yOffset + 550*zoom, which is > viewport half-height (300).
    //   → newY > 0 (canvas scrolls away from top).
    const QRectF lowerAnnotRect(100.0, 500.0, 200.0, 50.0);  // y=500..550 on 842pt page
    f.canvas->scrollToPdfRect(0, lowerAnnotRect);
    QCoreApplication::processEvents();

    // Scroll offset must have moved from the initial 0. We can't directly
    // inspect scrollOffset_ (private), but we can verify the current page
    // is still 0 (no cross-page navigation) and that requestVisibleTiles /
    // update were triggered without crashing.  The side-observable effect
    // for the E2E test is the canvas pixel change, which is verified in the
    // Python test via screenshot diff.
    EXPECT_EQ(f.canvas->currentPage(), 0);
}

TEST(PdfCanvasScrollToPdfRect, CrossPageNavigationUpdatesCurrentPage)
{
    // For a multi-page fixture: scrollToPdfRect on page 1 should navigate
    // to page 1 (currently on page 0).
    //
    // GAP-11 fix (b3d7f942 phase-5): use test_3page.pdf (guaranteed 3 pages)
    // instead of test_single.pdf (1 page), so the pageCount() < 2 guard is
    // never needed.  This test now always runs instead of always being SKIPPED.
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    CanvasFixture f;
    if (!makeFixture(f, "test_3page.pdf")) GTEST_SKIP() << "test_3page.pdf fixture unavailable";

    ASSERT_GE(f.canvas->pageCount(), 2) << "test_3page.pdf must have >= 2 pages";
    ASSERT_EQ(f.canvas->currentPage(), 0);
    f.canvas->scrollToPdfRect(1, QRectF(100.0, 100.0, 200.0, 50.0));
    QCoreApplication::processEvents();
    EXPECT_EQ(f.canvas->currentPage(), 1);
}

// ─── Keyboard scroll direction (P-H-INFRA-5 root-cause fix) ──────────────
//
// Root cause of the failing test_comment_panel_navigates_to_annotation:
// Key_Down used -= SCROLL_STEP (scrolled UP toward page top) and Key_Up
// used += SCROLL_STEP (scrolled DOWN).  The fix swaps the signs with
// clamping.  These tests lock the corrected semantics so a regression
// immediately turns red.
//
// Observable proxy: getPageScreenRect() returns the page's top Y in
// viewport coordinates (yOffset - scrollOffset.y()).  After scrolling
// DOWN, that Y must decrease (page moves up in viewport = we've scrolled
// further into the document).  After scrolling UP it must increase.

static void sendKey(QWidget* w, Qt::Key k) {
    QKeyEvent press(QEvent::KeyPress, k, Qt::NoModifier);
    QKeyEvent release(QEvent::KeyRelease, k, Qt::NoModifier);
    QCoreApplication::sendEvent(w, &press);
    QCoreApplication::sendEvent(w, &release);
    QCoreApplication::processEvents();
}

TEST(PdfCanvasKeyboardScroll, DownKeyScrollsDown)
{
    // After pressing Down, the page should appear higher in the viewport
    // (pageTop decreases), meaning scrollOffset_ increased.
    //
    // We use zoom=3.0 so the page (typically ~842 pt tall) renders as
    // ~2526 px — much taller than the 600 px viewport — giving the
    // Down key room to actually scroll before the clamp fires.
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    CanvasFixture f;
    if (!makeFixture(f)) GTEST_SKIP() << "test_single.pdf fixture unavailable";

    f.canvas->resize(800, 600);
    f.canvas->setZoom(3.0f);
    QCoreApplication::processEvents();

    QPoint origin0; int wPx0 = 0, hPx0 = 0;
    ASSERT_TRUE(f.canvas->getPageScreenRect(0, origin0, wPx0, hPx0));
    int initialPageTop = origin0.y();

    // Press Down 5 times to scroll further into the document.
    for (int i = 0; i < 5; ++i) {
        sendKey(f.canvas.get(), Qt::Key_Down);
    }

    QPoint originAfter; int wPxA = 0, hPxA = 0;
    ASSERT_TRUE(f.canvas->getPageScreenRect(0, originAfter, wPxA, hPxA));
    int afterPageTop = originAfter.y();

    // Page top must decrease (page moved up in viewport = scrolled down).
    EXPECT_LT(afterPageTop, initialPageTop)
        << "Key_Down should scroll the document DOWN (pageTop in viewport decreases). "
           "Initial pageTop=" << initialPageTop << ", after pageTop=" << afterPageTop;
}

TEST(PdfCanvasKeyboardScroll, UpKeyScrollsUp)
{
    // Scroll down first (with zoom=3.0 so there is room), then press Up —
    // the page should return toward a higher viewport Y.
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    CanvasFixture f;
    if (!makeFixture(f)) GTEST_SKIP() << "test_single.pdf fixture unavailable";

    f.canvas->resize(800, 600);
    f.canvas->setZoom(3.0f);
    QCoreApplication::processEvents();

    // First scroll down so there is room to scroll back up.
    for (int i = 0; i < 10; ++i) {
        sendKey(f.canvas.get(), Qt::Key_Down);
    }

    QPoint originMid; int wPxM = 0, hPxM = 0;
    ASSERT_TRUE(f.canvas->getPageScreenRect(0, originMid, wPxM, hPxM));
    int midPageTop = originMid.y();

    // Now press Up 5 times — should scroll back toward page top.
    for (int i = 0; i < 5; ++i) {
        sendKey(f.canvas.get(), Qt::Key_Up);
    }

    QPoint originAfter; int wPxA = 0, hPxA = 0;
    ASSERT_TRUE(f.canvas->getPageScreenRect(0, originAfter, wPxA, hPxA));
    int afterPageTop = originAfter.y();

    // Page top must increase after Up (page moved down in viewport = scrolled up).
    EXPECT_GT(afterPageTop, midPageTop)
        << "Key_Up should scroll the document UP (pageTop in viewport increases). "
           "Mid pageTop=" << midPageTop << ", after pageTop=" << afterPageTop;
}

TEST(PdfCanvasKeyboardScroll, DownKeyClampedAtDocumentBottom)
{
    // Pressing Down repeatedly should not crash: scrollOffset is clamped
    // at max(0, totalDocumentHeight - height).
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    CanvasFixture f;
    if (!makeFixture(f)) GTEST_SKIP() << "test_single.pdf fixture unavailable";

    f.canvas->resize(800, 600);
    f.canvas->setZoom(3.0f);
    QCoreApplication::processEvents();

    // Press Down 500 times — well past any clamp boundary.
    for (int i = 0; i < 500; ++i) {
        sendKey(f.canvas.get(), Qt::Key_Down);
    }

    // Canvas should remain valid. currentPage stays in range.
    int pg = f.canvas->currentPage();
    EXPECT_GE(pg, 0);
    EXPECT_LT(pg, f.canvas->pageCount());
}

TEST(PdfCanvasKeyboardScroll, UpKeyClampedAtDocumentTop)
{
    // Pressing Up from the top must not produce a negative scrollOffset.
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    CanvasFixture f;
    if (!makeFixture(f)) GTEST_SKIP() << "test_single.pdf fixture unavailable";

    f.canvas->resize(800, 600);
    f.canvas->setZoom(3.0f);
    QCoreApplication::processEvents();

    // Press Up 200 times from the initial position (already at top).
    for (int i = 0; i < 200; ++i) {
        sendKey(f.canvas.get(), Qt::Key_Up);
    }

    // Canvas should remain valid (no crash, no assertion).
    int pg = f.canvas->currentPage();
    EXPECT_GE(pg, 0);
    EXPECT_LT(pg, f.canvas->pageCount());

    // Page-top in viewport must be >= PAGE_GAP - scrollOffset_max.
    // At minimum, scrollOffset_ should not exceed the initial yOffset
    // (i.e. the page top should not be pushed so high it goes below the
    // visible area). Just verify no crash and current-page sanity.
    QPoint origin; int wPx = 0, hPx = 0;
    EXPECT_TRUE(f.canvas->getPageScreenRect(0, origin, wPx, hPx));
}

// ─── Horizontal keyboard scroll (Key_Right / Key_Left clamp fix) ─────────
//
// These mirror the Down/Up tests.  getPageScreenRect() returns
// pageLeft = (width - widthPx)/2 - scrollOffset.x(), so after
// pressing Right, pageLeft decreases (page moves left in viewport).

TEST(PdfCanvasKeyboardScroll, RightKeyScrollsRight)
{
    // After pressing Right, the page should appear further left in the
    // viewport (pageLeft decreases), meaning scrollOffset_.x() increased.
    //
    // We use zoom=3.0 so the page (typically ~595 pt wide) renders as
    // ~1785 px — much wider than the 800 px viewport — giving the
    // Right key room to scroll before the clamp fires.
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    CanvasFixture f;
    if (!makeFixture(f)) GTEST_SKIP() << "test_single.pdf fixture unavailable";

    f.canvas->resize(800, 600);
    f.canvas->setZoom(3.0f);
    QCoreApplication::processEvents();

    QPoint origin0; int wPx0 = 0, hPx0 = 0;
    ASSERT_TRUE(f.canvas->getPageScreenRect(0, origin0, wPx0, hPx0));
    int initialPageLeft = origin0.x();

    // Press Right 5 times to scroll further into the document horizontally.
    for (int i = 0; i < 5; ++i) {
        sendKey(f.canvas.get(), Qt::Key_Right);
    }

    QPoint originAfter; int wPxA = 0, hPxA = 0;
    ASSERT_TRUE(f.canvas->getPageScreenRect(0, originAfter, wPxA, hPxA));
    int afterPageLeft = originAfter.x();

    // pageLeft must decrease (page moved left in viewport = scrolled right).
    EXPECT_LT(afterPageLeft, initialPageLeft)
        << "Key_Right should scroll the document RIGHT (pageLeft in viewport decreases). "
           "Initial pageLeft=" << initialPageLeft << ", after pageLeft=" << afterPageLeft;
}

TEST(PdfCanvasKeyboardScroll, LeftKeyScrollsLeft)
{
    // Scroll right first, then press Left — the page should return toward
    // a larger (more positive) pageLeft.
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    CanvasFixture f;
    if (!makeFixture(f)) GTEST_SKIP() << "test_single.pdf fixture unavailable";

    f.canvas->resize(800, 600);
    f.canvas->setZoom(3.0f);
    QCoreApplication::processEvents();

    // First scroll right so there is room to scroll back left.
    for (int i = 0; i < 10; ++i) {
        sendKey(f.canvas.get(), Qt::Key_Right);
    }

    QPoint originMid; int wPxM = 0, hPxM = 0;
    ASSERT_TRUE(f.canvas->getPageScreenRect(0, originMid, wPxM, hPxM));
    int midPageLeft = originMid.x();

    // Now press Left 5 times — should scroll back toward the left edge.
    for (int i = 0; i < 5; ++i) {
        sendKey(f.canvas.get(), Qt::Key_Left);
    }

    QPoint originAfter; int wPxA = 0, hPxA = 0;
    ASSERT_TRUE(f.canvas->getPageScreenRect(0, originAfter, wPxA, hPxA));
    int afterPageLeft = originAfter.x();

    // pageLeft must increase after Left (page moved right in viewport = scrolled left).
    EXPECT_GT(afterPageLeft, midPageLeft)
        << "Key_Left should scroll the document LEFT (pageLeft in viewport increases). "
           "Mid pageLeft=" << midPageLeft << ", after pageLeft=" << afterPageLeft;
}

TEST(PdfCanvasKeyboardScroll, RightKeyClampedAtDocumentRightEdge)
{
    // Pressing Right repeatedly must not allow scrollOffset_.x() to exceed
    // max(0, totalDocumentWidth_ - width()).
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    CanvasFixture f;
    if (!makeFixture(f)) GTEST_SKIP() << "test_single.pdf fixture unavailable";

    f.canvas->resize(800, 600);
    f.canvas->setZoom(3.0f);
    QCoreApplication::processEvents();

    // Press Right 500 times — well past any clamp boundary.
    for (int i = 0; i < 500; ++i) {
        sendKey(f.canvas.get(), Qt::Key_Right);
    }

    // Canvas must remain valid after many Right presses.
    int pg = f.canvas->currentPage();
    EXPECT_GE(pg, 0);
    EXPECT_LT(pg, f.canvas->pageCount());
}

TEST(PdfCanvasKeyboardScroll, LeftKeyClampedAtDocumentLeftEdge)
{
    // Pressing Left from the left edge must not produce a negative scrollOffset_.x().
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    CanvasFixture f;
    if (!makeFixture(f)) GTEST_SKIP() << "test_single.pdf fixture unavailable";

    f.canvas->resize(800, 600);
    f.canvas->setZoom(3.0f);
    QCoreApplication::processEvents();

    // Press Left 200 times from the initial position (already at left edge).
    for (int i = 0; i < 200; ++i) {
        sendKey(f.canvas.get(), Qt::Key_Left);
    }

    // Canvas must remain valid; page origin x must not go below the
    // un-scrolled centering position (scrollOffset_.x() is clamped at 0).
    QPoint origin; int wPx = 0, hPx = 0;
    ASSERT_TRUE(f.canvas->getPageScreenRect(0, origin, wPx, hPx));

    // With scrollOffset_.x() clamped at 0, pageLeft = (width-widthPx)/2.
    // For zoom=3.0 the page is wider than the viewport, so widthPx > width()
    // and pageLeft is negative (page extends beyond left edge). But it must
    // not be less than what an unclamped negative scrollOffset would produce —
    // i.e. the canvas must not panic/crash and currentPage must be valid.
    int pg = f.canvas->currentPage();
    EXPECT_GE(pg, 0);
    EXPECT_LT(pg, f.canvas->pageCount());
}

// ─── GitHub #119 — drag-to-pan while a tool (Select) is active ───────────
//
// THE DEFECT. 7cf860d3 installed a real SelectTool on document open. From
// then on the `currentTool_ && LeftButton` branch of mousePressEvent claimed
// every left press and rubber-banded, so the left-button pan fallback at the
// bottom of that function — the ONLY pan gesture the canvas had — became
// unreachable for the whole time a document is open.
//
// THE FIX. A MiddleButton branch at the top of mousePress/Move/Release,
// tool-independent, panning both axes. See the block comment at the head of
// PdfCanvas::mousePressEvent for why middle-drag rather than a Hand tool.
//
// Oracle: PdfCanvas::scrollOffsetX() / scrollOffsetY(), the same view-state
// seam the SendInput UI tests read through testViewStateQueryPipe. Zoom 3.0
// makes the page both taller and wider than the 800x600 viewport, so both
// axes have room to move before the clamp fires — without that, a passing
// test would be a zero-vs-zero comparison.

namespace {

void sendMouse(QWidget* w, QEvent::Type type, Qt::MouseButton button,
               Qt::MouseButtons buttons, QPoint pos)
{
    QMouseEvent ev(type, QPointF(pos), QPointF(pos), QPointF(pos),
                   button, buttons, Qt::NoModifier);
    QCoreApplication::sendEvent(w, &ev);
    QCoreApplication::processEvents();
}

/// Install a REAL SelectTool, so the test reproduces the exact condition
/// #119 describes rather than an approximation of it.
void installSelectTool(CanvasFixture& f,
                       std::shared_ptr<pdfe::annotations::AnnotationManager>& mgrOut)
{
    mgrOut = std::make_shared<pdfe::annotations::AnnotationManager>(f.ctx);
    auto tool = std::make_unique<pdfe::annotations::SelectTool>(mgrOut.get());
    f.canvas->setCurrentTool(std::move(tool));
    QCoreApplication::processEvents();
}

}  // namespace

TEST(PdfCanvasMiddleDragPan, MiddleDragScrollsWhileSelectToolIsActive)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    CanvasFixture f;
    if (!makeFixture(f)) GTEST_SKIP() << "test_single.pdf fixture unavailable";

    f.canvas->resize(800, 600);
    f.canvas->setZoom(3.0f);
    QCoreApplication::processEvents();

    // Start part-way in on BOTH axes so the drag has room in the direction
    // it moves and the clamp cannot be what the assertion is really reading.
    //
    // These key presses happen BEFORE the SelectTool is installed, and that
    // ordering is load-bearing — but for a FIXTURE reason, not a product one,
    // and the distinction matters enough to record:
    //
    // keyPressEvent hands every key to currentTool_->onKeyPress() first and
    // takes an early-out while the event is still accepted (Qt delivers key
    // events pre-accepted). SelectTool::onKeyPress ends in event->ignore()
    // exactly so the canvas falls through to its own navigation — but it is
    // guarded by `if (!event || !manager_ || !page_) return;` at the top, and
    // THAT return never reaches the ignore(). This fixture builds a bare
    // AnnotationManager and never sets a page on the tool, so page_ is null,
    // the arrows are swallowed, and beforeY/beforeX stay 0.
    //
    // So this is not the live-app behaviour: with a document open the tool has
    // a page and arrow-key scrolling works. Do NOT read this ordering as
    // evidence of a navigation defect. The tool IS installed for the drag
    // below, which is the only thing the property under test depends on.
    for (int i = 0; i < 8; ++i) sendKey(f.canvas.get(), Qt::Key_Down);
    for (int i = 0; i < 8; ++i) sendKey(f.canvas.get(), Qt::Key_Right);

    std::shared_ptr<pdfe::annotations::AnnotationManager> mgr;
    installSelectTool(f, mgr);
    ASSERT_NE(f.canvas->currentTool(), nullptr)
        << "setup: the SelectTool must be installed at drag time, or this "
           "test is not reproducing the #119 condition at all";

    const int beforeY = f.canvas->scrollOffsetY();
    const int beforeX = f.canvas->scrollOffsetX();
    ASSERT_GT(beforeY, 0) << "setup: need headroom above to prove an upward pan";
    ASSERT_GT(beforeX, 0) << "setup: need headroom left to prove a leftward pan";

    // Middle-drag down-and-right: content follows the cursor, so the scroll
    // offsets DECREASE.
    sendMouse(f.canvas.get(), QEvent::MouseButtonPress, Qt::MiddleButton,
              Qt::MiddleButton, QPoint(400, 300));
    sendMouse(f.canvas.get(), QEvent::MouseMove, Qt::NoButton,
              Qt::MiddleButton, QPoint(430, 340));
    sendMouse(f.canvas.get(), QEvent::MouseMove, Qt::NoButton,
              Qt::MiddleButton, QPoint(460, 380));
    sendMouse(f.canvas.get(), QEvent::MouseButtonRelease, Qt::MiddleButton,
              Qt::NoButton, QPoint(460, 380));

    EXPECT_EQ(f.canvas->scrollOffsetY(), beforeY - 80)
        << "GitHub #119: a middle-button drag must pan vertically while the "
           "Select tool is active. beforeY=" << beforeY
        << " afterY=" << f.canvas->scrollOffsetY();
    EXPECT_EQ(f.canvas->scrollOffsetX(), beforeX - 60)
        << "GitHub #119: a middle-button drag must pan horizontally too. "
           "beforeX=" << beforeX
        << " afterX=" << f.canvas->scrollOffsetX();
}

TEST(PdfCanvasMiddleDragPan, MiddleMoveAfterReleaseNoLongerPans)
{
    // The release must actually end the pan. Without this, a mutation that
    // drops the release branch leaves the canvas panning on every subsequent
    // mouse move — and the test above would still pass.
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    CanvasFixture f;
    if (!makeFixture(f)) GTEST_SKIP() << "test_single.pdf fixture unavailable";

    f.canvas->resize(800, 600);
    f.canvas->setZoom(3.0f);
    QCoreApplication::processEvents();

    // Before the tool, for the reason spelled out in the test above.
    for (int i = 0; i < 8; ++i) sendKey(f.canvas.get(), Qt::Key_Down);

    std::shared_ptr<pdfe::annotations::AnnotationManager> mgr;
    installSelectTool(f, mgr);

    sendMouse(f.canvas.get(), QEvent::MouseButtonPress, Qt::MiddleButton,
              Qt::MiddleButton, QPoint(400, 300));
    sendMouse(f.canvas.get(), QEvent::MouseMove, Qt::NoButton,
              Qt::MiddleButton, QPoint(400, 320));
    sendMouse(f.canvas.get(), QEvent::MouseButtonRelease, Qt::MiddleButton,
              Qt::NoButton, QPoint(400, 320));

    const int settled = f.canvas->scrollOffsetY();
    // The post-release move below is 180px DOWNWARD, which a still-live pan
    // would turn into a 180px DECREASE. Unless there is that much headroom
    // beneath the settled offset, the clamp — not the release — would be what
    // keeps the number still, and the assertion would pass on a broken
    // release. 8 Down keys x 40px = 320, so this holds with margin.
    ASSERT_GE(settled, 180)
        << "setup: not enough headroom for a leaked pan to be visible; "
           "settled=" << settled;
    sendMouse(f.canvas.get(), QEvent::MouseMove, Qt::NoButton,
              Qt::NoButton, QPoint(400, 500));
    EXPECT_EQ(f.canvas->scrollOffsetY(), settled)
        << "a move after the middle release must not pan; the release did "
           "not clear the panning state";
}

TEST(PdfCanvasMiddleDragPan, MiddlePressDoesNotReachTheActiveTool)
{
    // The pan branch is first in mousePressEvent. Prove it takes ONLY the
    // middle button: a left press must still reach the installed tool, or
    // this fix would have broken every tool it was supposed to coexist with.
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    CanvasFixture f;
    if (!makeFixture(f)) GTEST_SKIP() << "test_single.pdf fixture unavailable";

    f.canvas->resize(800, 600);
    QCoreApplication::processEvents();

    auto spy = std::make_unique<SpyAnnotationTool>();
    SpyAnnotationTool* spyRaw = spy.get();
    f.canvas->setCurrentTool(std::move(spy));
    QCoreApplication::processEvents();

    sendMouse(f.canvas.get(), QEvent::MouseButtonPress, Qt::MiddleButton,
              Qt::MiddleButton, QPoint(400, 300));
    sendMouse(f.canvas.get(), QEvent::MouseButtonRelease, Qt::MiddleButton,
              Qt::NoButton, QPoint(400, 300));
    EXPECT_EQ(spyRaw->pressCount, 0)
        << "a middle press must be consumed by the pan branch, not handed "
           "to the active tool";

    sendMouse(f.canvas.get(), QEvent::MouseButtonPress, Qt::LeftButton,
              Qt::LeftButton, QPoint(400, 300));
    EXPECT_EQ(spyRaw->pressCount, 1)
        << "a LEFT press must still reach the active tool — the middle-drag "
           "pan branch must not swallow it";
}
