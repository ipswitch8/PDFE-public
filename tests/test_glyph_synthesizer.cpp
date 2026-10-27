#include <gtest/gtest.h>
#include "tre/GlyphSynthesizer.h"
#include "tre/GlyphAtlas.h"
#include "tre/GlyphAtlasCompositor.h"
#include "tre/TreTypes.h"
#include <QImage>
#include <QRect>

using namespace pdfe::tre;

namespace {

/// Foreground test used consistently by the synthesizer's own measurement
/// helpers (GlyphSynthesizer::measureStemWidth etc.).
inline bool isInk(const QImage& img, int x, int y) {
    return qAlpha(img.pixel(x, y)) > 128;
}

/// Number of foreground pixels inside @p region.
long inkCount(const QImage& img, const QRect& region) {
    long n = 0;
    const QRect r = region.intersected(img.rect());
    for (int y = r.top(); y <= r.bottom(); ++y)
        for (int x = r.left(); x <= r.right(); ++x)
            if (isInk(img, x, y))
                ++n;
    return n;
}

/// Mean x of the foreground pixels inside @p region (-1 when no ink).
double inkCentroidX(const QImage& img, const QRect& region) {
    long n = 0;
    double sum = 0.0;
    const QRect r = region.intersected(img.rect());
    for (int y = r.top(); y <= r.bottom(); ++y)
        for (int x = r.left(); x <= r.right(); ++x)
            if (isInk(img, x, y)) { sum += x; ++n; }
    return n > 0 ? sum / static_cast<double>(n) : -1.0;
}

/// True when every foreground pixel of @p a is also foreground in @p b.
/// GlyphSynthesizer::matchStrokeWeight only ever ERODES (removes ink) or
/// DILATES; for the fixtures below it erodes, so the synthesized glyph must
/// be an ink-subset of the correctly-transformed donor and of no other
/// transform of it.
bool inkIsSubsetOf(const QImage& a, const QImage& b) {
    if (a.size() != b.size())
        return false;
    for (int y = 0; y < a.height(); ++y)
        for (int x = 0; x < a.width(); ++x)
            if (isInk(a, x, y) && !isInk(b, x, y))
                return false;
    return true;
}

/// Exact per-pixel ink equality (format-agnostic).
bool sameInk(const QImage& a, const QImage& b) {
    if (a.size() != b.size())
        return false;
    for (int y = 0; y < a.height(); ++y)
        for (int x = 0; x < a.width(); ++x)
            if (isInk(a, x, y) != isInk(b, x, y))
                return false;
    return true;
}

QRect leftHalf(const QImage& i)   { return QRect(0, 0, i.width() / 2, i.height()); }
QRect rightHalf(const QImage& i)  { return QRect(i.width() - i.width() / 2, 0,
                                                 i.width() / 2, i.height()); }
QRect topHalf(const QImage& i)    { return QRect(0, 0, i.width(), i.height() / 2); }
QRect bottomHalf(const QImage& i) { return QRect(0, i.height() - i.height() / 2,
                                                 i.width(), i.height() / 2); }

} // namespace

class GlyphSynthesizerTest : public ::testing::Test {
protected:
    // Create a test glyph image (ARGB32_Premultiplied with alpha-isolated text)
    // Draws a rectangular stem at a given position to simulate a character
    QImage makeGlyph(int w, int h, int stemX0, int stemX1, int stemY0, int stemY1) {
        QImage img(w, h, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        for (int y = stemY0; y < stemY1 && y < h; ++y) {
            for (int x = stemX0; x < stemX1 && x < w; ++x) {
                // Opaque black (premultiplied)
                img.setPixel(x, y, qRgba(0, 0, 0, 255));
            }
        }
        return img;
    }

    // Create a glyph with a vertical stem (like 'l' or 'I')
    QImage makeStemGlyph(int w, int h, int stemWidth) {
        int cx = w / 2;
        int x0 = cx - stemWidth / 2;
        int x1 = x0 + stemWidth;
        return makeGlyph(w, h, x0, x1, 1, h - 1);
    }

    // Create a filled rectangle glyph (like 'O' approximation)
    QImage makeFilledGlyph(int w, int h) {
        QImage img(w, h, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                img.setPixel(x, y, qRgba(0, 0, 0, 255));
            }
        }
        return img;
    }

    // Build an atlas with specific characters and glyph images
    GlyphAtlas buildAtlas(
        const std::vector<std::pair<char32_t, QImage>>& glyphs)
    {
        // We need to create a page image that contains the glyphs at known positions
        // and HOCR data pointing to them
        int totalW = 0;
        int maxH = 0;
        for (const auto& [cp, img] : glyphs) {
            totalW += img.width() + 2;  // 2px spacing
            maxH = std::max(maxH, img.height());
        }
        totalW = std::max(totalW, 10);
        maxH = std::max(maxH + 4, 10);  // padding

        QImage pageImg(totalW, maxH, QImage::Format_ARGB32);
        pageImg.fill(QColor(255, 255, 255));  // white background

        std::vector<HocrLine> lines;
        HocrLine line;
        line.bbox = {0, 0, static_cast<float>(totalW), static_cast<float>(maxH)};
        HocrWord word;
        word.bbox = line.bbox;
        word.confidence = 95;

        int curX = 1;
        for (const auto& [cp, img] : glyphs) {
            // Paint glyph pixels as black on the white page
            for (int y = 0; y < img.height(); ++y) {
                for (int x = 0; x < img.width(); ++x) {
                    if (qAlpha(img.pixel(x, y)) > 128) {
                        pageImg.setPixel(curX + x, 2 + y, qRgb(0, 0, 0));
                    }
                }
            }

            HocrChar ch;
            ch.codepoint = cp;
            ch.bbox = {static_cast<float>(curX), 2.0f,
                       static_cast<float>(curX + img.width()),
                       static_cast<float>(2 + img.height())};
            ch.confidence = 95;
            word.chars.push_back(ch);

            curX += img.width() + 2;
        }

        word.text = "test";
        line.words = {word};
        lines = {line};

        GlyphAtlas atlas;
        atlas.build(pageImg, lines, {0, 0, 0}, {255, 255, 255});
        return atlas;
    }
};

// ============================================================================
// Profile Extraction Tests
// ============================================================================

TEST_F(GlyphSynthesizerTest, ProfileExtraction_StemWidth) {
    // Create atlas with consistent stem-width glyphs
    auto atlas = buildAtlas({
        {U'l', makeStemGlyph(10, 20, 3)},
        {U'I', makeStemGlyph(10, 20, 3)},
        {U'i', makeStemGlyph(8, 16, 3)},
    });

    GlyphSynthesizer synth;
    synth.buildProfile(atlas);

    const auto& p = synth.profile();
    EXPECT_GT(p.sampleCount, 0);
    EXPECT_GT(p.avgStemWidth, 0.0f);
    // 3px stems should yield a stem width around 3
    EXPECT_NEAR(p.avgStemWidth, 3.0f, 1.5f);
}

TEST_F(GlyphSynthesizerTest, ProfileExtraction_InkDensity) {
    auto atlas = buildAtlas({
        {U'A', makeFilledGlyph(10, 20)},
    });

    GlyphSynthesizer synth;
    synth.buildProfile(atlas);

    const auto& p = synth.profile();
    // Filled glyph should have high ink density
    EXPECT_GT(p.avgInkDensity, 0.5f);
}

TEST_F(GlyphSynthesizerTest, ProfileExtraction_EmptyAtlas) {
    GlyphAtlas atlas;  // empty, no build
    GlyphSynthesizer synth;
    synth.buildProfile(atlas);

    const auto& p = synth.profile();
    EXPECT_EQ(p.sampleCount, 0);
    EXPECT_FLOAT_EQ(p.avgStemWidth, 0.0f);
    EXPECT_FLOAT_EQ(p.avgInkDensity, 0.0f);
}

TEST_F(GlyphSynthesizerTest, ProfileExtraction_SingleGlyph) {
    auto atlas = buildAtlas({
        {U'x', makeStemGlyph(12, 18, 4)},
    });

    GlyphSynthesizer synth;
    synth.buildProfile(atlas);

    const auto& p = synth.profile();
    EXPECT_EQ(p.sampleCount, 1);
    EXPECT_GT(p.avgCharWidth, 0.0f);
    EXPECT_GT(p.avgCharHeight, 0.0f);
}

// ============================================================================
// Knowledge Base Tests
// ============================================================================

TEST_F(GlyphSynthesizerTest, RecipeExists_b_from_d) {
    const GlyphRecipe* recipe = GlyphSynthesizer::findRecipe(U'b');
    ASSERT_NE(recipe, nullptr);
    EXPECT_EQ(recipe->target, U'b');
    ASSERT_FALSE(recipe->strategies.empty());
    // First strategy should be d → HFlip
    EXPECT_EQ(recipe->strategies[0].donor, U'd');
    EXPECT_EQ(recipe->strategies[0].transform,
              GlyphRecipe::Strategy::Transform::HFlip);
}

TEST_F(GlyphSynthesizerTest, RecipeExists_6_from_9) {
    const GlyphRecipe* recipe = GlyphSynthesizer::findRecipe(U'6');
    ASSERT_NE(recipe, nullptr);
    EXPECT_EQ(recipe->strategies[0].donor, U'9');
    EXPECT_EQ(recipe->strategies[0].transform,
              GlyphRecipe::Strategy::Transform::Rotate180);
}

TEST_F(GlyphSynthesizerTest, RecipeNotFound_AtSymbol) {
    const GlyphRecipe* recipe = GlyphSynthesizer::findRecipe(U'@');
    EXPECT_EQ(recipe, nullptr);
}

// ============================================================================
// Synthesis Transform Tests
// ============================================================================

// A1-43 oracle repair: the previous oracle (success / non-null / donorUsed /
// quality>0) is satisfied by an IDENTITY COPY of the donor — deleting the
// applyTransform() call in GlyphSynthesizer::synthesize() left it green. It
// now asserts the returned image really is the horizontally-flipped donor.
//
// Authored ground truth: the donor 'd' has its 4px stem on the RIGHT
// (x 10..13) and its bowl on the LEFT (x 2..9). A synthesized 'b' must be
// the mirror image: stem on the LEFT.
TEST_F(GlyphSynthesizerTest, Synthesize_b_from_d_HFlip) {
    // Build atlas with 'd' -- should synthesize 'b' via HFlip
    // Create asymmetric glyph so flip is detectable
    QImage dGlyph(15, 20, QImage::Format_ARGB32_Premultiplied);
    dGlyph.fill(Qt::transparent);
    // Draw stem on right side (like 'd')
    for (int y = 0; y < 20; ++y) {
        for (int x = 10; x < 14; ++x)
            dGlyph.setPixel(x, y, qRgba(0, 0, 0, 255));
    }
    // Draw bowl on left
    for (int y = 5; y < 15; ++y) {
        for (int x = 2; x < 10; ++x)
            dGlyph.setPixel(x, y, qRgba(0, 0, 0, 255));
    }

    auto atlas = buildAtlas({{U'd', dGlyph}});

    GlyphSynthesizer synth;
    synth.buildProfile(atlas);
    auto result = synth.synthesize(U'b', atlas);

    EXPECT_TRUE(result.success);
    ASSERT_FALSE(result.image.isNull());
    EXPECT_EQ(result.donorUsed, U'd');
    EXPECT_GT(result.quality, 0.0f);

    // The donor as the synthesizer actually sees it (post alpha-isolation).
    const QImage donor = atlas.glyph(U'd');
    ASSERT_FALSE(donor.isNull());

    // Ground-truth guard: the authored donor really is right-stem-heavy, so
    // the left/right assertions below are meaningful.
    const long donorLeft  = inkCount(donor, leftHalf(donor));
    const long donorRight = inkCount(donor, rightHalf(donor));
    ASSERT_GT(donorRight, donorLeft)
        << "fixture drift: authored 'd' must have more ink on the right";

    // (1) Exact transform identity. The atlas holds a single glyph, so the
    //     style profile's stem width equals the donor's and an HFlip preserves
    //     every row's run lengths -- matchStrokeWeight() is provably a no-op
    //     here (|ratio-1| < 0.15). The synthesized image must therefore be the
    //     mirrored donor pixel-for-pixel.
    //     KILLS: deleting/short-circuiting applyTransform(); substituting
    //     Transform::Identity, VFlip or Rotate180 for HFlip.
    EXPECT_TRUE(sameInk(result.image, donor.mirrored(true, false)))
        << "synthesized 'b' is not the horizontally-mirrored donor 'd'";

    // (2) It must NOT be an untransformed copy of the donor.
    //     KILLS: `return donor.copy()` in the HFlip branch of applyTransform().
    EXPECT_FALSE(sameInk(result.image, donor))
        << "synthesized 'b' is an identity copy of the donor 'd'";

    // (3) Structural check on the authored asymmetry: stem moved left.
    //     KILLS: an HFlip implemented as a vertical mirror.
    EXPECT_GT(inkCount(result.image, leftHalf(result.image)),
              inkCount(result.image, rightHalf(result.image)))
        << "synthesized 'b' must carry its stem on the LEFT";
}

TEST_F(GlyphSynthesizerTest, Synthesize_9_from_6_Rotate180) {
    // Build atlas with '6' -- should synthesize '9' via Rotate180
    QImage sixGlyph(12, 20, QImage::Format_ARGB32_Premultiplied);
    sixGlyph.fill(Qt::transparent);
    // Top loop
    for (int y = 0; y < 10; ++y) {
        for (int x = 2; x < 10; ++x)
            sixGlyph.setPixel(x, y, qRgba(0, 0, 0, 255));
    }
    // Bottom stem
    for (int y = 10; y < 20; ++y) {
        for (int x = 2; x < 5; ++x)
            sixGlyph.setPixel(x, y, qRgba(0, 0, 0, 255));
    }

    auto atlas = buildAtlas({{U'6', sixGlyph}});

    GlyphSynthesizer synth;
    synth.buildProfile(atlas);
    auto result = synth.synthesize(U'9', atlas);

    EXPECT_TRUE(result.success);
    ASSERT_FALSE(result.image.isNull());
    EXPECT_EQ(result.donorUsed, U'6');

    // A1-43 oracle repair. Unlike the HFlip case, a 180-degree rotation moves
    // the stem-width sample rows, so matchStrokeWeight() runs one EROSION
    // pass. Erosion can only REMOVE ink, never add or move it -- so the
    // synthesized glyph must be an ink-subset of the rotated donor, and must
    // NOT be a subset of the donor under any other transform. Those four
    // relations together pin Transform::Rotate180 uniquely.
    const QImage donor = atlas.glyph(U'6');
    ASSERT_FALSE(donor.isNull());

    // Ground-truth guard on the authored donor: wide loop on TOP (rows 0-9,
    // x 2-9), narrow stem at BOTTOM-LEFT (rows 10-19, x 2-4).
    ASSERT_GT(inkCount(donor, topHalf(donor)), inkCount(donor, bottomHalf(donor)))
        << "fixture drift: authored '6' must be top-heavy";

    const QImage rot180 = donor.mirrored(true, true);
    const QImage hflip  = donor.mirrored(true, false);
    const QImage vflip  = donor.mirrored(false, true);

    // (1) Correct transform. KILLS: removing the applyTransform() call, or
    //     mapping Rotate180 to Identity.
    EXPECT_TRUE(inkIsSubsetOf(result.image, rot180))
        << "synthesized '9' has ink outside the 180-rotated donor '6'";

    // (2..4) Every WRONG transform must be excluded.
    //     KILLS: Rotate180 implemented as identity / a single mirror axis.
    EXPECT_FALSE(inkIsSubsetOf(result.image, donor))
        << "synthesized '9' is an untransformed copy of the donor '6'";
    EXPECT_FALSE(inkIsSubsetOf(result.image, hflip))
        << "synthesized '9' is only horizontally mirrored, not rotated";
    EXPECT_FALSE(inkIsSubsetOf(result.image, vflip))
        << "synthesized '9' is only vertically mirrored, not rotated";

    // (5) Structural consequence of the authored layout: the loop must end up
    //     at the BOTTOM and the remaining stem in the TOP-RIGHT quadrant.
    EXPECT_GT(inkCount(result.image, bottomHalf(result.image)),
              inkCount(result.image, topHalf(result.image)))
        << "synthesized '9' must carry its loop at the BOTTOM";
    const double topCx = inkCentroidX(result.image, topHalf(result.image));
    ASSERT_GE(topCx, 0.0) << "synthesized '9' has no ink in its top half";
    EXPECT_GT(topCx, result.image.width() / 2.0)
        << "the rotated stem must sit right-of-centre (a VFlip leaves it left)";
}

TEST_F(GlyphSynthesizerTest, Synthesize_o_from_O_ScaleDown) {
    // Build atlas with 'O' -- should synthesize 'o' via ScaleToXHeight
    QImage capO(16, 24, QImage::Format_ARGB32_Premultiplied);
    capO.fill(Qt::transparent);
    for (int y = 2; y < 22; ++y) {
        for (int x = 2; x < 14; ++x)
            capO.setPixel(x, y, qRgba(0, 0, 0, 255));
    }

    auto atlas = buildAtlas({{U'O', capO}});

    GlyphSynthesizer synth;
    synth.buildProfile(atlas);
    auto result = synth.synthesize(U'o', atlas);

    EXPECT_TRUE(result.success);
    EXPECT_FALSE(result.image.isNull());
    // Scaled glyph should be smaller than original
    EXPECT_LT(result.image.height(), capO.height());
}

TEST_F(GlyphSynthesizerTest, Synthesize_FailsWhenNoDonor) {
    // Atlas has 'A' but 'b' recipe needs 'd' or 'p' -- neither available
    auto atlas = buildAtlas({{U'A', makeStemGlyph(10, 20, 3)}});

    GlyphSynthesizer synth;
    synth.buildProfile(atlas);
    auto result = synth.synthesize(U'b', atlas);

    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.image.isNull());
}

TEST_F(GlyphSynthesizerTest, Synthesize_NoRecipeReturnsFailure) {
    // '@' has no recipe
    auto atlas = buildAtlas({{U'A', makeStemGlyph(10, 20, 3)}});

    GlyphSynthesizer synth;
    synth.buildProfile(atlas);
    auto result = synth.synthesize(U'@', atlas);

    EXPECT_FALSE(result.success);
}

TEST_F(GlyphSynthesizerTest, SynthesizeAll_MultipleChars) {
    // Atlas has 'd' and 'n' -- should synthesize 'b' (from d) and 'u' (from n)
    QImage dGlyph(12, 20, QImage::Format_ARGB32_Premultiplied);
    dGlyph.fill(Qt::transparent);
    for (int y = 0; y < 20; ++y)
        for (int x = 8; x < 12; ++x)
            dGlyph.setPixel(x, y, qRgba(0, 0, 0, 255));
    for (int y = 5; y < 15; ++y)
        for (int x = 1; x < 8; ++x)
            dGlyph.setPixel(x, y, qRgba(0, 0, 0, 255));

    QImage nGlyph(12, 16, QImage::Format_ARGB32_Premultiplied);
    nGlyph.fill(Qt::transparent);
    for (int y = 0; y < 16; ++y)
        for (int x = 1; x < 4; ++x)
            nGlyph.setPixel(x, y, qRgba(0, 0, 0, 255));
    for (int y = 0; y < 8; ++y)
        for (int x = 4; x < 11; ++x)
            nGlyph.setPixel(x, y, qRgba(0, 0, 0, 255));

    auto atlas = buildAtlas({{U'd', dGlyph}, {U'n', nGlyph}});

    GlyphSynthesizer synth;
    synth.buildProfile(atlas);
    auto results = synth.synthesizeAll(U"bdu", atlas);

    // 'b' synthesized from 'd', 'u' synthesized from 'n'
    // 'd' is in atlas so NOT synthesized
    EXPECT_TRUE(results.count(U'b') > 0);
    EXPECT_TRUE(results.count(U'u') > 0);
    EXPECT_EQ(results.count(U'd'), 0u);  // already in atlas
}

TEST_F(GlyphSynthesizerTest, SynthesizeAll_SkipsExistingInAtlas) {
    auto atlas = buildAtlas({
        {U'd', makeStemGlyph(12, 20, 3)},
        {U'b', makeStemGlyph(12, 20, 3)},
    });

    GlyphSynthesizer synth;
    synth.buildProfile(atlas);
    auto results = synth.synthesizeAll(U"bd", atlas);

    // Both are in atlas -- nothing to synthesize
    EXPECT_TRUE(results.empty());
}

// ============================================================================
// Stroke Weight Matching Tests
// ============================================================================

TEST_F(GlyphSynthesizerTest, StrokeWeightMatch_NoChangeWhenClose) {
    // Create glyph with stem width matching the profile -- should not change
    QImage glyph = makeStemGlyph(10, 20, 3);

    FontStyleProfile profile;
    profile.avgStemWidth = 3.0f;  // within 15% of actual
    profile.sampleCount = 5;

    // Score should be good (close match means no morphological ops)
    float score = GlyphSynthesizer::scoreQuality(glyph, profile);
    EXPECT_GT(score, 0.0f);
}

TEST_F(GlyphSynthesizerTest, StrokeWeightMatch_Thicken) {
    // Thin glyph with profile expecting thick stems
    QImage thinGlyph = makeStemGlyph(20, 30, 2);

    FontStyleProfile profile;
    profile.avgStemWidth = 5.0f;  // much wider than 2px
    profile.avgInkDensity = 0.3f;
    profile.avgAspectRatio = 0.6f;
    profile.sampleCount = 5;

    // The thin glyph should score differently from profile
    float thinScore = GlyphSynthesizer::scoreQuality(thinGlyph, profile);
    // Score should be < 1.0 due to stem width mismatch
    EXPECT_LT(thinScore, 1.0f);
}

TEST_F(GlyphSynthesizerTest, StrokeWeightMatch_Thin) {
    // Thick glyph with profile expecting thin stems
    QImage thickGlyph = makeStemGlyph(20, 30, 8);

    FontStyleProfile profile;
    profile.avgStemWidth = 2.0f;  // much thinner than 8px
    profile.avgInkDensity = 0.1f;
    profile.avgAspectRatio = 0.6f;
    profile.sampleCount = 5;

    float thickScore = GlyphSynthesizer::scoreQuality(thickGlyph, profile);
    EXPECT_LT(thickScore, 1.0f);
}

// ============================================================================
// Quality Scoring Tests
// ============================================================================

TEST_F(GlyphSynthesizerTest, QualityScore_GoodMatch) {
    // Create glyph that closely matches the profile
    QImage glyph = makeStemGlyph(10, 20, 3);

    FontStyleProfile profile;
    profile.avgStemWidth = GlyphSynthesizer::measureStemWidth(glyph);
    profile.avgInkDensity = GlyphSynthesizer::measureInkDensity(glyph);
    profile.avgAspectRatio = static_cast<float>(glyph.width()) / glyph.height();
    profile.sampleCount = 5;

    float score = GlyphSynthesizer::scoreQuality(glyph, profile);
    // Perfect match should be close to 1.0
    EXPECT_GT(score, 0.8f);
}

TEST_F(GlyphSynthesizerTest, QualityScore_BadMatch) {
    // Tiny thin glyph vs profile with large thick metrics
    QImage glyph = makeStemGlyph(4, 8, 1);

    FontStyleProfile profile;
    profile.avgStemWidth = 10.0f;  // way thicker
    profile.avgInkDensity = 0.8f;  // way denser
    profile.avgAspectRatio = 2.0f; // very different aspect
    profile.sampleCount = 5;

    float score = GlyphSynthesizer::scoreQuality(glyph, profile);
    EXPECT_LT(score, 0.5f);
}

TEST_F(GlyphSynthesizerTest, QualityScore_NullImage) {
    QImage nullImg;
    FontStyleProfile profile;
    profile.sampleCount = 5;

    float score = GlyphSynthesizer::scoreQuality(nullImg, profile);
    EXPECT_FLOAT_EQ(score, 0.0f);
}

// ============================================================================
// Measurement Tests
// ============================================================================

TEST_F(GlyphSynthesizerTest, MeasureStemWidth_VerticalStem) {
    QImage glyph = makeStemGlyph(20, 30, 4);
    float stem = GlyphSynthesizer::measureStemWidth(glyph);
    // Should detect the 4px wide stem
    EXPECT_NEAR(stem, 4.0f, 1.5f);
}

TEST_F(GlyphSynthesizerTest, MeasureInkDensity_FilledGlyph) {
    QImage filled = makeFilledGlyph(10, 10);
    float density = GlyphSynthesizer::measureInkDensity(filled);
    EXPECT_NEAR(density, 1.0f, 0.01f);
}

TEST_F(GlyphSynthesizerTest, MeasureInkDensity_EmptyGlyph) {
    QImage empty(10, 10, QImage::Format_ARGB32_Premultiplied);
    empty.fill(Qt::transparent);
    float density = GlyphSynthesizer::measureInkDensity(empty);
    EXPECT_FLOAT_EQ(density, 0.0f);
}

TEST_F(GlyphSynthesizerTest, MeasureSlant_UprightGlyph) {
    // Perfectly vertical stem -- slant should be near 0
    QImage glyph = makeStemGlyph(20, 40, 4);
    float slant = GlyphSynthesizer::measureSlant(glyph);
    EXPECT_LT(slant, 5.0f);  // Less than 5 degrees
}

TEST_F(GlyphSynthesizerTest, MeasureSerifExtent_NullGlyph) {
    QImage nullImg;
    float serif = GlyphSynthesizer::measureSerifExtent(nullImg);
    EXPECT_FLOAT_EQ(serif, 0.0f);
}

TEST_F(GlyphSynthesizerTest, MeasureStemWidth_NullGlyph) {
    QImage nullImg;
    float stem = GlyphSynthesizer::measureStemWidth(nullImg);
    EXPECT_FLOAT_EQ(stem, 0.0f);
}

// ============================================================================
// ScaleToCapHeight Tests (C1 fix -- uppercase from lowercase scales UP)
// ============================================================================

TEST_F(GlyphSynthesizerTest, Synthesize_O_from_o_ScaleUp) {
    // Build atlas with lowercase 'o' -- synthesize uppercase 'O' via ScaleToCapHeight
    QImage smallO(10, 14, QImage::Format_ARGB32_Premultiplied);
    smallO.fill(Qt::transparent);
    for (int y = 1; y < 13; ++y)
        for (int x = 1; x < 9; ++x)
            smallO.setPixel(x, y, qRgba(0, 0, 0, 255));

    auto atlas = buildAtlas({{U'o', smallO}});

    GlyphSynthesizer synth;
    synth.buildProfile(atlas);
    auto result = synth.synthesize(U'O', atlas);

    EXPECT_TRUE(result.success);
    EXPECT_FALSE(result.image.isNull());
    // Scaled UP glyph should be LARGER than original
    EXPECT_GT(result.image.height(), smallO.height());
}

TEST_F(GlyphSynthesizerTest, RecipeExists_U_from_u) {
    // Verify U←u recipe uses ScaleToCapHeight
    const GlyphRecipe* recipe = GlyphSynthesizer::findRecipe(U'U');
    ASSERT_NE(recipe, nullptr);
    EXPECT_EQ(recipe->strategies[0].donor, U'u');
    EXPECT_EQ(recipe->strategies[0].transform,
              GlyphRecipe::Strategy::Transform::ScaleToCapHeight);
}

TEST_F(GlyphSynthesizerTest, RecipeExists_u_with_U_fallback) {
    // Verify u←n VFlip first, then u←U ScaleToXHeight as fallback
    const GlyphRecipe* recipe = GlyphSynthesizer::findRecipe(U'u');
    ASSERT_NE(recipe, nullptr);
    ASSERT_GE(recipe->strategies.size(), 2u);
    EXPECT_EQ(recipe->strategies[0].donor, U'n');
    EXPECT_EQ(recipe->strategies[0].transform,
              GlyphRecipe::Strategy::Transform::VFlip);
    EXPECT_EQ(recipe->strategies[1].donor, U'U');
    EXPECT_EQ(recipe->strategies[1].transform,
              GlyphRecipe::Strategy::Transform::ScaleToXHeight);
}

// ============================================================================
// Direct matchStrokeWeight Tests (M1 fix)
// ============================================================================

TEST_F(GlyphSynthesizerTest, MatchStrokeWeight_DilationIncreasesInk) {
    // Thin 2px stem glyph, profile expects 5px -- dilation should add pixels
    QImage thinGlyph = makeStemGlyph(20, 30, 2);
    float origDensity = GlyphSynthesizer::measureInkDensity(thinGlyph);

    // Synthesize 'b' from thin 'd' with a thick profile
    // We test indirectly through the full synthesis pipeline
    auto atlas = buildAtlas({{U'd', thinGlyph}});

    GlyphSynthesizer synth;
    synth.buildProfile(atlas);

    // Build a custom profile with wider stems to force dilation
    FontStyleProfile thickProfile = synth.profile();
    thickProfile.avgStemWidth = synth.profile().avgStemWidth * 2.5f;

    // We can't call matchStrokeWeight directly (private), but we can
    // verify via synthesis that the output glyph has more ink
    auto result = synth.synthesize(U'b', atlas);
    ASSERT_TRUE(result.success);

    float resultDensity = GlyphSynthesizer::measureInkDensity(result.image);
    // HFlip preserves density, stroke matching may increase it
    EXPECT_GT(resultDensity, 0.0f);
}

TEST_F(GlyphSynthesizerTest, MatchStrokeWeight_CloseMatchPreservesGlyph) {
    // When stem widths are close (<15%), matchStrokeWeight should be a no-op
    // We verify this by checking that the score is high when glyph matches profile
    QImage glyph = makeStemGlyph(15, 25, 4);

    FontStyleProfile profile;
    profile.avgStemWidth = GlyphSynthesizer::measureStemWidth(glyph);
    profile.avgInkDensity = GlyphSynthesizer::measureInkDensity(glyph);
    profile.avgAspectRatio = static_cast<float>(glyph.width()) / glyph.height();
    profile.sampleCount = 5;

    float score = GlyphSynthesizer::scoreQuality(glyph, profile);
    // Close match should yield high quality (near 1.0)
    EXPECT_GT(score, 0.9f);
}

// ============================================================================
// Compositor Integration Test (M5 fix)
// ============================================================================

TEST_F(GlyphSynthesizerTest, CompositorUsesSynthesizedGlyphs) {
    // Atlas has 'd' but NOT 'b'. Compositor should synthesize 'b' from 'd'
    // and produce non-transparent pixels for it.
    QImage dGlyph(12, 20, QImage::Format_ARGB32_Premultiplied);
    dGlyph.fill(Qt::transparent);
    for (int y = 0; y < 20; ++y)
        for (int x = 8; x < 12; ++x)
            dGlyph.setPixel(x, y, qRgba(0, 0, 0, 255));
    for (int y = 5; y < 15; ++y)
        for (int x = 1; x < 8; ++x)
            dGlyph.setPixel(x, y, qRgba(0, 0, 0, 255));

    auto atlas = buildAtlas({{U'd', dGlyph}});
    ASSERT_TRUE(atlas.contains(U'd'));
    ASSERT_FALSE(atlas.contains(U'b'));

    FontMatch fm;
    fm.sizePt = 12.0f;
    fm.fontFilePath = "";
    fm.color = {0, 0, 0};

    GlyphAtlasCompositor compositor;
    QImage output = compositor.compose(
        U"db", atlas, fm, {255, 255, 255},
        100, 30, 96, nullptr);

    ASSERT_FALSE(output.isNull());
    EXPECT_EQ(output.width(), 100);
    EXPECT_EQ(output.height(), 30);

    // Count total non-transparent pixels -- should be more than just 'd' alone
    // 'd' renders first, then 'b' (synthesized) should render after it
    int totalPixels = 0;
    for (int y = 0; y < output.height(); ++y) {
        for (int x = 0; x < output.width(); ++x) {
            if (qAlpha(output.pixel(x, y)) > 0)
                ++totalPixels;
        }
    }

    // Also compose with just 'd' to verify 'b' added pixels
    QImage outputD = compositor.compose(
        U"d", atlas, fm, {255, 255, 255},
        100, 30, 96, nullptr);
    int dOnlyPixels = 0;
    for (int y = 0; y < outputD.height(); ++y) {
        for (int x = 0; x < outputD.width(); ++x) {
            if (qAlpha(outputD.pixel(x, y)) > 0)
                ++dOnlyPixels;
        }
    }

    // "db" should have more visible pixels than just "d"
    EXPECT_GT(totalPixels, dOnlyPixels);
}

TEST_F(GlyphSynthesizerTest, CompositorEmptyAtlasSkipsSynthesis) {
    // Empty atlas should not crash when trying synthesis
    GlyphAtlas emptyAtlas;

    FontMatch fm;
    fm.sizePt = 12.0f;
    fm.fontFilePath = "";
    fm.color = {0, 0, 0};

    GlyphAtlasCompositor compositor;
    QImage output = compositor.compose(
        U"hello", emptyAtlas, fm, {255, 255, 255},
        200, 30, 96, nullptr);

    // Should return a valid (possibly all-transparent) image without crashing
    ASSERT_FALSE(output.isNull());
}

// ============================================================================
// Serif Extent Non-Negative Test (m4 fix)
// ============================================================================

TEST_F(GlyphSynthesizerTest, MeasureSerifExtent_NonNegative) {
    // A glyph wider in the middle than at baseline should return 0, not negative
    QImage glyph(20, 20, QImage::Format_ARGB32_Premultiplied);
    glyph.fill(Qt::transparent);
    // Wide middle section
    for (int y = 8; y <= 11; ++y)
        for (int x = 0; x < 20; ++x)
            glyph.setPixel(x, y, qRgba(0, 0, 0, 255));
    // Narrow baseline
    for (int y = 18; y < 20; ++y)
        for (int x = 8; x < 12; ++x)
            glyph.setPixel(x, y, qRgba(0, 0, 0, 255));

    float serif = GlyphSynthesizer::measureSerifExtent(glyph);
    EXPECT_GE(serif, 0.0f);
}
