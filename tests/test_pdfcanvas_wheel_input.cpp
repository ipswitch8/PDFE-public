// -*- coding: utf-8 -*-
// Copyright (c) 2025 - PDFE Project
//
// Pipeline backlog-workable-p82 phase-14 -- Windows input events on PdfCanvas.
// GitHub #88 (precision / pixelDelta scrolling) and #89 (horizontal scrolling).
//
// WHY THESE TESTS BUILD THE QWheelEvent BY HAND
// ---------------------------------------------
// PdfCanvas::scrollByPixels() is an existing Q_INVOKABLE test seam, but it
// constructs its synthetic wheel event with BOTH fields populated:
//
//     QPoint(0, pixelDelta),
//     QPoint(0, pixelDelta * 120 / 15),   // approx convert
//
// so angleDelta() is never zero on that path. A test driven through it
// therefore CANNOT reproduce #88 -- it would pass against the broken handler
// and prove nothing. Every test below constructs the QWheelEvent directly so
// that the #88 case has angleDelta() genuinely QPoint(0, 0), and delivers it
// with QApplication::sendEvent() (the same delivery scrollByPixels uses).
//
// CONTROLS
// --------
// Each defect assertion is paired with a control so that a build which broke
// ALL scrolling, or one whose handler just scrolls vertically on any input,
// cannot pass:
//   * OrdinaryWheelStillScrolls   -- the classic 120-unit notch still moves the
//                                    view by exactly 120 px (no regression).
//   * HorizontalIsDistinctFromVertical -- a horizontal-only event moves X and
//                                    leaves Y untouched; a vertical-only event
//                                    moves Y and leaves X untouched.

#include <gtest/gtest.h>

#include "ui/PdfCanvas.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"

#include <QApplication>
#include <QPoint>
#include <QPointF>
#include <QWheelEvent>

#include <filesystem>
#include <memory>

using namespace pdfe::ui;

namespace {

std::filesystem::path fixturesDir()
{
    return std::filesystem::path(TEST_FIXTURES_DIR);
}

// Deliver a wheel event with EXACTLY the pixelDelta / angleDelta asked for.
// Nothing here derives one field from the other -- that derivation is the
// trap this whole file exists to avoid.
void sendWheel(PdfCanvas& canvas,
               QPoint pixelDelta,
               QPoint angleDelta,
               Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    const QPointF local(canvas.width() / 2.0, canvas.height() / 2.0);
    const QPointF global = canvas.mapToGlobal(local.toPoint());
    QWheelEvent evt(local, global,
                    pixelDelta, angleDelta,
                    Qt::NoButton, mods,
                    Qt::ScrollUpdate, false);
    QApplication::sendEvent(&canvas, &evt);
}

} // namespace

class PdfCanvasWheelInputTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        const auto fixture = fixturesDir() / "test_10page.pdf";
        if (!std::filesystem::exists(fixture)) {
            GTEST_SKIP() << "test_10page.pdf fixture not available";
        }
        ctx_ = std::make_shared<pdfe::core::MuPdfContext>();
        doc_ = pdfe::core::Document::open(ctx_, fixture);
        ASSERT_NE(doc_, nullptr);
        ASSERT_GE(doc_->pageCount(), 2) << "fixture must be multi-page";

        canvas_ = std::make_unique<PdfCanvas>();
        // Deliberately SMALLER than one page at the default zoom, so that both
        // totalDocumentHeight_ - height() and totalDocumentWidth_ - width() are
        // positive and the clamp in wheelEvent() cannot mask a real scroll.
        canvas_->resize(200, 300);
        canvas_->setDocument(doc_.get(), nullptr, nullptr);
        QApplication::processEvents();  // flush setDocument's deferred re-layout

        // Precondition: the view must actually be scrollable on BOTH axes,
        // otherwise every assertion below would be vacuously "no movement".
        ASSERT_TRUE(canvas_->scrollByPixels(-40));
        ASSERT_GT(canvas_->scrollOffsetY(), 0)
            << "fixture/viewport combination is not vertically scrollable; the "
               "wheel assertions would be vacuous";
        // Return to the top using the SAME pre-existing seam (it works on the
        // unfixed handler too, so the setup never depends on the fix under
        // test). goToPage(0) would be a no-op here -- currentPage_ is already 0.
        ASSERT_TRUE(canvas_->scrollByPixels(100000));
        ASSERT_EQ(canvas_->scrollOffsetY(), 0);
        ASSERT_EQ(canvas_->scrollOffsetX(), 0);
    }

    void TearDown() override
    {
        if (canvas_) {
            canvas_->clearDocument();
            canvas_.reset();
        }
        doc_.reset();
        ctx_.reset();
    }

    std::shared_ptr<pdfe::core::MuPdfContext> ctx_;
    std::unique_ptr<pdfe::core::Document> doc_;
    std::unique_ptr<PdfCanvas> canvas_;
};

// CONTROL for both defect rows. An ordinary notched wheel (angleDelta 120,
// pixelDelta empty) must still scroll, and by exactly the amount it did
// before the fix -- 120 px per notch. A build that broke all scrolling, or
// that changed the notch-to-pixel mapping, fails here.
TEST_F(PdfCanvasWheelInputTest, OrdinaryWheelStillScrollsByOneNotch)
{
    const int y0 = canvas_->scrollOffsetY();
    ASSERT_EQ(y0, 0) << "test starts at the top of the document";

    sendWheel(*canvas_, QPoint(0, 0), QPoint(0, -120));

    EXPECT_EQ(canvas_->scrollOffsetY(), y0 + 120)
        << "A 120-unit notch must scroll down by 120 px, exactly as it did "
           "before the #88/#89 fix. Any other value is a regression of "
           "ordinary-wheel behaviour.";
    EXPECT_EQ(canvas_->scrollOffsetX(), 0)
        << "A purely vertical notch must not pan horizontally.";
}

// GitHub #88 -- precision / high-resolution device. angleDelta is genuinely
// ZERO; only pixelDelta carries the movement. Before the fix delta was 0 and
// the canvas did not move at all.
TEST_F(PdfCanvasWheelInputTest, PrecisionPixelDeltaScrollsVertically)
{
    const int y0 = canvas_->scrollOffsetY();
    ASSERT_EQ(y0, 0);

    sendWheel(*canvas_, QPoint(0, -37), QPoint(0, 0));

    EXPECT_EQ(canvas_->scrollOffsetY(), y0 + 37)
        << "A precision device reports pixelDelta with angleDelta ZERO. "
           "Reading angleDelta alone makes such input a complete no-op "
           "(GitHub #88).";
    EXPECT_EQ(canvas_->scrollOffsetX(), 0)
        << "A vertical-only precision scroll must not pan horizontally.";
}

// GitHub #88, other direction: scrolling back up with pixelDelta only.
TEST_F(PdfCanvasWheelInputTest, PrecisionPixelDeltaScrollsBothDirections)
{
    sendWheel(*canvas_, QPoint(0, -100), QPoint(0, 0));
    const int down = canvas_->scrollOffsetY();
    ASSERT_EQ(down, 100);

    sendWheel(*canvas_, QPoint(0, 60), QPoint(0, 0));
    EXPECT_EQ(canvas_->scrollOffsetY(), 40)
        << "Precision scroll must work symmetrically, not only downwards.";
}

// GitHub #89 -- horizontal wheel (a tilt wheel reports angleDelta.x()).
// Before the fix angleDelta().x() was never read anywhere in the handler.
TEST_F(PdfCanvasWheelInputTest, HorizontalAngleDeltaPansHorizontally)
{
    const int x0 = canvas_->scrollOffsetX();
    ASSERT_EQ(x0, 0);

    sendWheel(*canvas_, QPoint(0, 0), QPoint(-120, 0));

    EXPECT_GT(canvas_->scrollOffsetX(), x0)
        << "Horizontal wheel input (angleDelta.x) was silently discarded "
           "(GitHub #89).";
}

// GitHub #88 + #89 together: a trackpad side-swipe -- horizontal pixelDelta
// with angleDelta entirely zero. This is the case neither original branch
// could ever see.
TEST_F(PdfCanvasWheelInputTest, HorizontalPixelDeltaPansHorizontally)
{
    const int x0 = canvas_->scrollOffsetX();
    ASSERT_EQ(x0, 0);

    sendWheel(*canvas_, QPoint(-25, 0), QPoint(0, 0));

    EXPECT_EQ(canvas_->scrollOffsetX(), x0 + 25)
        << "A trackpad side-swipe carries only pixelDelta.x(); both original "
           "branches ignored it entirely.";
}

// CONTROL for #89: the axes must be DISTINCT. A handler that simply scrolls
// vertically on any input, or that copies one axis onto the other, fails here.
TEST_F(PdfCanvasWheelInputTest, HorizontalIsDistinctFromVertical)
{
    ASSERT_EQ(canvas_->scrollOffsetX(), 0);
    ASSERT_EQ(canvas_->scrollOffsetY(), 0);

    // Horizontal only -> X moves, Y does not.
    sendWheel(*canvas_, QPoint(-30, 0), QPoint(0, 0));
    const int xAfterH = canvas_->scrollOffsetX();
    const int yAfterH = canvas_->scrollOffsetY();
    EXPECT_GT(xAfterH, 0) << "horizontal input must pan X";
    EXPECT_EQ(yAfterH, 0)
        << "horizontal input must NOT scroll vertically -- a handler that "
           "routes every wheel event to the vertical axis would pass the #89 "
           "assertion by accident without this control.";

    // Vertical only -> Y moves, X stays exactly where the horizontal step
    // left it (asserted against xAfterH, an independent earlier measurement).
    sendWheel(*canvas_, QPoint(0, -30), QPoint(0, 0));
    EXPECT_GT(canvas_->scrollOffsetY(), 0) << "vertical input must scroll Y";
    EXPECT_EQ(canvas_->scrollOffsetX(), xAfterH)
        << "vertical input must NOT pan horizontally.";
}

// Diagonal precision gesture: both axes carried by pixelDelta at once.
TEST_F(PdfCanvasWheelInputTest, DiagonalPixelDeltaMovesBothAxes)
{
    sendWheel(*canvas_, QPoint(-20, -45), QPoint(0, 0));

    EXPECT_EQ(canvas_->scrollOffsetX(), 20);
    EXPECT_EQ(canvas_->scrollOffsetY(), 45);
}

// Ctrl+wheel zoom must also honour a precision device (angleDelta zero).
TEST_F(PdfCanvasWheelInputTest, CtrlPrecisionWheelZooms)
{
    const float z0 = canvas_->zoom();
    sendWheel(*canvas_, QPoint(0, 40), QPoint(0, 0), Qt::ControlModifier);
    EXPECT_GT(canvas_->zoom(), z0)
        << "Ctrl + precision wheel up must zoom in; the Ctrl branch read "
           "angleDelta().y() only.";

    const float z1 = canvas_->zoom();
    sendWheel(*canvas_, QPoint(0, -40), QPoint(0, 0), Qt::ControlModifier);
    EXPECT_LT(canvas_->zoom(), z1) << "Ctrl + precision wheel down must zoom out.";
}

// CONTROL for the Ctrl branch: the classic notched Ctrl+wheel must still zoom.
TEST_F(PdfCanvasWheelInputTest, CtrlOrdinaryWheelStillZooms)
{
    const float z0 = canvas_->zoom();
    sendWheel(*canvas_, QPoint(0, 0), QPoint(0, 120), Qt::ControlModifier);
    EXPECT_GT(canvas_->zoom(), z0) << "Ctrl + notched wheel up must still zoom in.";
}

// The handler used to call accept() on EVERY path, including the ones that
// changed nothing -- which is precisely why #88 and #89 were silent. An event
// that moves nothing must now be left un-accepted so it can propagate.
TEST_F(PdfCanvasWheelInputTest, NoOpWheelEventIsNotAccepted)
{
    const QPointF local(canvas_->width() / 2.0, canvas_->height() / 2.0);
    const QPointF global = canvas_->mapToGlobal(local.toPoint());

    QWheelEvent empty(local, global, QPoint(0, 0), QPoint(0, 0),
                      Qt::NoButton, Qt::NoModifier, Qt::ScrollUpdate, false);
    QApplication::sendEvent(canvas_.get(), &empty);
    EXPECT_FALSE(empty.isAccepted())
        << "A wheel event carrying no delta changes nothing and must not be "
           "swallowed.";

    // ... while a real scroll IS accepted (control: not everything is ignored).
    QWheelEvent real(local, global, QPoint(0, -30), QPoint(0, 0),
                     Qt::NoButton, Qt::NoModifier, Qt::ScrollUpdate, false);
    QApplication::sendEvent(canvas_.get(), &real);
    EXPECT_TRUE(real.isAccepted())
        << "An event that actually scrolled must still be accepted.";
}

// A canvas with no document must not crash and must not move -- the clamp
// bound is 0 on both axes.
TEST(PdfCanvasWheelInputNoDocTest, NoDocumentWheelIsHarmless)
{
    PdfCanvas canvas;
    canvas.resize(200, 300);

    const QPointF local(100, 150);
    QWheelEvent evt(local, canvas.mapToGlobal(local.toPoint()),
                    QPoint(-30, -30), QPoint(0, 0),
                    Qt::NoButton, Qt::NoModifier, Qt::ScrollUpdate, false);
    EXPECT_NO_THROW({ QApplication::sendEvent(&canvas, &evt); });
    EXPECT_EQ(canvas.scrollOffsetX(), 0);
    EXPECT_EQ(canvas.scrollOffsetY(), 0);
}
