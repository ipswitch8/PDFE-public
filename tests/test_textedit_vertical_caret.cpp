// Vertical-writing-mode & RTL overlay caret/selection geometry (R2-21b).
//
// docs/audit/round-2/remediation-addendum.md section F2 "DEFERRED (b)": the on-screen
// text-edit overlay built its caret and selection rectangles entirely around a
// horizontal, left-to-right X-axis (penX accumulation, left + advance). That is
// geometrically wrong for
//   * vertical-writing (TTB / CJK wmode=1) text -- the caret must step in Y with
//     the column stacked in X, and
//   * right-to-left (Arabic / Hebrew) text -- increasing LOGICAL index must move
//     the caret toward the LEFT visual edge, and a selection of a logical prefix
//     must cover the RIGHT visual span.
//
// TextEditTool::overlayCaretLine / overlaySelectionRects are the pure,
// window-free geometry seam the live overlay paint now consumes. They take the
// per-character along-axis advances + the span's screen bbox + an
// OverlayGeometryParams{vertical,rtl} and return VISUAL geometry. Being pure,
// they are asserted here directly (no Document, no live window, no fonts).
//
// RED (pre-fix): the overlay advanced the caret in X regardless of writing mode
// and ignored RTL order -- so the vertical Y-advance assertions and the RTL
// leftward / right-span assertions below fail. GREEN requires the axis-aware +
// mirrored geometry.
//
// Mutation check (re-reds): make overlayCaretLine / overlaySelectionRects ignore
// `params` and always take the LTR branch (x = left + pre; rect grows rightward
// from left) -- VerticalCaretStepsDownInY, RtlCaretMovesLeft and
// RtlSelectionCoversRightSpan then fail while HorizontalLtr* still pass.

#include <gtest/gtest.h>

#include "textedit/TextEditTool.h"

#include <QPointF>
#include <QRectF>

#include <string>
#include <vector>

using pdfe::textedit::TextEditTool;
using Params = pdfe::textedit::TextEditTool::OverlayGeometryParams;

namespace {

// Three equal 10px advances; a 30x20 (or 30x60) screen bbox at origin (100,200).
std::vector<float> uniformAdvances() { return {10.0f, 10.0f, 10.0f}; }

constexpr double kEps = 1e-3;

} // namespace

// --- Horizontal LTR: regression lock (unchanged X-advance behaviour) ---------

TEST(TextEditVerticalCaret, HorizontalLtrCaretAdvancesInX) {
    const auto adv = uniformAdvances();
    const QRectF bbox(100.0, 200.0, 30.0, 20.0);
    Params p;  // vertical=false, rtl=false

    double prevX = -1e9;
    for (int i = 0; i <= 3; ++i) {
        auto [a, b] = TextEditTool::overlayCaretLine(adv, bbox, i, p);
        // Horizontal text -> vertical caret line (same X top and bottom).
        EXPECT_NEAR(a.x(), b.x(), kEps) << "i=" << i;
        // X grows to the right by the cumulative advance.
        EXPECT_NEAR(a.x(), 100.0 + 10.0 * i, kEps) << "i=" << i;
        // Caret spans the full bbox height, constant.
        EXPECT_NEAR(a.y(), 200.0, kEps);
        EXPECT_NEAR(b.y(), 220.0, kEps);
        EXPECT_GT(a.x(), prevX) << "monotonic increasing X, i=" << i;
        prevX = a.x();
    }
}

TEST(TextEditVerticalCaret, HorizontalLtrSelectionGrowsRightward) {
    const auto adv = uniformAdvances();
    const QRectF bbox(100.0, 200.0, 30.0, 20.0);
    Params p;

    auto rects = TextEditTool::overlaySelectionRects(adv, bbox, 0, 2, p);
    ASSERT_EQ(rects.size(), 1u);
    // [0,2) -> left edge, width = 20, full height.
    EXPECT_NEAR(rects[0].left(), 100.0, kEps);
    EXPECT_NEAR(rects[0].width(), 20.0, kEps);
    EXPECT_NEAR(rects[0].top(), 200.0, kEps);
    EXPECT_NEAR(rects[0].height(), 20.0, kEps);
}

// --- Vertical (TTB): caret steps in Y, column stacks in X --------------------

TEST(TextEditVerticalCaret, VerticalCaretStepsDownInY) {
    const auto adv = uniformAdvances();
    const QRectF bbox(100.0, 200.0, 30.0, 60.0);
    Params p;
    p.vertical = true;

    double prevY = -1e9;
    for (int i = 0; i <= 3; ++i) {
        auto [a, b] = TextEditTool::overlayCaretLine(adv, bbox, i, p);
        // Vertical text -> HORIZONTAL caret stroke (same Y left and right).
        EXPECT_NEAR(a.y(), b.y(), kEps) << "i=" << i;
        // Y advances downward by the cumulative advance.
        EXPECT_NEAR(a.y(), 200.0 + 10.0 * i, kEps) << "i=" << i;
        // X spans the column width, constant across indices.
        EXPECT_NEAR(a.x(), 100.0, kEps);
        EXPECT_NEAR(b.x(), 130.0, kEps);
        EXPECT_GT(a.y(), prevY) << "monotonic increasing Y, i=" << i;
        prevY = a.y();
    }
}

TEST(TextEditVerticalCaret, VerticalSelectionStacksVertically) {
    const auto adv = uniformAdvances();
    const QRectF bbox(100.0, 200.0, 30.0, 60.0);
    Params p;
    p.vertical = true;

    auto r0 = TextEditTool::overlaySelectionRects(adv, bbox, 0, 2, p);
    auto r1 = TextEditTool::overlaySelectionRects(adv, bbox, 1, 3, p);
    ASSERT_EQ(r0.size(), 1u);
    ASSERT_EQ(r1.size(), 1u);
    // Each rect spans the full column width and stacks in Y.
    EXPECT_NEAR(r0[0].left(), 100.0, kEps);
    EXPECT_NEAR(r0[0].width(), 30.0, kEps);
    EXPECT_NEAR(r0[0].top(), 200.0, kEps);
    EXPECT_NEAR(r0[0].height(), 20.0, kEps);
    // The [1,3) selection sits below the [0,2) selection.
    EXPECT_GT(r1[0].top(), r0[0].top());
    EXPECT_NEAR(r1[0].top(), 210.0, kEps);
    EXPECT_NEAR(r1[0].height(), 20.0, kEps);
    EXPECT_NEAR(r1[0].left(), 100.0, kEps);  // constant X (same column)
}

// --- RTL: increasing logical index moves the caret LEFT ----------------------

TEST(TextEditVerticalCaret, RtlCaretMovesLeft) {
    const auto adv = uniformAdvances();
    const QRectF bbox(100.0, 200.0, 30.0, 20.0);  // right edge = 130
    Params p;
    p.rtl = true;

    double prevX = 1e9;
    for (int i = 0; i <= 3; ++i) {
        auto [a, b] = TextEditTool::overlayCaretLine(adv, bbox, i, p);
        EXPECT_NEAR(a.x(), b.x(), kEps) << "i=" << i;
        // Logical index 0 sits at the RIGHT edge; index grows -> caret moves LEFT.
        EXPECT_NEAR(a.x(), 130.0 - 10.0 * i, kEps) << "i=" << i;
        EXPECT_LT(a.x(), prevX) << "monotonic decreasing X (RTL), i=" << i;
        prevX = a.x();
    }
}

TEST(TextEditVerticalCaret, RtlSelectionCoversRightSpan) {
    const auto adv = uniformAdvances();
    const QRectF bbox(100.0, 200.0, 30.0, 20.0);  // right edge = 130
    Params p;
    p.rtl = true;

    // A logical prefix [0,2) reads at the START of an RTL line = its RIGHT edge.
    auto rects = TextEditTool::overlaySelectionRects(adv, bbox, 0, 2, p);
    ASSERT_EQ(rects.size(), 1u);
    EXPECT_NEAR(rects[0].width(), 20.0, kEps);
    // The selection abuts the right visual edge of the span.
    EXPECT_NEAR(rects[0].right(), 130.0, kEps);
    EXPECT_NEAR(rects[0].left(), 110.0, kEps);
    EXPECT_NEAR(rects[0].height(), 20.0, kEps);
}

// --- Base-direction detection via the shaper's bidi --------------------------

TEST(TextEditVerticalCaret, BaseDirectionDetection) {
    // Latin -> LTR base.
    EXPECT_FALSE(TextEditTool::overlayBaseIsRtl(U"abc"));
    // Hebrew (aleph-bet-gimel, U+05D0..U+05D2) -> RTL base. Code points given
    // numerically to keep the source pure-ASCII (no non-ASCII literals).
    EXPECT_TRUE(TextEditTool::overlayBaseIsRtl(
        std::u32string{char32_t(0x05D0), char32_t(0x05D1), char32_t(0x05D2)}));
    // Arabic (alef U+0627, beh U+0628) -> RTL base.
    EXPECT_TRUE(TextEditTool::overlayBaseIsRtl(
        std::u32string{char32_t(0x0627), char32_t(0x0628)}));
    // Empty / neutral-only -> LTR.
    EXPECT_FALSE(TextEditTool::overlayBaseIsRtl(U""));
    EXPECT_FALSE(TextEditTool::overlayBaseIsRtl(U"123 .,"));
}
