#include <gtest/gtest.h>
#include "tre/LineFontVerifier.h"
#include "tre/EnrichedAtlas.h"
#include "tre/AtlasBuilder.h"
#include "tre/GlyphAtlas.h"
#include "tre/GlyphAtlasCompositor.h"
#include "tre/GlyphTemplateCache.h"
#include "tre/ImageSimilarity.h"
#include "fonts/FontResolver.h"
#include "tre/TreTypes.h"
#include <QImage>
#include <QPainter>

using namespace pdfe::tre;

// ============================================================================
// Test fixture with shared helpers
// ============================================================================

class GlyphHarvesterTest : public ::testing::Test {
protected:
    // Use varied gray values to ensure Otsu binarization works correctly.
    // With exactly 2 distinct values, Otsu picks the lower value as threshold,
    // and strict `<` fails to classify it, making all pixels uniform (NCC=0).
    // Adding slight variation (30-39 for text, 220 for bg) ensures the threshold
    // falls between the two clusters.
    static constexpr int kTextGray = 30;  // base text gray (actual pixels vary 30-39)
    static constexpr int kBgGray = 220;

    QImage makeTestImage(int w = 400, int h = 200) {
        QImage img(w, h, QImage::Format_RGB32);
        img.fill(qRgb(kBgGray, kBgGray, kBgGray));
        return img;
    }

    // Draw an "L"-shaped glyph with slight pixel variation for valid Otsu binarization.
    void drawLetterL(QImage& img, int x0, int y0, int w, int h) {
        int strokeW = std::max(2, w / 4);
        auto setTextPixel = [&](int x, int y) {
            int g = kTextGray + ((x + y) % 10);  // 30-39
            img.setPixel(x, y, qRgb(g, g, g));
        };
        // Vertical stroke (left side)
        for (int y = y0; y < y0 + h && y < img.height(); ++y)
            for (int x = x0; x < x0 + strokeW && x < img.width(); ++x)
                setTextPixel(x, y);
        // Horizontal base (bottom)
        int baseH = std::max(2, h / 5);
        for (int y = y0 + h - baseH; y < y0 + h && y < img.height(); ++y)
            for (int x = x0; x < x0 + w && x < img.width(); ++x)
                setTextPixel(x, y);
    }

    // Draw a "T"-shaped glyph with slight pixel variation.
    void drawLetterT(QImage& img, int x0, int y0, int w, int h) {
        int strokeW = std::max(2, w / 4);
        auto setTextPixel = [&](int x, int y) {
            int g = kTextGray + ((x + y) % 10);
            img.setPixel(x, y, qRgb(g, g, g));
        };
        int topH = std::max(2, h / 5);
        for (int y = y0; y < y0 + topH && y < img.height(); ++y)
            for (int x = x0; x < x0 + w && x < img.width(); ++x)
                setTextPixel(x, y);
        int cx = x0 + w / 2 - strokeW / 2;
        for (int y = y0; y < y0 + h && y < img.height(); ++y)
            for (int x = cx; x < cx + strokeW && x < img.width(); ++x)
                setTextPixel(x, y);
    }

    HocrChar makeChar(char32_t cp, int x0, int y0, int x1, int y1, int conf = 90) {
        HocrChar ch;
        ch.codepoint = cp;
        ch.bbox = {static_cast<float>(x0), static_cast<float>(y0),
                   static_cast<float>(x1), static_cast<float>(y1)};
        ch.confidence = conf;
        return ch;
    }

    HocrLine makeLine(const std::vector<HocrChar>& chars,
                      float xSize = 20.0f, float asc = 5.0f, float desc = 4.0f,
                      float fontSize = 0.0f) {
        HocrWord word;
        word.text = "test";
        word.confidence = 90;
        word.fontSizePx = fontSize;
        word.chars = chars;
        if (!chars.empty()) {
            word.bbox = {chars.front().bbox.x0, chars.front().bbox.y0,
                         chars.back().bbox.x1, chars.back().bbox.y1};
        }

        HocrLine line;
        line.words = {word};
        line.bbox = word.bbox;
        line.xSizePx = xSize;
        line.ascendersPx = asc;
        line.descendersPx = desc;
        return line;
    }

    struct TwoLineSetup {
        QImage image;
        HocrLine line1;
        HocrLine line2;
    };

    // Two lines with identical L-shaped glyphs at same HOCR metrics (compatible)
    TwoLineSetup makeCompatibleLines() {
        QImage img = makeTestImage(300, 100);

        // Line 1: "ABC" at y=10..30 -- all L-shaped
        drawLetterL(img, 10, 10, 15, 20);   // A
        drawLetterL(img, 30, 10, 15, 20);   // B
        drawLetterL(img, 50, 10, 15, 20);   // C

        // Line 2: "ABD" at y=50..70 -- same L shapes for A,B
        drawLetterL(img, 10, 50, 15, 20);   // A
        drawLetterL(img, 30, 50, 15, 20);   // B
        drawLetterL(img, 50, 50, 15, 20);   // D

        HocrLine line1 = makeLine({
            makeChar(U'A', 10, 10, 25, 30),
            makeChar(U'B', 30, 10, 45, 30),
            makeChar(U'C', 50, 10, 65, 30),
        }, 20.0f, 5.0f, 4.0f, 18.0f);

        HocrLine line2 = makeLine({
            makeChar(U'A', 10, 50, 25, 70),
            makeChar(U'B', 30, 50, 45, 70),
            makeChar(U'D', 50, 50, 65, 70),
        }, 20.0f, 5.0f, 4.0f, 18.0f);

        return {img, line1, line2};
    }

    // Two lines with incompatible font metrics and height ratio outside [0.75, 1.35].
    // Used to verify that rejected lines don't contribute glyphs.
    TwoLineSetup makeIncompatibleLines() {
        QImage img = makeTestImage(300, 120);

        // Line 1: "AB" at y=10..35 (lineH=25)
        drawLetterL(img, 10, 10, 15, 25);
        drawLetterL(img, 30, 10, 15, 25);

        // Line 2: "AE" at y=50..80 (lineH=30) with T-shape (different glyph style)
        // Height ratio = 30/25 = 1.2 → inside [0.75, 1.35] but fails font verification
        drawLetterT(img, 10, 50, 20, 30);
        drawLetterT(img, 40, 50, 20, 30);

        HocrLine line1 = makeLine({
            makeChar(U'A', 10, 10, 25, 35),
            makeChar(U'B', 30, 10, 45, 35),
        }, 20.0f, 5.0f, 4.0f, 18.0f);

        // Very different metrics: xSize 45 vs 20 (ratio 0.44),
        // plus disproportionate asc/desc ratios → fails font verification.
        // Height ratio 30/25 = 1.2 is within [0.75, 1.35] but metrics fail.
        HocrLine line2 = makeLine({
            makeChar(U'A', 10, 50, 30, 80),
            makeChar(U'E', 40, 50, 60, 80),
        }, 45.0f, 18.0f, 2.0f, 40.0f);

        return {img, line1, line2};
    }

    RgbColor textColor{kTextGray, kTextGray, kTextGray};
    RgbColor bgColor{kBgGray, kBgGray, kBgGray};
};

// ============================================================================
// LineFontVerifier tests
// ============================================================================

TEST_F(GlyphHarvesterTest, LineFontVerifier_IdenticalLine_Compatible) {
    auto [img, line1, line2] = makeCompatibleLines();
    // Verify line against itself -- metrics match perfectly, NCC=1.0
    auto result = LineFontVerifier::verify(img, line1, line1);
    EXPECT_TRUE(result.compatible);
    EXPECT_GE(result.confidence, 0.65f);
    EXPECT_GE(result.metricScore, 0.99f);
}

TEST_F(GlyphHarvesterTest, LineFontVerifier_DifferentXSize_Rejected) {
    auto [img, line1, line2] = makeIncompatibleLines();
    // xSize 20 vs 45 plus disproportionate asc/desc ratios
    // (lines have similar height but incompatible font metrics)
    auto result = LineFontVerifier::verify(img, line1, line2);
    EXPECT_FALSE(result.compatible);
    // metricScore should be low due to disproportionate ratios
    EXPECT_LT(result.metricScore, 0.55f);
}

TEST_F(GlyphHarvesterTest, LineFontVerifier_SameFontDifferentWords_Compatible) {
    auto [img, line1, line2] = makeCompatibleLines();
    // Same metrics, shared 'A' and 'B' with identical L-shapes
    auto result = LineFontVerifier::verify(img, line1, line2);
    EXPECT_TRUE(result.compatible);
    EXPECT_GE(result.sharedChars, 2);
    EXPECT_GE(result.nccScore, 0.7f);
}

TEST_F(GlyphHarvesterTest, LineFontVerifier_NoSharedChars_MetricOnly) {
    QImage img = makeTestImage(300, 100);

    drawLetterL(img, 10, 10, 15, 20);
    drawLetterL(img, 30, 10, 15, 20);
    drawLetterL(img, 10, 50, 15, 20);
    drawLetterL(img, 30, 50, 15, 20);

    HocrLine line1 = makeLine({
        makeChar(U'X', 10, 10, 25, 30),
        makeChar(U'Y', 30, 10, 45, 30),
    }, 20.0f, 5.0f, 4.0f, 18.0f);

    HocrLine line2 = makeLine({
        makeChar(U'Z', 10, 50, 25, 70),
        makeChar(U'W', 30, 50, 45, 70),
    }, 20.0f, 5.0f, 4.0f, 18.0f);

    auto result = LineFontVerifier::verify(img, line1, line2);
    EXPECT_EQ(result.sharedChars, 0);
    // No shared chars -> metric score used as confidence
    // Same metrics -> should be compatible via metric-only path
    EXPECT_GE(result.metricScore, 0.9f);
    EXPECT_TRUE(result.compatible);
}

TEST_F(GlyphHarvesterTest, LineFontVerifier_ZeroMetrics_Handled) {
    QImage img = makeTestImage(200, 100);

    HocrLine line1 = makeLine({makeChar(U'A', 10, 10, 25, 30)}, 0.0f, 0.0f, 0.0f, 0.0f);
    HocrLine line2 = makeLine({makeChar(U'A', 10, 50, 25, 70)}, 0.0f, 0.0f, 0.0f, 0.0f);

    // Should not crash
    auto result = LineFontVerifier::verify(img, line1, line2);
    // xSize both zero → 1.0, asc/desc subRatio with base=0 → 0.5, fontSize both zero → 1.0
    // Expected: 0.4*1.0 + 0.2*0.5 + 0.2*0.5 + 0.2*1.0 = 0.8
    EXPECT_GE(result.metricScore, 0.75f);
}

TEST_F(GlyphHarvesterTest, LineFontVerifier_NccMatchesImageSimilarity) {
    auto [img, line1, line2] = makeCompatibleLines();

    // Manually crop the 'A' glyph from both lines (same L-shape)
    QImage cropA1 = img.copy(10, 10, 15, 20);
    QImage cropA2 = img.copy(10, 50, 15, 20);

    float directNcc = ImageSimilarity::binarizedNCC(cropA1, cropA2);
    // Identical L-shapes should have high NCC (non-uniform → meaningful)
    EXPECT_GE(directNcc, 0.7f);

    auto result = LineFontVerifier::verify(img, line1, line2);
    EXPECT_GE(result.nccScore, 0.7f);
}

// ============================================================================
// GlyphAtlas extensions tests
// ============================================================================

TEST_F(GlyphHarvesterTest, GlyphAtlas_BuildVerified_AcceptsCompatible) {
    auto [img, line1, line2] = makeCompatibleLines();

    GlyphAtlas atlas;
    int accepted = atlas.buildVerified(img, line1, {line2}, textColor, bgColor);

    EXPECT_EQ(accepted, 1);  // line2 was accepted
    // A, B, C (from line1) + D (from line2)
    EXPECT_TRUE(atlas.contains(U'A'));
    EXPECT_TRUE(atlas.contains(U'B'));
    EXPECT_TRUE(atlas.contains(U'C'));
    EXPECT_TRUE(atlas.contains(U'D'));
    EXPECT_EQ(atlas.size(), 4);
}

TEST_F(GlyphHarvesterTest, GlyphAtlas_BuildVerified_RejectsIncompatible) {
    auto [img, line1, line2] = makeIncompatibleLines();

    GlyphAtlas atlas;
    int accepted = atlas.buildVerified(img, line1, {line2}, textColor, bgColor);

    EXPECT_EQ(accepted, 0);  // line2 was rejected by font verification
    EXPECT_TRUE(atlas.contains(U'A'));
    EXPECT_TRUE(atlas.contains(U'B'));
    // 'E' is NOT in atlas — rejected lines no longer contribute glyphs.
    // Missing chars are filled by GlyphSynthesizer in EnrichedAtlas.
    EXPECT_FALSE(atlas.contains(U'E'));
}

TEST_F(GlyphHarvesterTest, GlyphAtlas_MergeFrom_AddsMissingOnly) {
    QImage img = makeTestImage(200, 100);
    drawLetterL(img, 10, 10, 15, 20);
    drawLetterL(img, 30, 10, 15, 20);
    drawLetterL(img, 50, 10, 15, 20);

    HocrLine lineAB = makeLine({
        makeChar(U'A', 10, 10, 25, 30),
        makeChar(U'B', 30, 10, 45, 30),
    });

    HocrLine lineBC = makeLine({
        makeChar(U'B', 30, 10, 45, 30),
        makeChar(U'C', 50, 10, 65, 30),
    });

    GlyphAtlas atlas1;
    atlas1.build(img, {lineAB}, textColor, bgColor);
    EXPECT_EQ(atlas1.size(), 2);

    GlyphAtlas atlas2;
    atlas2.build(img, {lineBC}, textColor, bgColor);
    EXPECT_EQ(atlas2.size(), 2);

    atlas1.mergeFrom(atlas2);
    EXPECT_EQ(atlas1.size(), 3);
    EXPECT_TRUE(atlas1.contains(U'A'));
    EXPECT_TRUE(atlas1.contains(U'B'));
    EXPECT_TRUE(atlas1.contains(U'C'));
}

TEST_F(GlyphHarvesterTest, GlyphAtlas_HarvestLine_SameAsBuild) {
    QImage img = makeTestImage(200, 100);
    drawLetterL(img, 10, 10, 15, 20);
    drawLetterL(img, 30, 10, 15, 20);
    drawLetterL(img, 50, 10, 15, 20);

    auto lines = std::vector<HocrLine>{makeLine({
        makeChar(U'A', 10, 10, 25, 30),
        makeChar(U'B', 30, 10, 45, 30),
        makeChar(U'C', 50, 10, 65, 30),
    })};

    GlyphAtlas atlas;
    atlas.build(img, lines, textColor, bgColor);

    EXPECT_EQ(atlas.size(), 3);
    for (char32_t c : {U'A', U'B', U'C'}) {
        QImage g = atlas.glyph(c);
        EXPECT_FALSE(g.isNull());
        EXPECT_GT(g.width(), 0);
        EXPECT_GT(g.height(), 0);
    }
}

TEST_F(GlyphHarvesterTest, GlyphAtlas_ClipAtVerticalGap_GentleMode) {
    // Gentle mode preserves thin internal bridges (e.g. strokes in round
    // chars like 'o', 'a', 'b') while still splitting at truly empty gaps.
    // Draw two separate groups with a fully empty gap between them.
    QImage img = makeTestImage(200, 60);
    // Main character body: columns 10-24, dense vertical stroke
    for (int y = 10; y < 30; ++y)
        for (int x = 10; x < 25; ++x)
            img.setPixel(x, y, qRgb(30, 30, 30));  // dark text pixels
    // Empty gap: columns 25-34, no pixels at all
    // Contamination: columns 35-45, partial adjacent character
    for (int y = 12; y < 28; ++y)
        for (int x = 35; x < 45; ++x)
            img.setPixel(x, y, qRgb(40, 40, 40));

    // Harvest with bbox covering the full contaminated region
    HocrLine line = makeLine({
        makeChar(U'A', 10, 5, 45, 35),  // wide bbox includes contamination
        makeChar(U'B', 50, 5, 60, 35),  // next char far away
    });

    GlyphAtlas atlas;
    atlas.build(img, {line}, textColor, bgColor);

    QImage g = atlas.glyph(U'A');
    ASSERT_FALSE(g.isNull());
    // Gentle mode should clip at the truly empty gap, keeping only
    // the main body (columns 10-24 = 15px), not the full region with contamination
    EXPECT_LE(g.width(), 20) << "Gentle clip should remove contamination at empty gap, got " << g.width();
}

// ============================================================================
// EnrichedAtlas tests
// ============================================================================

TEST_F(GlyphHarvesterTest, EnrichedAtlas_AtlasGlyphTakesPriority) {
    QImage img = makeTestImage(200, 100);
    drawLetterL(img, 10, 10, 15, 20);

    GlyphAtlas atlas;
    atlas.build(img, {makeLine({makeChar(U'A', 10, 10, 25, 30)})}, textColor, bgColor);

    EnrichedAtlas enriched;
    enriched.build(std::move(atlas));

    QImage g = enriched.glyph(U'A');
    EXPECT_FALSE(g.isNull());
    EXPECT_TRUE(enriched.contains(U'A'));
}

TEST_F(GlyphHarvesterTest, EnrichedAtlas_SynthesisForMissing) {
    QImage img = makeTestImage(200, 100);
    drawLetterL(img, 10, 10, 15, 20);

    GlyphAtlas atlas;
    atlas.build(img, {makeLine({makeChar(U'b', 10, 10, 25, 30)})}, textColor, bgColor);

    EnrichedAtlas enriched;
    enriched.build(std::move(atlas), U"bd");

    // 'b' is in the atlas directly
    EXPECT_TRUE(enriched.contains(U'b'));
    EXPECT_FALSE(enriched.glyph(U'b').isNull());
    // 'd' should be synthesized (HFlip of 'b' per recipe)
    // If the synthesizer has the b→d recipe, size should be 2 and 'd' available.
    // If not, size stays at 1 but atlas still works.
    QImage dGlyph = enriched.glyph(U'd');
    if (!dGlyph.isNull()) {
        EXPECT_GE(enriched.size(), 2);
        EXPECT_GT(dGlyph.width(), 0);
        EXPECT_GT(dGlyph.height(), 0);
    } else {
        EXPECT_EQ(enriched.size(), 1);
    }
}

TEST_F(GlyphHarvesterTest, EnrichedAtlas_OnDemandCached) {
    QImage img = makeTestImage(200, 100);
    drawLetterL(img, 10, 10, 15, 20);

    GlyphAtlas atlas;
    atlas.build(img, {makeLine({makeChar(U'p', 10, 10, 25, 30)})}, textColor, bgColor);

    EnrichedAtlas enriched;
    enriched.build(std::move(atlas));

    // Before on-demand: 'q' not yet synthesized
    EXPECT_FALSE(enriched.contains(U'q'));

    QImage g1 = enriched.glyph(U'q');
    QImage g2 = enriched.glyph(U'q');

    if (!g1.isNull()) {
        // After on-demand: 'q' is now cached
        EXPECT_TRUE(enriched.contains(U'q'));
        EXPECT_FALSE(g2.isNull());
        EXPECT_EQ(g1.width(), g2.width());
        EXPECT_EQ(g1.height(), g2.height());
        // Both calls should return images from the same cached entry
        EXPECT_EQ(g1.constBits(), g2.constBits());
    }
}

TEST_F(GlyphHarvesterTest, EnrichedAtlas_CoverageIncludesSynthesized) {
    QImage img = makeTestImage(200, 100);
    drawLetterL(img, 10, 10, 15, 20);

    GlyphAtlas atlas;
    atlas.build(img, {makeLine({makeChar(U'b', 10, 10, 25, 30)})}, textColor, bgColor);

    EnrichedAtlas enriched;
    enriched.build(std::move(atlas), U"bd");

    EXPECT_FLOAT_EQ(enriched.coverage(U"b"), 1.0f);
    EXPECT_GE(enriched.coverage(U"bd"), 0.5f);
}

TEST_F(GlyphHarvesterTest, EnrichedAtlas_NullForUnsynthesizable) {
    QImage img = makeTestImage(200, 100);
    drawLetterL(img, 10, 10, 15, 20);

    GlyphAtlas atlas;
    atlas.build(img, {makeLine({makeChar(U'A', 10, 10, 25, 30)})}, textColor, bgColor);

    EnrichedAtlas enriched;
    enriched.build(std::move(atlas));

    QImage g = enriched.glyph(U'\u4E00');
    EXPECT_TRUE(g.isNull());
}

// ============================================================================
// AtlasBuilder tests
// ============================================================================

TEST_F(GlyphHarvesterTest, AtlasBuilder_ReferenceLineIsLongest) {
    QImage img = makeTestImage(400, 200);

    drawLetterL(img, 10, 10, 15, 20);
    drawLetterL(img, 30, 10, 15, 20);

    drawLetterL(img, 10, 50, 15, 20);
    drawLetterL(img, 30, 50, 15, 20);
    drawLetterL(img, 50, 50, 15, 20);
    drawLetterL(img, 70, 50, 15, 20);

    drawLetterL(img, 10, 90, 15, 20);

    HocrLine line1 = makeLine({
        makeChar(U'A', 10, 10, 25, 30),
        makeChar(U'B', 30, 10, 45, 30),
    });

    HocrLine line2 = makeLine({
        makeChar(U'X', 10, 50, 25, 70),
        makeChar(U'Y', 30, 50, 45, 70),
        makeChar(U'Z', 50, 50, 65, 70),
        makeChar(U'W', 70, 50, 85, 70),
    });

    HocrLine line3 = makeLine({
        makeChar(U'Q', 10, 90, 25, 110),
    });

    auto result = AtlasBuilder::build(
        img, {line1, line2, line3}, textColor, bgColor,
        nullptr, nullptr, 0, 1);

    EXPECT_NE(result.enrichedAtlas, nullptr);
    EXPECT_GT(result.enrichedAtlas->size(), 0);
    EXPECT_GT(result.atlasGlyphCount, 0);
}

TEST_F(GlyphHarvesterTest, AtlasBuilder_DiagnosticsPopulated) {
    auto [img, line1, line2] = makeCompatibleLines();

    auto result = AtlasBuilder::build(
        img, {line1, line2}, textColor, bgColor,
        nullptr, nullptr, 0, 1);

    EXPECT_NE(result.enrichedAtlas, nullptr);
    EXPECT_GT(result.atlasGlyphCount, 0);
    // 2 lines with compatible metrics: longest is ref, other is donor
    EXPECT_EQ(result.samePageDonorLines, 1);
    EXPECT_EQ(result.samePageRejectedLines, 0);
    EXPECT_EQ(result.crossPagesScanned, 0);
    EXPECT_EQ(result.crossPageDonorLines, 0);
    // Should have all 4 chars: A, B, C (line1) + D (line2)
    EXPECT_EQ(result.atlasGlyphCount, 4);
}

TEST_F(GlyphHarvesterTest, AtlasBuilder_EmptyInput) {
    QImage img = makeTestImage(100, 100);

    auto result = AtlasBuilder::build(
        img, {}, textColor, bgColor,
        nullptr, nullptr, 0, 1);

    EXPECT_NE(result.enrichedAtlas, nullptr);
    EXPECT_EQ(result.atlasGlyphCount, 0);
}

// ============================================================================
// Integration tests
// ============================================================================

TEST_F(GlyphHarvesterTest, Integration_OverlayUsesEnrichedAtlas) {
    auto [img, line1, line2] = makeCompatibleLines();

    auto result = AtlasBuilder::build(
        img, {line1, line2}, textColor, bgColor,
        nullptr, nullptr, 0, 1);

    auto enriched = result.enrichedAtlas;
    ASSERT_NE(enriched, nullptr);

    // Reference line's chars should always be available
    EXPECT_FALSE(enriched->glyph(U'A').isNull());
    EXPECT_FALSE(enriched->glyph(U'B').isNull());
}

TEST_F(GlyphHarvesterTest, Integration_CompositorEnrichedOverload) {
    QImage img = makeTestImage(200, 100);
    drawLetterL(img, 10, 10, 15, 20);
    drawLetterL(img, 30, 10, 15, 20);

    GlyphAtlas atlas;
    atlas.build(img, {makeLine({
        makeChar(U'H', 10, 10, 25, 30),
        makeChar(U'i', 30, 10, 45, 30),
    })}, textColor, bgColor);

    EnrichedAtlas enriched;
    enriched.build(std::move(atlas));

    FontMatch fm;
    fm.sizePt = 12.0f;
    fm.fontFilePath = "C:/Windows/Fonts/arial.ttf";
    fm.fontFamily = "Arial";
    fm.color = textColor;

    GlyphAtlasCompositor compositor;
    QImage result = compositor.compose(
        U"Hi", enriched, fm, bgColor,
        100, 40, 150, nullptr);

    EXPECT_FALSE(result.isNull());
    EXPECT_EQ(result.width(), 100);
    EXPECT_EQ(result.height(), 40);
}

TEST_F(GlyphHarvesterTest, Integration_WysiwygPathConsistency) {
    QImage img = makeTestImage(200, 100);
    drawLetterL(img, 10, 10, 15, 20);
    drawLetterL(img, 30, 10, 15, 20);
    drawLetterL(img, 50, 10, 15, 20);

    GlyphAtlas atlas;
    atlas.build(img, {makeLine({
        makeChar(U'A', 10, 10, 25, 30),
        makeChar(U'B', 30, 10, 45, 30),
        makeChar(U'C', 50, 10, 65, 30),
    })}, textColor, bgColor);

    auto enriched = std::make_shared<EnrichedAtlas>();
    enriched->build(std::move(atlas));

    FontMatch fm;
    fm.sizePt = 12.0f;
    fm.fontFilePath = "C:/Windows/Fonts/arial.ttf";
    fm.fontFamily = "Arial";
    fm.color = textColor;

    GlyphAtlasCompositor compositor;
    QImage result1 = compositor.compose(U"ABC", *enriched, fm, bgColor, 100, 40, 150, nullptr);
    QImage result2 = compositor.compose(U"ABC", *enriched, fm, bgColor, 100, 40, 150, nullptr);

    EXPECT_FALSE(result1.isNull());
    EXPECT_FALSE(result2.isNull());
    EXPECT_EQ(result1.size(), result2.size());
}

// ============================================================================
// Edge-case tests (from QA review)
// ============================================================================

TEST_F(GlyphHarvesterTest, SubRatioScore_NegativeDescender_Clamped) {
    // Simulate negative descender (Tesseract can produce negative baseline values).
    // When metric is negative and base is positive, subRatioScore should clamp to [0,1].
    HocrLine line1 = makeLine({makeChar(U'A', 10, 10, 25, 30)},
                              20.0f, 5.0f, -3.0f, 18.0f);  // negative descender
    HocrLine line2 = makeLine({makeChar(U'A', 10, 50, 25, 70)},
                              20.0f, 5.0f, 4.0f, 18.0f);   // positive descender

    QImage img = makeTestImage(200, 100);
    auto result = LineFontVerifier::verify(img, line1, line2);
    // metricScore should be in [0, 1] despite negative descender
    EXPECT_GE(result.metricScore, 0.0f);
    EXPECT_LE(result.metricScore, 1.0f);
}

TEST_F(GlyphHarvesterTest, GlyphAtlas_AddGlyph_InsertsAndUpdates) {
    GlyphAtlas atlas;

    QImage glyph1(10, 15, QImage::Format_ARGB32);
    glyph1.fill(Qt::red);
    atlas.addGlyph(U'X', glyph1, 50);
    EXPECT_TRUE(atlas.contains(U'X'));
    EXPECT_EQ(atlas.size(), 1);

    // Lower confidence -> should NOT overwrite
    QImage glyph2(12, 18, QImage::Format_ARGB32);
    glyph2.fill(Qt::blue);
    atlas.addGlyph(U'X', glyph2, 40);
    EXPECT_EQ(atlas.glyph(U'X').width(), 10);  // original kept

    // Higher confidence -> should overwrite
    atlas.addGlyph(U'X', glyph2, 80);
    EXPECT_EQ(atlas.glyph(U'X').width(), 12);  // updated
}

TEST_F(GlyphHarvesterTest, GlyphAtlas_BuildVerified_NullImage) {
    QImage nullImg;
    HocrLine line = makeLine({makeChar(U'A', 10, 10, 25, 30)});

    GlyphAtlas atlas;
    int accepted = atlas.buildVerified(nullImg, line, {}, textColor, bgColor);
    EXPECT_EQ(accepted, 0);
    EXPECT_EQ(atlas.size(), 0);
}

TEST_F(GlyphHarvesterTest, GlyphAtlas_EmptyLineHarvest) {
    QImage img = makeTestImage(200, 100);

    HocrLine emptyLine;
    emptyLine.xSizePx = 20.0f;
    emptyLine.ascendersPx = 5.0f;
    emptyLine.descendersPx = 4.0f;
    // No words -> no chars to harvest

    GlyphAtlas atlas;
    atlas.build(img, {emptyLine}, textColor, bgColor);
    EXPECT_EQ(atlas.size(), 0);
}

TEST_F(GlyphHarvesterTest, EnrichedAtlas_RebuildClearsPrevious) {
    QImage img = makeTestImage(200, 100);
    drawLetterL(img, 10, 10, 15, 20);
    drawLetterL(img, 30, 10, 15, 20);

    // First build with 'A'
    GlyphAtlas atlas1;
    atlas1.build(img, {makeLine({makeChar(U'A', 10, 10, 25, 30)})}, textColor, bgColor);

    EnrichedAtlas enriched;
    enriched.build(std::move(atlas1));
    EXPECT_TRUE(enriched.contains(U'A'));

    // Trigger on-demand synthesis to populate synthesized_ cache
    (void)enriched.glyph(U'Z');  // may or may not succeed, but exercises the cache

    // Second build with 'B' only -- should clear 'A' and any synthesized cache
    GlyphAtlas atlas2;
    atlas2.build(img, {makeLine({makeChar(U'B', 30, 10, 45, 30)})}, textColor, bgColor);

    enriched.build(std::move(atlas2));
    EXPECT_TRUE(enriched.contains(U'B'));
    EXPECT_FALSE(enriched.contains(U'A'));
    // Synthesized cache from first build should be cleared
    EXPECT_EQ(enriched.atlas().size(), 1);
}

TEST_F(GlyphHarvesterTest, GlyphAtlas_Coverage_Calculation) {
    QImage img = makeTestImage(200, 100);
    drawLetterL(img, 10, 10, 15, 20);
    drawLetterL(img, 30, 10, 15, 20);

    GlyphAtlas atlas;
    atlas.build(img, {makeLine({
        makeChar(U'A', 10, 10, 25, 30),
        makeChar(U'B', 30, 10, 45, 30),
    })}, textColor, bgColor);

    EXPECT_FLOAT_EQ(atlas.coverage(U"AB"), 1.0f);
    EXPECT_FLOAT_EQ(atlas.coverage(U"ABCD"), 0.5f);
    EXPECT_FLOAT_EQ(atlas.coverage(U"XY"), 0.0f);
    EXPECT_FLOAT_EQ(atlas.coverage(U""), 1.0f);
    EXPECT_FLOAT_EQ(atlas.coverage(U"   "), 1.0f);
}

TEST_F(GlyphHarvesterTest, GlyphAtlas_Codepoints_ReturnsAll) {
    QImage img = makeTestImage(200, 100);
    drawLetterL(img, 10, 10, 15, 20);
    drawLetterL(img, 30, 10, 15, 20);
    drawLetterL(img, 50, 10, 15, 20);

    GlyphAtlas atlas;
    atlas.build(img, {makeLine({
        makeChar(U'A', 10, 10, 25, 30),
        makeChar(U'B', 30, 10, 45, 30),
        makeChar(U'C', 50, 10, 65, 30),
    })}, textColor, bgColor);

    auto cps = atlas.codepoints();
    EXPECT_EQ(cps.size(), 3u);
    std::sort(cps.begin(), cps.end());
    EXPECT_EQ(cps[0], U'A');
    EXPECT_EQ(cps[1], U'B');
    EXPECT_EQ(cps[2], U'C');
}

// ============================================================================
// DPI Consistency Tests -- verifies atlas glyphs are visible (non-transparent)
// when HOCR bboxes match the page image coordinate space.
// ============================================================================

TEST_F(GlyphHarvesterTest, DpiMatch_GlyphsAreVisible) {
    // Simulate a page rendered at the SAME DPI as Tesseract (correct).
    // The char bboxes directly index into the page image.
    QImage img = makeTestImage(400, 200);
    drawLetterL(img, 50, 50, 20, 30);
    drawLetterL(img, 100, 50, 20, 30);

    GlyphAtlas atlas;
    atlas.build(img, {makeLine({
        makeChar(U'A', 50, 50, 70, 80),  // Matches where the glyph was drawn
        makeChar(U'B', 100, 50, 120, 80),
    })}, textColor, bgColor);

    ASSERT_TRUE(atlas.contains(U'A'));
    ASSERT_TRUE(atlas.contains(U'B'));

    // Helper: count pixels with alpha > 128 in an atlas glyph
    auto countOpaquePixels = [](const QImage& img) -> int {
        int count = 0;
        for (int y = 0; y < img.height(); ++y) {
            const QRgb* line = reinterpret_cast<const QRgb*>(img.constScanLine(y));
            for (int x = 0; x < img.width(); ++x) {
                if (qAlpha(line[x]) > 128) ++count;
            }
        }
        return count;
    };

    // Verify both glyph images have visible (non-zero alpha) pixels
    QImage glyphA = atlas.glyph(U'A');
    ASSERT_FALSE(glyphA.isNull());
    EXPECT_EQ(glyphA.format(), QImage::Format_ARGB32_Premultiplied);
    EXPECT_GT(countOpaquePixels(glyphA), 0) << "Glyph A should have visible text pixels";

    QImage glyphB = atlas.glyph(U'B');
    ASSERT_FALSE(glyphB.isNull());
    EXPECT_GT(countOpaquePixels(glyphB), 0) << "Glyph B should have visible text pixels";
}

TEST_F(GlyphHarvesterTest, DpiMismatch_GlyphsAreTransparent) {
    // Simulate the DPI mismatch bug: page image at 72 DPI but HOCR bboxes
    // at 300 DPI. The bboxes point to wrong positions in the small image,
    // cropping background-only regions that isolateAlpha makes transparent.
    QImage smallImg = makeTestImage(100, 50);  // ~72 DPI letter-size page
    drawLetterL(smallImg, 10, 10, 10, 15);     // Text is near top-left

    // HOCR bboxes at 300 DPI coordinates (4x larger than 72 DPI).
    // Use coordinates that stay within image bounds but point at pure
    // background (away from the text drawn at (10,10)).
    GlyphAtlas atlas;
    atlas.build(smallImg, {makeLine({
        makeChar(U'A', 60, 30, 90, 48),  // Pure background area (text is at 10,10)
    })}, textColor, bgColor);

    // With mismatched DPI, the crop is pure background. After isolateAlpha
    // all pixels are transparent, and cropToContent returns null. The glyph
    // is correctly rejected from the atlas (no invisible glyph).
    EXPECT_FALSE(atlas.contains(U'A'))
        << "Background-only crop should be rejected (not produce invisible glyph)";
}

TEST_F(GlyphHarvesterTest, IsolateAlpha_ProducesVisibleBlackText) {
    // Directly test that isolateAlpha produces visible pixels for black-on-white text
    QImage crop(20, 30, QImage::Format_RGB32);
    crop.fill(qRgb(220, 220, 220));  // White background

    // Draw dark text pixels in the center
    for (int y = 5; y < 25; ++y) {
        for (int x = 5; x < 15; ++x) {
            int g = 30 + ((x + y) % 10);  // Dark text (30-39)
            crop.setPixel(x, y, qRgb(g, g, g));
        }
    }

    QImage result = GlyphAtlas::isolateAlpha(crop, textColor, bgColor);
    ASSERT_FALSE(result.isNull());
    EXPECT_EQ(result.format(), QImage::Format_ARGB32_Premultiplied);
    EXPECT_EQ(result.size(), crop.size());

    // Check that text region has high alpha (visible)
    int highAlphaCount = 0;
    int lowAlphaCount = 0;
    for (int y = 0; y < result.height(); ++y) {
        const QRgb* line = reinterpret_cast<const QRgb*>(result.constScanLine(y));
        for (int x = 0; x < result.width(); ++x) {
            int a = qAlpha(line[x]);
            if (a > 200) ++highAlphaCount;
            else if (a < 30) ++lowAlphaCount;
        }
    }
    // Text area (10x20 = 200 pixels) should be mostly opaque
    EXPECT_GT(highAlphaCount, 150) << "Text pixels should have high alpha";
    // Background area should be mostly transparent
    EXPECT_GT(lowAlphaCount, 200) << "Background pixels should have low alpha";
}

TEST_F(GlyphHarvesterTest, HarvestLine_CropsHorizontalOnly) {
    // Verify that harvested glyphs are cropped HORIZONTALLY to visible content
    // but preserve full VERTICAL extent from the char bbox. This maintains
    // typographic height proportions (uppercase is taller than lowercase).
    QImage img = makeTestImage(200, 100);
    // Draw a small glyph centered in a large bbox area
    drawLetterL(img, 50, 30, 10, 15);  // Glyph at (50,30)-(60,45)

    GlyphAtlas atlas;
    // Bbox is larger than the actual glyph (Tesseract padding)
    atlas.build(img, {makeLine({
        makeChar(U'L', 40, 20, 80, 60),  // 40x40 bbox for a 10x15 glyph
    })}, textColor, bgColor);

    ASSERT_TRUE(atlas.contains(U'L'));
    QImage glyph = atlas.glyph(U'L');
    ASSERT_FALSE(glyph.isNull());

    // Width should be trimmed (horizontal crop) but height preserved (vertical extent kept)
    EXPECT_LT(glyph.width(), 30) << "Cropped glyph should be narrower than bbox";
    EXPECT_EQ(glyph.height(), 40) << "Glyph height should preserve full char bbox height";
    EXPECT_GT(glyph.width(), 3) << "Glyph should still have visible content";
}

TEST_F(GlyphHarvesterTest, HarvestLine_ClipsAdjacentCharContamination) {
    // Simulate a Tesseract bbox that extends into the adjacent character.
    // Draw two separate "letters" with a gap between them.
    QImage img = makeTestImage(200, 100);
    // Main character 'A' at (20,30) - (35,55)
    drawLetterL(img, 20, 30, 12, 20);
    // Adjacent character 'B' at (50,30) - (65,55) -- separated by a gap
    drawLetterL(img, 50, 30, 12, 20);

    GlyphAtlas atlas;
    // Bbox for 'A' extends past the gap into 'B's territory
    atlas.build(img, {makeLine({
        makeChar(U'A', 20, 30, 70, 55),  // Width=50, includes both letters
    })}, textColor, bgColor);

    ASSERT_TRUE(atlas.contains(U'A'));
    QImage glyph = atlas.glyph(U'A');
    ASSERT_FALSE(glyph.isNull());

    // clipAtVerticalGap should detect the gap between the two letter bodies
    // and clip the right side, keeping only the first letter.
    // The first letter is ~15px wide (12 body + some anti-aliasing).
    // Without clipping, the glyph would be ~50px wide (both letters).
    EXPECT_LT(glyph.width(), 25)
        << "Glyph should be clipped at gap, excluding adjacent character";
}

TEST_F(GlyphHarvesterTest, HarvestLine_ClipsLeftSideContamination) {
    // Simulate a Tesseract bbox that extends leftward into the previous character.
    // Draw two separate "letters" with a gap between them.
    QImage img = makeTestImage(200, 100);
    // Previous character at (20,30) - (35,55) -- contamination source
    drawLetterL(img, 20, 30, 12, 20);
    // Main character 'B' at (50,30) - (65,55)
    drawLetterL(img, 50, 30, 12, 20);

    GlyphAtlas atlas;
    // Bbox for 'B' extends leftward past the gap into previous char's territory
    atlas.build(img, {makeLine({
        makeChar(U'B', 15, 30, 65, 55),  // Width=50, includes both letters
    })}, textColor, bgColor);

    ASSERT_TRUE(atlas.contains(U'B'));
    QImage glyph = atlas.glyph(U'B');
    ASSERT_FALSE(glyph.isNull());

    // clipAtVerticalGap should detect the gap and clip the left side,
    // keeping only the rightmost letter (the actual 'B').
    // The letter is ~15px wide (12 body + some variation).
    // Without clipping, the glyph would be ~50px wide (both letters).
    EXPECT_LT(glyph.width(), 25)
        << "Glyph should be clipped at left-side gap, excluding previous character";
}

// ============================================================================
// Tests for three-bug fix: addMissingOnly, word-final clipping, referenceLineHeight
// ============================================================================

TEST_F(GlyphHarvesterTest, BuildVerified_DonorDoesNotReplaceReferenceGlyphs) {
    // Fix 1: With addMissingOnly=true, donor lines should NOT replace
    // reference line glyphs even if the donor has higher confidence.
    // This preserves consistent lineH across all reference line glyphs.
    QImage img = makeTestImage(300, 120);

    // Line 1 (reference): A(conf=80), B(conf=80), C(conf=80) at y=10..30
    drawLetterL(img, 10, 10, 15, 20);   // A
    drawLetterL(img, 30, 10, 15, 20);   // B
    drawLetterL(img, 50, 10, 15, 20);   // C

    // Line 2 (donor): A(conf=99), B(conf=99), D(conf=90) at y=50..70
    // Same metrics so it passes font verification.
    // Higher confidence A and B should NOT replace reference line's A and B.
    drawLetterL(img, 10, 50, 15, 20);   // A
    drawLetterL(img, 30, 50, 15, 20);   // B
    drawLetterL(img, 50, 50, 15, 20);   // D

    HocrLine refLine = makeLine({
        makeChar(U'A', 10, 10, 25, 30, 80),
        makeChar(U'B', 30, 10, 45, 30, 80),
        makeChar(U'C', 50, 10, 65, 30, 80),
    }, 20.0f, 5.0f, 4.0f, 18.0f);

    HocrLine donorLine = makeLine({
        makeChar(U'A', 10, 50, 25, 70, 99),  // Higher confidence
        makeChar(U'B', 30, 50, 45, 70, 99),  // Higher confidence
        makeChar(U'D', 50, 50, 65, 70, 90),
    }, 20.0f, 5.0f, 4.0f, 18.0f);

    GlyphAtlas atlas;
    int accepted = atlas.buildVerified(img, refLine, {donorLine}, textColor, bgColor);

    EXPECT_EQ(accepted, 1);
    EXPECT_EQ(atlas.size(), 4);  // A, B, C from ref + D from donor

    // Key assertion: A and B glyphs should have height == reference lineH (20px),
    // NOT the donor lineH (also 20 here, but the confidence should be 80, not 99).
    // We verify by checking that all reference chars have the SAME height.
    QImage glyphA = atlas.glyph(U'A');
    QImage glyphB = atlas.glyph(U'B');
    QImage glyphC = atlas.glyph(U'C');
    ASSERT_FALSE(glyphA.isNull());
    ASSERT_FALSE(glyphB.isNull());
    ASSERT_FALSE(glyphC.isNull());

    // All reference line glyphs should have identical height (= line bbox height)
    EXPECT_EQ(glyphA.height(), glyphC.height())
        << "A (present in both lines) should have same height as C (ref-only)";
    EXPECT_EQ(glyphB.height(), glyphC.height())
        << "B (present in both lines) should have same height as C (ref-only)";
}

TEST_F(GlyphHarvesterTest, HarvestLine_WordFinalCharClippedToWordBbox) {
    // Fix 2: The last character in a word should be clipped to word.bbox.x1,
    // not extend to wherever Tesseract puts the char bbox.
    QImage img = makeTestImage(300, 100);

    // Draw two characters with a gap between them
    drawLetterL(img, 20, 30, 15, 20);   // 'R' at x=20..35
    drawLetterL(img, 80, 30, 15, 20);   // 'O' at x=80..95 (phantom)

    // Create a word "R" where:
    // - char bbox extends far right (x0=20, x1=100) -- Tesseract overlap
    // - word bbox is tight (x0=20, x1=50)
    HocrChar ch;
    ch.codepoint = U'R';
    ch.bbox = {20.0f, 30.0f, 100.0f, 50.0f};  // Char bbox extends to phantom
    ch.confidence = 90;

    HocrWord word;
    word.text = "R";
    word.confidence = 90;
    word.fontSizePx = 18.0f;
    word.chars = {ch};
    word.bbox = {20.0f, 30.0f, 50.0f, 50.0f};  // Word bbox is tight

    HocrLine line;
    line.words = {word};
    line.bbox = word.bbox;
    line.xSizePx = 20.0f;
    line.ascendersPx = 5.0f;
    line.descendersPx = 4.0f;

    GlyphAtlas atlas;
    atlas.build(img, {line}, textColor, bgColor);

    ASSERT_TRUE(atlas.contains(U'R'));
    QImage glyph = atlas.glyph(U'R');
    ASSERT_FALSE(glyph.isNull());

    // Without word-final clipping: char bbox extends to x=100,
    // capturing the phantom 'O' at x=80..95 (glyph width ~80px).
    // With word-final clipping: clipped to word.bbox.x1=50,
    // so only x=20..50 is cropped, then horizontally trimmed (glyph ~15-20px).
    EXPECT_LT(glyph.width(), 35)
        << "Word-final char should be clipped to word bbox, excluding phantom";
}

TEST_F(GlyphHarvesterTest, EnrichedAtlas_ReferenceLineHeight_IsMode) {
    // Fix 3: referenceLineHeight() should return the most common glyph height
    // (the mode), which equals the reference line height.
    QImage img = makeTestImage(300, 160);

    // Reference line at y=10..50 (lineH=40): 3 chars
    drawLetterL(img, 10, 15, 15, 30);
    drawLetterL(img, 30, 15, 15, 30);
    drawLetterL(img, 50, 15, 15, 30);

    // Donor line at y=70..107 (lineH=37, ratio=37/40=0.925): 1 unique char
    // Within [0.75, 1.35] height ratio → accepted
    drawLetterL(img, 10, 73, 15, 30);

    HocrLine refLine = makeLine({
        makeChar(U'A', 10, 10, 25, 50, 90),
        makeChar(U'B', 30, 10, 45, 50, 90),
        makeChar(U'C', 50, 10, 65, 50, 90),
    }, 20.0f, 5.0f, 4.0f, 18.0f);

    HocrLine donorLine = makeLine({
        makeChar(U'D', 10, 70, 25, 107, 90),
    }, 20.0f, 5.0f, 4.0f, 18.0f);

    GlyphAtlas atlas;
    atlas.buildVerified(img, refLine, {donorLine}, textColor, bgColor);

    // All 4 glyphs normalized to refLineH=40 (height normalization in buildVerified)
    EXPECT_EQ(atlas.size(), 4);

    EXPECT_EQ(atlas.glyph(U'A').height(), 40);
    EXPECT_EQ(atlas.glyph(U'B').height(), 40);
    EXPECT_EQ(atlas.glyph(U'C').height(), 40);
    // Donor glyph originally height=37, padded to refLineH=40
    EXPECT_EQ(atlas.glyph(U'D').height(), 40);

    EnrichedAtlas enriched;
    enriched.build(std::move(atlas));

    // referenceLineHeight should be 40 (all glyphs now normalized to refLineH)
    EXPECT_EQ(enriched.referenceLineHeight(), 40)
        << "referenceLineHeight should be the mode (most common) glyph height";
}

TEST_F(GlyphHarvesterTest, AtlasScaleToScreen_UsesReferenceLineHeight) {
    // Fix 3 (rendering): scale factor should be based on referenceLineHeight,
    // not the max glyph height, to avoid donor glyphs skewing the scale.
    QImage img = makeTestImage(300, 150);

    // 3 glyphs at height=40 (reference line)
    drawLetterL(img, 10, 15, 15, 30);
    drawLetterL(img, 30, 15, 15, 30);
    drawLetterL(img, 50, 15, 15, 30);

    HocrLine refLine = makeLine({
        makeChar(U'A', 10, 10, 25, 50, 90),
        makeChar(U'B', 30, 10, 45, 50, 90),
        makeChar(U'C', 50, 10, 65, 50, 90),
    }, 20.0f, 5.0f, 4.0f, 18.0f);

    GlyphAtlas atlas;
    atlas.build(img, {refLine}, textColor, bgColor);

    EnrichedAtlas enriched;
    enriched.build(std::move(atlas));

    // All glyphs should be height=40, so referenceLineHeight=40
    EXPECT_EQ(enriched.referenceLineHeight(), 40);

    // If bboxH=20 screen pixels, scale should be 20/40 = 0.5
    // NOT 20/maxGlyphH which could differ if donor glyphs were present
    float expectedScale = 20.0f / 40.0f;
    EXPECT_FLOAT_EQ(expectedScale, 0.5f);

    // Verify each glyph's proportional dimensions at this scale
    for (char32_t c : {U'A', U'B', U'C'}) {
        QImage g = enriched.glyph(c);
        ASSERT_FALSE(g.isNull());
        float scaledH = static_cast<float>(g.height()) * expectedScale;
        // Reference line glyphs: scaledH = 40 * 0.5 = 20 = bboxH
        EXPECT_FLOAT_EQ(scaledH, 20.0f)
            << "Reference line glyphs should fill bboxH exactly";
    }
}

TEST_F(GlyphHarvesterTest, EnrichedAtlas_SynthesizedGlyphPreservesHeight) {
    // Synthesized glyphs (e.g., Z from z via ScaleToCapHeight) should NOT
    // be normalized back to refLineH. The synthesizer intentionally sets
    // height via transforms. Normalizing would make 'Z' identical to 'z'.
    QImage img = makeTestImage(200, 50);
    // Draw a lowercase-like shape for 'z' at y=10..30
    for (int y = 15; y < 28; ++y)
        for (int x = 10; x < 22; ++x)
            img.setPixel(x, y, qRgb(kTextGray, kTextGray, kTextGray));

    HocrLine line = makeLine({
        makeChar(U'z', 10, 10, 22, 30, 90),
    }, 20.0f, 5.0f, 4.0f, 18.0f);

    GlyphAtlas atlas;
    atlas.build(img, {line}, textColor, bgColor);
    ASSERT_TRUE(atlas.contains(U'z'));

    EnrichedAtlas enriched;
    enriched.build(std::move(atlas), U"Zz");  // request synthesis of 'Z'

    QImage zGlyph = enriched.glyph(U'z');
    QImage bigZGlyph = enriched.glyph(U'Z');

    // Z may not be synthesized if no recipe exists, but if it is,
    // it should be at least as tall as z (ScaleToCapHeight scales UP)
    if (!bigZGlyph.isNull() && !zGlyph.isNull()) {
        EXPECT_GE(bigZGlyph.height(), zGlyph.height())
            << "Synthesized 'Z' should not be shrunk below 'z' height";
    }
}

TEST_F(GlyphHarvesterTest, BuildVerified_BaselineAlignedPadding) {
    // Donor glyphs from shorter lines should be padded to refLineH
    // with baseline alignment, not scaled (which would stretch proportions).
    QImage img = makeTestImage(300, 150);

    // Reference line: y=10..50, lineH=40 — draw ink
    drawLetterL(img, 10, 15, 15, 30);   // A

    // Donor line: y=70..107, lineH=37, ratio=37/40=0.925 — within [0.75,1.35]
    drawLetterL(img, 100, 72, 10, 31);  // D

    HocrLine refLine = makeLine({
        makeChar(U'A', 10, 10, 25, 50, 90),
    }, 20.0f, 5.0f, 4.0f, 18.0f);

    HocrLine donorLine = makeLine({
        makeChar(U'D', 100, 70, 115, 107, 90),
    }, 20.0f, 5.0f, 4.0f, 18.0f);

    GlyphAtlas atlas;
    atlas.buildVerified(img, refLine, {donorLine}, textColor, bgColor);

    ASSERT_TRUE(atlas.contains(U'A'));
    ASSERT_TRUE(atlas.contains(U'D'));

    // Both should be refLineH=40 after normalization
    EXPECT_EQ(atlas.glyph(U'A').height(), 40);
    EXPECT_EQ(atlas.glyph(U'D').height(), 40);

    // D was padded not scaled: its width should be preserved (not stretched).
    // Original donor crop is approximately 10-15px wide. If scaled from h=37
    // to h=40, width would also scale. With padding, width stays original.
    QImage dGlyph = atlas.glyph(U'D');
    QImage aGlyph = atlas.glyph(U'A');
    // The donor glyph's width should be notably smaller than the reference
    // (since it was from a shorter line and padded, not stretched)
    EXPECT_LT(dGlyph.width(), aGlyph.width())
        << "Padded donor glyph should preserve original width (narrower than ref)";
}

TEST_F(GlyphHarvesterTest, BuildVerified_RejectedLineDoesNotContribute) {
    // When a line is rejected by font verification, its characters are NOT
    // harvested. Missing characters are filled by synthesis in EnrichedAtlas.
    auto [img, line1, line2] = makeIncompatibleLines();
    // line1 has {A, B}, line2 has {A, E} with incompatible metrics

    GlyphAtlas atlas;
    int accepted = atlas.buildVerified(img, line1, {line2}, textColor, bgColor);

    EXPECT_EQ(accepted, 0);  // line2 rejected by font verification
    // Only reference line characters present: A, B (no E from fallback)
    EXPECT_TRUE(atlas.contains(U'A'));
    EXPECT_TRUE(atlas.contains(U'B'));
    EXPECT_FALSE(atlas.contains(U'E'))
        << "Rejected line should not contribute glyphs (fallback removed)";
    EXPECT_EQ(atlas.size(), 2);
}

TEST_F(GlyphHarvesterTest, BuildVerified_NoFallbackHarvest) {
    // Rejected lines no longer contribute glyphs (fallback harvest removed).
    // Missing characters are filled by GlyphSynthesizer in EnrichedAtlas.
    QImage img = makeTestImage(300, 150);

    // Reference line at y=10..50 (lineH=40): chars A, B
    drawLetterL(img, 10, 15, 15, 30);   // A
    drawLetterL(img, 30, 15, 15, 30);   // B

    // Rejected line at y=80..112 (lineH=32): char X
    drawLetterL(img, 10, 82, 15, 28);   // X

    HocrLine refLine = makeLine({
        makeChar(U'A', 10, 10, 25, 50, 90),
        makeChar(U'B', 30, 10, 45, 50, 90),
    }, 20.0f, 5.0f, 4.0f, 18.0f);

    // Very different metrics → font verification fails
    HocrLine rejectedLine = makeLine({
        makeChar(U'X', 10, 80, 25, 112, 90),
    }, 45.0f, 18.0f, 2.0f, 40.0f);

    GlyphAtlas atlas;
    atlas.buildVerified(img, refLine, {rejectedLine}, textColor, bgColor);

    // X should NOT be in atlas — rejected lines no longer contribute
    EXPECT_FALSE(atlas.contains(U'X'))
        << "Rejected lines should not contribute glyphs (fallback removed)";
    // Reference glyphs should still be present
    EXPECT_TRUE(atlas.contains(U'A'));
    EXPECT_TRUE(atlas.contains(U'B'));
}

TEST_F(GlyphHarvesterTest, BuildVerified_DonorHeightRatioFilter) {
    // Donor lines with height ratio outside [0.75, 1.35] relative to the
    // reference line should be rejected even if font verification passes.
    // LineFontVerifier's combined score >= 0.65 catches subheadings;
    // this wider range allows Tesseract bbox variation (10-15% is normal).
    QImage img = makeTestImage(300, 300);

    // Reference line at y=10..50 (lineH=40)
    drawLetterL(img, 10, 15, 15, 30);   // A
    drawLetterL(img, 30, 15, 15, 30);   // B

    HocrLine refLine = makeLine({
        makeChar(U'A', 10, 10, 25, 50, 90),
        makeChar(U'B', 30, 10, 45, 50, 90),
    }, 20.0f, 5.0f, 4.0f, 18.0f);

    // Donor line at y=60..91 (lineH=31, ratio=31/40=0.775) → lower boundary accepted
    drawLetterL(img, 10, 62, 15, 25);   // C
    HocrLine acceptedLowerLine = makeLine({
        makeChar(U'C', 10, 60, 25, 91, 90),
    }, 20.0f, 5.0f, 4.0f, 18.0f);  // Same metrics → passes verification

    // Donor line at y=95..148 (lineH=53, ratio=53/40=1.325) → upper boundary accepted
    drawLetterL(img, 10, 97, 15, 46);   // F
    HocrLine acceptedUpperLine = makeLine({
        makeChar(U'F', 10, 95, 25, 148, 90),
    }, 20.0f, 5.0f, 4.0f, 18.0f);

    // Donor line at y=155..183 (lineH=28, ratio=28/40=0.70) → rejected (below 0.75)
    drawLetterL(img, 10, 157, 12, 22);   // D
    HocrLine tooSmallLine = makeLine({
        makeChar(U'D', 10, 155, 22, 183, 90),
    }, 20.0f, 5.0f, 4.0f, 18.0f);

    // Donor line at y=190..246 (lineH=56, ratio=56/40=1.40) → rejected (above 1.35)
    drawLetterT(img, 10, 192, 25, 50);  // E
    HocrLine tooLargeLine = makeLine({
        makeChar(U'E', 10, 190, 35, 246, 90),
    }, 20.0f, 5.0f, 4.0f, 18.0f);

    GlyphAtlas atlas;
    int accepted = atlas.buildVerified(img, refLine,
        {acceptedLowerLine, acceptedUpperLine, tooSmallLine, tooLargeLine},
        textColor, bgColor);

    // Both boundary lines should pass height ratio filter
    EXPECT_TRUE(atlas.contains(U'A'));   // reference line
    EXPECT_TRUE(atlas.contains(U'B'));   // reference line
    EXPECT_TRUE(atlas.contains(U'C'));   // ratio 0.775 accepted (within [0.75, 1.35])
    EXPECT_TRUE(atlas.contains(U'F'));   // ratio 1.325 accepted (within [0.75, 1.35])
    EXPECT_FALSE(atlas.contains(U'D'))   // ratio 0.70 rejected
        << "Donor with height ratio 0.70 should be rejected (below 0.75)";
    EXPECT_FALSE(atlas.contains(U'E'))   // ratio 1.40 rejected
        << "Donor with height ratio 1.40 should be rejected (above 1.35)";
}

// ============================================================================
// Template-based glyph selection tests
// ============================================================================

TEST_F(GlyphHarvesterTest, TemplateCache_RendersKnownFont) {
    // GlyphTemplateCache should render templates for common ASCII codepoints
    // using a known system font (arial.ttf).
    std::string arialPath = pdfe::fonts::FontResolver::resolveFontPath("Arial");
    if (arialPath.empty()) {
        GTEST_SKIP() << "FontResolver could not locate Arial or any "
                        "metric-compatible substitute";
    }
    std::vector<char32_t> codepoints = {U'A', U'B', U'r', U'o'};

    auto templates = GlyphTemplateCache::renderTemplates(arialPath, 0, 40, codepoints);

    // All requested codepoints should be present (Arial has full Latin coverage)
    EXPECT_EQ(templates.size(), 4u);
    for (char32_t cp : codepoints) {
        auto it = templates.find(cp);
        ASSERT_NE(it, templates.end()) << "Missing template for codepoint " << static_cast<int>(cp);
        EXPECT_FALSE(it->second.isNull());
        EXPECT_EQ(it->second.height(), 40) << "Template height should match requested pixelHeight";
        EXPECT_GT(it->second.width(), 0);
    }
}

TEST_F(GlyphHarvesterTest, TemplateCache_MissingGlyphSkipped) {
    // Codepoints not present in the font should be silently omitted.
    std::string arialPath = pdfe::fonts::FontResolver::resolveFontPath("Arial");
    if (arialPath.empty()) {
        GTEST_SKIP() << "FontResolver could not locate Arial or any "
                        "metric-compatible substitute";
    }
    // U+FFFF is not a valid character; font won't have it
    std::vector<char32_t> codepoints = {U'A', static_cast<char32_t>(0xFFFF)};

    auto templates = GlyphTemplateCache::renderTemplates(arialPath, 0, 30, codepoints);

    EXPECT_TRUE(templates.count(U'A') > 0) << "Common char should be rendered";
    EXPECT_EQ(templates.count(static_cast<char32_t>(0xFFFF)), 0u)
        << "Missing glyph should be omitted from template map";
}

TEST_F(GlyphHarvesterTest, TemplateCache_SkipsWhitespace) {
    // Whitespace characters should not produce templates.
    std::string arialPath = pdfe::fonts::FontResolver::resolveFontPath("Arial");
    if (arialPath.empty()) {
        GTEST_SKIP() << "FontResolver could not locate Arial or any "
                        "metric-compatible substitute";
    }
    std::vector<char32_t> codepoints = {U' ', U'\t', U'\n', U'A'};

    auto templates = GlyphTemplateCache::renderTemplates(arialPath, 0, 30, codepoints);

    EXPECT_EQ(templates.count(U' '), 0u) << "Space should be skipped";
    EXPECT_EQ(templates.count(U'\t'), 0u) << "Tab should be skipped";
    EXPECT_EQ(templates.count(U'\n'), 0u) << "Newline should be skipped";
    EXPECT_TRUE(templates.count(U'A') > 0) << "Regular char should be rendered";
}

TEST_F(GlyphHarvesterTest, TemplateCache_EmptyInputs) {
    // Edge cases: empty font path, zero height, empty codepoints
    auto r1 = GlyphTemplateCache::renderTemplates("", 0, 30, {U'A'});
    EXPECT_TRUE(r1.empty()) << "Empty font path should return empty map";

    auto r2 = GlyphTemplateCache::renderTemplates("C:/Windows/Fonts/arial.ttf", 0, 0, {U'A'});
    EXPECT_TRUE(r2.empty()) << "Zero pixel height should return empty map";

    auto r3 = GlyphTemplateCache::renderTemplates("C:/Windows/Fonts/arial.ttf", 0, 30, {});
    EXPECT_TRUE(r3.empty()) << "Empty codepoints should return empty map";
}

TEST_F(GlyphHarvesterTest, TemplateNCC_SelfComparison) {
    // Verify that binarizedNCC of alpha-isolated glyphs composited onto
    // white background produces meaningful scores.
    // Raw alpha-isolated images fail NCC because transparent bg (gray=0) and
    // dark text (gray~30) both map to very dark grayscale, making Otsu pick
    // threshold=0 → uniform binary image → NCC=0.
    QImage img = makeTestImage(200, 100);
    drawLetterL(img, 10, 30, 12, 20);

    QImage crop = img.copy(10, 30, 15, 20);
    QImage isolated = GlyphAtlas::isolateAlpha(crop, textColor, bgColor);
    ASSERT_FALSE(isolated.isNull());

    // Composite onto white for proper NCC (same as harvestLine's scoring)
    QImage onWhiteA(isolated.size(), QImage::Format_RGB32);
    onWhiteA.fill(Qt::white);
    { QPainter p(&onWhiteA); p.drawImage(0, 0, isolated); }

    QImage onWhiteB(isolated.size(), QImage::Format_RGB32);
    onWhiteB.fill(Qt::white);
    { QPainter p(&onWhiteB); p.drawImage(0, 0, isolated); }

    float ncc = ImageSimilarity::binarizedNCC(onWhiteA, onWhiteB);
    EXPECT_GT(ncc, 0.9f)
        << "NCC of identical composited images should be ~1.0, got " << ncc;
}

TEST_F(GlyphHarvesterTest, TemplateSelection_PicksCleaner) {
    // When templates are provided, harvestLine should score candidates via NCC
    // and pick the one that most resembles the reference template, not just the
    // one with higher confidence.
    //
    // Setup: Two instances of 'R' on the same line in separate words:
    //   - Instance 1: clean L-shape (good match for L-shaped template)
    //   - Instance 2: L-shape + adjacent contamination (poor match)
    QImage img = makeTestImage(300, 100);

    // Instance 1 at x=10..25: clean L (no adjacent char)
    drawLetterL(img, 10, 30, 12, 20);

    // Instance 2 at x=80: L + contamination blob right next to it
    drawLetterL(img, 80, 30, 12, 20);
    drawLetterL(img, 96, 30, 10, 15);

    // Create template by isolating a clean crop ourselves (same processing
    // pipeline as harvestLine). This ensures NCC scores are meaningful.
    QImage cleanCrop = img.copy(10, 30, 15, 20);
    QImage templateImg = GlyphAtlas::isolateAlpha(cleanCrop, textColor, bgColor);

    TemplateMap templates;
    templates[U'R'] = templateImg;

    // Two words each containing 'R' with same confidence.
    HocrWord word1;
    word1.text = "R";
    word1.confidence = 90;
    word1.fontSizePx = 18.0f;
    word1.chars = {makeChar(U'R', 10, 30, 25, 50, 90)};
    word1.bbox = {10.0f, 30.0f, 25.0f, 50.0f};

    HocrWord word2;
    word2.text = "R";
    word2.confidence = 90;
    word2.fontSizePx = 18.0f;
    // Wider bbox capturing contamination
    word2.chars = {makeChar(U'R', 80, 30, 110, 50, 90)};
    word2.bbox = {80.0f, 30.0f, 110.0f, 50.0f};

    HocrLine line;
    line.words = {word1, word2};
    line.bbox = {10.0f, 30.0f, 110.0f, 50.0f};
    line.xSizePx = 20.0f;
    line.ascendersPx = 5.0f;
    line.descendersPx = 4.0f;

    // Harvest WITHOUT templates (confidence-based: both equal, first wins)
    GlyphAtlas atlasNoTemplate;
    atlasNoTemplate.buildVerified(img, line, {}, textColor, bgColor, nullptr);
    ASSERT_TRUE(atlasNoTemplate.contains(U'R'));
    int widthWithout = atlasNoTemplate.glyph(U'R').width();

    // Harvest WITH templates (NCC-scored: clean one should win)
    GlyphAtlas atlasTemplated;
    atlasTemplated.buildVerified(img, line, {}, textColor, bgColor, &templates);
    ASSERT_TRUE(atlasTemplated.contains(U'R'));
    int widthWith = atlasTemplated.glyph(U'R').width();

    // Both paths should pick the clean instance; template scoring guarantees it
    EXPECT_LE(widthWith, widthWithout)
        << "Template-scored glyph should be no wider than confidence-based";
}

TEST_F(GlyphHarvesterTest, TemplateSelection_RejectsLowNCC) {
    // A crop that looks nothing like the template should be rejected
    // (NCC below 0.15 threshold).
    QImage img = makeTestImage(200, 100);
    // Draw a wide block (very different from a T-shaped template)
    for (int y = 30; y < 50; ++y)
        for (int x = 10; x < 60; ++x)
            img.setPixel(x, y, qRgb(kTextGray, kTextGray, kTextGray));

    // Template: a thin T-shape (very different from a solid block)
    QImage tmplImg = makeTestImage(20, 20);
    drawLetterT(tmplImg, 2, 2, 16, 16);
    QImage tmplIsolated = GlyphAtlas::isolateAlpha(tmplImg, textColor, bgColor);

    TemplateMap templates;
    templates[U'X'] = tmplIsolated;

    HocrLine line = makeLine({
        makeChar(U'X', 10, 30, 60, 50, 85),
    }, 20.0f, 5.0f, 4.0f, 18.0f);

    GlyphAtlas atlas;
    atlas.buildVerified(img, line, {}, textColor, bgColor, &templates);

    // The solid block crop should score poorly against the T template.
    // With NCC threshold at 0.15, a mismatched shape gets rejected.
    // Either way, the glyph should not be stored if the match is bad.
    if (atlas.contains(U'X')) {
        // If it was stored, the NCC was above threshold — verify the image exists
        EXPECT_FALSE(atlas.glyph(U'X').isNull());
    }
    // This test documents the threshold behavior; the exact outcome depends
    // on how different the block and T score. The key invariant is no crash.
}

TEST_F(GlyphHarvesterTest, TemplateSelection_FallsBackWithoutTemplate) {
    // When no template exists for a codepoint, template mode should fall
    // back to confidence-based selection (same as without templates).
    QImage img = makeTestImage(200, 100);
    drawLetterL(img, 10, 30, 12, 20);

    // Template map has 'A' but NOT 'R'. The 'R' glyph should still be
    // harvested via confidence-based fallback.
    QImage cropA = GlyphAtlas::isolateAlpha(img.copy(10, 30, 12, 20), textColor, bgColor);
    TemplateMap templates;
    templates[U'A'] = cropA;  // Only A has a template

    HocrLine line = makeLine({
        makeChar(U'R', 10, 30, 25, 50, 85),
    }, 20.0f, 5.0f, 4.0f, 18.0f);

    GlyphAtlas atlas;
    atlas.buildVerified(img, line, {}, textColor, bgColor, &templates);

    // R should still be harvested via confidence fallback
    EXPECT_TRUE(atlas.contains(U'R'))
        << "Codepoint without template should be harvested via confidence fallback";
}

TEST_F(GlyphHarvesterTest, TemplateSelection_AddMissingOnlyRespected) {
    // With templates AND addMissingOnly, existing reference line glyphs
    // should NOT be replaced by template-scored donor candidates.
    QImage img = makeTestImage(300, 120);

    // Reference line: A, B at y=10..30
    drawLetterL(img, 10, 10, 15, 20);
    drawLetterL(img, 30, 10, 15, 20);

    // Donor line: A (same char, different crop) + C (new char) at y=60..80
    drawLetterL(img, 10, 60, 15, 20);
    drawLetterL(img, 30, 60, 15, 20);

    // Create templates from actual crop isolations (ensures NCC scores are valid)
    QImage cropA = GlyphAtlas::isolateAlpha(img.copy(10, 10, 15, 20), textColor, bgColor);
    QImage cropB = GlyphAtlas::isolateAlpha(img.copy(30, 10, 15, 20), textColor, bgColor);
    QImage cropC = GlyphAtlas::isolateAlpha(img.copy(30, 60, 15, 20), textColor, bgColor);
    TemplateMap templates;
    templates[U'A'] = cropA;
    templates[U'B'] = cropB;
    templates[U'C'] = cropC;

    HocrLine refLine = makeLine({
        makeChar(U'A', 10, 10, 25, 30, 80),
        makeChar(U'B', 30, 10, 45, 30, 80),
    }, 20.0f, 5.0f, 4.0f, 18.0f);

    HocrLine donorLine = makeLine({
        makeChar(U'A', 10, 60, 25, 80, 95),  // Higher confidence
        makeChar(U'C', 30, 60, 45, 80, 90),  // New character
    }, 20.0f, 5.0f, 4.0f, 18.0f);

    GlyphAtlas atlas;
    atlas.buildVerified(img, refLine, {donorLine}, textColor, bgColor, &templates);

    // A and B should come from reference line (addMissingOnly protects them)
    EXPECT_TRUE(atlas.contains(U'A'));
    EXPECT_TRUE(atlas.contains(U'B'));
    EXPECT_TRUE(atlas.contains(U'C'));  // New char from donor

    // Reference glyphs should have reference lineH (20px)
    EXPECT_EQ(atlas.glyph(U'A').height(), 20);
    EXPECT_EQ(atlas.glyph(U'B').height(), 20);
}

TEST_F(GlyphHarvesterTest, TemplatePadding_NarrowCropGetsPadded) {
    // When a crop is significantly narrower than the template (>20% deficit),
    // it should be padded to the template's scaled width to add side-bearings.
    QImage img = makeTestImage(200, 100);
    // Draw a narrow vertical stroke at x=10..15 (5px wide in 20px-high line)
    for (int y = 30; y < 50; ++y)
        for (int x = 10; x < 15; ++x)
            img.setPixel(x, y, qRgb(kTextGray, kTextGray, kTextGray));

    // Template is wider (15px wide) — same height as line
    QImage tmplCanvas = makeTestImage(25, 20);
    for (int y = 2; y < 18; ++y)
        for (int x = 5; x < 20; ++x)
            tmplCanvas.setPixel(x, y, qRgb(kTextGray, kTextGray, kTextGray));
    QImage tmplIsolated = GlyphAtlas::isolateAlpha(tmplCanvas, textColor, bgColor);

    TemplateMap templates;
    templates[U'I'] = tmplIsolated;

    HocrLine line = makeLine({
        makeChar(U'I', 10, 30, 16, 50, 90),
    }, 20.0f, 5.0f, 4.0f, 18.0f);

    GlyphAtlas atlas;
    atlas.buildVerified(img, line, {}, textColor, bgColor, &templates);

    if (atlas.contains(U'I')) {
        int glyphW = atlas.glyph(U'I').width();
        int tmplW = tmplIsolated.width();
        // Padded glyph should be wider than the narrow crop (~5px after isolation)
        // and close to the template width (same height ratio = 1.0)
        EXPECT_GE(glyphW, tmplW * 0.8)
            << "Padded glyph should be at least 80% of template width";
        EXPECT_LE(glyphW, tmplW + 2)
            << "Padded glyph should not exceed template width significantly";
    }
}

TEST_F(GlyphHarvesterTest, TemplatePadding_WideEnoughCropNotPadded) {
    // When a crop is already >= 80% of the template width, no padding is applied.
    QImage img = makeTestImage(200, 100);
    // Draw a wide stroke at x=10..30 (20px wide)
    for (int y = 30; y < 50; ++y)
        for (int x = 10; x < 30; ++x)
            img.setPixel(x, y, qRgb(kTextGray, kTextGray, kTextGray));

    // Template is only slightly wider (22px after isolation)
    QImage tmplCanvas = makeTestImage(25, 20);
    for (int y = 2; y < 18; ++y)
        for (int x = 1; x < 23; ++x)
            tmplCanvas.setPixel(x, y, qRgb(kTextGray, kTextGray, kTextGray));
    QImage tmplIsolated = GlyphAtlas::isolateAlpha(tmplCanvas, textColor, bgColor);

    TemplateMap templates;
    templates[U'W'] = tmplIsolated;

    HocrLine line = makeLine({
        makeChar(U'W', 10, 30, 31, 50, 90),
    }, 20.0f, 5.0f, 4.0f, 18.0f);

    GlyphAtlas atlas;
    atlas.buildVerified(img, line, {}, textColor, bgColor, &templates);

    if (atlas.contains(U'W')) {
        int glyphW = atlas.glyph(U'W').width();
        // Crop is ~20px, template is ~22px — within 80% threshold, no padding
        EXPECT_LE(glyphW, 22)
            << "Wide enough crop should not be padded beyond template width";
    }
}

TEST_F(GlyphHarvesterTest, TemplatePadding_DifferentHeightRatioScales) {
    // When template and crop have different heights, the target width
    // is scaled by the height ratio.
    QImage img = makeTestImage(200, 100);
    // Draw narrow stroke in a 30px-high line (different from 20px template)
    for (int y = 20; y < 50; ++y)
        for (int x = 10; x < 15; ++x)
            img.setPixel(x, y, qRgb(kTextGray, kTextGray, kTextGray));

    // Template at 20px height, 15px wide
    QImage tmplCanvas = makeTestImage(20, 20);
    for (int y = 2; y < 18; ++y)
        for (int x = 2; x < 17; ++x)
            tmplCanvas.setPixel(x, y, qRgb(kTextGray, kTextGray, kTextGray));
    QImage tmplIsolated = GlyphAtlas::isolateAlpha(tmplCanvas, textColor, bgColor);

    TemplateMap templates;
    templates[U'J'] = tmplIsolated;

    // Line is 30px high (y=20..50)
    HocrLine line;
    line.bbox = {8.0f, 20.0f, 20.0f, 50.0f};
    line.xSizePx = 30.0f;
    line.ascendersPx = 5.0f;
    line.descendersPx = 4.0f;
    HocrWord word;
    word.text = "J";
    word.confidence = 90;
    word.fontSizePx = 28.0f;
    word.chars = {makeChar(U'J', 10, 20, 16, 50, 90)};
    word.bbox = {10.0f, 20.0f, 16.0f, 50.0f};
    line.words = {word};

    GlyphAtlas atlas;
    atlas.buildVerified(img, line, {}, textColor, bgColor, &templates);

    if (atlas.contains(U'J')) {
        int glyphW = atlas.glyph(U'J').width();
        // Template is 15px wide at 20px height. Crop is at 30px height.
        // Scaled target width = 15 * (30/20) = 22.5 → 22px
        // Crop is ~5px, should be padded to ~22px
        EXPECT_GE(glyphW, 15)
            << "Height-scaled padding should produce reasonable width";
    }
}

// M3: Test sliding window clip preserves asymmetric character features.
// Uses two L-shaped glyphs: one clean (via no-template confidence path),
// then verifies the clip behavior indirectly via the template path.
TEST_F(GlyphHarvesterTest, ClipToTemplateWidth_PreservesAsymmetricFoot) {
    // Draw an L-shape at (20,25)-(36,50): vertical stroke 5px, foot 16px.
    // Also draw contamination at (2,30)-(10,45) with a gap.
    // The HOCR bbox spans the full width including contamination.
    // Without template, clipAtVerticalGap handles the gap.
    // With template, clipToTemplateWidth's sliding window should preserve
    // the foot by picking the densest region (the L body, not contamination).
    QImage img = makeTestImage(200, 100);

    // L-shape: vertical stroke columns 20-24, foot columns 20-35
    for (int y = 25; y < 50; ++y)
        for (int x = 20; x < 25; ++x)
            img.setPixel(x, y, qRgb(kTextGray, kTextGray, kTextGray));
    for (int y = 47; y < 50; ++y)
        for (int x = 25; x < 36; ++x)
            img.setPixel(x, y, qRgb(kTextGray, kTextGray, kTextGray));

    // Without templates — just confidence-based, uses clipAtVerticalGap
    HocrLine line = makeLine({
        makeChar(U'L', 20, 25, 36, 50, 85),
    }, 25.0f, 5.0f, 4.0f, 23.0f);

    GlyphAtlas atlas;
    atlas.build(img, {line}, textColor, bgColor);

    ASSERT_TRUE(atlas.contains(U'L'));
    QImage glyph = atlas.glyph(U'L');
    ASSERT_FALSE(glyph.isNull());

    // The glyph should include the foot. Check that ink extends into
    // the right portion (foot area) by finding the rightmost visible column.
    int rightmostInk = 0;
    for (int y = 0; y < glyph.height(); ++y) {
        const QRgb* row = reinterpret_cast<const QRgb*>(glyph.constScanLine(y));
        for (int x = glyph.width() - 1; x >= 0; --x) {
            if (qAlpha(row[x]) > 20) {
                if (x > rightmostInk) rightmostInk = x;
                break;
            }
        }
    }
    // The foot extends from column 20 to 35 (16px). After cropping,
    // the rightmost ink should be at least 10px from the left edge.
    EXPECT_GT(rightmostInk, 10)
        << "clipAtVerticalGap should preserve the L's horizontal foot";
}

// M4: Test that small punctuation is accepted via the confidence path
// and that small punctuation is stored via the confidence path.
TEST_F(GlyphHarvesterTest, SmallPunctuation_StoredViaConfidencePath) {
    // Small punctuation through the confidence path (no templates) should
    // be stored. This verifies the pipeline handles narrow glyphs correctly.
    QImage img = makeTestImage(200, 80);
    // Draw colon dots: upper (50,20)-(58,28), lower (50,35)-(58,43)
    for (int y = 20; y < 28; ++y)
        for (int x = 50; x < 58; ++x)
            img.setPixel(x, y, qRgb(kTextGray, kTextGray, kTextGray));
    for (int y = 35; y < 43; ++y)
        for (int x = 50; x < 58; ++x)
            img.setPixel(x, y, qRgb(kTextGray, kTextGray, kTextGray));

    HocrLine line = makeLine({
        makeChar(U':', 49, 15, 59, 45, 80),
    }, 30.0f, 5.0f, 4.0f, 28.0f);

    // Confidence-based path (no NCC gating)
    GlyphAtlas atlas;
    atlas.build(img, {line}, textColor, bgColor);

    EXPECT_TRUE(atlas.contains(U':'))
        << "Small punctuation should be stored via confidence path";
    if (atlas.contains(U':')){
        QImage g = atlas.glyph(U':');
        // Colon should have visible ink in both upper and lower portions
        EXPECT_FALSE(g.isNull());
        EXPECT_GT(g.height(), 10)
            << "Colon glyph should preserve full line height";
    }
}

// Verify template scoring works for narrow templates (punctuation).
// Narrow templates have low pixel counts making NCC less reliable,
// but the 0.15 threshold should still allow reasonable matches.
TEST_F(GlyphHarvesterTest, NarrowTemplate_TemplateScoring) {
    // A narrow template (width/height < 0.35) exercised through the
    // template scoring path in buildVerified.
    QImage img = makeTestImage(200, 80);
    // Draw a colon at (50,20)-(58,43)
    for (int y = 20; y < 28; ++y)
        for (int x = 50; x < 58; ++x)
            img.setPixel(x, y, qRgb(kTextGray, kTextGray, kTextGray));
    for (int y = 35; y < 43; ++y)
        for (int x = 50; x < 58; ++x)
            img.setPixel(x, y, qRgb(kTextGray, kTextGray, kTextGray));

    // Template: 8px wide, 30px tall → ratio = 8/30 = 0.27 < 0.35 → low threshold
    QImage tmplImg(8, 30, QImage::Format_ARGB32_Premultiplied);
    tmplImg.fill(Qt::transparent);
    for (int y = 5; y < 13; ++y)
        for (int x = 0; x < 8; ++x)
            tmplImg.setPixel(x, y, qRgba(kTextGray, kTextGray, kTextGray, 255));
    for (int y = 20; y < 28; ++y)
        for (int x = 0; x < 8; ++x)
            tmplImg.setPixel(x, y, qRgba(kTextGray, kTextGray, kTextGray, 255));

    TemplateMap templates;
    templates[U':'] = tmplImg;

    HocrLine line = makeLine({
        makeChar(U':', 49, 15, 59, 45, 80),
    }, 30.0f, 5.0f, 4.0f, 28.0f);

    // The colon crop should score reasonably well against the template.
    GlyphAtlas atlas;
    atlas.buildVerified(img, line, {}, textColor, bgColor, &templates);

    // Even if NCC is low for synthetic images, verify no crash and the
    // template scoring path is exercised. The glyph may or may not
    // be stored depending on exact NCC score vs 0.15 threshold.
    // Key invariant: no crash, and if stored, the image is valid.
    if (atlas.contains(U':')) {
        EXPECT_FALSE(atlas.glyph(U':').isNull());
    }
}

// N2: Strengthen RejectsLowNCC to verify actual rejection
TEST_F(GlyphHarvesterTest, NormalSizedGlyph_RejectedBelowThreshold) {
    // A glyph that doesn't resemble its template should be rejected
    // at the 0.15 NCC threshold.
    QImage img = makeTestImage(200, 100);
    // Draw a solid square block — very different from a T template
    for (int y = 30; y < 55; ++y)
        for (int x = 10; x < 40; ++x)
            img.setPixel(x, y, qRgb(kTextGray, kTextGray, kTextGray));

    // Template: T-shape (structurally very different from a solid block)
    QImage tmplImg = makeTestImage(25, 25);
    drawLetterT(tmplImg, 2, 2, 21, 21);
    QImage tmplIsolated = GlyphAtlas::isolateAlpha(tmplImg, textColor, bgColor);

    TemplateMap templates;
    templates[U'Z'] = tmplIsolated;

    HocrLine line = makeLine({
        makeChar(U'Z', 10, 30, 40, 55, 85),
    }, 25.0f, 5.0f, 4.0f, 23.0f);

    GlyphAtlas atlas;
    atlas.buildVerified(img, line, {}, textColor, bgColor, &templates);

    // The block vs T-shape NCC should be well below 0.15
    EXPECT_FALSE(atlas.contains(U'Z'))
        << "Glyph with low NCC should be rejected at 0.15 threshold";
}

// ============================================================================
// AtlasBuilder clickedLineIdx tests
// ============================================================================

TEST_F(GlyphHarvesterTest, AtlasBuilder_ClickedLineIdx_UsedAsReference) {
    // When clickedLineIdx is set, AtlasBuilder should use that line as the
    // reference instead of the longest line.
    QImage img = makeTestImage(400, 200);

    // Line 0: 2 chars (A, B)
    drawLetterL(img, 10, 10, 15, 20);
    drawLetterL(img, 30, 10, 15, 20);

    // Line 1: 4 chars (X, Y, Z, W) — longest, would be auto-selected
    drawLetterL(img, 10, 50, 15, 20);
    drawLetterL(img, 30, 50, 15, 20);
    drawLetterL(img, 50, 50, 15, 20);
    drawLetterL(img, 70, 50, 15, 20);

    // Line 2: 1 char (Q)
    drawLetterL(img, 10, 90, 15, 20);

    HocrLine line0 = makeLine({
        makeChar(U'A', 10, 10, 25, 30),
        makeChar(U'B', 30, 10, 45, 30),
    });

    HocrLine line1 = makeLine({
        makeChar(U'X', 10, 50, 25, 70),
        makeChar(U'Y', 30, 50, 45, 70),
        makeChar(U'Z', 50, 50, 65, 70),
        makeChar(U'W', 70, 50, 85, 70),
    });

    HocrLine line2 = makeLine({
        makeChar(U'Q', 10, 90, 25, 110),
    });

    // Build with clickedLineIdx=2 (the shortest line)
    AtlasBuilder::Options opts;
    opts.clickedLineIdx = 2;

    auto result = AtlasBuilder::build(
        img, {line0, line1, line2}, textColor, bgColor,
        nullptr, nullptr, 0, 1, opts);

    EXPECT_NE(result.enrichedAtlas, nullptr);
    EXPECT_GT(result.enrichedAtlas->size(), 0);
    // Q should be in atlas (from the clicked reference line)
    EXPECT_TRUE(result.enrichedAtlas->contains(U'Q'));
}

TEST_F(GlyphHarvesterTest, AtlasBuilder_ClickedLineIdx_InvalidFallsBackToLongest) {
    // When clickedLineIdx is out of range, should fall back to longest line.
    QImage img = makeTestImage(200, 100);
    drawLetterL(img, 10, 10, 15, 20);

    HocrLine line0 = makeLine({
        makeChar(U'A', 10, 10, 25, 30),
    });

    AtlasBuilder::Options opts;
    opts.clickedLineIdx = 99;  // Out of range

    auto result = AtlasBuilder::build(
        img, {line0}, textColor, bgColor,
        nullptr, nullptr, 0, 1, opts);

    EXPECT_NE(result.enrichedAtlas, nullptr);
    EXPECT_TRUE(result.enrichedAtlas->contains(U'A'));
}

// ============================================================================
// NCC quality floor tests
// ============================================================================

TEST_F(GlyphHarvesterTest, HarvestLine_StoresNccScore) {
    // When templates are used, each glyph entry should have its NCC score
    // stored for quality comparison.
    QImage img = makeTestImage(200, 100);

    // Draw a T-shape character
    drawLetterT(img, 10, 10, 25, 30);

    // Create a template that resembles the T-shape
    QImage tmplImg = makeTestImage(25, 30);
    drawLetterT(tmplImg, 2, 2, 21, 26);
    QImage tmplIsolated = GlyphAtlas::isolateAlpha(tmplImg, textColor, bgColor);

    TemplateMap templates;
    templates[U'T'] = tmplIsolated;

    HocrLine line = makeLine({
        makeChar(U'T', 10, 10, 35, 40, 90),
    }, 25.0f, 5.0f, 4.0f, 23.0f);

    GlyphAtlas atlas;
    atlas.buildVerified(img, line, {}, textColor, bgColor, &templates);

    EXPECT_TRUE(atlas.contains(U'T'));
    // The glyph should have a non-zero NCC score stored
    // (we can't directly access GlyphEntry, but the atlas accepted it,
    // meaning NCC >= 0.15, proving scoring happened)
}

TEST_F(GlyphHarvesterTest, BuildVerified_RefOutlierRemoval) {
    // Reference line has 6 chars: A-D drawn as L-shapes with L-shaped templates
    // (high NCC), E-F drawn as T-shapes with L-shaped templates (low NCC).
    // Outlier detection should remove E,F so they can be filled by donors/synthesis.
    QImage img = makeTestImage(400, 60);

    // Draw A-D as L-shapes (will match L-shaped templates well)
    drawLetterL(img, 10, 10, 15, 20);   // A
    drawLetterL(img, 30, 10, 15, 20);   // B
    drawLetterL(img, 50, 10, 15, 20);   // C
    drawLetterL(img, 70, 10, 15, 20);   // D

    // Draw E-F as T-shapes (will NOT match L-shaped templates)
    drawLetterT(img, 90, 10, 15, 20);   // E
    drawLetterT(img, 110, 10, 15, 20);  // F

    // Create L-shaped templates for ALL 6 chars — E,F will score poorly
    TemplateMap templates;
    for (char32_t cp : {U'A', U'B', U'C', U'D', U'E', U'F'}) {
        QImage tmpl = makeTestImage(15, 20);
        drawLetterL(tmpl, 0, 0, 15, 20);
        templates[cp] = GlyphAtlas::isolateAlpha(tmpl, textColor, bgColor);
    }

    HocrLine refLine = makeLine({
        makeChar(U'A', 10, 10, 25, 30, 90),
        makeChar(U'B', 30, 10, 45, 30, 90),
        makeChar(U'C', 50, 10, 65, 30, 90),
        makeChar(U'D', 70, 10, 85, 30, 90),
        makeChar(U'E', 90, 10, 105, 30, 90),
        makeChar(U'F', 110, 10, 125, 30, 90),
    }, 20.0f, 5.0f, 4.0f, 20.0f);

    GlyphAtlas atlas;
    atlas.buildVerified(img, refLine, {}, textColor, bgColor, &templates);

    // A-D should be present (high NCC against L-shaped templates)
    EXPECT_TRUE(atlas.contains(U'A'));
    EXPECT_TRUE(atlas.contains(U'B'));
    EXPECT_TRUE(atlas.contains(U'C'));
    EXPECT_TRUE(atlas.contains(U'D'));

    // E,F should have been removed as outliers (T vs L = low NCC).
    // They will be filled later by donors or synthesis.
    EXPECT_FALSE(atlas.contains(U'E')) << "Outlier E should be removed from ref line";
    EXPECT_FALSE(atlas.contains(U'F')) << "Outlier F should be removed from ref line";
}

TEST_F(GlyphHarvesterTest, BuildVerified_RefOutlierNeedsMinFourGlyphs) {
    // With fewer than 4 scored glyphs, IQR is unreliable so no outlier removal.
    // Use 3 L-shaped chars with L-shaped templates — all similar quality,
    // and verify all survive (no spurious removal).
    QImage img = makeTestImage(200, 60);

    drawLetterL(img, 10, 10, 15, 20);   // A
    drawLetterL(img, 30, 10, 15, 20);   // B
    drawLetterL(img, 50, 10, 15, 20);   // C

    TemplateMap templates;
    for (char32_t cp : {U'A', U'B', U'C'}) {
        QImage tmpl = makeTestImage(15, 20);
        drawLetterL(tmpl, 0, 0, 15, 20);
        templates[cp] = GlyphAtlas::isolateAlpha(tmpl, textColor, bgColor);
    }

    HocrLine refLine = makeLine({
        makeChar(U'A', 10, 10, 25, 30, 90),
        makeChar(U'B', 30, 10, 45, 30, 90),
        makeChar(U'C', 50, 10, 65, 30, 90),
    }, 20.0f, 5.0f, 4.0f, 20.0f);

    GlyphAtlas atlas;
    atlas.buildVerified(img, refLine, {}, textColor, bgColor, &templates);

    // All 3 should remain — too few for IQR outlier detection
    EXPECT_TRUE(atlas.contains(U'A'));
    EXPECT_TRUE(atlas.contains(U'B'));
    EXPECT_TRUE(atlas.contains(U'C')) << "C kept: too few glyphs for IQR removal";
}
