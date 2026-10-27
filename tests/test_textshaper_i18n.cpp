// -*- coding: utf-8 -*-
// R2-20 + R2-21: internationalized text shaping.
//
// Oracles for the acceptance criterion: FontMatcher script detection + MuPDF
// wmode are threaded through to TextShaper (per-run HarfBuzz direction / script
// / language), a bidi reordering pass exists, and vertical writing-mode is
// honoured in the caret model. These tests shape + inspect Arabic (RTL, joined),
// Hebrew (RTL), and vertical-CJK, asserting glyph order / joining and caret
// behaviour.
//
// Non-ASCII text uses \u escapes (project convention; no /utf-8 flag is set,
// see tests/test_a2_script_detection.cpp).
//
// RED-first note: each oracle is defended by a one-line mutation (documented in
// the handoff report) that re-reds it against the implementation.

#include <gtest/gtest.h>

#include "fonts/TextShaper.h"
#include "fonts/FontMetricProfile.h"  // FontScript
#include "tre/BitmapCompositor.h"     // R2-20/R2-21 product-path integration

#include <ft2build.h>
#include FT_FREETYPE_H

#include <QImage>
#include <QRect>

#include <algorithm>
#include <cmath>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace pdfe::fonts;

namespace {

// Try a list of font paths; return the first FT_Face that loads AND covers the
// probe text. Returns nullptr only if none work (SKIP=FAIL: callers ASSERT).
FT_Face loadFirstCovering(FT_Library lib,
                          const std::vector<const char*>& paths,
                          const std::u32string& probe)
{
    TextShaper probeShaper;
    for (const char* p : paths) {
        FT_Face face = nullptr;
        if (FT_New_Face(lib, p, 0, &face) != 0 || !face)
            continue;
        if (probe.empty() || probeShaper.canRender(face, probe))
            return face;
        FT_Done_Face(face);
    }
    return nullptr;
}

} // namespace

// ===========================================================================
// Bidi reordering pass (font-free) — Hebrew RTL + mixed Latin/Hebrew
// ===========================================================================

TEST(TextShaperBidi, HebrewOnlyReversesToVisualOrder) {
    // Logical Hebrew: aleph(05D0) bet(05D1) gimel(05D2).
    const std::u32string logical = U"\u05D0\u05D1\u05D2";
    const std::u32string visual = TextShaper::reorderForDisplay(logical);

    // Pure RTL string is fully reversed for visual (LTR-on-screen) order.
    const std::u32string expected = U"\u05D2\u05D1\u05D0";
    EXPECT_EQ(visual, expected)
        << "Hebrew must be reordered right-to-left by the bidi pass";
}

TEST(TextShaperBidi, MixedLatinHebrewKeepsLatinReversesHebrew) {
    // Logical: 'a' 'b' aleph(05D0) bet(05D1)  (LTR base — first strong is Latin).
    const std::u32string logical = U"ab\u05D0\u05D1";
    const std::u32string visual = TextShaper::reorderForDisplay(logical);

    // Visual: Latin kept "ab", Hebrew run reversed -> bet(05D1) aleph(05D0).
    const std::u32string expected = U"ab\u05D1\u05D0";
    EXPECT_EQ(visual, expected)
        << "LTR base must preserve Latin order and reverse only the RTL run";
}

TEST(TextShaperBidi, RunsPartitionByDirection) {
    const std::u32string logical = U"ab\u05D0\u05D1";
    const auto runs = TextShaper::bidiRuns(logical);

    ASSERT_EQ(runs.size(), 2u);
    EXPECT_EQ(runs[0].start, 0u);
    EXPECT_EQ(runs[0].end, 2u);
    EXPECT_FALSE(runs[0].rtl);
    EXPECT_EQ(runs[1].start, 2u);
    EXPECT_EQ(runs[1].end, 4u);
    EXPECT_TRUE(runs[1].rtl);
}

TEST(TextShaperBidi, EmptyStringYieldsNoRuns) {
    EXPECT_TRUE(TextShaper::bidiRuns(std::u32string()).empty());
    EXPECT_TRUE(TextShaper::reorderForDisplay(std::u32string()).empty());
}

// ===========================================================================
// Arabic: joining (GSUB positional forms) + RTL glyph order
// ===========================================================================

class ArabicShapingTest : public ::testing::Test {
protected:
    void SetUp() override {
        shaper = std::make_unique<TextShaper>();
        ASSERT_EQ(FT_Init_FreeType(&lib), 0);
        // "marhaba": meem(0645) reh(0631) hah(062D) beh(0628) alef(0627).
        arabic = U"\u0645\u0631\u062D\u0628\u0627";
        face = loadFirstCovering(lib, {
            "C:\\Windows\\Fonts\\NotoNaskhArabic-Regular.ttf",
            "C:\\Windows\\Fonts\\arial.ttf",
            "C:\\Windows\\Fonts\\tahoma.ttf",
        }, arabic);
        ASSERT_NE(face, nullptr)
            << "No Arabic-covering font found; required for R2-20 shaping test";
    }
    void TearDown() override {
        if (face) FT_Done_Face(face);
        if (lib) FT_Done_FreeType(lib);
    }
    std::unique_ptr<TextShaper> shaper;
    FT_Library lib = nullptr;
    FT_Face face = nullptr;
    std::u32string arabic;
};

TEST_F(ArabicShapingTest, JoiningSubstitutesPositionalForms) {
    const ShapeParams params = ShapeParams::forScript(FontScript::Arabic);
    auto glyphs = shaper->shape(face, arabic, 24.0f, params);
    ASSERT_FALSE(glyphs.empty());

    // Nominal (isolated) glyph ids for each code point via the cmap.
    std::set<uint32_t> nominal;
    for (char32_t ch : arabic)
        nominal.insert(FT_Get_Char_Index(face, static_cast<FT_ULong>(ch)));

    // Arabic joining substitutes initial/medial/final positional forms whose
    // glyph ids are NOT the isolated cmap glyphs. At least one shaped glyph
    // must therefore be a positional form absent from the nominal set.
    bool sawPositionalForm = false;
    for (const auto& g : glyphs) {
        if (nominal.find(g.glyphId) == nominal.end()) {
            sawPositionalForm = true;
            break;
        }
    }
    EXPECT_TRUE(sawPositionalForm)
        << "Arabic shaping must apply GSUB joining (positional forms), not "
           "emit isolated cmap glyphs";
}

TEST_F(ArabicShapingTest, RtlVisualOrderFirstLogicalCharIsRightmost) {
    const ShapeParams params = ShapeParams::forScript(FontScript::Arabic);
    ASSERT_EQ(params.direction, ShapeDirection::RTL);

    auto glyphs = shaper->shape(face, arabic, 24.0f, params);
    ASSERT_GE(glyphs.size(), 2u);

    // HarfBuzz emits RTL glyphs left-to-right in VISUAL order: the leftmost
    // output glyph is the LAST logical char, and the first logical char (cluster
    // 0) is the rightmost = last element. So clusters descend across the array.
    EXPECT_GT(glyphs.front().cluster, glyphs.back().cluster)
        << "RTL output must run in visual order (clusters descending)";
    EXPECT_EQ(glyphs.back().cluster, 0u)
        << "First logical char must map to the rightmost (last) glyph";
}

// ===========================================================================
// Hebrew: RTL glyph order via HarfBuzz shaping (complements the bidi pass)
// ===========================================================================

TEST(HebrewShaping, RtlClustersDescend) {
    auto shaper = std::make_unique<TextShaper>();
    FT_Library lib = nullptr;
    ASSERT_EQ(FT_Init_FreeType(&lib), 0);

    // "shalom": shin(05E9) lamed(05DC) vav(05D5) final-mem(05DD).
    const std::u32string hebrew = U"\u05E9\u05DC\u05D5\u05DD";
    FT_Face face = loadFirstCovering(lib, {
        "C:\\Windows\\Fonts\\DavidLibre-Regular.ttf",
        "C:\\Windows\\Fonts\\arial.ttf",
        "C:\\Windows\\Fonts\\times.ttf",
    }, hebrew);
    ASSERT_NE(face, nullptr) << "No Hebrew-covering font found (R2-21)";

    const ShapeParams params = ShapeParams::forScript(FontScript::Hebrew);
    EXPECT_EQ(params.direction, ShapeDirection::RTL);

    auto glyphs = shaper->shape(face, hebrew, 24.0f, params);
    ASSERT_GE(glyphs.size(), 2u);
    EXPECT_GT(glyphs.front().cluster, glyphs.back().cluster)
        << "Hebrew must shape in RTL visual order";
    EXPECT_EQ(glyphs.back().cluster, 0u);

    FT_Done_Face(face);
    FT_Done_FreeType(lib);
}

// ===========================================================================
// Vertical CJK: writing-mode threaded to shaping + caret advances in Y
// ===========================================================================

class VerticalCjkTest : public ::testing::Test {
protected:
    void SetUp() override {
        shaper = std::make_unique<TextShaper>();
        ASSERT_EQ(FT_Init_FreeType(&lib), 0);
        // "Nihon" (Japan): 65E5 672C.
        cjk = U"\u65E5\u672C";
        face = loadFirstCovering(lib, {
            "C:\\Windows\\Fonts\\msgothic.ttc",
            "C:\\Windows\\Fonts\\simsun.ttc",
            "C:\\Windows\\Fonts\\YuGothM.ttc",
            "C:\\Windows\\Fonts\\msjh.ttc",
            "C:\\Windows\\Fonts\\malgun.ttf",
        }, cjk);
        ASSERT_NE(face, nullptr) << "No CJK-covering font found (R2-21 vertical)";
    }
    void TearDown() override {
        if (face) FT_Done_Face(face);
        if (lib) FT_Done_FreeType(lib);
    }
    std::unique_ptr<TextShaper> shaper;
    FT_Library lib = nullptr;
    FT_Face face = nullptr;
    std::u32string cjk;
};

TEST_F(VerticalCjkTest, VerticalModeAdvancesInYNotX) {
    const ShapeParams vparams = ShapeParams::forScript(FontScript::CJK, /*vertical=*/true);
    ASSERT_EQ(vparams.direction, ShapeDirection::TTB);
    ASSERT_TRUE(vparams.vertical);

    auto glyphs = shaper->shape(face, cjk, 24.0f, vparams);
    ASSERT_FALSE(glyphs.empty());

    for (const auto& g : glyphs) {
        EXPECT_NEAR(g.xAdvance, 0.0f, 0.5f)
            << "Vertical text must not advance horizontally";
        EXPECT_GT(std::fabs(g.yAdvance), 5.0f)
            << "Vertical text must carry a real vertical advance";
    }
}

TEST_F(VerticalCjkTest, CaretAdvanceHonoursVerticalMode) {
    const ShapeParams vparams = ShapeParams::forScript(FontScript::CJK, /*vertical=*/true);
    auto glyphs = shaper->shape(face, cjk, 24.0f, vparams);
    ASSERT_FALSE(glyphs.empty());

    CaretAdvance c = TextShaper::caretAdvance(glyphs.front(), /*vertical=*/true);
    EXPECT_FLOAT_EQ(c.dx, 0.0f) << "Vertical caret must not move in X";
    EXPECT_GT(std::fabs(c.dy), 5.0f) << "Vertical caret must step along Y";

    // Horizontal caret for the same glyph moves in X only.
    CaretAdvance h = TextShaper::caretAdvance(glyphs.front(), /*vertical=*/false);
    EXPECT_FLOAT_EQ(h.dy, 0.0f);
}

TEST_F(VerticalCjkTest, HorizontalModeAdvancesInX) {
    // Same font/text, horizontal params: proves the vertical branch is what
    // produced the Y advance above (differential oracle).
    const ShapeParams hparams = ShapeParams::forScript(FontScript::CJK, /*vertical=*/false);
    ASSERT_EQ(hparams.direction, ShapeDirection::LTR);

    auto glyphs = shaper->shape(face, cjk, 24.0f, hparams);
    ASSERT_FALSE(glyphs.empty());
    for (const auto& g : glyphs) {
        EXPECT_GT(g.xAdvance, 5.0f) << "Horizontal CJK must advance in X";
        EXPECT_NEAR(g.yAdvance, 0.0f, 0.5f);
    }
}

// ===========================================================================
// ShapeParams mapping + backward-compatibility of the 3-arg shape()
// ===========================================================================

TEST(ShapeParamsMapping, ScriptDrivesDirection) {
    EXPECT_EQ(ShapeParams::forScript(FontScript::Latin).direction, ShapeDirection::LTR);
    EXPECT_EQ(ShapeParams::forScript(FontScript::Arabic).direction, ShapeDirection::RTL);
    EXPECT_EQ(ShapeParams::forScript(FontScript::Hebrew).direction, ShapeDirection::RTL);
    EXPECT_EQ(ShapeParams::forScript(FontScript::CJK).direction, ShapeDirection::LTR);
    // Vertical flag overrides to TTB regardless of script.
    EXPECT_EQ(ShapeParams::forScript(FontScript::CJK, true).direction, ShapeDirection::TTB);
    EXPECT_EQ(ShapeParams::forScript(FontScript::Latin, true).direction, ShapeDirection::TTB);
}

TEST(ShapeParamsMapping, LegacyShapeStillLtrLatin) {
    auto shaper = std::make_unique<TextShaper>();
    FT_Library lib = nullptr;
    ASSERT_EQ(FT_Init_FreeType(&lib), 0);
    FT_Face face = loadFirstCovering(lib, {
        "C:\\Windows\\Fonts\\arial.ttf",
        "C:\\Windows\\Fonts\\times.ttf",
    }, U"Hi");
    ASSERT_NE(face, nullptr);

    // The 3-arg overload must still work and advance in X (LTR).
    auto glyphs = shaper->shape(face, U"Hi", 12.0f);
    ASSERT_EQ(glyphs.size(), 2u);
    for (const auto& g : glyphs)
        EXPECT_GT(g.xAdvance, 0.0f);

    FT_Done_Face(face);
    FT_Done_FreeType(lib);
}

// ===========================================================================
// INTEGRATION: the i18n shaping API is threaded THROUGH the real product
// renderer (tre::BitmapCompositor), not exercised standalone. Each test below
// drives a BitmapCompositor *public* method (renderText / measureTextWidth /
// setVerticalWritingMode / visualOrder) and asserts an observable, product-path
// effect of script detection + HarfBuzz direction/script + vertical wmode +
// the bidi reorder pass. This is the karen-gate remediation for R2-20/R2-21:
// before the integration these tests do not even compile (the vertical /
// visualOrder API is absent) and the width/ink oracles collapse to the
// LTR/Latin baseline (RED); after it they pass (GREEN). See the handoff report
// for the exact one-line mutations that re-red each oracle.
// ===========================================================================

namespace {

// First path in @p paths that both loads AND covers @p probe (SKIP=FAIL:
// callers ASSERT non-empty). Returns "" only if none qualify.
std::string firstCoveringPath(const std::vector<const char*>& paths,
                              const std::u32string& probe)
{
    FT_Library lib = nullptr;
    if (FT_Init_FreeType(&lib) != 0)
        return {};
    TextShaper probeShaper;
    std::string result;
    for (const char* p : paths) {
        FT_Face face = nullptr;
        if (FT_New_Face(lib, p, 0, &face) != 0 || !face)
            continue;
        const bool ok = probe.empty() || probeShaper.canRender(face, probe);
        FT_Done_Face(face);
        if (ok) { result = p; break; }
    }
    FT_Done_FreeType(lib);
    return result;
}

// Number of non-transparent pixels in an ARGB image.
int inkPixelCount(const QImage& img)
{
    int count = 0;
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x)
            if (qAlpha(img.pixel(x, y)) > 0)
                ++count;
    return count;
}

// Tight bounding box of the non-transparent ink. Null rect if no ink.
QRect inkBBox(const QImage& img)
{
    int minX = img.width(), minY = img.height(), maxX = -1, maxY = -1;
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) {
            if (qAlpha(img.pixel(x, y)) > 0) {
                minX = std::min(minX, x); minY = std::min(minY, y);
                maxX = std::max(maxX, x); maxY = std::max(maxY, y);
            }
        }
    }
    if (maxX < 0)
        return QRect();
    return QRect(minX, minY, maxX - minX + 1, maxY - minY + 1);
}

} // namespace

// ---------------------------------------------------------------------------
// R2-20: Arabic joining is reflected in the product measure path.
// measureTextWidth() now shapes script-aware (BitmapCompositor::shapeLine ->
// FontMatcher::scriptForRun -> ShapeParams::forScript(Arabic) -> HarfBuzz
// Arabic GSUB). Joining substitutes medial/initial/final forms whose combined
// advance differs from the isolated-glyph concatenation. Under the legacy
// LTR/Latin shaping the same codepoints came out as isolated cmap glyphs, so
// the word width EQUALLED the sum of the per-character (isolated) widths and
// the relative difference was ~0 — the RED state.
// ---------------------------------------------------------------------------
TEST(BitmapCompositorI18n, ArabicJoiningChangesShapedWidth) {
    // "marhaba": meem(0645) reh(0631) hah(062D) beh(0628) alef(0627).
    const std::u32string word = U"\u0645\u0631\u062D\u0628\u0627";
    const std::string font = firstCoveringPath({
        "C:\\Windows\\Fonts\\NotoNaskhArabic-Regular.ttf",
        "C:\\Windows\\Fonts\\arial.ttf",
        "C:\\Windows\\Fonts\\tahoma.ttf",
    }, word);
    ASSERT_FALSE(font.empty()) << "No Arabic-covering font found (R2-20)";

    pdfe::tre::BitmapCompositor comp;
    const int wordW = comp.measureTextWidth(word, font, 24.0f, 300);
    ASSERT_GT(wordW, 0);

    int sumSolo = 0;
    for (char32_t ch : word)
        sumSolo += comp.measureTextWidth(std::u32string(1, ch), font, 24.0f, 300);
    ASSERT_GT(sumSolo, 0);

    // Joining shifts the shaped word width away from the isolated-glyph sum.
    const double rel =
        std::fabs(static_cast<double>(wordW) - static_cast<double>(sumSolo))
        / static_cast<double>(sumSolo);
    EXPECT_GT(rel, 0.02)
        << "Arabic shaped word width (" << wordW << ") must differ from the "
           "isolated-glyph concatenation (" << sumSolo << ") because GSUB "
           "joining is applied through the product measure path";
}

// ---------------------------------------------------------------------------
// R2-21: vertical writing mode is honoured on the real render path.
// setVerticalWritingMode(true) forces ShapeParams TTB, HarfBuzz emits vertical
// advances, and the render loop steps the pen along Y (caretAdvance). Two CJK
// glyphs therefore STACK (tall, narrow ink) instead of running left-to-right
// (wide, short ink). Pre-integration the API does not exist (compile RED); the
// mutation (force vertical=false in shapeLine's ShapeParams) collapses the
// vertical bitmap back onto the horizontal one and re-reds this oracle.
// ---------------------------------------------------------------------------
TEST(BitmapCompositorI18n, VerticalWritingModeStacksCjkGlyphs) {
    const std::u32string cjk = U"\u65E5\u672C"; // "Nihon" (Japan)
    const std::string font = firstCoveringPath({
        "C:\\Windows\\Fonts\\msgothic.ttc",
        "C:\\Windows\\Fonts\\simsun.ttc",
        "C:\\Windows\\Fonts\\YuGothM.ttc",
        "C:\\Windows\\Fonts\\msjh.ttc",
        "C:\\Windows\\Fonts\\malgun.ttf",
    }, cjk);
    ASSERT_FALSE(font.empty()) << "No CJK-covering font found (R2-21 vertical)";

    const pdfe::tre::RgbColor black{0, 0, 0};
    const int W = 400, H = 400, dpi = 300;

    pdfe::tre::BitmapCompositor comp;
    ASSERT_FALSE(comp.verticalWritingMode());

    comp.setVerticalWritingMode(false);
    const QImage horiz = comp.renderText(cjk, font, 24.0f, black, W, H, dpi);
    comp.setVerticalWritingMode(true);
    ASSERT_TRUE(comp.verticalWritingMode());
    const QImage vert = comp.renderText(cjk, font, 24.0f, black, W, H, dpi);

    const QRect hb = inkBBox(horiz);
    const QRect vb = inkBBox(vert);
    ASSERT_FALSE(hb.isNull()) << "Horizontal CJK produced no ink";
    ASSERT_FALSE(vb.isNull()) << "Vertical CJK produced no ink";

    // Two ideographs side by side are wider than tall; stacked they are taller
    // than wide. This flip only happens if the writing mode actually reached
    // the shaping + pen path.
    EXPECT_GT(hb.width(), hb.height())
        << "Horizontal CJK ink should be wider than tall";
    EXPECT_GT(vb.height(), vb.width())
        << "Vertical CJK ink should be taller than wide";
    EXPECT_GT(vb.height(), hb.height())
        << "Vertical stacking must extend the ink further in Y than horizontal";
}

// ---------------------------------------------------------------------------
// R2-20: Arabic actually renders ink through the real product renderer with
// script detection active (no LTR/Latin fallback swallowing the glyphs).
// ---------------------------------------------------------------------------
TEST(BitmapCompositorI18n, ArabicRendersInkThroughProductRenderer) {
    const std::u32string word = U"\u0645\u0631\u062D\u0628\u0627";
    const std::string font = firstCoveringPath({
        "C:\\Windows\\Fonts\\NotoNaskhArabic-Regular.ttf",
        "C:\\Windows\\Fonts\\arial.ttf",
        "C:\\Windows\\Fonts\\tahoma.ttf",
    }, word);
    ASSERT_FALSE(font.empty()) << "No Arabic-covering font found (R2-20)";

    pdfe::tre::BitmapCompositor comp;
    const pdfe::tre::RgbColor black{0, 0, 0};
    const QImage img = comp.renderText(word, font, 24.0f, black, 600, 200, 300);
    EXPECT_GT(inkPixelCount(img), 0)
        << "Arabic must rasterize through BitmapCompositor::renderText";
}

// ---------------------------------------------------------------------------
// R2-21: the bidi reorder pass is reachable through the product class. The
// text-edit overlay maps logical caret indices to on-screen order via
// BitmapCompositor::visualOrder (-> TextShaper::reorderForDisplay). Pure Hebrew
// is fully reversed; an LTR base keeps Latin and reverses only the RTL run.
// Pre-integration this method does not exist (compile RED).
// ---------------------------------------------------------------------------
TEST(BitmapCompositorI18n, VisualOrderReordersRtlThroughProductApi) {
    pdfe::tre::BitmapCompositor comp;

    // Pure Hebrew aleph-bet-gimel -> reversed for visual order.
    EXPECT_EQ(comp.visualOrder(U"\u05D0\u05D1\u05D2"),
              std::u32string(U"\u05D2\u05D1\u05D0"));

    // LTR base "ab" + Hebrew aleph-bet: Latin kept, Hebrew run reversed.
    EXPECT_EQ(comp.visualOrder(U"\u0061\u0062\u05D0\u05D1"),
              std::u32string(U"\u0061\u0062\u05D1\u05D0"));

    // Latin-only text is unchanged (backward-compat).
    EXPECT_EQ(comp.visualOrder(U"Hello"), std::u32string(U"Hello"));
}
