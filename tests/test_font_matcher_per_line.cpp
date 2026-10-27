// tests/test_font_matcher_per_line.cpp
//
// Pipeline 3f8a1c72 Phase 3 — per-line font size in FontMatcher::detectProperties.
//
// Phase 1 (docs/pipeline-3f8a1c72-findings.md) measured a per-line font-size
// coefficient of variation up to 0.24 on real fixtures (a 30pt heading over
// 10-12pt body): a single region-average fontSizePt is wrong for BOTH the
// heading and the body. detectProperties() now also computes a per-line size
// (ScannedTextProperties::perLineFontSizePt) from each line's OWN x_size /
// baseline, keeping the region average only as a fallback for lines without a
// valid measurement — never broadcast over a line that has its own.
//
// These tests use baselineOffset = 0 so the per-line baseline-to-ascender
// estimate's validity gate fails (ascender == full line height > 0.95*height),
// reducing each line's size to the pure x_size estimate: x_size * 72 / dpi.
// That makes every expected value exact and independent of pixel content.

#include <gtest/gtest.h>

#include "tre/FontMatcher.h"
#include "tre/TreTypes.h"
#include "fonts/SubstituteFontSelector.h"
#include "fonts/EmbeddedFontAnalyzer.h"
#include "fonts/FontAnalysisCache.h"
#include "core/MuPdfContext.h"

#include <QImage>
#include <QColor>
#include <vector>

using namespace pdfe::tre;

namespace {

// Build a region with one line per entry in `xSizesPx`. baselineOffset = 0 on
// every line, so only the x_size estimate contributes (see file header).
RegionTextData makeLinesRegion(const std::vector<float>& xSizesPx,
                               float descPx, float ascPx,
                               float baselineSlope = 0.0f) {
    RegionTextData region;
    double yPos = 0.0;
    for (float xs : xSizesPx) {
        const double lineH = static_cast<double>(xs + descPx + ascPx);
        HocrLine line;
        line.bbox = PixelRect{0, yPos, 100, yPos + lineH};
        line.baselineSlope = baselineSlope;
        line.baselineOffset = 0.0f;
        line.xSizePx = xs;
        line.descendersPx = descPx;
        line.ascendersPx = ascPx;

        HocrWord word;
        word.text = "test";
        word.bbox = PixelRect{0, yPos, 50, yPos + lineH};
        word.confidence = 90;
        word.fontSizePx = xs;
        line.words.push_back(word);

        region.lines.push_back(line);
        yPos += lineH + 5.0;
    }
    region.plainText = "test";
    region.pixelBbox = PixelRect{0, 0, 100, yPos};
    region.pdfBbox = PdfRect{0, 0, 100, yPos};
    return region;
}

QImage blankImage() {
    QImage img(200, 400, QImage::Format_ARGB32);
    img.fill(Qt::white);
    return img;
}

// Owns the object graph detectProperties() needs and exposes the matcher.
struct MatcherFixture {
    pdfe::core::MuPdfContext context;
    pdfe::fonts::FontAnalysisCache cache;
    pdfe::fonts::EmbeddedFontAnalyzer analyzer{&context};
    pdfe::fonts::SubstituteFontSelector selector{&analyzer, &cache};
    FontMatcher matcher{&selector, &analyzer};
};

}  // namespace

// ─────────────────────────────────────────────────────────────────────
// (a) A multi-line region with different x_size per line yields DIFFERENT
//     per-line fontSizePt values — not the region average broadcast to all.
// ─────────────────────────────────────────────────────────────────────
TEST(FontMatcherPerLineTest, DifferentXSizePerLineGivesDifferentSizes) {
    MatcherFixture fx;
    // Line 0: x_size 10 → 2.4pt; Line 1: x_size 30 → 7.2pt (@300 DPI).
    const RegionTextData region = makeLinesRegion({10.0f, 30.0f}, 3.0f, 12.0f);
    const ScannedTextProperties props =
        fx.matcher.detectProperties(region, blankImage(), 300);

    ASSERT_EQ(props.perLineFontSizePt.size(), 2u);
    EXPECT_NEAR(props.perLineFontSizePt[0], 2.4f, 0.1f);
    EXPECT_NEAR(props.perLineFontSizePt[1], 7.2f, 0.1f);

    // The two lines must differ — the whole point of the per-line fix.
    EXPECT_GT(props.perLineFontSizePt[1], props.perLineFontSizePt[0] + 1.0f);

    // Region average (x_size 20 → 4.8pt) is NOT broadcast over either line.
    EXPECT_NEAR(props.fontSizePt, 4.8f, 0.1f);
    EXPECT_NE(props.perLineFontSizePt[0], props.fontSizePt);
    EXPECT_NE(props.perLineFontSizePt[1], props.fontSizePt);
}

// ─────────────────────────────────────────────────────────────────────
// (b) A single-line region: the one per-line value matches that line's own
//     x_size-derived size (and equals the region representative).
// ─────────────────────────────────────────────────────────────────────
TEST(FontMatcherPerLineTest, SingleLineMatchesItsOwnXSize) {
    MatcherFixture fx;
    const RegionTextData region = makeLinesRegion({20.0f}, 5.0f, 25.0f);
    const ScannedTextProperties props =
        fx.matcher.detectProperties(region, blankImage(), 300);

    ASSERT_EQ(props.perLineFontSizePt.size(), 1u);
    // x_size 20 @300 DPI → 20 * 72 / 300 = 4.8pt.
    EXPECT_NEAR(props.perLineFontSizePt[0], 4.8f, 0.1f);
    EXPECT_NEAR(props.perLineFontSizePt[0], props.fontSizePt, 0.01f);
}

// ─────────────────────────────────────────────────────────────────────
// (c) A region whose lines all share the same x_size produces per-line values
//     equal to each other AND to the current region-average path — proving the
//     refactor does not change behaviour on uniform regions.
// ─────────────────────────────────────────────────────────────────────
TEST(FontMatcherPerLineTest, UniformLinesEqualRegionAverage) {
    MatcherFixture fx;
    const RegionTextData region = makeLinesRegion({15.0f, 15.0f, 15.0f}, 4.0f, 18.0f);
    const ScannedTextProperties props =
        fx.matcher.detectProperties(region, blankImage(), 300);

    ASSERT_EQ(props.perLineFontSizePt.size(), 3u);
    // x_size 15 @300 DPI → 3.6pt for every line and for the region average.
    EXPECT_NEAR(props.fontSizePt, 3.6f, 0.1f);
    for (float s : props.perLineFontSizePt) {
        EXPECT_NEAR(s, 3.6f, 0.1f);
        EXPECT_NEAR(s, props.fontSizePt, 0.01f);
    }
    EXPECT_FLOAT_EQ(props.perLineFontSizePt[0], props.perLineFontSizePt[1]);
    EXPECT_FLOAT_EQ(props.perLineFontSizePt[1], props.perLineFontSizePt[2]);
}

// ─────────────────────────────────────────────────────────────────────
// (d) Regression guard: the per-line SIZE refactor must not perturb the
//     region italic decision, which is driven solely by baseline slope.
// ─────────────────────────────────────────────────────────────────────
TEST(FontMatcherPerLineTest, ItalicUnchangedByPerLineSizeRefactor) {
    MatcherFixture fx;

    // Mixed sizes but no slant → per-line sizes differ, italic stays false.
    const RegionTextData upright = makeLinesRegion({10.0f, 30.0f}, 3.0f, 12.0f,
                                                   /*baselineSlope=*/0.0f);
    const ScannedTextProperties uprightProps =
        fx.matcher.detectProperties(upright, blankImage(), 300);
    ASSERT_EQ(uprightProps.perLineFontSizePt.size(), 2u);
    EXPECT_GT(uprightProps.perLineFontSizePt[1],
              uprightProps.perLineFontSizePt[0] + 1.0f);
    EXPECT_FALSE(uprightProps.isItalic)
        << "per-line size must not introduce a false italic";

    // Clear slant (slope 0.12 > 0.08 threshold) → italic true, regardless of
    // the per-line size computation running alongside.
    const RegionTextData slanted = makeLinesRegion({12.0f, 12.0f}, 3.0f, 14.0f,
                                                   /*baselineSlope=*/0.12f);
    const ScannedTextProperties slantedProps =
        fx.matcher.detectProperties(slanted, blankImage(), 300);
    EXPECT_TRUE(slantedProps.isItalic)
        << "baseline-slope italic detection must be unaffected by the refactor";
}
