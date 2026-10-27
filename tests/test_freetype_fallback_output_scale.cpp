// GitHub #268 — the FreeType fallback must RASTERISE at the composite DPI,
// not rasterise at the scan DPI and let the compositor resample.
//
// #268's measurement was end-to-end: replacing a 9-character word on
// mixed_dpi_page.pdf dropped edge-energy sharpness from 13.92 to 11.27. That
// measurement is a live-UI render-oracle run; these tests pin the same property
// at the unit level, where it is deterministic and cheap, and where the
// comparison can be made EXACTLY — the same glyph, the same target size, the
// two code paths differing only in where the resampling happens.
//
// Note what this file is also: the first direct test of the FreeType fallback
// path at all. Nothing in tests/ called setFreeTypeFallback() before, which is
// a large part of why #268 survived #266.

#include "tre/EnrichedAtlas.h"
#include "tre/BitmapCompositor.h"
#include "tre/GlyphAtlas.h"
#include "tre/TreTypes.h"

#include <gtest/gtest.h>

#include <QImage>
#include <QPainter>

#include <cmath>
#include <filesystem>
#include <limits>
#include <string>

namespace {

using pdfe::tre::BitmapCompositor;
using pdfe::tre::EnrichedAtlas;
using pdfe::tre::FontMatch;
using pdfe::tre::GlyphAtlas;
using pdfe::tre::RgbColor;

/// #268's own metric: mean absolute edge energy over the alpha channel.
///
/// The Python render oracle (tests/ui_automation/test_render_images.py::
/// _sharpness) measures this on a grayscale screenshot. Here the glyph is an
/// ARGB image with the ink in alpha, so alpha is the right channel — using
/// luminance would read the transparent background as uniform and dilute the
/// very edges being measured.
double edgeEnergy(const QImage& img)
{
    if (img.isNull() || img.width() < 2 || img.height() < 2)
        return 0.0;
    const QImage a = img.convertToFormat(QImage::Format_ARGB32);
    long long total = 0;
    long long n = 0;
    for (int y = 0; y < a.height() - 1; ++y) {
        const QRgb* row = reinterpret_cast<const QRgb*>(a.constScanLine(y));
        const QRgb* next = reinterpret_cast<const QRgb*>(a.constScanLine(y + 1));
        for (int x = 0; x < a.width() - 1; ++x) {
            const int c = qAlpha(row[x]);
            total += std::abs(c - qAlpha(row[x + 1]));
            total += std::abs(c - qAlpha(next[x]));
            n += 2;
        }
    }
    return n ? static_cast<double>(total) / static_cast<double>(n) : 0.0;
}

std::string systemFont()
{
    // Same fonts other tests in this suite use. Checked rather than assumed so
    // a missing font is a clear skip rather than a null-image mystery.
    for (const char* p : {"C:/Windows/Fonts/arial.ttf",
                          "C:/Windows/Fonts/calibri.ttf",
                          "C:/Windows/Fonts/cour.ttf"}) {
        if (std::filesystem::exists(p))
            return p;
    }
    return {};
}

/// A 1-glyph atlas, enough to give EnrichedAtlas a cap height and a reference
/// line height so the fallback's crop-scale-position path is exercised rather
/// than its degraded branch.
GlyphAtlas atlasWithAnH(int lineH)
{
    QImage h(lineH / 2, lineH, QImage::Format_ARGB32_Premultiplied);
    h.fill(Qt::transparent);
    {
        QPainter p(&h);
        p.setPen(Qt::NoPen);
        p.setBrush(Qt::black);
        // Crude 'H': two stems and a bar, ink from 20% to 80% of the height so
        // there is a measurable cap height.
        const int top = lineH / 5, bot = (lineH * 4) / 5;
        p.drawRect(0, top, 2, bot - top);
        p.drawRect(h.width() - 2, top, 2, bot - top);
        p.drawRect(0, (top + bot) / 2, h.width(), 2);
    }
    GlyphAtlas a;
    a.addGlyph(U'H', h, /*confidence=*/100);
    return a;
}

struct FallbackFixture {
    BitmapCompositor compositor;
    EnrichedAtlas atlas;
    bool ready = false;

    explicit FallbackFixture(int lineH = 40)
    {
        const std::string font = systemFont();
        if (font.empty())
            return;
        atlas.build(atlasWithAnH(lineH), U"");
        FontMatch fm;
        fm.fontFilePath = font;
        fm.sizePt = 12.0f;
        fm.color = RgbColor{0, 0, 0};
        // 300 is the scan DPI in the defect: the page-wide render's DPI.
        atlas.setFreeTypeFallback(&compositor, fm, 300);
        ready = true;
    }
};

// A codepoint the 1-glyph atlas cannot supply, so glyph() must reach level 4.
constexpr char32_t kFallbackChar = U'w';

}  // namespace

TEST(FreeTypeFallbackOutputScale, DefaultsToOneAndReportsTheScanDpi)
{
    FallbackFixture f;
    if (!f.ready) GTEST_SKIP() << "no system TrueType font available";

    EXPECT_DOUBLE_EQ(f.atlas.freeTypeOutputScale(), 1.0)
        << "the default must reproduce the pre-#268 behaviour exactly";
    EXPECT_EQ(f.atlas.effectiveFtDpi(), 300)
        << "at scale 1.0 the rasterisation DPI is the configured scan DPI";
}

TEST(FreeTypeFallbackOutputScale, ScaleMovesTheRasterisationDpi)
{
    FallbackFixture f;
    if (!f.ready) GTEST_SKIP() << "no system TrueType font available";

    f.atlas.setFreeTypeOutputScale(0.5);
    EXPECT_EQ(f.atlas.effectiveFtDpi(), 150)
        << "compositing a 300-DPI atlas at 150 must rasterise at 150, which is "
           "the whole point of #268 -- a scale that does not reach the "
           "rasteriser would leave the resample in place";

    f.atlas.setFreeTypeOutputScale(2.0);
    EXPECT_EQ(f.atlas.effectiveFtDpi(), 600);

    f.atlas.setFreeTypeOutputScale(1.0);
    EXPECT_EQ(f.atlas.effectiveFtDpi(), 300) << "and it must come back";
}

TEST(FreeTypeFallbackOutputScale, RejectsNonsenseInsteadOfRenderingIt)
{
    FallbackFixture f;
    if (!f.ready) GTEST_SKIP() << "no system TrueType font available";

    for (double bad : {0.0, -1.0, std::nan(""), std::numeric_limits<double>::infinity()}) {
        f.atlas.setFreeTypeOutputScale(1.0);
        f.atlas.setFreeTypeOutputScale(bad);
        EXPECT_DOUBLE_EQ(f.atlas.freeTypeOutputScale(), 1.0)
            << "a non-finite or non-positive scale must fall back to 1.0 rather "
               "than produce a 0-pixel or NaN-sized glyph";
    }
    f.atlas.setFreeTypeOutputScale(1000.0);
    EXPECT_LE(f.atlas.freeTypeOutputScale(), 10.0) << "clamped, not honoured";

    // The LOWER bound. phase-9's claim-check noted that nothing exercised it,
    // so a change of 0.1 to 0.01 would have gone undetected: 0.001 is a
    // positive, finite number, so it survives the nonsense check above and
    // reaches the clamp. Without the clamp it would ask the rasteriser for a
    // glyph a thousandth of its size.
    f.atlas.setFreeTypeOutputScale(0.001);
    EXPECT_GE(f.atlas.freeTypeOutputScale(), 0.1)
        << "a positive-but-absurd scale must be clamped UP to the lower bound, "
           "not passed through to the rasteriser";
    EXPECT_LE(f.atlas.freeTypeOutputScale(), 0.1)
        << "...and clamped to exactly the bound, not some other value";
}

TEST(FreeTypeFallbackOutputScale, GlyphReportsTheFreeTypeSourceSoCallersCanSkipRescaling)
{
    FallbackFixture f;
    if (!f.ready) GTEST_SKIP() << "no system TrueType font available";

    EnrichedAtlas::GlyphSource src = EnrichedAtlas::GlyphSource::None;
    const QImage fallback = f.atlas.glyph(kFallbackChar, &src);
    ASSERT_FALSE(fallback.isNull())
        << "the fallback did not produce a glyph; the rest of this file is "
           "vacuous without it";
    EXPECT_EQ(src, EnrichedAtlas::GlyphSource::FreeType)
        << "GlyphAtlasCompositor decides whether to rescale from this value; if "
           "a FreeType glyph reports anything else it gets scaled twice";

    EnrichedAtlas::GlyphSource atlasSrc = EnrichedAtlas::GlyphSource::None;
    const QImage fromAtlas = f.atlas.glyph(U'H', &atlasSrc);
    ASSERT_FALSE(fromAtlas.isNull());
    EXPECT_EQ(atlasSrc, EnrichedAtlas::GlyphSource::Atlas)
        << "an atlas crop must NOT claim to be pre-scaled -- it still needs the "
           "compositor's rescale";
}

// WHAT THIS TEST DOES NOT ASSERT, AND WHY.
//
// It was written to assert that rasterising at the target size is SHARPER than
// resampling to it — #268's claim — using #268's own edge-energy metric. It
// measured the opposite, repeatably: at a 0.5 target, rasterised E=15.83 against
// resampled E=33.03 on identically-sized 12x20 images.
//
// The metric is the problem, and #268 says so itself: "a fixed-crop edge-energy
// ratio cannot be used as the acceptance bar here". Downsampling a 24x40 alpha
// mask to 12x20 halves a 2-pixel stem to one pixel at near-full opacity, so
// adjacent-pixel differences get LARGER — that is aliasing, read by this metric
// as sharpness. FreeType rendering the same glyph at 150 DPI spreads each stroke
// edge across partial-coverage pixels, which is more faithful and scores LOWER.
//
// The sign of the metric therefore depends on the operation: downsampling a
// composited PAGE REGION blurs it (which is what #268 measured end-to-end,
// 13.92 -> 11.27, lower being worse), while downsampling a GLYPH's alpha mask
// sharpens it into aliasing. A unit-level edge-energy comparison cannot
// adjudicate the page-level claim, and forcing it to agree would be bending the
// oracle to fit the change.
//
// So this test asserts the thing that IS true and checkable at this level: both
// paths land on the same output geometry, which is what proves the scale
// actually reached the rasteriser. The edge-energy numbers are printed as data,
// not asserted.
//
// Finding this is what caught a real bug in the change itself: before
// ensureFreeTypeMetrics() was switched to effectiveFtDpi(), the reference 'H'
// was measured at 300 DPI while the glyph rendered at 150, so the cap-match
// ratio was applied twice and the glyph arrived 6 px wide where 12 was correct.
// The dimension assertion below is the one that would have caught it.
TEST(FreeTypeFallbackOutputScale, RasterisingAtTargetLandsOnTheSameGeometryAsResamplingToIt)
{
    FallbackFixture f;
    if (!f.ready) GTEST_SKIP() << "no system TrueType font available";

    // OLD behaviour: rasterise in atlas space, then let the caller resample to
    // the composite size. This is exactly what GlyphAtlasCompositor's
    // toCompositeScale() did to every glyph before #268.
    f.atlas.setFreeTypeOutputScale(1.0);
    const QImage atlasSpace = f.atlas.glyph(kFallbackChar);
    ASSERT_FALSE(atlasSpace.isNull());
    const QImage resampled = atlasSpace.scaled(
        std::max(1, static_cast<int>(std::lround(atlasSpace.width() * 0.5))),
        std::max(1, static_cast<int>(std::lround(atlasSpace.height() * 0.5))),
        Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

    // NEW behaviour: rasterise straight at the composite scale.
    f.atlas.setFreeTypeOutputScale(0.5);
    const QImage rasterised = f.atlas.glyph(kFallbackChar);
    ASSERT_FALSE(rasterised.isNull());

    // THE assertion of this test. Both dimensions, not just height: the bug
    // this change shipped with was a correct height and a half-width glyph.
    EXPECT_NEAR(rasterised.height(), resampled.height(), 2)
        << "rasterising at the target must land on the same height as resampling "
           "to it; rasterised=" << rasterised.height()
        << " resampled=" << resampled.height();
    EXPECT_NEAR(rasterised.width(), resampled.width(), 2)
        << "...and the same WIDTH. A correct height with a half-width glyph is "
           "exactly the stale-metrics bug this change was found to have: the "
           "reference 'H' measured at the unscaled DPI makes the cap-match "
           "ratio apply twice. rasterised=" << rasterised.width()
        << " resampled=" << resampled.width();

    const double sharpRaster = edgeEnergy(rasterised);
    const double sharpResample = edgeEnergy(resampled);

    // Diagnostics first: an edge-energy comparison between images of different
    // ink extent is not a sharpness comparison at all, and #268 itself warns
    // that a fixed-crop edge-energy ratio cannot be an acceptance bar.
    std::fprintf(stderr,
                 "[i268] atlasSpace %dx%d  resampled %dx%d (E=%.3f)  "
                 "rasterised %dx%d (E=%.3f)\n",
                 atlasSpace.width(), atlasSpace.height(),
                 resampled.width(), resampled.height(), sharpResample,
                 rasterised.width(), rasterised.height(), sharpRaster);

    // Both must actually have edges. A glyph measuring 0 would make the printed
    // numbers above meaningless, and that is the one thing worth asserting
    // about them here.
    EXPECT_GT(sharpRaster, 0.0)
        << "the rasterised glyph has no alpha edges at all -- it is blank, and "
           "every other number in this test is then vacuous";
    EXPECT_GT(sharpResample, 0.0)
        << "the resampled glyph has no alpha edges at all";

    // DELIBERATELY NOT ASSERTED: sharpRaster > sharpResample. See the comment
    // above this test. The measurement goes the other way and the metric cannot
    // adjudicate the page-level claim; #268's own text says a fixed-crop
    // edge-energy ratio is not a valid acceptance bar.
}

TEST(FreeTypeFallbackOutputScale, TheCacheHoldsBothScalesAtOnce)
{
    FallbackFixture f;
    if (!f.ready) GTEST_SKIP() << "no system TrueType font available";

    // A mixed-DPI page asks for the same character at two target scales while
    // alternating between regions. #266 could not fix #268 precisely because
    // one normalised image per codepoint cannot hold both, and clearing the
    // cache on every switch would re-render every glyph twice per alternation.
    f.atlas.setFreeTypeOutputScale(1.0);
    const QImage big = f.atlas.glyph(kFallbackChar);
    ASSERT_FALSE(big.isNull());

    f.atlas.setFreeTypeOutputScale(0.5);
    const QImage small = f.atlas.glyph(kFallbackChar);
    ASSERT_FALSE(small.isNull());

    EXPECT_LT(small.height(), big.height())
        << "the two scales must produce genuinely different images; equal "
           "heights would mean the scale never reached the renderer";

    // RESIDENCY, not just correctness. phase-9's claim-check was right that
    // comparing heights cannot tell a cache HIT from a deterministic
    // RE-RENDER: both give the same pixels. The entry count can.
    EXPECT_EQ(f.atlas.freeTypeCacheEntryCount(), 2u)
        << "two scales of one codepoint must occupy TWO cache entries; one "
           "would mean the second render evicted the first, which is the "
           "thrash the (codepoint, scale) key exists to prevent";

    // Going back must return the ORIGINAL image, not a re-render, and
    // critically not the small one.
    f.atlas.setFreeTypeOutputScale(1.0);
    const QImage bigAgain = f.atlas.glyph(kFallbackChar);
    ASSERT_FALSE(bigAgain.isNull());
    EXPECT_EQ(bigAgain.height(), big.height())
        << "switching scale and back must not have evicted the first scale's "
           "glyph -- that is the thrash the (codepoint, scale) key exists to "
           "prevent on a mixed-DPI page";
    EXPECT_EQ(f.atlas.freeTypeCacheEntryCount(), 2u)
        << "and switching back must have been a cache HIT: a third entry would "
           "mean it re-rendered, which is the very thing the height comparison "
           "above cannot distinguish";

    // And size() must count the character once, not once per cached scale.
    const int distinct = f.atlas.size();
    f.atlas.setFreeTypeOutputScale(0.5);
    (void)f.atlas.glyph(kFallbackChar);
    EXPECT_EQ(f.atlas.size(), distinct)
        << "size() reports glyph COVERAGE; caching one character at a second "
           "scale must not inflate it";
}
