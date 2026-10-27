// tests/test_per_line_font_size_wiring.cpp
//
// Pipeline c4a9e271 Phase 1 — per-line font size reaches the overlay/save path.
//
// Phase 3 of the prior pipeline computed ScannedTextProperties::perLineFontSizePt
// but nothing consumed it: the text-edit overlay and atlas/save path used a single
// region size, so a scanned region mixing a large heading line with smaller body
// lines rendered every line at the region average. This phase wires the CLICKED
// line's own size into the overlay (MainWindow's spanActivated -> setOverlayFontSize)
// and the atlas FreeType fallback (fm.sizePt).
//
// The wiring is built on two shared statics that this test exercises directly:
//   * FontMatcher::lineFontSizePt(line, script, dpi) — the per-line size formula,
//     applied in MainWindow over the char-box `allLines` the click resolves against.
//   * FontMatcher::clickedLineSizePt(perLine, idx, fallback) — the selection that
//     determines exactly what value spanActivated() passes to setOverlayFontSize().
// Building the vector with the first and selecting with the second reproduces the
// production decision: index 0 (heading clicked) -> heading size, index 1 (body
// clicked) -> body size. The literal setOverlayFontSize() call against the live
// window is verified end-to-end by the native-UI test in Phase 2.
//
// baselineOffset = 0 makes the baseline-to-ascender estimate's validity gate fail
// (ascender == full line height > 0.95*height), so each line's size reduces to the
// pure x_size estimate x_size * 72 / dpi — making expected values exact.

#include <gtest/gtest.h>

#include "tre/FontMatcher.h"
#include "tre/TreTypes.h"

#include <vector>

using pdfe::tre::FontMatcher;
using pdfe::tre::HocrLine;
using pdfe::tre::PixelRect;
using pdfe::fonts::FontScript;

namespace {

// One HOCR line with a given x-height in pixels (baselineOffset 0 -> x_size-only).
HocrLine makeLine(float xSizePx) {
    HocrLine line;
    const double h = static_cast<double>(xSizePx) + 3.0 + 12.0;
    line.bbox = PixelRect{0, 0, 100, h};
    line.baselineSlope = 0.0f;
    line.baselineOffset = 0.0f;
    line.xSizePx = xSizePx;
    line.descendersPx = 3.0f;
    line.ascendersPx = 12.0f;
    return line;
}

constexpr int kDpi = 300;
// x_size * 72 / dpi : heading x_size 125 -> 30.0pt, body x_size 46 -> 11.04pt.
constexpr float kHeadingXSize = 125.0f;
constexpr float kBodyXSize = 46.0f;

}  // namespace

// ─────────────────────────────────────────────────────────────────────
// 1. The shared per-line formula gives a heading line a larger size than a
//    body line (the heterogeneity the fix must preserve into the overlay).
// ─────────────────────────────────────────────────────────────────────
TEST(PerLineFontSizeWiringTest, LineFormulaHeadingLargerThanBody) {
    const auto [headPt, headOk] =
        FontMatcher::lineFontSizePt(makeLine(kHeadingXSize), FontScript::Latin, kDpi);
    const auto [bodyPt, bodyOk] =
        FontMatcher::lineFontSizePt(makeLine(kBodyXSize), FontScript::Latin, kDpi);

    ASSERT_TRUE(headOk);
    ASSERT_TRUE(bodyOk);
    EXPECT_NEAR(headPt, 30.0f, 0.5f);
    EXPECT_NEAR(bodyPt, 11.04f, 0.5f);
    EXPECT_GT(headPt, bodyPt + 5.0f);
}

// ─────────────────────────────────────────────────────────────────────
// 2. CORE WIRING: build the per-line vector exactly as MainWindow does over
//    allLines, then select by clicked-line index. Clicking line 0 (heading)
//    yields ~30pt; clicking line 1 (body) yields ~11pt — i.e. setOverlayFontSize
//    receives the clicked line's own size, not the region average.
// ─────────────────────────────────────────────────────────────────────
TEST(PerLineFontSizeWiringTest, ClickedIndexSelectsHeadingThenBody) {
    std::vector<float> perLine;
    for (float xs : {kHeadingXSize, kBodyXSize}) {
        const auto [pt, ok] = FontMatcher::lineFontSizePt(makeLine(xs), FontScript::Latin, kDpi);
        perLine.push_back(ok ? pt : 0.0f);
    }
    ASSERT_EQ(perLine.size(), 2u);

    const float regionFallback = 18.0f;  // arbitrary region average, must NOT win
    const float clickHeading = FontMatcher::clickedLineSizePt(perLine, 0, regionFallback);
    const float clickBody = FontMatcher::clickedLineSizePt(perLine, 1, regionFallback);

    EXPECT_NEAR(clickHeading, 30.0f, 0.5f);
    EXPECT_NEAR(clickBody, 11.04f, 0.5f);
    EXPECT_NE(clickHeading, regionFallback);
    EXPECT_NE(clickBody, regionFallback);
    EXPECT_GT(clickHeading, clickBody + 5.0f);
}

// ─────────────────────────────────────────────────────────────────────
// 3. Fallback to the region size when the per-line vector is empty
//    (e.g. a page with no usable line metrics).
// ─────────────────────────────────────────────────────────────────────
TEST(PerLineFontSizeWiringTest, FallsBackWhenVectorEmpty) {
    const std::vector<float> empty;
    EXPECT_FLOAT_EQ(FontMatcher::clickedLineSizePt(empty, 0, 13.5f), 13.5f);
}

// ─────────────────────────────────────────────────────────────────────
// 4. Fallback to the region size when the clicked index is out of range
//    (negative or past the end).
// ─────────────────────────────────────────────────────────────────────
TEST(PerLineFontSizeWiringTest, FallsBackWhenIndexOutOfRange) {
    const std::vector<float> perLine{30.0f, 11.0f};
    EXPECT_FLOAT_EQ(FontMatcher::clickedLineSizePt(perLine, 5, 13.5f), 13.5f);
    EXPECT_FLOAT_EQ(FontMatcher::clickedLineSizePt(perLine, -1, 13.5f), 13.5f);
}

// ─────────────────────────────────────────────────────────────────────
// 5. A line with no valid measurement (0.0 sentinel) falls back; an adjacent
//    valid line still returns its own size (no broadcast of the fallback).
// ─────────────────────────────────────────────────────────────────────
TEST(PerLineFontSizeWiringTest, ZeroSentinelLineFallsBackNeighborDoesNot) {
    const std::vector<float> perLine{0.0f, 11.0f};
    EXPECT_FLOAT_EQ(FontMatcher::clickedLineSizePt(perLine, 0, 13.5f), 13.5f);
    EXPECT_FLOAT_EQ(FontMatcher::clickedLineSizePt(perLine, 1, 13.5f), 11.0f);
}

// ─────────────────────────────────────────────────────────────────────
// 6. Regression guard: a valid per-line value is preferred over the region
//    average — the fix must not silently fall back when real data exists.
// ─────────────────────────────────────────────────────────────────────
TEST(PerLineFontSizeWiringTest, ValidPerLineValuePreferredOverRegionAverage) {
    const std::vector<float> perLine{30.0f};
    EXPECT_FLOAT_EQ(FontMatcher::clickedLineSizePt(perLine, 0, 18.0f), 30.0f);
}

// ─────────────────────────────────────────────────────────────────────
// 7. The per-line formula scales with DPI (same x_size, half DPI -> double pt),
//    confirming the size is a real physical measurement, not a constant.
// ─────────────────────────────────────────────────────────────────────
TEST(PerLineFontSizeWiringTest, LineSizeScalesWithDpi) {
    const auto [pt300, ok300] =
        FontMatcher::lineFontSizePt(makeLine(kBodyXSize), FontScript::Latin, 300);
    const auto [pt150, ok150] =
        FontMatcher::lineFontSizePt(makeLine(kBodyXSize), FontScript::Latin, 150);
    ASSERT_TRUE(ok300);
    ASSERT_TRUE(ok150);
    EXPECT_NEAR(pt150, pt300 * 2.0f, 0.5f);
}
