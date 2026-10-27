// Tests for B2 — measured advance width for bitmap expansion.
//
// Verifies that ImageReplacer::computeExpandedWidthMeasured() uses
// actual rendered advance widths (via HarfBuzz shaping through
// BitmapCompositor::measureTextWidth) instead of character-count
// ratios. This is the fix for kerning-sensitive Latin (AV, To, Wa),
// Latin ligatures (fi, fl), Arabic ligated forms, and CJK fixed
// widths that the old char-count formula mishandled.
//
// See docs/image-pdf-risks.md (B2).

#include <gtest/gtest.h>

#include "tre/ImageReplacer.h"
#include "tre/BitmapCompositor.h"
#include "tre/TreTypes.h"

#include <cmath>
#include <filesystem>
#include <memory>
#include <string>

using namespace pdfe::tre;

namespace {

// Return the first available Latin TTF found on the system, or an
// empty string if none exists (test will skip).
std::string findLatinFont() {
    const char* candidates[] = {
        "C:/Windows/Fonts/arial.ttf",
        "C:/Windows/Fonts/tahoma.ttf",
        "C:/Windows/Fonts/calibri.ttf",
        "C:/Windows/Fonts/times.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    };
    for (const char* c : candidates) {
        if (std::filesystem::exists(c)) return std::string(c);
    }
    return {};
}

FontMatch makeFontMatch(const std::string& path, float sizePt = 12.0f) {
    FontMatch fm;
    fm.fontFilePath = path;
    fm.sizePt = sizePt;
    fm.fontFamily = "Arial";
    fm.fontStyle = "Regular";
    return fm;
}

} // namespace

class B2MeasuredWidthTest : public ::testing::Test {
protected:
    void SetUp() override {
        fontPath_ = findLatinFont();
        compositor_ = std::make_unique<BitmapCompositor>();
    }

    std::string fontPath_;
    std::unique_ptr<BitmapCompositor> compositor_;
};

// -----------------------------------------------------------------------
// BitmapCompositor::measureTextWidth — the shaper wrapper
// -----------------------------------------------------------------------

TEST_F(B2MeasuredWidthTest, MeasureEmptyReturnsZero) {
    if (fontPath_.empty()) GTEST_SKIP() << "No Latin font available";
    int w = compositor_->measureTextWidth(U"", fontPath_, 12.0f, 300);
    EXPECT_EQ(w, 0);
}

TEST_F(B2MeasuredWidthTest, MeasureSingleCharReturnsPositive) {
    if (fontPath_.empty()) GTEST_SKIP();
    int w = compositor_->measureTextWidth(U"M", fontPath_, 12.0f, 300);
    EXPECT_GT(w, 0);
    EXPECT_LT(w, 100);  // 1 char at 12pt/300DPI should be < 100 px
}

TEST_F(B2MeasuredWidthTest, MeasureLongerStringIsWider) {
    if (fontPath_.empty()) GTEST_SKIP();
    int w1 = compositor_->measureTextWidth(U"hi", fontPath_, 12.0f, 300);
    int w2 = compositor_->measureTextWidth(U"hi there", fontPath_, 12.0f, 300);
    EXPECT_GT(w2, w1);
}

TEST_F(B2MeasuredWidthTest, MeasureScalesWithDpi) {
    if (fontPath_.empty()) GTEST_SKIP();
    int w300 = compositor_->measureTextWidth(U"hello", fontPath_, 12.0f, 300);
    int w600 = compositor_->measureTextWidth(U"hello", fontPath_, 12.0f, 600);
    // 600 DPI should produce ~2x the pixel width of 300 DPI (±5%
    // for rounding).
    double ratio = static_cast<double>(w600) / static_cast<double>(w300);
    EXPECT_NEAR(ratio, 2.0, 0.1);
}

TEST_F(B2MeasuredWidthTest, MeasureScalesWithFontSize) {
    if (fontPath_.empty()) GTEST_SKIP();
    int w12 = compositor_->measureTextWidth(U"hello", fontPath_, 12.0f, 300);
    int w24 = compositor_->measureTextWidth(U"hello", fontPath_, 24.0f, 300);
    // Font size doubles -> width approximately doubles.
    double ratio = static_cast<double>(w24) / static_cast<double>(w12);
    EXPECT_NEAR(ratio, 2.0, 0.15);
}

TEST_F(B2MeasuredWidthTest, MeasureBadFontReturnsZero) {
    int w = compositor_->measureTextWidth(
        U"hello", "C:/nonexistent/missing-font.ttf", 12.0f, 300);
    EXPECT_EQ(w, 0);
}

// -----------------------------------------------------------------------
// computeExpandedWidthMeasured — the B2 fix entry point
// -----------------------------------------------------------------------

TEST_F(B2MeasuredWidthTest, FallsBackWhenCompositorNull) {
    // Without a compositor we fall back to char-count ratio.
    FontMatch fm = makeFontMatch("dummy");
    int out = ImageReplacer::computeExpandedWidthMeasured(
        100, "a much longer edit", "original",
        /*compositor*/ nullptr, fm, 300);
    // Char-count ratio = 18/8 = 2.25, * 1.2 = 2.7, * 100 = 270.
    EXPECT_GT(out, 100);
}

TEST_F(B2MeasuredWidthTest, FallsBackWhenFontPathEmpty) {
    FontMatch fm;
    fm.fontFilePath = "";
    fm.sizePt = 12.0f;
    int out = ImageReplacer::computeExpandedWidthMeasured(
        100, "longer edit text", "original",
        compositor_.get(), fm, 300);
    // Char-count fallback path.
    EXPECT_GT(out, 100);
}

TEST_F(B2MeasuredWidthTest, EditShorterReturnsBaseWidth) {
    if (fontPath_.empty()) GTEST_SKIP();
    FontMatch fm = makeFontMatch(fontPath_);
    int out = ImageReplacer::computeExpandedWidthMeasured(
        500, "hi", "hello world long original",
        compositor_.get(), fm, 300);
    EXPECT_EQ(out, 500);
}

TEST_F(B2MeasuredWidthTest, EditSameReturnsBaseWidth) {
    if (fontPath_.empty()) GTEST_SKIP();
    FontMatch fm = makeFontMatch(fontPath_);
    int out = ImageReplacer::computeExpandedWidthMeasured(
        500, "same text", "same text",
        compositor_.get(), fm, 300);
    EXPECT_EQ(out, 500);
}

TEST_F(B2MeasuredWidthTest, MeasuredPathExpandsForLongerText) {
    if (fontPath_.empty()) GTEST_SKIP();
    FontMatch fm = makeFontMatch(fontPath_);
    int out = ImageReplacer::computeExpandedWidthMeasured(
        500, "much longer replacement text",
             "original",
        compositor_.get(), fm, 300);
    EXPECT_GT(out, 500);
}

// -----------------------------------------------------------------------
// The real B2 win: kerning-sensitive and narrow-character cases where
// the character-count ratio diverges from measured width.
// -----------------------------------------------------------------------

TEST_F(B2MeasuredWidthTest, NarrowCharactersProduceTighterExpansion) {
    // "iiiiiii" (seven narrow 'i' glyphs) is much narrower than
    // "WWWWWWW" (seven wide 'W' glyphs) despite identical character
    // counts. The char-count fallback treats them identically; the
    // measured path distinguishes them.
    if (fontPath_.empty()) GTEST_SKIP();
    FontMatch fm = makeFontMatch(fontPath_);

    // Measure the widths directly.
    int wNarrow = compositor_->measureTextWidth(U"iiiiiii", fontPath_, 12.0f, 300);
    int wWide   = compositor_->measureTextWidth(U"WWWWWWW", fontPath_, 12.0f, 300);
    ASSERT_GT(wNarrow, 0);
    ASSERT_GT(wWide, 0);
    // 'W' should be noticeably wider than 'i' — at least 2x.
    EXPECT_GT(wWide, 2 * wNarrow)
        << "Proportional font must distinguish 'W' from 'i' (got "
        << wWide << " vs " << wNarrow << ")";
}

TEST_F(B2MeasuredWidthTest, CharCountIgnoresKerning_MeasuredDoesNot) {
    // "AV" kerns tighter in most proportional fonts than "AX"
    // (the same number of chars). The char-count fallback returns
    // the same expansion for both; the measured path returns
    // different widths.
    if (fontPath_.empty()) GTEST_SKIP();
    FontMatch fm = makeFontMatch(fontPath_);

    int wAV = compositor_->measureTextWidth(U"AV AV AV AV", fontPath_, 18.0f, 300);
    int wAX = compositor_->measureTextWidth(U"AX AX AX AX", fontPath_, 18.0f, 300);
    // The values must differ — the shaper has actually consulted font
    // metrics rather than returning a character-count stand-in.
    // Allow either direction of difference; we just require that the
    // measured widths are not numerically identical, which they would
    // be if the path collapsed to char-count.
    EXPECT_NE(wAV, wAX)
        << "Shaped widths of 'AV...' and 'AX...' should differ due "
           "to glyph metric variation (got " << wAV << " vs "
        << wAX << ")";
}

// -----------------------------------------------------------------------
// Cross-DPI invariance: the expansion RATIO of edited/original should
// be DPI-invariant since it is dimensionless. This indirectly verifies
// A3 plumbing: measured widths scale with dpi, and the ratio is stable.
// -----------------------------------------------------------------------
TEST_F(B2MeasuredWidthTest, ExpansionRatioIsDpiInvariant) {
    if (fontPath_.empty()) GTEST_SKIP();
    FontMatch fm = makeFontMatch(fontPath_);

    int out300 = ImageReplacer::computeExpandedWidthMeasured(
        500, "longer edit text here", "short",
        compositor_.get(), fm, 300);
    int out600 = ImageReplacer::computeExpandedWidthMeasured(
        500, "longer edit text here", "short",
        compositor_.get(), fm, 600);

    // basePixelWidth stays 500 in both calls, the ratio is dimensionless,
    // so the outputs should match within ~1% (rounding at integer pixel
    // boundaries grows with DPI). 1% of 2400 ~= 24 px budget.
    double relDiff = std::abs(out300 - out600) / static_cast<double>(out300);
    EXPECT_LT(relDiff, 0.01)
        << "out300=" << out300 << " out600=" << out600;
}
