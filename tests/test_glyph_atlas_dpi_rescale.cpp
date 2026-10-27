// -*- coding: utf-8 -*-
// GitHub #266 — an atlas glyph rendered as a solid block.
//
// THE DEFECT
//
// `EnrichedAtlas` holds glyph images cropped from a page render, and every
// length in it — the images, `referenceLineHeight()` — is in that render's
// pixels. `MainWindow` builds it from `OcrPipeline::renderPage()`, which is
// PAGE-WIDE at the OCR DPI (300 on this rig).
//
// `ImageReplacer` has, since defect #2 was fixed, composited each edit at the
// NATIVE DPI of the image under the edited region — 150 for the thumbnail on
// mixed_dpi_page.pdf. `GlyphAtlasCompositor` then drew atlas glyphs at their
// native pixel size into that composite. So they arrived at exactly twice the
// correct size, and the one character of "EDIT" that came from the atlas
// rendered as a block covering its cell while its neighbours looked right.
//
// Measured 2026-09-29 from the app's own provenance log: `U+54 'T' from=atlas
// h=58 w=30` for a line whose text is 40 px tall in the 300x300 thumbnail.
// Synthesised and FreeType glyphs are normalised independently, which is why
// "EDI" was correct and only the atlas-sourced glyph was not.
//
// WHY THIS TEST IS AT THIS LEVEL
//
// The defect is only reachable end-to-end through a live UI probe against a
// mixed-DPI page, which is slow, needs a Release binary, and was itself blind
// to this for months. The rule it violates is arithmetic and belongs in a
// headless test: a glyph harvested at one DPI and composited at another must be
// rescaled by the ratio.
//
// The identity case is asserted too, and matters more than it looks: the ratio
// is exactly 1.0 on every single-DPI page, so if it were ever NOT 1.0 there,
// this fix would silently resize glyphs for every ordinary scanned edit in the
// product. That is the regression this pins shut.
#include <gtest/gtest.h>

#include "tre/EnrichedAtlas.h"
#include "tre/GlyphAtlas.h"
#include "tre/GlyphAtlasCompositor.h"
#include "tre/TreTypes.h"

#include <QImage>
#include <QPainter>

#include <string>

using namespace pdfe::tre;

namespace {

/// A solid opaque block of a known size, standing in for a harvested glyph.
/// Content does not matter here — only how large it lands in the composite.
QImage blockGlyph(int w, int h)
{
    QImage img(w, h, QImage::Format_ARGB32_Premultiplied);
    img.fill(QColor(0, 0, 0, 255));
    return img;
}

/// Build an EnrichedAtlas holding one glyph of a known size, harvested
/// (notionally) at `sourceDpi`.
EnrichedAtlas atlasWithGlyph(char32_t cp, int w, int h, int sourceDpi)
{
    GlyphAtlas raw;
    raw.addGlyph(cp, blockGlyph(w, h), /*confidence=*/100);

    EnrichedAtlas enriched;
    enriched.build(std::move(raw), std::u32string(1, cp));
    enriched.setSourceDpi(sourceDpi);
    return enriched;
}

/// Width of the drawn ink in a composited bitmap — the bounding box of any
/// pixel with non-zero alpha. Measures what actually landed, rather than what
/// the canvas was sized to.
int inkWidth(const QImage& img)
{
    if (img.isNull()) return 0;
    int minX = img.width(), maxX = -1;
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) {
            if (qAlpha(img.pixel(x, y)) != 0) {
                if (x < minX) minX = x;
                if (x > maxX) maxX = x;
            }
        }
    }
    return maxX < 0 ? 0 : (maxX - minX + 1);
}

int inkHeight(const QImage& img)
{
    if (img.isNull()) return 0;
    int minY = img.height(), maxY = -1;
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) {
            if (qAlpha(img.pixel(x, y)) != 0) {
                if (y < minY) minY = y;
                if (y > maxY) maxY = y;
                break;
            }
        }
    }
    return maxY < 0 ? 0 : (maxY - minY + 1);
}

constexpr int kGlyphW = 30;
constexpr int kGlyphH = 58;   // the measured 'T' from the live provenance log
constexpr int kCanvasW = 400;
constexpr int kCanvasH = 200;

FontMatch plainFont()
{
    FontMatch fm;
    fm.sizePt = 19.2f;
    return fm;
}

}  // namespace

// The identity case. Composite at the DPI the atlas was harvested at and the
// glyph must land at its native size, untouched.
TEST(GlyphAtlasDpiRescale, SameDpiDrawsTheGlyphAtItsNativeSize)
{
    const EnrichedAtlas atlas = atlasWithGlyph(U'T', kGlyphW, kGlyphH, 300);
    GlyphAtlasCompositor compositor;

    const QImage out = compositor.compose(
        std::u32string(1, U'T'), atlas, plainFont(), RgbColor{255, 255, 255},
        kCanvasW, kCanvasH, /*dpi=*/300, nullptr);

    ASSERT_FALSE(out.isNull());
    EXPECT_EQ(inkWidth(out), kGlyphW)
        << "a glyph composited at the DPI it was harvested at was resized. "
           "This path runs for every ordinary single-DPI scanned edit, so any "
           "scaling here is a product-wide regression.";
    EXPECT_EQ(inkHeight(out), kGlyphH);
}

// The defect. Composite at half the harvest DPI and the glyph must halve.
TEST(GlyphAtlasDpiRescale, HalfDpiHalvesTheGlyph)
{
    const EnrichedAtlas atlas = atlasWithGlyph(U'T', kGlyphW, kGlyphH, 300);
    GlyphAtlasCompositor compositor;

    const QImage out = compositor.compose(
        std::u32string(1, U'T'), atlas, plainFont(), RgbColor{255, 255, 255},
        kCanvasW, kCanvasH, /*dpi=*/150, nullptr);

    ASSERT_FALSE(out.isNull());
    // Allow a pixel of rounding on each axis; the point is the factor, not the
    // exact rasterisation.
    EXPECT_NEAR(inkWidth(out), kGlyphW / 2, 1)
        << "a glyph harvested from a 300-DPI page render was drawn at native "
           "size into a 150-DPI composite, so it is twice the size it should "
           "be. That is GitHub #266: the character renders as a block over its "
           "neighbours.";
    EXPECT_NEAR(inkHeight(out), kGlyphH / 2, 1);
}

// And the other direction, so the rule is a ratio rather than a one-way clamp.
TEST(GlyphAtlasDpiRescale, DoubleDpiDoublesTheGlyph)
{
    const EnrichedAtlas atlas = atlasWithGlyph(U'T', kGlyphW, kGlyphH, 150);
    GlyphAtlasCompositor compositor;

    const QImage out = compositor.compose(
        std::u32string(1, U'T'), atlas, plainFont(), RgbColor{255, 255, 255},
        kCanvasW, kCanvasH, /*dpi=*/300, nullptr);

    ASSERT_FALSE(out.isNull());
    EXPECT_NEAR(inkWidth(out), kGlyphW * 2, 1)
        << "scaling is not symmetric: an atlas harvested BELOW the composite "
           "DPI must be scaled up, or a low-DPI donor region renders tiny";
    EXPECT_NEAR(inkHeight(out), kGlyphH * 2, 1);
}

// An atlas built before the DPI was recorded must behave exactly as it did
// before this fix — assume the DPIs match rather than guess a ratio.
TEST(GlyphAtlasDpiRescale, AnUnrecordedSourceDpiIsTreatedAsMatching)
{
    EnrichedAtlas atlas = atlasWithGlyph(U'T', kGlyphW, kGlyphH, 300);
    atlas.setSourceDpi(0);  // "not recorded"
    ASSERT_EQ(atlas.sourceDpi(), 0);

    GlyphAtlasCompositor compositor;
    const QImage out = compositor.compose(
        std::u32string(1, U'T'), atlas, plainFont(), RgbColor{255, 255, 255},
        kCanvasW, kCanvasH, /*dpi=*/150, nullptr);

    ASSERT_FALSE(out.isNull());
    EXPECT_EQ(inkWidth(out), kGlyphW)
        << "an atlas with no recorded source DPI was rescaled anyway. With "
           "nothing to compare against, the only safe assumption is the "
           "historical one: that the DPIs match.";
}

// A negative or nonsensical DPI must not produce a degenerate glyph.
TEST(GlyphAtlasDpiRescale, ANonPositiveDpiDoesNotCollapseTheGlyph)
{
    const EnrichedAtlas atlas = atlasWithGlyph(U'T', kGlyphW, kGlyphH, 300);
    GlyphAtlasCompositor compositor;

    const QImage out = compositor.compose(
        std::u32string(1, U'T'), atlas, plainFont(), RgbColor{255, 255, 255},
        kCanvasW, kCanvasH, /*dpi=*/0, nullptr);

    ASSERT_FALSE(out.isNull());
    EXPECT_EQ(inkWidth(out), kGlyphW)
        << "a zero composite DPI produced a scaled glyph; it must fall back to "
           "the identity rather than divide by it";
}
