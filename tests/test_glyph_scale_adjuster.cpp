#include <gtest/gtest.h>
#include "fonts/GlyphScaleAdjuster.h"
#include "fonts/TextShaper.h"
#include <memory>
#include <string>
#include <cmath>

#include <ft2build.h>
#include FT_FREETYPE_H

using namespace pdfe::fonts;

class GlyphScaleAdjusterTest : public ::testing::Test {
protected:
    void SetUp() override {
        shaper = std::make_unique<TextShaper>();
        adjuster = std::make_unique<GlyphScaleAdjuster>(shaper.get());

        // Initialize FreeType
        FT_Error err = FT_Init_FreeType(&ftLibrary_);
        if (err != 0) {
            return;
        }

        // Load Arial font
        err = FT_New_Face(ftLibrary_, "C:\\Windows\\Fonts\\arial.ttf", 0, &arialFace_);
        if (err != 0) {
            arialFace_ = nullptr;
        }

        // Load Times New Roman font
        err = FT_New_Face(ftLibrary_, "C:\\Windows\\Fonts\\times.ttf", 0, &timesFace_);
        if (err != 0) {
            timesFace_ = nullptr;
        }
    }

    void TearDown() override {
        if (arialFace_) {
            FT_Done_Face(arialFace_);
            arialFace_ = nullptr;
        }
        if (timesFace_) {
            FT_Done_Face(timesFace_);
            timesFace_ = nullptr;
        }
        if (ftLibrary_) {
            FT_Done_FreeType(ftLibrary_);
            ftLibrary_ = nullptr;
        }
    }

    std::unique_ptr<TextShaper> shaper;
    std::unique_ptr<GlyphScaleAdjuster> adjuster;
    FT_Library ftLibrary_ = nullptr;
    FT_Face arialFace_ = nullptr;
    FT_Face timesFace_ = nullptr;
};

TEST_F(GlyphScaleAdjusterTest, SameFontScaleIsOne) {
    if (!arialFace_) {
        GTEST_SKIP() << "Arial font not available for testing";
    }

    std::u32string text = U"Hello";
    float fontSize = 12.0f;

    ScaleAdjustment result = adjuster->computeScale(
        arialFace_,
        arialFace_,
        text,
        fontSize
    );

    // When using the same font as original and substitute,
    // the horizontal scale should be very close to 1.0
    EXPECT_NEAR(result.horizontalScale, 1.0f, 0.01f)
        << "Same font should result in scale ≈ 1.0";

    // Width ratio should also be approximately 1.0
    EXPECT_NEAR(result.widthRatio, 1.0f, 0.01f)
        << "Width ratio for same font should be ≈ 1.0";
}

TEST_F(GlyphScaleAdjusterTest, DifferentFontsScaleNonOne) {
    if (!arialFace_ || !timesFace_) {
        GTEST_SKIP() << "Arial or Times font not available for testing";
    }

    std::u32string text = U"Hello";
    float fontSize = 12.0f;

    ScaleAdjustment result = adjuster->computeScale(
        arialFace_,
        timesFace_,
        text,
        fontSize
    );

    // Arial and Times have different glyph widths, so the scale
    // should be different from 1.0 but within reasonable bounds
    EXPECT_GT(result.horizontalScale, 0.5f)
        << "Scale should be greater than 0.5 for typical fonts";
    EXPECT_LT(result.horizontalScale, 2.0f)
        << "Scale should be less than 2.0 for typical fonts";

    // The scale should not be exactly 1.0 since fonts differ
    EXPECT_NE(result.horizontalScale, 1.0f)
        << "Different fonts should produce scale != 1.0";

    // Width ratio should also be in a reasonable range
    EXPECT_GT(result.widthRatio, 0.5f);
    EXPECT_LT(result.widthRatio, 2.0f);
}

// M15 KILL (phase-3 remediation): horizontalScale must squeeze/stretch a
// metric-incompatible substitute to the ORIGINAL run width, i.e. it must equal
// originalWidth / substituteWidth — NOT its inverse. The pre-existing cases
// only bound the scale into the wide (0.5, 2.0) band or check same-font
// symmetry (scale ≈ 1.0), both of which are invariant under the width-inversion
// mutation `horizontalScale = substituteWidth / originalWidth`. This case pins
// the exact directional ratio for a genuinely ASYMMETRIC font pair, so the
// inversion is observable and fails the assertion.
TEST_F(GlyphScaleAdjusterTest, HorizontalScaleIsOriginalOverSubstituteNotInverted) {
    if (!arialFace_ || !timesFace_) {
        GTEST_SKIP() << "Arial or Times font not available for testing";
    }

    // Long text accumulates the per-glyph width difference between Arial and
    // Times so the two measured widths are clearly distinct (not a near-1.0
    // ratio that would be indistinguishable from its reciprocal).
    const std::u32string text =
        U"The quick brown fox jumps over the lazy dog. "
        U"WWWWMMMM iiillll 0123456789";
    const float fontSize = 12.0f;

    // Measure the SAME widths the production code measures internally, using
    // the same shaper, so the expected ratio is exact rather than approximate.
    const float originalWidth = shaper->measureWidth(arialFace_, text, fontSize);
    const float substituteWidth = shaper->measureWidth(timesFace_, text, fontSize);
    ASSERT_GT(originalWidth, 0.0f);
    ASSERT_GT(substituteWidth, 0.0f);

    // The pair must be genuinely asymmetric, otherwise scale == 1/scale and the
    // inversion is unobservable. Fail loudly rather than silently passing on a
    // degenerate pair.
    const float relDiff =
        std::fabs(originalWidth - substituteWidth) / originalWidth;
    ASSERT_GT(relDiff, 0.02f)
        << "Arial/Times widths too close (relDiff=" << relDiff
        << ") to distinguish the correct scale from its inverse; "
           "choose a more asymmetric fixture.";

    ScaleAdjustment result =
        adjuster->computeScale(arialFace_, timesFace_, text, fontSize);

    const float expected = originalWidth / substituteWidth;   // correct
    const float inverted = substituteWidth / originalWidth;   // the M15 bug

    EXPECT_NEAR(result.horizontalScale, expected, 1e-3f)
        << "horizontalScale must equal originalWidth/substituteWidth ("
        << expected << "), squeezing the substitute to the original run width.";

    EXPECT_GT(std::fabs(result.horizontalScale - inverted), 1e-3f)
        << "horizontalScale must NOT equal the inverse substituteWidth/"
           "originalWidth (" << inverted << ") — that renders substituted "
           "text at the wrong width (the M15 inversion defect).";

    // Directional cross-check: a substitute NARROWER than the original must be
    // stretched (scale > 1); a WIDER substitute must be squeezed (scale < 1).
    if (originalWidth > substituteWidth) {
        EXPECT_GT(result.horizontalScale, 1.0f)
            << "Original wider than substitute → scale must stretch (>1).";
    } else {
        EXPECT_LT(result.horizontalScale, 1.0f)
            << "Original narrower than substitute → scale must squeeze (<1).";
    }
}

TEST_F(GlyphScaleAdjusterTest, EmptyTextReturnsDefault) {
    if (!arialFace_ || !timesFace_) {
        GTEST_SKIP() << "Arial or Times font not available for testing";
    }

    std::u32string emptyText = U"";
    float fontSize = 12.0f;

    ScaleAdjustment result = adjuster->computeScale(
        arialFace_,
        timesFace_,
        emptyText,
        fontSize
    );

    // Empty text should result in default scale values
    EXPECT_FLOAT_EQ(result.horizontalScale, 1.0f)
        << "Empty text should return default horizontalScale = 1.0";

    EXPECT_FLOAT_EQ(result.widthRatio, 1.0f)
        << "Empty text should return default widthRatio = 1.0";
}

TEST_F(GlyphScaleAdjusterTest, NullFaceReturnsDefault) {
    if (!arialFace_) {
        GTEST_SKIP() << "Arial font not available for testing";
    }

    std::u32string text = U"Hello";
    float fontSize = 12.0f;

    // Test with null original face
    ScaleAdjustment result1 = adjuster->computeScale(
        nullptr,
        arialFace_,
        text,
        fontSize
    );

    EXPECT_FLOAT_EQ(result1.horizontalScale, 1.0f)
        << "Null original face should return default horizontalScale = 1.0";
    EXPECT_FLOAT_EQ(result1.widthRatio, 1.0f)
        << "Null original face should return default widthRatio = 1.0";

    // Test with null substitute face
    ScaleAdjustment result2 = adjuster->computeScale(
        arialFace_,
        nullptr,
        text,
        fontSize
    );

    EXPECT_FLOAT_EQ(result2.horizontalScale, 1.0f)
        << "Null substitute face should return default horizontalScale = 1.0";
    EXPECT_FLOAT_EQ(result2.widthRatio, 1.0f)
        << "Null substitute face should return default widthRatio = 1.0";

    // Test with both null
    ScaleAdjustment result3 = adjuster->computeScale(
        nullptr,
        nullptr,
        text,
        fontSize
    );

    EXPECT_FLOAT_EQ(result3.horizontalScale, 1.0f)
        << "Both null faces should return default horizontalScale = 1.0";
    EXPECT_FLOAT_EQ(result3.widthRatio, 1.0f)
        << "Both null faces should return default widthRatio = 1.0";
}

TEST_F(GlyphScaleAdjusterTest, FontSizePreserved) {
    if (!arialFace_ || !timesFace_) {
        GTEST_SKIP() << "Arial or Times font not available for testing";
    }

    std::u32string text = U"Hello World";

    // Test with various font sizes
    float fontSizes[] = {8.0f, 12.0f, 16.0f, 24.0f, 36.0f};

    for (float fontSize : fontSizes) {
        ScaleAdjustment result = adjuster->computeScale(
            arialFace_,
            timesFace_,
            text,
            fontSize
        );

        EXPECT_FLOAT_EQ(result.adjustedFontSize, fontSize)
            << "adjustedFontSize should equal input fontSize ("
            << fontSize << ")";
    }
}

TEST_F(GlyphScaleAdjusterTest, ConsistentResultsForSameInput) {
    if (!arialFace_ || !timesFace_) {
        GTEST_SKIP() << "Arial or Times font not available for testing";
    }

    std::u32string text = U"Test";
    float fontSize = 12.0f;

    // Compute scale multiple times with same inputs
    ScaleAdjustment result1 = adjuster->computeScale(
        arialFace_,
        timesFace_,
        text,
        fontSize
    );

    ScaleAdjustment result2 = adjuster->computeScale(
        arialFace_,
        timesFace_,
        text,
        fontSize
    );

    // Results should be identical
    EXPECT_FLOAT_EQ(result1.horizontalScale, result2.horizontalScale)
        << "Repeated calls should produce same horizontalScale";
    EXPECT_FLOAT_EQ(result1.adjustedFontSize, result2.adjustedFontSize)
        << "Repeated calls should produce same adjustedFontSize";
    EXPECT_FLOAT_EQ(result1.widthRatio, result2.widthRatio)
        << "Repeated calls should produce same widthRatio";
}

// A1-48 / A1-11 repair. The previous version of this case computed two scales
// and then NEVER COMPARED THEM — its four assertions only bounded each scale
// into the wide (0.5, 2.0) band, which an adjuster returning a CONSTANT 1.0 for
// every input satisfies. The in-test comment even said it "just verifies no
// crashes"; the test name promised the opposite.
//
// AUTHORED GROUND TRUTH (hmtx advances read straight from the two faces this
// test loads, normalised by unitsPerEm = 2048):
//
//     glyph   arial.ttf    times.ttf     original/substitute
//     'i'      0.2222       0.2778        0.2222/0.2778 = 0.800
//     'W'      0.9438       0.9438        0.9438/0.9438 = 1.000
//
// horizontalScale squeezes/stretches the SUBSTITUTE run to the ORIGINAL run
// width (= originalWidth / substituteWidth — pinned directionally by
// HorizontalScaleIsOriginalOverSubstituteNotInverted above). Times' 'i' is
// proportionally 25% wider than Arial's, while 'W' has an IDENTICAL advance in
// both faces. So the all-'i' run must be squeezed (scale ≈ 0.80) and the
// all-'W' run must not be scaled at all (scale ≈ 1.00) — a strict, authored
// ordering: scale("iii") < scale("WWW").
//
// Mutations killed:
//   * returning a constant scale (1.0, or any fixed value) — the strict
//     inequality fails;
//   * ignoring the text and scaling from face-level averages (avgCharWidth /
//     units-per-em) instead of the actual run — both runs would then get the
//     same scale;
//   * measuring only the first glyph, or measuring the substitute run against
//     itself — 'W' would stop pinning at 1.0.
TEST_F(GlyphScaleAdjusterTest, DifferentTextProducesDifferentScales) {
    if (!arialFace_ || !timesFace_) {
        GTEST_SKIP() << "Arial or Times font not available for testing";
    }

    const float fontSize = 12.0f;

    // Narrow text: Arial 'i' is NARROWER than Times 'i' -> squeeze.
    ScaleAdjustment narrow = adjuster->computeScale(
        arialFace_,
        timesFace_,
        U"iii",
        fontSize
    );

    // Wide text: 'W' has the same advance in both faces -> no scaling.
    ScaleAdjustment wide = adjuster->computeScale(
        arialFace_,
        timesFace_,
        U"WWW",
        fontSize
    );

    // Sanity band retained (a scale outside this is nonsense for these faces).
    EXPECT_GT(narrow.horizontalScale, 0.5f);
    EXPECT_LT(narrow.horizontalScale, 2.0f);
    EXPECT_GT(wide.horizontalScale, 0.5f);
    EXPECT_LT(wide.horizontalScale, 2.0f);

    // THE oracle the old version was missing: the two scales must be compared,
    // in the direction the authored glyph metrics dictate.
    EXPECT_LT(narrow.horizontalScale, wide.horizontalScale)
        << "Times' 'i' is proportionally wider than Arial's (0.2778 vs 0.2222 em) "
           "while 'W' is identical in both faces, so the all-'i' run MUST get a "
           "strictly smaller horizontalScale than the all-'W' run. Got iii="
        << narrow.horizontalScale << " WWW=" << wide.horizontalScale
        << " — a text-independent (constant) scale would produce exactly this "
           "equality.";

    // The gap is authored at 0.200; require most of it so a near-tie (i.e. a
    // scale that barely depends on the text) cannot sneak past the inequality.
    EXPECT_GT(wide.horizontalScale - narrow.horizontalScale, 0.10f)
        << "Authored scale gap is 1.000 - 0.800 = 0.200; a materially smaller "
           "gap means the scale is not being driven by the actual run glyphs.";

    // Pin each scale to its authored ratio (tolerance covers hinting/rounding
    // in the HarfBuzz advance sum, which is far below this margin).
    EXPECT_NEAR(narrow.horizontalScale, 0.2222f / 0.2778f, 0.05f)
        << "scale(\"iii\") must equal Arial-i / Times-i = 0.800.";
    EXPECT_NEAR(wide.horizontalScale, 1.0f, 0.05f)
        << "scale(\"WWW\") must be 1.0 — 'W' has an identical 0.9438 em advance "
           "in Arial and Times, so nothing needs squeezing.";
}

TEST_F(GlyphScaleAdjusterTest, LongTextHandledCorrectly) {
    if (!arialFace_ || !timesFace_) {
        GTEST_SKIP() << "Arial or Times font not available for testing";
    }

    // Test with longer text to ensure the adjuster handles it properly
    std::u32string longText = U"The quick brown fox jumps over the lazy dog. "
                              U"Pack my box with five dozen liquor jugs.";
    float fontSize = 12.0f;

    ScaleAdjustment result = adjuster->computeScale(
        arialFace_,
        timesFace_,
        longText,
        fontSize
    );

    // Should produce reasonable scale values
    EXPECT_GT(result.horizontalScale, 0.5f);
    EXPECT_LT(result.horizontalScale, 2.0f);
    EXPECT_FLOAT_EQ(result.adjustedFontSize, fontSize);
}

TEST_F(GlyphScaleAdjusterTest, SpecialCharactersHandled) {
    if (!arialFace_ || !timesFace_) {
        GTEST_SKIP() << "Arial or Times font not available for testing";
    }

    std::u32string text = U"Hello, World! 123 @#$%";
    float fontSize = 12.0f;

    ScaleAdjustment result = adjuster->computeScale(
        arialFace_,
        timesFace_,
        text,
        fontSize
    );

    // Should handle special characters without crashing
    EXPECT_GT(result.horizontalScale, 0.0f);
    EXPECT_FLOAT_EQ(result.adjustedFontSize, fontSize);
}

TEST_F(GlyphScaleAdjusterTest, ZeroFontSizeHandledGracefully) {
    if (!arialFace_ || !timesFace_) {
        GTEST_SKIP() << "Arial or Times font not available for testing";
    }

    std::u32string text = U"Hello";
    float zeroFontSize = 0.0f;

    ScaleAdjustment result = adjuster->computeScale(
        arialFace_,
        timesFace_,
        text,
        zeroFontSize
    );

    // Should handle zero font size gracefully
    // (implementation may return default values or handle specially)
    EXPECT_FLOAT_EQ(result.adjustedFontSize, zeroFontSize);
    // Don't assert specific behavior for scale with zero font size,
    // just ensure no crash
}

TEST_F(GlyphScaleAdjusterTest, NegativeFontSizeHandledGracefully) {
    if (!arialFace_ || !timesFace_) {
        GTEST_SKIP() << "Arial or Times font not available for testing";
    }

    std::u32string text = U"Hello";
    float negativeFontSize = -12.0f;

    // Should not crash with negative font size
    ScaleAdjustment result = adjuster->computeScale(
        arialFace_,
        timesFace_,
        text,
        negativeFontSize
    );

    // Just verify no crash; implementation defines behavior
    (void)result;
}

TEST_F(GlyphScaleAdjusterTest, VeryLargeFontSizeHandled) {
    if (!arialFace_ || !timesFace_) {
        GTEST_SKIP() << "Arial or Times font not available for testing";
    }

    std::u32string text = U"Hello";
    float largeFontSize = 1000.0f;

    ScaleAdjustment result = adjuster->computeScale(
        arialFace_,
        timesFace_,
        text,
        largeFontSize
    );

    EXPECT_FLOAT_EQ(result.adjustedFontSize, largeFontSize);
    EXPECT_GT(result.horizontalScale, 0.0f);
}

TEST_F(GlyphScaleAdjusterTest, UnicodeTextHandled) {
    if (!arialFace_ || !timesFace_) {
        GTEST_SKIP() << "Arial or Times font not available for testing";
    }

    // Test with various Unicode characters (if supported by fonts)
    std::u32string text = U"Héllo Wörld";
    float fontSize = 12.0f;

    ScaleAdjustment result = adjuster->computeScale(
        arialFace_,
        timesFace_,
        text,
        fontSize
    );

    // Should handle accented characters
    EXPECT_GT(result.horizontalScale, 0.5f);
    EXPECT_LT(result.horizontalScale, 2.0f);
    EXPECT_FLOAT_EQ(result.adjustedFontSize, fontSize);
}

TEST_F(GlyphScaleAdjusterTest, SingleCharacterText) {
    if (!arialFace_ || !timesFace_) {
        GTEST_SKIP() << "Arial or Times font not available for testing";
    }

    std::u32string text = U"A";
    float fontSize = 12.0f;

    ScaleAdjustment result = adjuster->computeScale(
        arialFace_,
        timesFace_,
        text,
        fontSize
    );

    // Should handle single character
    EXPECT_GT(result.horizontalScale, 0.5f);
    EXPECT_LT(result.horizontalScale, 2.0f);
    EXPECT_FLOAT_EQ(result.adjustedFontSize, fontSize);
}

TEST_F(GlyphScaleAdjusterTest, WhitespaceOnlyText) {
    if (!arialFace_ || !timesFace_) {
        GTEST_SKIP() << "Arial or Times font not available for testing";
    }

    std::u32string text = U"   ";
    float fontSize = 12.0f;

    ScaleAdjustment result = adjuster->computeScale(
        arialFace_,
        timesFace_,
        text,
        fontSize
    );

    // Should handle whitespace-only text
    EXPECT_GT(result.horizontalScale, 0.0f);
    EXPECT_FLOAT_EQ(result.adjustedFontSize, fontSize);
}
