// -*- coding: utf-8 -*-
//
// GitHub #98 / WIN-74: the main window and the canvas must remain correctly
// laid out across a LIVE display-scaling change.
//
// SCOPE — WHY THIS IS AN IN-PROCESS TEST AND NOT AN RDP ONE
// ---------------------------------------------------------
// WIN-74's headline is an ``mstsc`` disconnect/reconnect, and the disconnect
// half is not automatable on this host: GitHub #132 measured it directly on
// 2026-09-04 — ``OpenInputDesktop`` starts failing one second after the WTS
// state goes Disconnected and is still failing eleven seconds AFTER the
// session reports Active again (evidence in docs/audit/issue-132/ and
// issue-133/). The DPI/resolution half of the row needs no disconnect at
// all, which is why it is carved out here.
//
// The change is driven through Qt's own live-DPI notification,
// ``QWindowSystemInterface::handleScreenLogicalDotsPerInchChange()`` — the
// exact call the Windows platform plugin makes when a monitor's scaling
// changes underneath a running app. Nothing here calls
// ``ChangeDisplaySettingsEx``: switching the real display mode on this host
// would resize every other window on the desktop, including whatever else
// the suite is driving at the time.
//
// WHAT IS ASSERTED — three properties, three different failure modes
// -------------------------------------------------------------------
//   1. DELIVERY + STABILITY. The screen really does report the new DPI and
//      really does emit ``logicalDotsPerInchChanged`` (asserted with a
//      QSignalSpy, so the test cannot pass by not-changing-anything), and
//      the notification on its own must NOT disturb the layout: the canvas
//      keeps its geometry and the page keeps its exact logical rectangle.
//      A DPI change that silently re-scrolled or re-centred the page would
//      move every hit-test target under the user's cursor.
//
//   2. RELAYOUT. Windows follows a DPI change by handing the application a
//      new suggested window rectangle. After that resize the main window
//      must have relaid out — the canvas still fills the central area, is
//      not clipped by the docks or toolbars, and the page is re-centred for
//      the new viewport width. A canvas that keeps painting the page at its
//      old horizontal offset is the "window survived but the layout did
//      not" defect this row is about.
//
//   3. DPR INVARIANCE. The device-pixel ratio that accompanies the DPI
//      change must reach the render path (effectiveRenderZoom scales) while
//      staying out of layout space (the page's logical rectangle and the
//      PDF<->canvas mapping are byte-identical). Over-application and
//      under-application therefore fail different assertions.
//
// Complements tests/test_dpi_scale_geometry.cpp, which covers the tile grid
// for a bare PdfCanvas. Nothing there involves a MainWindow, a QScreen DPI
// change, or the window-level layout, which is what #98 asks about.

#include <gtest/gtest.h>

#include "ui/MainWindow.h"
#include "ui/PdfCanvas.h"

#include <QApplication>
#include <QGuiApplication>
#include <QPoint>
#include <QRect>
#include <QRectF>
#include <QScreen>
#include <QSignalSpy>
#include <QSize>
#include <QWidget>
#include <QWindow>

#include <qpa/qwindowsysteminterface.h>

#include <filesystem>

using pdfe::ui::MainWindow;
using pdfe::ui::PdfCanvas;

namespace {

// Restores the screen's logical DPI on the way out. QScreen state is
// process-global: leaving a doubled DPI behind would follow every later test
// in the same pdfe_tests.exe.
class ScreenDpiGuard {
public:
    explicit ScreenDpiGuard(QScreen* screen)
        : screen_(screen),
          dpiX_(screen ? screen->logicalDotsPerInchX() : 0.0),
          dpiY_(screen ? screen->logicalDotsPerInchY() : 0.0) {}

    ~ScreenDpiGuard() {
        if (!screen_) return;
        QWindowSystemInterface::handleScreenLogicalDotsPerInchChange(
            screen_, dpiX_, dpiY_);
        QWindowSystemInterface::flushWindowSystemEvents();
        QCoreApplication::processEvents();
    }

    ScreenDpiGuard(const ScreenDpiGuard&) = delete;
    ScreenDpiGuard& operator=(const ScreenDpiGuard&) = delete;

    qreal originalDpiX() const { return dpiX_; }

private:
    QScreen* screen_;
    qreal dpiX_;
    qreal dpiY_;
};

// Everything the layout oracles compare, captured in one shot.
struct LayoutSnapshot {
    QRect  canvasInWindow;
    QSize  canvasSize;
    QPoint pageOrigin;
    int    pageWidth = 0;
    int    pageHeight = 0;
    QRectF pdfFromCanvas;
    QRectF canvasFromPdf;
    float  effectiveRenderZoom = 0.0f;
};

bool capture(MainWindow& mw, PdfCanvas* canvas, LayoutSnapshot& out)
{
    if (!canvas) return false;
    out.canvasInWindow = QRect(canvas->mapTo(&mw, QPoint(0, 0)), canvas->size());
    out.canvasSize = canvas->size();
    if (!canvas->getPageScreenRect(0, out.pageOrigin, out.pageWidth,
                                   out.pageHeight)) {
        return false;
    }
    out.pdfFromCanvas = canvas->pdfRectFromCanvas(0, QRectF(20, 30, 120, 90));
    out.canvasFromPdf = canvas->canvasRectFromPdf(0, QRectF(10, 10, 50, 40));
    out.effectiveRenderZoom = canvas->effectiveRenderZoom();
    return true;
}

class DpiChangeRelayoutTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_NE(qApp, nullptr) << "QApplication is created by test_main.cpp";
        const std::filesystem::path pdf =
            std::filesystem::path(TEST_FIXTURES_DIR) / "test_single.pdf";
        ASSERT_TRUE(std::filesystem::exists(pdf))
            << "fixture required: " << pdf.string();

        mw_ = std::make_unique<MainWindow>();
        mw_->resize(1100, 820);
        mw_->show();
        QCoreApplication::processEvents();

        mw_->openFile(QString::fromStdString(pdf.string()));
        QCoreApplication::processEvents();

        canvas_ = mw_->findChild<PdfCanvas*>("pdfCanvas");
        ASSERT_NE(canvas_, nullptr) << "pdfCanvas not found in MainWindow";
        centralArea_ = mw_->findChild<QWidget*>("pdfCentralArea");
        ASSERT_NE(centralArea_, nullptr) << "pdfCentralArea not found";

        // Pin the zoom so page size is a constant of the test and any change
        // in the page's on-screen rectangle is a LAYOUT effect.
        canvas_->setZoom(1.0f);
        canvas_->setDevicePixelRatioForTest(1.0f);
        QCoreApplication::processEvents();

        ASSERT_TRUE(capture(*mw_, canvas_, baseline_))
            << "canvas reported no page rectangle for page 0 — the document "
               "did not load, so nothing below would be measuring layout";
        ASSERT_GT(baseline_.pageWidth, 0);
        ASSERT_GT(baseline_.canvasSize.width(), 0);
    }

    void TearDown() override
    {
        if (mw_) {
            mw_->close();
            QCoreApplication::processEvents();
            mw_.reset();
        }
    }

    QScreen* screen() const
    {
        if (mw_ && mw_->windowHandle() && mw_->windowHandle()->screen()) {
            return mw_->windowHandle()->screen();
        }
        return QGuiApplication::primaryScreen();
    }

    // The page must sit centred in the canvas: this is recomputed live from
    // the widget width, so it is the observable that tells a relaid-out
    // canvas from one still painting for the previous viewport.
    void expectPageCentred(const LayoutSnapshot& s, const char* when) const
    {
        const int expectedLeft = (s.canvasSize.width() - s.pageWidth) / 2;
        EXPECT_LE(std::abs(s.pageOrigin.x() - expectedLeft), 1)
            << when << ": page left is " << s.pageOrigin.x()
            << " but a canvas " << s.canvasSize.width()
            << "px wide holding a " << s.pageWidth
            << "px page centres it at " << expectedLeft;
    }

    void expectCanvasFillsCentralArea(const char* when) const
    {
        EXPECT_EQ(canvas_->width(), centralArea_->width())
            << when << ": canvas width does not match the central area";
        EXPECT_LE(canvas_->height(), centralArea_->height())
            << when << ": canvas is taller than the central area";
        EXPECT_GE(canvas_->height(), centralArea_->height() - 2)
            << when << ": canvas does not fill the central area vertically";
        const QRect inWindow(canvas_->mapTo(mw_.get(), QPoint(0, 0)),
                             canvas_->size());
        EXPECT_TRUE(mw_->rect().contains(inWindow))
            << when << ": canvas " << inWindow.x() << "," << inWindow.y()
            << " " << inWindow.width() << "x" << inWindow.height()
            << " is clipped by the window " << mw_->width() << "x"
            << mw_->height();
        EXPECT_GT(inWindow.top(), 0)
            << when << ": canvas starts at the very top of the window — the "
                       "menu bar / toolbars are not laid out above it";
    }

    std::unique_ptr<MainWindow> mw_;
    PdfCanvas* canvas_ = nullptr;
    QWidget* centralArea_ = nullptr;
    LayoutSnapshot baseline_;
};

}  // namespace

// ── (1) the DPI change is delivered, and on its own disturbs nothing ──────

TEST_F(DpiChangeRelayoutTest, ScreenDpiChangeIsDeliveredAndLeavesLayoutIntact)
{
    QScreen* scr = screen();
    ASSERT_NE(scr, nullptr);

    ScreenDpiGuard guard(scr);
    QSignalSpy spy(scr, &QScreen::logicalDotsPerInchChanged);
    ASSERT_TRUE(spy.isValid());

    const qreal before = scr->logicalDotsPerInch();
    ASSERT_GT(before, 0.0);

    // THE LIVE DPI CHANGE — the same entry point the Windows platform plugin
    // uses when the user changes display scaling under a running app.
    QWindowSystemInterface::handleScreenLogicalDotsPerInchChange(
        scr, before * 2.0, before * 2.0);
    QWindowSystemInterface::flushWindowSystemEvents();
    QCoreApplication::processEvents();

    // Delivery. Without this the whole test would be asserting that nothing
    // changed after nothing happened.
    ASSERT_EQ(spy.count(), 1)
        << "QScreen::logicalDotsPerInchChanged did not fire — the DPI change "
           "was never delivered, so the assertions below prove nothing";
    EXPECT_NEAR(scr->logicalDotsPerInch(), before * 2.0, 0.5);

    LayoutSnapshot after;
    ASSERT_TRUE(capture(*mw_, canvas_, after))
        << "the canvas stopped reporting a page rectangle after the DPI change";

    expectCanvasFillsCentralArea("after the DPI notification");
    expectPageCentred(after, "after the DPI notification");

    // A DPI notification alone must not move anything: the window was not
    // resized, so every logical coordinate has to be exactly what it was.
    EXPECT_EQ(after.canvasSize, baseline_.canvasSize);
    EXPECT_EQ(after.pageOrigin, baseline_.pageOrigin);
    EXPECT_EQ(after.pageWidth, baseline_.pageWidth);
    EXPECT_EQ(after.pageHeight, baseline_.pageHeight);
    EXPECT_NEAR(after.pdfFromCanvas.x(), baseline_.pdfFromCanvas.x(), 1e-3);
    EXPECT_NEAR(after.pdfFromCanvas.y(), baseline_.pdfFromCanvas.y(), 1e-3);
    EXPECT_NEAR(after.canvasFromPdf.x(), baseline_.canvasFromPdf.x(), 1e-3);
    EXPECT_NEAR(after.canvasFromPdf.y(), baseline_.canvasFromPdf.y(), 1e-3);
}

// ── (2) the window resize a DPI switch brings with it must relayout ───────

TEST_F(DpiChangeRelayoutTest, MainWindowAndCanvasRelayoutForTheDpiSuggestedRect)
{
    QScreen* scr = screen();
    ASSERT_NE(scr, nullptr);
    ScreenDpiGuard guard(scr);

    const qreal before = scr->logicalDotsPerInch();
    QWindowSystemInterface::handleScreenLogicalDotsPerInchChange(
        scr, before * 1.5, before * 1.5);
    QWindowSystemInterface::flushWindowSystemEvents();
    QCoreApplication::processEvents();

    // Windows follows WM_DPICHANGED with a suggested window rectangle scaled
    // by the same factor. Apply it.
    mw_->resize(mw_->width() * 3 / 2, mw_->height() * 3 / 2);
    QCoreApplication::processEvents();

    LayoutSnapshot after;
    ASSERT_TRUE(capture(*mw_, canvas_, after));

    ASSERT_GT(after.canvasSize.width(), baseline_.canvasSize.width())
        << "the canvas did not grow with the window — the resize never "
           "reached the central area, so the relayout oracle below is blind";

    expectCanvasFillsCentralArea("after the DPI-suggested resize");
    expectPageCentred(after, "after the DPI-suggested resize");

    // The zoom is pinned, so the page keeps its size and only its position
    // may change. Both halves matter: a canvas that re-centres but also
    // silently rescales the page is just as wrong.
    EXPECT_EQ(after.pageWidth, baseline_.pageWidth);
    EXPECT_EQ(after.pageHeight, baseline_.pageHeight);
    EXPECT_NE(after.pageOrigin.x(), baseline_.pageOrigin.x())
        << "the page did not move when the viewport got wider — the canvas "
           "is still positioning it for the pre-DPI-change width";
}

// ── (3) the DPR half reaches rendering and stays out of layout ────────────

TEST_F(DpiChangeRelayoutTest, DevicePixelRatioFromTheDpiChangeStaysOutOfLayout)
{
    QScreen* scr = screen();
    ASSERT_NE(scr, nullptr);
    ScreenDpiGuard guard(scr);

    const qreal before = scr->logicalDotsPerInch();
    QWindowSystemInterface::handleScreenLogicalDotsPerInchChange(
        scr, before * 2.0, before * 2.0);
    QWindowSystemInterface::flushWindowSystemEvents();
    QCoreApplication::processEvents();

    // The ratio half of the same change, through the product's own seam
    // (it mirrors updateDevicePixelRatio(), the slot a real screen change
    // drives).
    canvas_->setDevicePixelRatioForTest(2.0f);
    QCoreApplication::processEvents();

    // ...followed by the window resize Windows sends with a DPI change. This
    // is NOT decoration: updateDocumentLayout() only re-runs on a resize (or
    // a zoom/document change), so a ratio that has leaked into layout space
    // is INVISIBLE until something recomputes the layout. A first draft of
    // this test asserted invariance without the resize and a mutant that
    // multiplied the page extent by devicePixelRatio_ in
    // updateDocumentLayout() sailed through it.
    mw_->resize(mw_->width() + 40, mw_->height() + 30);
    QCoreApplication::processEvents();

    LayoutSnapshot after;
    ASSERT_TRUE(capture(*mw_, canvas_, after));

    // Reached the render path.
    EXPECT_NEAR(after.effectiveRenderZoom,
                2.0f * baseline_.effectiveRenderZoom, 1e-4f)
        << "the device-pixel ratio never reached the render zoom: the page "
           "would keep rasterising at the old resolution (blurry on a "
           "rescaled monitor)";

    // Stayed out of layout space. The window is 40x30 larger than the
    // baseline, so the page may have MOVED — but at a pinned zoom its
    // logical extent, and the PDF<->canvas mapping built from it, must be
    // exactly what they were. Doubling either is the HiDPI defect where the
    // page jumps and every click lands somewhere else.
    expectCanvasFillsCentralArea("after the DPR change");
    expectPageCentred(after, "after the DPR change");
    EXPECT_EQ(after.pageWidth, baseline_.pageWidth)
        << "the device-pixel ratio leaked into the page LAYOUT extent";
    EXPECT_EQ(after.pageHeight, baseline_.pageHeight)
        << "the device-pixel ratio leaked into the page LAYOUT extent";
    EXPECT_NEAR(after.pdfFromCanvas.width(), baseline_.pdfFromCanvas.width(),
                1e-3);
    EXPECT_NEAR(after.canvasFromPdf.width(), baseline_.canvasFromPdf.width(),
                1e-3);
}
