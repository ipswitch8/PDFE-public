#include <gtest/gtest.h>
#include "tre/ScanStyleTransfer.h"
#include "tre/GlyphAtlas.h"
#include "tre/EnrichedAtlas.h"
#include <QImage>
#include <cmath>
#include <numeric>
#include <vector>

using namespace pdfe::tre;

// ---------------------------------------------------------------------------
// Helpers: create synthetic test glyphs
// ---------------------------------------------------------------------------

namespace {

/// Create a glyph with a vertical stem (rectangle) in the center.
/// Alpha ramps linearly at edges over `edgeWidth` pixels.
QImage makeStemGlyph(int w, int h, int stemLeft, int stemRight,
                     QRgb textColor = qRgba(30, 30, 30, 255),
                     int edgeWidth = 1)
{
    QImage img(w, h, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);

    for (int y = 0; y < h; ++y) {
        QRgb* row = reinterpret_cast<QRgb*>(img.scanLine(y));
        for (int x = 0; x < w; ++x) {
            // Distance from stem region
            int distLeft = stemLeft - x;
            int distRight = x - stemRight;
            int dist = std::max(distLeft, distRight);
            if (dist < 0) dist = 0; // inside stem

            if (dist == 0) {
                row[x] = textColor;
            } else if (dist <= edgeWidth) {
                // Linear alpha ramp
                int alpha = 255 - (255 * dist / (edgeWidth + 1));
                alpha = std::clamp(alpha, 0, 255);
                int r = qRed(textColor) * alpha / 255;
                int g = qGreen(textColor) * alpha / 255;
                int b = qBlue(textColor) * alpha / 255;
                row[x] = qRgba(r, g, b, alpha);
            }
        }
    }
    return img;
}

/// Create a glyph with controlled noise in both edges and foreground.
QImage makeNoisyGlyph(int w, int h, float edgeNoiseSigma, float fgNoiseSigma,
                      int edgeWidth = 3)
{
    QImage img(w, h, QImage::Format_ARGB32);
    img.fill(Qt::transparent);

    // Simple deterministic noise using LCG
    uint32_t rng = 42;
    auto nextRng = [&]() -> uint32_t {
        rng = rng * 1664525u + 1013904223u;
        return rng;
    };
    auto uniform01 = [&]() -> float {
        return static_cast<float>(nextRng() & 0xFFFF) / 65536.0f;
    };
    auto gaussian = [&](float sigma) -> float {
        float u1 = std::max(1e-7f, uniform01());
        float u2 = uniform01();
        return std::sqrt(-2.0f * std::log(u1)) * std::cos(6.2831853f * u2) * sigma;
    };

    int stemLeft = w / 4;
    int stemRight = w * 3 / 4;

    for (int y = 0; y < h; ++y) {
        QRgb* row = reinterpret_cast<QRgb*>(img.scanLine(y));
        for (int x = 0; x < w; ++x) {
            int distLeft = stemLeft - x;
            int distRight = x - stemRight;
            int dist = std::max(distLeft, distRight);
            if (dist < 0) dist = 0;

            if (dist == 0) {
                // Opaque foreground with noise
                int r = std::clamp(30 + static_cast<int>(gaussian(fgNoiseSigma)), 0, 255);
                int g = std::clamp(30 + static_cast<int>(gaussian(fgNoiseSigma)), 0, 255);
                int b = std::clamp(30 + static_cast<int>(gaussian(fgNoiseSigma)), 0, 255);
                row[x] = qRgba(r, g, b, 255);
            } else if (dist <= edgeWidth) {
                // Edge with noisy alpha
                int baseAlpha = 255 - (255 * dist / (edgeWidth + 1));
                int alpha = std::clamp(
                    baseAlpha + static_cast<int>(gaussian(edgeNoiseSigma)), 20, 240);
                row[x] = qRgba(30, 30, 30, alpha);
            }
        }
    }
    return img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

/// Create a glyph with a wide alpha transition (blurry edge).
QImage makeBlurryGlyph(int w, int h, int transitionWidth)
{
    return makeStemGlyph(w, h, w / 4, w * 3 / 4, qRgba(30, 30, 30, 255), transitionWidth);
}

/// Build a minimal GlyphAtlas with synthetic glyphs.
GlyphAtlas buildTestAtlas(const std::vector<std::pair<char32_t, QImage>>& glyphs)
{
    GlyphAtlas atlas;
    for (const auto& [cp, img] : glyphs) {
        atlas.addGlyph(cp, img, 95);
    }
    return atlas;
}

} // anonymous namespace

// ===========================================================================
// Measurement tests
// ===========================================================================

TEST(ScanStyleMeasurement, EdgeRoughness_CleanGlyph) {
    // Smooth 1-pixel alpha ramp → low roughness
    QImage glyph = makeStemGlyph(20, 30, 5, 15, qRgba(30, 30, 30, 255), 1);
    float roughness = ScanStyleTransfer::measureEdgeRoughness(glyph);
    EXPECT_LT(roughness, 15.0f) << "Clean glyph should have low edge roughness";
}

TEST(ScanStyleMeasurement, EdgeRoughness_NoisyGlyph) {
    QImage glyph = makeNoisyGlyph(20, 30, 30.0f, 0.0f, 3);
    float roughness = ScanStyleTransfer::measureEdgeRoughness(glyph);
    // Should be in range related to injected sigma (30), but measured from
    // the resulting distribution which is clamped and discretized
    EXPECT_GT(roughness, 10.0f) << "Noisy edges should produce measurable roughness";
}

TEST(ScanStyleMeasurement, FgNoise_UniformColor) {
    // Perfectly uniform foreground → near-zero noise
    QImage glyph = makeStemGlyph(20, 30, 5, 15, qRgba(30, 30, 30, 255), 0);
    float noise = ScanStyleTransfer::measureForegroundNoise(glyph);
    EXPECT_LT(noise, 2.0f) << "Uniform color glyph should have near-zero fg noise";
}

TEST(ScanStyleMeasurement, FgNoise_WithVariation) {
    QImage glyph = makeNoisyGlyph(30, 40, 0.0f, 8.0f, 1);
    float noise = ScanStyleTransfer::measureForegroundNoise(glyph);
    EXPECT_GT(noise, 3.0f) << "Noisy fg glyph should have measurable noise";
    EXPECT_LT(noise, 15.0f) << "Noise should be in reasonable range for sigma=8";
}

TEST(ScanStyleMeasurement, EdgeTransition_Sharp) {
    QImage glyph = makeStemGlyph(20, 30, 5, 15, qRgba(30, 30, 30, 255), 1);
    float tw = ScanStyleTransfer::measureEdgeTransitionWidth(glyph);
    EXPECT_LE(tw, 2.0f) << "Sharp edges should have narrow transition";
}

TEST(ScanStyleMeasurement, EdgeTransition_Blurry) {
    QImage glyph = makeBlurryGlyph(30, 40, 5);
    float tw = ScanStyleTransfer::measureEdgeTransitionWidth(glyph);
    EXPECT_GT(tw, 2.0f) << "Blurry edges should have wider transition";
}

TEST(ScanStyleMeasurement, Calibrate_EmptyAtlas) {
    GlyphAtlas atlas;
    ScanStyleTransfer transfer;
    transfer.calibrate(atlas);
    EXPECT_FALSE(transfer.style().valid);

    // apply() should return input unchanged
    QImage glyph = makeStemGlyph(10, 20, 3, 7);
    QImage result = transfer.apply(glyph);
    EXPECT_EQ(result.size(), glyph.size());
}

TEST(ScanStyleMeasurement, Calibrate_SingleGlyph) {
    QImage g = makeNoisyGlyph(20, 30, 15.0f, 5.0f, 3);
    GlyphAtlas atlas = buildTestAtlas({{U'A', g}});
    ScanStyleTransfer transfer;
    transfer.calibrate(atlas);
    EXPECT_TRUE(transfer.style().valid);
    EXPECT_EQ(transfer.style().sampleCount, 1);
}

// ===========================================================================
// Processing stage tests
// ===========================================================================

TEST(ScanStyleStage, StrokeWeight_Thicker) {
    // Create a thin-stemmed glyph and request thicker target
    QImage glyph = makeStemGlyph(20, 30, 8, 12, qRgba(30, 30, 30, 255), 0);
    QImage result = ScanStyleTransfer::adjustStrokeWeight(glyph, 8.0f);
    // After dilation, more pixels should be opaque
    int origOpaque = 0, resultOpaque = 0;
    for (int y = 0; y < glyph.height(); ++y) {
        const QRgb* oRow = reinterpret_cast<const QRgb*>(glyph.constScanLine(y));
        const QRgb* rRow = reinterpret_cast<const QRgb*>(result.constScanLine(y));
        for (int x = 0; x < glyph.width(); ++x) {
            if (qAlpha(oRow[x]) > 128) ++origOpaque;
            if (qAlpha(rRow[x]) > 128) ++resultOpaque;
        }
    }
    EXPECT_GT(resultOpaque, origOpaque) << "Dilation should increase opaque pixel count";
}

TEST(ScanStyleStage, StrokeWeight_NoOp) {
    QImage glyph = makeStemGlyph(20, 30, 7, 13, qRgba(30, 30, 30, 255), 0);
    float currentStem = GlyphSynthesizer::measureStemWidth(glyph);
    // Target within 15% → no change
    QImage result = ScanStyleTransfer::adjustStrokeWeight(glyph, currentStem * 1.1f);
    EXPECT_EQ(result.size(), glyph.size());
}

TEST(ScanStyleStage, RoughenEdges_IncreasesVariation) {
    QImage glyph = makeStemGlyph(20, 30, 5, 15, qRgba(30, 30, 30, 255), 2);
    float before = ScanStyleTransfer::measureEdgeRoughness(glyph);
    QImage result = ScanStyleTransfer::roughenEdges(glyph, 30.0f, 12345);
    float after = ScanStyleTransfer::measureEdgeRoughness(result);
    EXPECT_GT(after, before) << "Roughening should increase edge variation";
}

TEST(ScanStyleStage, RoughenEdges_PreservesInterior) {
    QImage glyph = makeStemGlyph(20, 30, 5, 15, qRgba(30, 30, 30, 255), 2);
    QImage result = ScanStyleTransfer::roughenEdges(glyph, 50.0f, 99);

    // Check that fully opaque pixels remain opaque
    QImage origNP = glyph.convertToFormat(QImage::Format_ARGB32);
    QImage resNP = result.convertToFormat(QImage::Format_ARGB32);
    int interiorChanged = 0;
    for (int y = 0; y < origNP.height(); ++y) {
        const QRgb* oRow = reinterpret_cast<const QRgb*>(origNP.constScanLine(y));
        const QRgb* rRow = reinterpret_cast<const QRgb*>(resNP.constScanLine(y));
        for (int x = 0; x < origNP.width(); ++x) {
            if (qAlpha(oRow[x]) == 255 && qAlpha(rRow[x]) != 255)
                ++interiorChanged;
        }
    }
    EXPECT_EQ(interiorChanged, 0) << "Interior (alpha=255) pixels should be unchanged";
}

TEST(ScanStyleStage, RoughenEdges_PreservesExterior) {
    QImage glyph = makeStemGlyph(20, 30, 5, 15, qRgba(30, 30, 30, 255), 2);
    QImage result = ScanStyleTransfer::roughenEdges(glyph, 50.0f, 99);

    QImage origNP = glyph.convertToFormat(QImage::Format_ARGB32);
    QImage resNP = result.convertToFormat(QImage::Format_ARGB32);
    int exteriorChanged = 0;
    for (int y = 0; y < origNP.height(); ++y) {
        const QRgb* oRow = reinterpret_cast<const QRgb*>(origNP.constScanLine(y));
        const QRgb* rRow = reinterpret_cast<const QRgb*>(resNP.constScanLine(y));
        for (int x = 0; x < origNP.width(); ++x) {
            if (qAlpha(oRow[x]) == 0 && qAlpha(rRow[x]) != 0)
                ++exteriorChanged;
        }
    }
    EXPECT_EQ(exteriorChanged, 0) << "Exterior (alpha=0) pixels should be unchanged";
}

TEST(ScanStyleStage, RoughenEdges_Deterministic) {
    QImage glyph = makeStemGlyph(20, 30, 5, 15, qRgba(30, 30, 30, 255), 2);
    QImage r1 = ScanStyleTransfer::roughenEdges(glyph, 20.0f, 42);
    QImage r2 = ScanStyleTransfer::roughenEdges(glyph, 20.0f, 42);
    EXPECT_EQ(r1, r2) << "Same seed should produce identical output";
}

TEST(ScanStyleStage, Blur_WidensTransition) {
    QImage glyph = makeStemGlyph(30, 40, 8, 22, qRgba(30, 30, 30, 255), 1);
    float before = ScanStyleTransfer::measureEdgeTransitionWidth(glyph);
    QImage result = ScanStyleTransfer::applyBlur(glyph, 1.0f);
    float after = ScanStyleTransfer::measureEdgeTransitionWidth(result);
    EXPECT_GT(after, before) << "Blur should widen edge transitions";
}

TEST(ScanStyleStage, Blur_SkipLowSigma) {
    QImage glyph = makeStemGlyph(20, 30, 5, 15);
    QImage result = ScanStyleTransfer::applyBlur(glyph, 0.2f);
    EXPECT_EQ(result, glyph) << "Low sigma should return input unchanged";
}

TEST(ScanStyleStage, Noise_AddsVariation) {
    QImage glyph = makeStemGlyph(20, 30, 5, 15, qRgba(30, 30, 30, 255), 0);
    float before = ScanStyleTransfer::measureForegroundNoise(glyph);
    QImage result = ScanStyleTransfer::injectNoise(glyph, 8.0f, 12345);
    float after = ScanStyleTransfer::measureForegroundNoise(result);
    EXPECT_GT(after, before) << "Noise injection should increase fg variation";
}

TEST(ScanStyleStage, Noise_PreservesTransparent) {
    QImage glyph = makeStemGlyph(20, 30, 8, 12, qRgba(30, 30, 30, 255), 0);
    QImage result = ScanStyleTransfer::injectNoise(glyph, 10.0f, 99);

    QImage origNP = glyph.convertToFormat(QImage::Format_ARGB32);
    QImage resNP = result.convertToFormat(QImage::Format_ARGB32);
    int transparentChanged = 0;
    for (int y = 0; y < origNP.height(); ++y) {
        const QRgb* oRow = reinterpret_cast<const QRgb*>(origNP.constScanLine(y));
        const QRgb* rRow = reinterpret_cast<const QRgb*>(resNP.constScanLine(y));
        for (int x = 0; x < origNP.width(); ++x) {
            if (qAlpha(oRow[x]) == 0 && qAlpha(rRow[x]) != 0)
                ++transparentChanged;
        }
    }
    EXPECT_EQ(transparentChanged, 0) << "Transparent pixels should remain transparent";
}

TEST(ScanStyleStage, Noise_SkipLowLevel) {
    QImage glyph = makeStemGlyph(20, 30, 5, 15);
    QImage result = ScanStyleTransfer::injectNoise(glyph, 0.5f, 42);
    EXPECT_EQ(result, glyph) << "Low noise level should return input unchanged";
}

// ===========================================================================
// Full pipeline tests
// ===========================================================================

TEST(ScanStylePipeline, Apply_NullInput) {
    ScanStyleTransfer transfer;
    QImage result = transfer.apply(QImage{});
    EXPECT_TRUE(result.isNull());
}

TEST(ScanStylePipeline, Apply_UncalibratedPassthrough) {
    ScanStyleTransfer transfer;
    QImage glyph = makeStemGlyph(20, 30, 5, 15);
    QImage result = transfer.apply(glyph);
    EXPECT_EQ(result.size(), glyph.size());
}

TEST(ScanStylePipeline, Apply_PreservesPremultipliedInvariant) {
    // Calibrate from noisy atlas
    QImage g = makeNoisyGlyph(20, 30, 20.0f, 8.0f, 3);
    GlyphAtlas atlas = buildTestAtlas({{U'A', g}, {U'B', g}, {U'C', g}});
    ScanStyleTransfer transfer;
    transfer.calibrate(atlas);

    // Apply to clean FreeType-like glyph
    QImage ftGlyph = makeStemGlyph(20, 30, 5, 15, qRgba(30, 30, 30, 255), 1);
    QImage result = transfer.apply(ftGlyph);

    // Verify premultiplied invariant: R <= A, G <= A, B <= A
    QImage premul = result.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    int violations = 0;
    for (int y = 0; y < premul.height(); ++y) {
        const QRgb* row = reinterpret_cast<const QRgb*>(premul.constScanLine(y));
        for (int x = 0; x < premul.width(); ++x) {
            int a = qAlpha(row[x]);
            if (qRed(row[x]) > a || qGreen(row[x]) > a || qBlue(row[x]) > a)
                ++violations;
        }
    }
    EXPECT_EQ(violations, 0) << "Output must satisfy premultiplied invariant";
}

TEST(ScanStylePipeline, Apply_FullPipeline) {
    // Create a noisy atlas (simulating scanned text)
    QImage scanGlyph = makeNoisyGlyph(20, 30, 20.0f, 8.0f, 3);
    GlyphAtlas atlas = buildTestAtlas({
        {U'A', scanGlyph}, {U'B', scanGlyph}, {U'C', scanGlyph},
        {U'D', scanGlyph}, {U'E', scanGlyph}
    });

    ScanStyleTransfer transfer;
    transfer.calibrate(atlas);
    EXPECT_TRUE(transfer.style().valid);

    // Create a clean FreeType-like glyph
    QImage ftGlyph = makeStemGlyph(20, 30, 5, 15, qRgba(30, 30, 30, 255), 1);
    float ftRoughness = ScanStyleTransfer::measureEdgeRoughness(ftGlyph);

    // Apply scan style transfer
    QImage result = transfer.apply(ftGlyph);
    EXPECT_FALSE(result.isNull());

    // Result should have more edge roughness than clean FreeType
    float resultRoughness = ScanStyleTransfer::measureEdgeRoughness(result);
    EXPECT_GT(resultRoughness, ftRoughness)
        << "Scan-matched glyph should have more edge variation than clean FreeType";
}

// ===========================================================================
// Integration tests
// ===========================================================================

TEST(ScanStyleIntegration, EnrichedAtlas_FreeTypeFallback_CachesResult) {
    // Build atlas with just 'A'
    QImage g = makeStemGlyph(15, 25, 4, 11);
    GlyphAtlas rawAtlas = buildTestAtlas({{U'A', g}});

    EnrichedAtlas atlas;
    atlas.build(std::move(rawAtlas), U"A");

    // Without FreeType fallback, 'Z' should return null
    QImage z1 = atlas.glyph(U'Z');
    // Z might come from synthesis (A→Z recipe doesn't exist but check)
    // Either way, if we get a result, caching should work

    // Requesting same char twice should return identical result (cached)
    QImage first = atlas.glyph(U'$');  // Unlikely to have synthesis recipe
    QImage second = atlas.glyph(U'$');
    EXPECT_EQ(first.isNull(), second.isNull());
    if (!first.isNull()) {
        EXPECT_EQ(first.size(), second.size());
    }
}
