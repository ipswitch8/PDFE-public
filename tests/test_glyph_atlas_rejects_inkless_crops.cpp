// -*- coding: utf-8 -*-
// GitHub #266 — the atlas accepted a crop that contained no ink, and it won.
//
// THE DEFECT
//
// `GlyphAtlas::isolateAlpha()` scores every pixel by where it falls between the
// detected text and background colours:
//
//     alpha = 255 * distToBg / (distToText + distToBg)
//
// so alpha 255 means "this pixel IS the text colour" and alpha 128 means
// "equidistant from both". The harvest then stored whatever came out, with no
// question asked about whether it was a letter.
//
// Measured on mixed_dpi_page.pdf, 2026-09-29, from the application's own
// diagnostics: the harvested 'T' was a featureless grey BAR — 30x58,
// alphaMin=0, **alphaMax=127**, alphaMean=76.6, ink over 60% of its box. Not a
// faint 'T'; no letter structure at all. Peak alpha 127 says precisely that no
// pixel in that crop was more text-like than background-like.
//
// It was stored anyway, and because the scan-cropped atlas is the
// HIGHEST-FIDELITY source in `EnrichedAtlas::glyphFromScanImpl`'s chain, it
// outranked every fallback. So editing `THUMBNAIL` to `EDIT` drew `EDI` plus a
// grey block: E, D and I are absent from the source string, fell through to
// synthesis and FreeType, and rendered correctly. **The one character with the
// best available source produced the only wrong output.**
//
// WHY THE FIX IS A REJECTION AND NOT A REPAIR
//
// The fallback chain already renders those three correctly on this very page.
// An unusable harvest does not need mending; it needs to stop outranking a path
// that works. Rejecting costs nothing and cannot make output worse than the
// fallback, which is the same fallback used for any character the scan never
// contained.
//
// WHY 128 AND NOT A TUNED NUMBER
//
// It is the discriminator's own midpoint, so it means something statable: below
// it, no pixel is predominantly text. A tuned threshold would drift and nobody
// could later say what it was supposed to express — which is how the 0.85
// sharpness ratio in this same issue's probes came to be applied to grey
// margin for months.
#include <gtest/gtest.h>

#include "tre/GlyphAtlas.h"

#include <QColor>
#include <QImage>

using namespace pdfe::tre;

namespace {

/// A mask whose alpha peaks at `peak`, shaped like a crude glyph so the test
/// cannot pass merely because the image is empty.
QImage maskWithPeak(int peak, int w = 30, int h = 58)
{
    QImage img(w, h, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    for (int y = h / 4; y < h - h / 4; ++y) {
        for (int x = w / 4; x < w - w / 4; ++x) {
            const int a = peak;
            img.setPixel(x, y, qRgba(0, 0, 0, a));
        }
    }
    return img;
}

}  // namespace

// The escaped case, at the exact measured value.
TEST(GlyphAtlasInkRejection, AMaskPeakingAt127IsNotInk)
{
    EXPECT_FALSE(GlyphAtlas::maskCarriesInkForTesting(maskWithPeak(127)))
        << "the mask measured on the live page — peak alpha 127 — was accepted "
           "as a glyph. Peak 127 means no pixel in the crop is more text-like "
           "than background-like, so whatever was cropped is not ink. This is "
           "GitHub #266: it was stored, it outranked the working fallback, and "
           "it rendered as a grey block.";
}

// The boundary itself, stated from both sides so the rule is a threshold and
// not an accident of one value.
TEST(GlyphAtlasInkRejection, TheBoundaryIsTheDiscriminatorsMidpoint)
{
    EXPECT_FALSE(GlyphAtlas::maskCarriesInkForTesting(maskWithPeak(128)))
        << "alpha exactly 128 is equidistant between text and background; it "
           "is not evidence of ink";
    EXPECT_TRUE(GlyphAtlas::maskCarriesInkForTesting(maskWithPeak(129)))
        << "one step past the midpoint IS predominantly text, and must be "
           "accepted — a rejection rule that keeps climbing would start "
           "discarding legitimately faint scans";
}

// The case that must not regress: an ordinary crisp glyph.
TEST(GlyphAtlasInkRejection, AnOrdinaryOpaqueGlyphIsAccepted)
{
    EXPECT_TRUE(GlyphAtlas::maskCarriesInkForTesting(maskWithPeak(255)))
        << "a fully opaque mask was rejected. Every ordinary scanned edit goes "
           "through this path, so a rule that rejects solid ink would empty the "
           "atlas for the whole product rather than for one bad crop.";
}

// Degenerate inputs must decide, not crash.
TEST(GlyphAtlasInkRejection, EmptyAndNullMasksAreNotInk)
{
    EXPECT_FALSE(GlyphAtlas::maskCarriesInkForTesting(QImage()))
        << "a null mask must be rejected rather than dereferenced";

    QImage blank(10, 10, QImage::Format_ARGB32_Premultiplied);
    blank.fill(Qt::transparent);
    EXPECT_FALSE(GlyphAtlas::maskCarriesInkForTesting(blank))
        << "a fully transparent crop contains no ink by definition";
}
