// GitHub #302 — the FreeType glyph fallback had no end-to-end coverage, so a
// 4x error in its rasterisation DPI was invisible in the saved document.
//
// THE MEASURED BLIND SPOT (2026-10-13, docs/audit/p96-phase9/
// 268-fallback-dpi-verification.md). Editing the 150-DPI thumbnail region of
// `mixed_dpi_page.pdf` through the live UI and comparing the written image
// XObject pixel for pixel between builds:
//
//   fallback at composite DPI  vs  at the scan DPI (the #268 defect)  -> 0.0000%
//   fallback at composite DPI  vs  at FOUR TIMES the scan DPI         -> 0.0000%
//
// with a control proving the recomposite really did write into the image being
// compared (fixture vs saved document: 5.4156%). So the fallback could be
// arbitrarily wrong and every suite stayed green. `#268`'s own fix was
// therefore unobservable, which is the same blind spot that let `#266`'s fix
// leave `#268` behind.
//
// WHY IT WAS INVISIBLE — now measured rather than guessed. #302 lists two
// unmeasured candidates, and it is the first: `EnrichedAtlas::glyph()` resolves
// atlas -> pre-synth -> on-demand synth -> FreeType, so the fallback is only
// reached for a codepoint the scan atlas cannot supply AND synthesis cannot
// build. Every glyph of `THUMBNAIL` -> `BOOKMARKS` came from the scan, so the
// FreeType branch was never taken by that edit. The measurement was sound; the
// scenario simply never exercised the path.
//
// There is a second, independent reason the end-to-end numbers could not move,
// and it matters for anyone reading `#268`: `GlyphAtlasCompositor` computes
// `glyphScale = compositeDpi / atlasDpi` and calls `setFreeTypeOutputScale()`
// with it. That ratio is exactly 1.0 on every single-DPI page. So the fallback's
// output scale is only observable at all on a MIXED-DPI composite. A test needs
// both conditions at once — a mixed-DPI composite AND a codepoint that reaches
// the fallback — and the original scenario had only the first.
//
// WHAT THIS FILE ADDS. `tests/test_freetype_fallback_output_scale.cpp` pins the
// mechanism in memory: that `setFreeTypeOutputScale()` moves the rasterisation
// DPI. It cannot show the mechanism reaches a page. These cases carry a
// fallback-sourced glyph all the way through compose -> embed -> SAVE -> reopen
// -> render, and assert against the pixels of the reopened document.

#include <gtest/gtest.h>

#include <QImage>
#include <QPainter>
#include <QRectF>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "image/ImageData.h"
#include "image/ImageManager.h"
#include "tre/BitmapCompositor.h"
#include "tre/EnrichedAtlas.h"
#include "tre/GlyphAtlas.h"
#include "tre/GlyphAtlasCompositor.h"
#include "tre/FontMatcher.h"

#include "helpers/RoundTripHelper.h"

#include <algorithm>
#include <iostream>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

using pdfe::image::ImageManager;
using pdfe::image::InsertImageOptions;
using pdfe::tre::BitmapCompositor;
using pdfe::tre::EnrichedAtlas;
using pdfe::tre::FontMatch;
using pdfe::tre::GlyphAtlas;
using pdfe::tre::GlyphAtlasCompositor;
using pdfe::tre::RgbColor;

namespace {

/// The scan DPI the atlas is harvested at, and the DPI the region composites
/// at. They MUST differ: `glyphScale` is their ratio and is 1.0 on every
/// single-DPI page, which would make the fallback's output scale unobservable
/// and this whole test vacuous. 150 -> 300 is the fixture's own mixed-DPI
/// shape (a 150-DPI thumbnail over a 300-DPI photo).
constexpr int kAtlasDpi = 150;
constexpr int kCompositeDpi = 300;

/// A codepoint the one-glyph atlas cannot supply and synthesis cannot build
/// from an 'H', so `glyph()` must reach level 4 — the FreeType fallback.
/// Asserted, not assumed: see the GlyphSource check in every case below.
constexpr char32_t kFallbackChar = U'w';

std::string systemFont()
{
    for (const char* p : {"C:/Windows/Fonts/arial.ttf",
                          "C:/Windows/Fonts/times.ttf",
                          "C:/Windows/Fonts/calibri.ttf"}) {
        if (std::filesystem::exists(p)) return p;
    }
    return {};
}

/// A one-glyph atlas: enough to give EnrichedAtlas a cap height and reference
/// line height, so the fallback runs its crop-scale-position path rather than
/// a degraded branch. Same shape as the fixture in
/// `test_freetype_fallback_output_scale.cpp` — a fixture, not a decision, so a
/// local copy costs nothing to keep correct.
GlyphAtlas atlasWithAnH(int lineH)
{
    QImage h(lineH / 2, lineH, QImage::Format_ARGB32_Premultiplied);
    h.fill(Qt::transparent);
    {
        QPainter p(&h);
        p.setPen(Qt::NoPen);
        p.setBrush(Qt::black);
        const int top = lineH / 5, bot = (lineH * 4) / 5;
        p.drawRect(0, top, 2, bot - top);
        p.drawRect(h.width() - 2, top, 2, bot - top);
        p.drawRect(0, (top + bot) / 2, h.width(), 2);
    }
    GlyphAtlas a;
    a.addGlyph(U'H', h, /*confidence=*/100);
    return a;
}

/// The vertical extent of ink, in pixels. The measurable that a wrong
/// rasterisation DPI moves: a glyph normalised to 4x its correct scale is
/// about four times as tall.
///
/// Returns -1 for a blank image, which callers must treat as "could not
/// measure" and FAIL on rather than compare against 0 — a blank composite
/// would otherwise read as a very small glyph and pass a bounds check.
/// Percent of pixels that differ between two same-size renders.
///
/// THE MEASURABLE #302 ITSELF USED. Its measurement was "differing pixels in
/// the written region", and the finding was 0.0000% for a 4x error. Comparing
/// the same quantity keeps this test commensurable with the issue instead of
/// inventing a different metric whose numbers cannot be set against the
/// recorded ones.
///
/// Returns -1.0 when the two cannot be compared at all (null or mismatched
/// geometry), which a caller must FAIL on rather than read as 0% -- "they did
/// not differ" and "we could not tell" are different findings, and collapsing
/// them is how a 0.0000% result gets mistaken for a clean bill of health.
double percentDiffering(const QImage& a, const QImage& b)
{
    if (a.isNull() || b.isNull()) return -1.0;
    if (a.size() != b.size()) return -1.0;
    const QImage pa = a.convertToFormat(QImage::Format_ARGB32);
    const QImage pb = b.convertToFormat(QImage::Format_ARGB32);
    long long differing = 0;
    int dx0 = 1 << 30, dy0 = 1 << 30, dx1 = -1, dy1 = -1;
    const long long total =
        static_cast<long long>(pa.width()) * pa.height();
    if (total <= 0) return -1.0;
    for (int y = 0; y < pa.height(); ++y) {
        const QRgb* ra = reinterpret_cast<const QRgb*>(pa.constScanLine(y));
        const QRgb* rb = reinterpret_cast<const QRgb*>(pb.constScanLine(y));
        for (int x = 0; x < pa.width(); ++x) {
            // A tolerance of 8 per channel: JPEG-ish recompression and
            // resampling move low bits, and a test that reddens on those is a
            // test nobody keeps.
            // ALPHA IS PART OF THE COMPARISON, and leaving it out was a real
            // defect in this oracle. `compose()` returns a TRANSPARENT ARGB
            // bitmap: ink is black (0,0,0) over transparent black (0,0,0,0),
            // so two visibly different composites can be byte-identical in
            // R, G and B and differ only in alpha. The RGB-only version
            // reported two obviously different composites as 0% different
            // while `inkHeight` measured 47px vs 50px -- a contradiction that
            // is what exposed it. An oracle blind to the channel the signal
            // lives in is the exact shape of the #302 blind spot it was
            // written to close.
            if (std::abs(qAlpha(ra[x]) - qAlpha(rb[x])) > 8
                || std::abs(qRed(ra[x]) - qRed(rb[x])) > 8
                || std::abs(qGreen(ra[x]) - qGreen(rb[x])) > 8
                || std::abs(qBlue(ra[x]) - qBlue(rb[x])) > 8) {
                ++differing;
                dx0 = std::min(dx0, x); dx1 = std::max(dx1, x);
                dy0 = std::min(dy0, y); dy1 = std::max(dy1, y);
            }
        }
    }
    // Report the raw counts too. Two independent comparisons in this file once
    // printed byte-identical percentages, and a percentage alone cannot tell
    // "the same region differed" from "a coincidence of ratios".
    std::cout << "[i302]   diff detail: " << differing << " of " << total
              << " px (" << pa.width() << "x" << pa.height() << ")"
              << " bbox=[" << dx0 << "," << dy0 << " .. " << dx1 << ","
              << dy1 << "]" << std::endl;
    return 100.0 * static_cast<double>(differing) / static_cast<double>(total);
}

int inkHeight(const QImage& img)
{
    if (img.isNull()) return -1;
    const QImage p = img.convertToFormat(QImage::Format_ARGB32);
    int top = -1, bot = -1;
    for (int y = 0; y < p.height(); ++y) {
        const QRgb* row = reinterpret_cast<const QRgb*>(p.constScanLine(y));
        bool ink = false;
        for (int x = 0; x < p.width(); ++x) {
            // Ink is either opaque-ish or dark: the composite is drawn on a
            // background colour, so alpha alone is not the signal after the
            // image has been through a PDF round trip.
            const QRgb c = row[x];
            if (qAlpha(c) > 40 && qGray(c) < 200) { ink = true; break; }
        }
        if (ink) {
            if (top < 0) top = y;
            bot = y;
        }
    }
    if (top < 0) return -1;
    return bot - top + 1;
}

struct Harness {
    BitmapCompositor compositor;
    EnrichedAtlas atlas;
    FontMatch fm;
    bool ready = false;

    explicit Harness(int lineH = 40)
    {
        const std::string font = systemFont();
        if (font.empty()) return;
        atlas.build(atlasWithAnH(lineH), U"");
        fm.fontFilePath = font;
        fm.sizePt = 12.0f;
        fm.color = RgbColor{0, 0, 0};
        // TWO DIFFERENT KNOBS, and the test's first run proved it. The `dpi`
        // argument to setFreeTypeFallback is the DPI the FALLBACK RENDERS at;
        // the atlas-harvest DPI the compositor divides by lives behind
        // setSourceDpi(). Setting only the first left sourceDpi() == 0, and
        // GlyphAtlasCompositor treats 0 as "assume the DPIs match" -> a
        // glyphScale of exactly 1.0 -> the fallback's output scale
        // unobservable. That is the same shape as #302's own blind spot, found
        // here by asserting the precondition instead of assuming it.
        atlas.setFreeTypeFallback(&compositor, fm, kCompositeDpi);
        atlas.setSourceDpi(kAtlasDpi);
        ready = true;
    }

    /// Compose the fallback glyph through the REAL compositor.
    ///
    /// `GlyphAtlasCompositor::compose()` computes
    /// `glyphScale = compositeDpi / atlasDpi` and calls
    /// `setFreeTypeOutputScale(glyphScale)` itself, so this is the production
    /// path end to end — no scale is injected and none can be.
    QImage composeViaCompositor() const
    {
        GlyphAtlasCompositor comp;
        const std::u32string text(1, kFallbackChar);
        return comp.compose(text, atlas, fm, RgbColor{255, 255, 255},
                            /*targetWidth=*/160, /*targetHeight=*/80,
                            kCompositeDpi,
                            const_cast<BitmapCompositor*>(&compositor));
    }

    /// The fallback glyph rasterised at a CHOSEN output scale, drawn onto an
    /// opaque tile ready for embedding.
    ///
    /// Why not inject the defect into `compose()`: that function sets the
    /// output scale itself from the two DPIs, so a scale set beforehand is
    /// overwritten. Reproducing the 4x defect therefore means driving
    /// `setFreeTypeOutputScale` + `glyph()` directly — the same two public
    /// calls the compositor makes, in the same order, with the only difference
    /// being the number. That keeps the mutation to exactly one variable and
    /// requires no source edit, so nothing can be left behind in the tree.
    QImage glyphTileAtScale(double outputScale,
                            EnrichedAtlas::GlyphSource* src) const
    {
        atlas.setFreeTypeOutputScale(outputScale);
        const QImage g = atlas.glyph(kFallbackChar, src);
        if (g.isNull()) return {};
        // TIGHT to the glyph, on purpose. A generous fixed tile (the first
        // draft used 400x400) is mostly white, and the embed scales the whole
        // tile into a fixed target rect -- so the glyph's pixels become a
        // vanishing fraction of what is compared. Measured consequence: a
        // genuine, large in-memory difference (34px vs 9px of ink) showed up as
        // 0.0556% of differing pixels in the saved render against a 0.0000%
        // control. The signal was real and the oracle was measuring margin.
        // That is the `oracle_measured_the_grey_margin` mistake, and this is
        // the fix: the tile IS the glyph, so the comparison is dominated by
        // glyph pixels.
        QImage tile(g.width(), g.height(), QImage::Format_ARGB32);
        tile.fill(Qt::white);
        {
            QPainter p(&tile);
            p.drawImage(0, 0, g);
        }
        return tile;
    }
};

/// Embed a bitmap, SAVE, reopen, and render the page back.
///
/// This is the whole point of the file: #302's gap is that nothing observed a
/// fallback glyph in a WRITTEN document. Returning the reopened render means
/// every assertion below is made against bytes that survived a save.
QImage saveAndRenderBack(const QImage& bitmap,
                         const std::filesystem::path& tempDir,
                         const std::filesystem::path& fixture,
                         const std::shared_ptr<pdfe::core::MuPdfContext>& ctx)
{
    const auto png = tempDir / "composed.png";
    if (!bitmap.save(QString::fromStdString(png.string()), "PNG")) return {};

    auto doc = pdfe::core::Document::open(ctx, fixture.string());
    if (!doc) return {};
    auto page = doc->page(0);
    if (!page) return {};

    ImageManager mgr(ctx);
    InsertImageOptions opts;
    opts.filePath = png.string();
    opts.targetRect = QRectF(56, 56, 500, 500);
    opts.maintainAspectRatio = false;
    const auto res = mgr.insertImage(page.get(), doc.get(), opts);
    if (!res.success) return {};
    page.reset();

    auto reopened = pdfe::test::SaveAndReopen(*doc, ctx, tempDir);
    if (!reopened) return {};
    auto rePage = reopened->page(0);
    if (!rePage) return {};
    // Render at 2x so the glyph's ink extent is measured on enough pixels for
    // a 4x error to be unmistakable rather than a rounding argument.
    return rePage->render(QRectF(56, 56, 500, 500), 2.0f);
}

class FreeTypeFallbackSavedDocTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ctx_ = std::make_shared<pdfe::core::MuPdfContext>();
        ASSERT_NE(ctx_, nullptr);
        fixture_ = std::filesystem::path(TEST_FIXTURES_DIR) / "test_pdf_with_images.pdf";
        ASSERT_TRUE(std::filesystem::exists(fixture_)) << fixture_.string();
        tempDir_ = std::filesystem::temp_directory_path()
                   / "pdfe_i302_fallback_saved";
        std::filesystem::remove_all(tempDir_);
        std::filesystem::create_directories(tempDir_);
    }

    void TearDown() override
    {
        ctx_.reset();
        std::error_code ec;
        std::filesystem::remove_all(tempDir_, ec);
    }

    std::shared_ptr<pdfe::core::MuPdfContext> ctx_;
    std::filesystem::path fixture_;
    std::filesystem::path tempDir_;
};

}  // namespace

// ---------------------------------------------------------- criterion 3

TEST_F(FreeTypeFallbackSavedDocTest, TheFixtureReallyReachesTheFreeTypeFallback)
{
    // #302 criterion 3, and the first thing that must be true: a test that
    // silently never takes the fallback would pass while measuring nothing,
    // which is precisely how the original 0.0000% result arose. So the SOURCE
    // is asserted, not assumed.
    Harness h;
    if (!h.ready) GTEST_SKIP() << "no system TrueType font available";

    EnrichedAtlas::GlyphSource src = EnrichedAtlas::GlyphSource::None;
    const QImage g = h.atlas.glyph(kFallbackChar, &src);
    ASSERT_FALSE(g.isNull()) << "the fallback produced no glyph at all";
    EXPECT_EQ(src, EnrichedAtlas::GlyphSource::FreeType)
        << "the codepoint under test did NOT reach the FreeType fallback, so "
           "every assertion in this file would be measuring the atlas or "
           "synthesis instead. That is GitHub #302's own failure mode: the "
           "original end-to-end measurement reported 0.0000% because every "
           "glyph of its edit came from the scan.";

    // ...and the control: a codepoint the atlas DOES hold must not report
    // FreeType, or the out-param is simply always FreeType and proves nothing.
    EnrichedAtlas::GlyphSource atlasSrc = EnrichedAtlas::GlyphSource::None;
    const QImage hg = h.atlas.glyph(U'H', &atlasSrc);
    ASSERT_FALSE(hg.isNull());
    EXPECT_EQ(atlasSrc, EnrichedAtlas::GlyphSource::Atlas)
        << "an atlas-held codepoint reported a non-Atlas source, so the "
           "GlyphSource seam cannot distinguish the paths";
}

TEST_F(FreeTypeFallbackSavedDocTest, TheCompositeIsMixedDpiSoTheOutputScaleIsObservable)
{
    // The second condition, and the one #302 could not have known it was
    // missing. `glyphScale = compositeDpi / atlasDpi` is 1.0 on every
    // single-DPI page, and `setFreeTypeOutputScale(1.0)` moves nothing. A
    // fallback-reaching test on a single-DPI page would still measure zero.
    Harness h;
    if (!h.ready) GTEST_SKIP() << "no system TrueType font available";
    ASSERT_NE(kAtlasDpi, kCompositeDpi)
        << "atlas and composite DPI are equal, so glyphScale is 1.0 and the "
           "fallback's output scale cannot affect anything. This test would "
           "be vacuous.";
    EXPECT_EQ(h.atlas.sourceDpi(), kAtlasDpi)
        << "the atlas does not report the scan DPI it was given, so the "
           "compositor cannot compute a glyph scale and the mixed-DPI "
           "condition is not actually established";
}

// ---------------------------------------------------------- criterion 1

TEST_F(FreeTypeFallbackSavedDocTest, AFallbackGlyphSurvivesTheSaveAndIsVisibleInTheReopenedPage)
{
    // #302 criterion 1. The gap was that nothing observed a FreeType-fallback
    // glyph in a WRITTEN document -- only in memory. This drives the real
    // compositor, embeds its output, SAVES, reopens, and renders the region
    // back, so the assertion is made against bytes that survived a save.
    Harness h;
    if (!h.ready) GTEST_SKIP() << "no system TrueType font available";

    EnrichedAtlas::GlyphSource src = EnrichedAtlas::GlyphSource::None;
    ASSERT_FALSE(h.atlas.glyph(kFallbackChar, &src).isNull());
    ASSERT_EQ(src, EnrichedAtlas::GlyphSource::FreeType)
        << "precondition: the glyph under test must come from the fallback";

    const QImage composed = h.composeViaCompositor();
    ASSERT_FALSE(composed.isNull())
        << "the compositor produced no bitmap, so nothing could be saved";
    ASSERT_GT(inkHeight(composed), 0)
        << "the composed bitmap has no ink at all -- a blank composite would "
           "pass a pixel-difference test for the wrong reason, which is the "
           "vacuity #302's 0.0000% result could not distinguish";

    const QImage back = saveAndRenderBack(composed, tempDir_, fixture_, ctx_);
    ASSERT_FALSE(back.isNull())
        << "embed -> save -> reopen -> render produced nothing; the end-to-end "
           "path is not exercised and no claim about saved output can be made";
    EXPECT_GT(inkHeight(back), 0)
        << "the fallback glyph is NOT present in the reopened document. "
           "GitHub #302: the fallback can be arbitrarily wrong and every suite "
           "stays green precisely because nothing looked here.";
}

// ---------------------------------------------------------- criterion 2

TEST_F(FreeTypeFallbackSavedDocTest, A4xRasterisationDpiErrorIsVISIBLEInTheSavedDocument)
{
    // #302 criterion 2, and the whole point of the phase. Measured 2026-10-13:
    // a 4x error in the fallback's rasterisation DPI produced **0.0000%**
    // differing pixels in the written document. This asserts the same quantity
    // and requires it to be non-zero, with the defect injected through the
    // public scale seam rather than by editing the compositor -- so nothing can
    // be left behind in the tree and the mutation is repeatable in CI.
    Harness h;
    if (!h.ready) GTEST_SKIP() << "no system TrueType font available";

    const double correct = static_cast<double>(kCompositeDpi) / kAtlasDpi;

    EnrichedAtlas::GlyphSource srcOk = EnrichedAtlas::GlyphSource::None;
    const QImage tileOk = h.glyphTileAtScale(correct, &srcOk);
    ASSERT_FALSE(tileOk.isNull());
    ASSERT_EQ(srcOk, EnrichedAtlas::GlyphSource::FreeType)
        << "the correct-scale glyph did not come from the fallback, so this "
           "measures something other than the fallback path";

    EnrichedAtlas::GlyphSource srcBad = EnrichedAtlas::GlyphSource::None;
    const QImage tileBad = h.glyphTileAtScale(correct * 4.0, &srcBad);
    ASSERT_FALSE(tileBad.isNull());
    ASSERT_EQ(srcBad, EnrichedAtlas::GlyphSource::FreeType);

    // PRECONDITION, in memory. If the 4x scale did not actually produce a
    // bigger rasterisation, nothing downstream could possibly differ and the
    // saved-document assertion would fail for a reason that has nothing to do
    // with saving. This separates "the defect was not injected" from "the
    // defect was injected and is invisible" -- #302 could not tell those apart.
    const int inkOk = inkHeight(tileOk);
    const int inkBad = inkHeight(tileBad);
    ASSERT_GT(inkOk, 0) << "no ink in the correct-scale glyph tile";
    ASSERT_GT(inkBad, 0) << "no ink in the 4x glyph tile";
    // TWO-SIDED, and measured rather than assumed. The first draft of this
    // assertion demanded the 4x glyph be TALLER and failed: 34px -> 9px, i.e.
    // SMALLER by 3.8x. That is correct behaviour, not a defect.
    // `effectiveFtDpi = baseDpi * scale`, so a 4x scale rasterises at 4x the
    // DPI and the glyph is then normalised against the atlas cap height -- a
    // higher-DPI render has a larger ftCapH, so atlasCapH/ftCapH crops it to a
    // smaller fraction of the canvas. #268's point was sharpness at the output
    // grid, never size. The claim worth defending is that a 4x error CHANGES
    // the rasterisation, not which way it moves.
    const double hi = std::max(inkOk, inkBad);
    const double lo = std::max(1, std::min(inkOk, inkBad));
    ASSERT_GT(hi / lo, 2.0)
        << "the 4x output scale did NOT materially change the rasterisation in "
           "memory (" << inkOk << "px vs " << inkBad << "px), so the defect "
           "was never injected and the saved-document comparison below would "
           "be meaningless";

    // ...and now the thing #302 says nothing observes: the SAVED document.
    const QImage backOk = saveAndRenderBack(tileOk, tempDir_, fixture_, ctx_);
    const QImage backBad = saveAndRenderBack(tileBad, tempDir_, fixture_, ctx_);
    ASSERT_FALSE(backOk.isNull()) << "correct-scale round trip produced nothing";
    ASSERT_FALSE(backBad.isNull()) << "4x round trip produced nothing";

    const double diff = percentDiffering(backOk, backBad);
    ASSERT_GE(diff, 0.0)
        << "the two saved renders could not be compared at all (sizes "
        << backOk.width() << "x" << backOk.height() << " vs "
        << backBad.width() << "x" << backBad.height()
        << "). That is 'we could not find out', not 'they match'.";

    // CONTROL FIRST, so the assertion that follows cannot pass on round-trip
    // noise. The SAME tile through the SAME path twice must be essentially
    // identical; if this floor is breached, every number here is noise and the
    // comparison proves nothing -- which is exactly the vacuity trap #302's
    // own 0.0000% result sat next to.
    const QImage backOk2 = saveAndRenderBack(tileOk, tempDir_, fixture_, ctx_);
    ASSERT_FALSE(backOk2.isNull());
    const double noise = percentDiffering(backOk, backOk2);
    ASSERT_GE(noise, 0.0);
    EXPECT_LT(noise, 0.5)
        << "the same bitmap through the same embed/save/render path differed by "
        << noise << "% of pixels, so this path is not deterministic enough to "
           "attribute any difference to rasterisation DPI";

    // Print the measurement even on success. #302 is a record of NUMBERS, and
    // a test that only speaks when it fails leaves the next reader re-deriving
    // them -- which is how "0.0000%" became folklore rather than a datum.
    std::cout << "[i302] saved-document differing pixels: 4x-vs-correct="
              << diff << "%, same-tile control=" << noise
              << "%, in-memory ink " << inkOk << "px vs " << inkBad << "px"
              << std::endl;

    EXPECT_GT(diff, 1.0)
        << "GitHub #302 is NOT closed: a 4x error in the fallback's "
           "rasterisation DPI changed only " << diff << "% of pixels in the "
           "saved document (same-tile control: " << noise << "%). The recorded "
           "blind spot was 0.0000%; if this is still ~0 the fallback is not "
           "honouring its output scale end to end and #268's fix remains "
           "unobservable. In-memory the two glyphs differ in ink height "
        << inkOk << "px vs " << inkBad << "px, so the defect IS present -- it "
           "is the saved document that cannot see it.";
}

// ------------------------------------- criterion 1, through the REAL compose()

TEST_F(FreeTypeFallbackSavedDocTest, A4xDefectDrivenThroughTheRealCompositorIsVisibleInTheSavedDocument)
{
    // THE STRICT READING of criterion 1, added because the phase-4 karen gate
    // flagged that no single case proved "the real end-to-end compose() path is
    // DPI-sensitive in the saved document" -- that claim was synthesised from
    // two tests, one of which replicated the compositor's two public calls
    // rather than calling it.
    //
    // HOW THE DEFECT GETS IN WITHOUT TOUCHING THE COMPOSITOR. `compose()`
    // computes `glyphScale = compositeDpi / atlasDpi` and calls
    // `setFreeTypeOutputScale(glyphScale)` itself, so a scale set beforehand is
    // overwritten -- which is why the other case could not use it. But the
    // INPUT to that computation is reachable: an atlas whose recorded harvest
    // DPI is four times too low makes `compose()` ask the fallback for 4x the
    // correct scale, of its own accord. That is a faithful reintroduction of
    // the #266/#268 defect class (atlas DPI confused with composite DPI), it
    // travels the production path, and it needs no source edit.
    Harness ok;
    Harness bad;
    if (!ok.ready || !bad.ready) GTEST_SKIP() << "no system TrueType font available";

    // The ONLY difference: the recorded atlas harvest DPI, four times too
    // HIGH. Direction measured, not assumed: `kAtlasDpi / 4` gives
    // glyphScale = 300/37 = 8.1, and at that scale the glyph normalises to
    // nothing -- `inkHeight` returned -1 and the comparison had no subject.
    // `kAtlasDpi * 4` gives glyphScale = 0.5 against a correct 2.0, which is
    // the same 4x error with a glyph that still exists to be compared. A
    // defect severe enough to erase its own evidence cannot be measured, and
    // reporting that as a pass would be the exact failure this phase is about.
    bad.atlas.setSourceDpi(kAtlasDpi * 4);
    ASSERT_EQ(ok.atlas.sourceDpi(), kAtlasDpi);
    ASSERT_EQ(bad.atlas.sourceDpi(), kAtlasDpi * 4)
        << "the defect was not injected, so this test would compare two "
           "identical composites and pass while measuring nothing";

    // Both composites must still take the fallback, or this measures synthesis.
    EnrichedAtlas::GlyphSource sOk = EnrichedAtlas::GlyphSource::None;
    EnrichedAtlas::GlyphSource sBad = EnrichedAtlas::GlyphSource::None;
    ASSERT_FALSE(ok.atlas.glyph(kFallbackChar, &sOk).isNull());
    ASSERT_FALSE(bad.atlas.glyph(kFallbackChar, &sBad).isNull());
    ASSERT_EQ(sOk, EnrichedAtlas::GlyphSource::FreeType);
    ASSERT_EQ(sBad, EnrichedAtlas::GlyphSource::FreeType);

    const QImage compOk = ok.composeViaCompositor();
    const QImage compBad = bad.composeViaCompositor();
    ASSERT_FALSE(compOk.isNull()) << "correct composite produced nothing";
    ASSERT_FALSE(compBad.isNull()) << "defective composite produced nothing";
    ASSERT_GT(inkHeight(compOk), 0) << "no ink in the correct composite";
    ASSERT_GT(inkHeight(compBad), 0) << "no ink in the defective composite";

    const QImage backOk = saveAndRenderBack(compOk, tempDir_, fixture_, ctx_);
    const QImage backBad = saveAndRenderBack(compBad, tempDir_, fixture_, ctx_);
    ASSERT_FALSE(backOk.isNull()) << "correct-path round trip produced nothing";
    ASSERT_FALSE(backBad.isNull()) << "defect-path round trip produced nothing";

    const double diff = percentDiffering(backOk, backBad);
    ASSERT_GE(diff, 0.0)
        << "the two saved renders could not be compared (" << backOk.width()
        << "x" << backOk.height() << " vs " << backBad.width() << "x"
        << backBad.height() << "). 'Could not tell' is not 'they match'.";

    // Control before the claim, as in the other case: the same composite twice
    // must be essentially identical, or any difference above is noise.
    const QImage backOk2 = saveAndRenderBack(compOk, tempDir_, fixture_, ctx_);
    ASSERT_FALSE(backOk2.isNull());
    const double noise = percentDiffering(backOk, backOk2);
    ASSERT_GE(noise, 0.0);
    EXPECT_LT(noise, 0.5)
        << "the same composite through the same path differed by " << noise
        << "% of pixels, so the comparison below is measuring noise";

    std::cout << "[i302] compose()-driven 4x atlas-DPI defect: differing="
              << diff << "%, same-composite control=" << noise << "%"
              << std::endl;

    EXPECT_GT(diff, 1.0)
        << "GitHub #302, strict reading: a 4x atlas-DPI defect driven through "
           "the REAL GlyphAtlasCompositor::compose() changed only " << diff
        << "% of pixels in the saved document (control " << noise << "%). The "
           "recorded blind spot was 0.0000%; if this is still ~0 then the "
           "production composite path is insensitive to the fallback's "
           "rasterisation DPI and #268 remains unobservable end to end.";
}

// ----------------------------- criterion 1: GlyphSource IN the saved document

TEST_F(FreeTypeFallbackSavedDocTest, TheSavedDocumentDISCRIMINATESByGlyphSource)
{
    // THE LITERAL READING of criterion 1, which the phase-4 validator gate
    // ruled NOT MET on the first attempt: GlyphSource was asserted only in
    // memory, before any save, in every case -- so the criterion's
    // "(not in-memory state only)" was unsatisfied.
    //
    // A rasterised bitmap carries no provenance: no pixel can say "a FreeType
    // glyph made me", and no amount of test code can recover it. So the
    // assertion that IS available, and is what the criterion is reaching for,
    // is that the saved output is a FUNCTION of the glyph source -- that a
    // document built from a FreeType-sourced glyph is distinguishable, in its
    // saved pixels, from one built from an atlas-sourced glyph.
    //
    // THE CONFOUND THIS AVOIDS. The obvious version -- compose 'w' (fallback)
    // and 'H' (atlas) and show they differ -- proves nothing: two different
    // letters differ because they are different letters. So both composites
    // here use the SAME codepoint, and only the SOURCE differs: one atlas
    // holds 'w', the other does not. Same text, same geometry, same DPIs; the
    // single variable is which level of `glyph()` answered.
    const std::string font = systemFont();
    if (font.empty()) GTEST_SKIP() << "no system TrueType font available";

    BitmapCompositor comp1, comp2;
    FontMatch fm;
    fm.fontFilePath = font;
    fm.sizePt = 12.0f;
    fm.color = RgbColor{0, 0, 0};

    // (a) 'w' absent from the atlas -> the FreeType fallback answers.
    EnrichedAtlas viaFallback;
    viaFallback.build(atlasWithAnH(40), U"");
    viaFallback.setFreeTypeFallback(&comp1, fm, kCompositeDpi);
    viaFallback.setSourceDpi(kAtlasDpi);

    // (b) the same 'w' PRESENT in the atlas -> the atlas answers. The glyph
    // image is a deliberately distinct mark, because the point is that the
    // saved document reflects which source was used; an atlas 'w' that looked
    // identical to the rendered one would make the discrimination untestable
    // for reasons of drawing, not of provenance.
    GlyphAtlas withW = atlasWithAnH(40);
    {
        QImage w(20, 40, QImage::Format_ARGB32_Premultiplied);
        w.fill(Qt::transparent);
        {
            QPainter p(&w);
            p.setPen(Qt::NoPen);
            p.setBrush(Qt::black);
            p.drawRect(2, 8, 16, 24);
        }
        withW.addGlyph(kFallbackChar, w, /*confidence=*/100);
    }
    EnrichedAtlas viaAtlas;
    viaAtlas.build(withW, U"");
    viaAtlas.setFreeTypeFallback(&comp2, fm, kCompositeDpi);
    viaAtlas.setSourceDpi(kAtlasDpi);

    // PROVE the sources really differ for the same codepoint. Without this the
    // two composites might both be FreeType-sourced and the comparison would
    // be measuring nothing -- the vacuity that produced #302's 0.0000%.
    EnrichedAtlas::GlyphSource sFt = EnrichedAtlas::GlyphSource::None;
    EnrichedAtlas::GlyphSource sAt = EnrichedAtlas::GlyphSource::None;
    ASSERT_FALSE(viaFallback.glyph(kFallbackChar, &sFt).isNull());
    ASSERT_FALSE(viaAtlas.glyph(kFallbackChar, &sAt).isNull());
    ASSERT_EQ(sFt, EnrichedAtlas::GlyphSource::FreeType)
        << "the fallback atlas did not reach FreeType for the codepoint";
    ASSERT_EQ(sAt, EnrichedAtlas::GlyphSource::Atlas)
        << "the atlas-holding variant did not answer from the atlas, so both "
           "composites would share a source and nothing is being compared";

    GlyphAtlasCompositor compositor;
    const std::u32string text(1, kFallbackChar);
    const QImage cFt = compositor.compose(text, viaFallback, fm,
                                          RgbColor{255, 255, 255}, 160, 80,
                                          kCompositeDpi, &comp1);
    const QImage cAt = compositor.compose(text, viaAtlas, fm,
                                          RgbColor{255, 255, 255}, 160, 80,
                                          kCompositeDpi, &comp2);
    ASSERT_FALSE(cFt.isNull()) << "fallback-sourced composite is null";
    ASSERT_FALSE(cAt.isNull()) << "atlas-sourced composite is null";
    // BOTH must carry ink. If one composite were blank the comparison would
    // degenerate into "something vs nothing", which yields a large difference
    // for a reason that has nothing to do with glyph source. Caught exactly
    // this way: this test first reported 1.18082% -- byte-identical to the
    // unrelated compose()-driven case above, and two independent measurements
    // agreeing to five decimal places is a degeneracy signal, not a
    // coincidence.
    const int inkFt = inkHeight(cFt);
    const int inkAt = inkHeight(cAt);
    std::cout << "[i302] composite ink: FreeType=" << inkFt
              << "px Atlas=" << inkAt << "px" << std::endl;
    ASSERT_GT(inkFt, 0) << "the fallback-sourced composite is blank";
    ASSERT_GT(inkAt, 0) << "the atlas-sourced composite is blank";

    // IN MEMORY FIRST. If the two composites barely differ here, then any
    // difference measured after the save cannot be attributed to glyph source
    // -- and if they differ a lot here but the saved renders differ in a fixed
    // region, the save/render path is flattening the signal and the oracle is
    // measuring an artefact. Both of those are findings, not passes.
    const double memDiff = percentDiffering(cFt, cAt);
    std::cout << "[i302] composites differ IN MEMORY by " << memDiff << "%"
              << std::endl;
    ASSERT_GT(memDiff, 1.0)
        << "the fallback-sourced and atlas-sourced composites are nearly "
           "identical in memory (" << memDiff << "%), so this test cannot "
           "attribute a saved-document difference to glyph source";

    const QImage backFt = saveAndRenderBack(cFt, tempDir_, fixture_, ctx_);
    const QImage backAt = saveAndRenderBack(cAt, tempDir_, fixture_, ctx_);
    ASSERT_FALSE(backFt.isNull()) << "fallback round trip produced nothing";
    ASSERT_FALSE(backAt.isNull()) << "atlas round trip produced nothing";

    const double diff = percentDiffering(backFt, backAt);
    ASSERT_GE(diff, 0.0) << "the two saved renders could not be compared";

    const QImage backFt2 = saveAndRenderBack(cFt, tempDir_, fixture_, ctx_);
    ASSERT_FALSE(backFt2.isNull());
    const double noise = percentDiffering(backFt, backFt2);
    ASSERT_GE(noise, 0.0);
    EXPECT_LT(noise, 0.5) << "same composite twice differed by " << noise << "%";

    std::cout << "[i302] saved document discriminates by GlyphSource: "
                 "FreeType-vs-Atlas differing=" << diff
              << "%, same-source control=" << noise << "%" << std::endl;

    EXPECT_GT(diff, 1.0)
        << "the SAVED document does not discriminate between a "
           "FreeType-sourced and an atlas-sourced glyph for the same "
           "codepoint (differing=" << diff << "%, control=" << noise
        << "%). GlyphSource would then be observable only in memory, which is "
           "exactly what GitHub #302 reports: the fallback can be arbitrarily "
           "wrong and the written document looks the same.";
}

// ---------------------------------------------------------------------------
// GitHub #307 / #268 — the ABSOLUTE size oracle.
//
// WHY THE SIX TESTS ABOVE WERE NOT ENOUGH, measured rather than argued.
//
// #307's fix (sequencing the `GlyphSource` out-param read, commit `9d7cfdca`)
// was already in the tree when p98 phase-1 started. The question was therefore
// not "does it work" but "would anything notice if it stopped". It would not:
// re-introducing the defect deterministically -- passing
// `GlyphSource::None` to `toCompositeScale`, so its "a FreeType glyph is
// already pre-scaled" guard never fires and `glyphScale` is applied twice --
// left **all six tests above GREEN**.
//
// The reason is structural, and it is worth naming because it is a reusable
// mistake: every oracle above asserts a DIFFERENCE between a correct and a
// deliberately-broken configuration (`percentDiffering`, source-discrimination).
// A defect that scales BOTH arms by the same factor preserves every one of
// those differences. They measure the pipeline's SENSITIVITY to DPI, which is
// what #302 needed, and they are silent about whether the absolute size is
// right -- which is what #307 and #268 are about.
//
// THE ORACLE HERE. Compose the same fallback glyph twice:
//
//   mixed   sourceDpi=150, composite=300  ->  glyphScale 2.0  (defect can fire)
//   matched sourceDpi=300, composite=300  ->  glyphScale 1.0  (defect CANNOT fire)
//
// `toCompositeScale` early-returns when `glyphScale == 1.0`, so the matched
// case is untouchable by the defect and is a genuine CONTROL rather than a
// second output sharing the fault. That premise is asserted below, not assumed.
//
// WHAT THE CORRECT RATIO IS, and why it is not 1.0. Both composites draw at
// the same composite DPI, but the SAME atlas bitmap is interpreted as 150 DPI
// in one and 300 DPI in the other. The mixed-DPI case therefore SHOULD be
// twice as tall -- its atlas was harvested at half the output resolution, so
// reaching composite space genuinely needs a 2x upscale. The expectation is
// `kCompositeDpi / kAtlasDpi`, derived here rather than hardcoded, and the
// defect DOUBLES it to 4x.
//
// This value was measured, and the first attempt got it wrong in an
// instructive way: the test was written expecting 1.0, and the run reported
// 4.12. That looked like a live defect. It was a STALE BINARY -- the restore
// step of the mutation harness used `shutil.copy2`, which preserves the
// snapshot's mtime, so the restored source looked older than the object built
// from the mutant and MSBuild skipped the recompile. The mutant's answer
// (4.12) leaked into two "pristine" measurements before an unrelated edit
// forced a real rebuild and showed 2.0. The harness now uses `copy` plus an
// explicit touch. Recorded because "the fix is broken" and "you measured the
// previous build" are indistinguishable from the number alone.
//
// No pixel size is hardcoded: the expectation is a RATIO against the control,
// so the test does not depend on which system font answered.
TEST_F(FreeTypeFallbackSavedDocTest, AMixedDpiFallbackGlyphIsNotScaledTwice)
{
    Harness mixed;    // sourceDpi = kAtlasDpi (150) -> glyphScale 2.0
    Harness matched;  // about to become glyphScale 1.0
    if (!mixed.ready || !matched.ready)
        GTEST_SKIP() << "no system TrueType font available";

    matched.atlas.setSourceDpi(kCompositeDpi);

    // Premises, asserted. Without these the comparison could be measuring a
    // scale of 1.0 against a scale of 1.0 and would be vacuous -- the shape
    // that produced #302's 0.0000%.
    ASSERT_EQ(mixed.atlas.sourceDpi(), kAtlasDpi);
    ASSERT_EQ(matched.atlas.sourceDpi(), kCompositeDpi)
        << "the control's source DPI did not take, so its glyphScale is not "
           "1.0 and it is not immune to the defect";
    ASSERT_NE(kAtlasDpi, kCompositeDpi)
        << "the two DPIs are equal, so glyphScale is 1.0 everywhere and this "
           "test cannot observe a double application at all";

    // Both must reach FreeType, or this measures synthesis or the atlas.
    EnrichedAtlas::GlyphSource sMixed = EnrichedAtlas::GlyphSource::None;
    EnrichedAtlas::GlyphSource sMatched = EnrichedAtlas::GlyphSource::None;
    ASSERT_FALSE(mixed.atlas.glyph(kFallbackChar, &sMixed).isNull());
    ASSERT_FALSE(matched.atlas.glyph(kFallbackChar, &sMatched).isNull());
    ASSERT_EQ(sMixed, EnrichedAtlas::GlyphSource::FreeType);
    ASSERT_EQ(sMatched, EnrichedAtlas::GlyphSource::FreeType);

    // A canvas tall enough that a 2x glyph is NOT clipped. Clipping would cap
    // the measured height and hide the very error being looked for, turning a
    // real failure into a pass.
    constexpr int kW = 320;
    constexpr int kH = 240;
    const std::u32string text(1, kFallbackChar);

    auto composeOn = [&](const Harness& h) {
        GlyphAtlasCompositor comp;
        return comp.compose(text, h.atlas, h.fm, RgbColor{255, 255, 255},
                            kW, kH, kCompositeDpi,
                            const_cast<BitmapCompositor*>(&h.compositor));
    };

    const QImage cMixed = composeOn(mixed);
    const QImage cMatched = composeOn(matched);
    ASSERT_FALSE(cMixed.isNull()) << "mixed-DPI composite produced nothing";
    ASSERT_FALSE(cMatched.isNull()) << "control composite produced nothing";

    const int hMixed = inkHeight(cMixed);
    const int hMatched = inkHeight(cMatched);
    ASSERT_GT(hMixed, 0) << "no ink in the mixed-DPI composite";
    ASSERT_GT(hMatched, 0) << "no ink in the control composite";

    // Not clipped: ink touching the bottom edge means the glyph may have been
    // cut off, so the height is a floor and not a measurement.
    ASSERT_LT(hMixed, kH - 1)
        << "the mixed-DPI glyph fills the canvas (" << hMixed << "/" << kH
        << "), so its height is clipped and cannot be compared";

    const double ratio = static_cast<double>(hMixed)
                       / static_cast<double>(hMatched);
    const double expected = static_cast<double>(kCompositeDpi)
                          / static_cast<double>(kAtlasDpi);
    std::cout << "[i307] ink height mixed=" << hMixed
              << " control=" << hMatched << " ratio=" << ratio
              << " expected=" << expected
              << " (defect would give " << (2.0 * expected) << ")" << std::endl;

    // Tolerance is generous because rasterisation and hinting differ by font,
    // and the defect being caught is a further factor of 2 -- far outside this
    // band. A tight tolerance would buy nothing and make the test
    // font-dependent. MEASURED on this host: 2.0 correct, 4.0 with the defect.
    EXPECT_NEAR(ratio, expected, 0.4)
        << "GitHub #307/#268: a FreeType fallback glyph on a mixed-DPI page "
           "rendered " << ratio << "x the size of the same glyph on a "
           "matched-DPI page, where " << expected << "x is correct "
           "(kCompositeDpi/kAtlasDpi). A ratio near " << (2.0 * expected)
        << " means glyphScale was applied TWICE: `toCompositeScale` received a "
           "GlyphSource that was not the one `glyph()` wrote, so its "
           "pre-scaled guard never fired. Check that BOTH call sites in "
           "GlyphAtlasCompositor::compose() still read the out-param in a "
           "SEQUENCED statement rather than inside the argument list -- "
           "function arguments are unsequenced in C++, which is the whole of "
           "GitHub #307.";
}

// ---------------------------------------------------------------------------
// GitHub #307, criterion 1 — the SOURCE-level half.
//
// The behavioural test above is the real guard, but it can only fail if this
// host has a usable TrueType font and the fallback is reached. This check
// needs neither, and it states the structural invariant directly: there is ONE
// shared scaling path, and BOTH call sites hand it a GlyphSource read in a
// SEQUENCED statement rather than inside an argument list.
//
// Source-level on purpose. The defect is undefined behaviour -- function
// arguments are unsequenced in C++ -- so whether it misbehaves depends on the
// compiler's chosen evaluation order. A build where the order happens to be
// benign would make the behavioural test green while the latent defect sat
// there waiting for a compiler upgrade. (On THIS host it is not benign: the
// re-inlined form reproduces the 2x error exactly, measured.) A structural
// assertion does not care which order the compiler picked.
TEST(GlyphScaleSequencing, BothCallSitesReadTheGlyphSourceInASequencedStatement)
{
    const std::filesystem::path src =
        std::filesystem::path(__FILE__).parent_path().parent_path() /
        "src" / "tre" / "GlyphAtlasCompositor.cpp";
    ASSERT_TRUE(std::filesystem::exists(src))
        << "could not resolve GlyphAtlasCompositor.cpp from __FILE__ ("
        << __FILE__ << "); this test must not pass without reading it";

    std::ifstream in(src);
    ASSERT_TRUE(in.good());
    std::string line;
    int defs = 0;          // definitions of the shared lambda
    int sequenced = 0;     // toCompositeScale(<ident>, gsrc)
    int inlined = 0;       // toCompositeScale(<call>(...), gsrc)  <-- the defect
    while (std::getline(in, line)) {
        const auto firstNonSpace = line.find_first_not_of(" \t");
        if (firstNonSpace != std::string::npos &&
            line.compare(firstNonSpace, 2, "//") == 0) {
            continue;  // this file discusses the pattern at length in comments
        }
        if (line.find("auto toCompositeScale") != std::string::npos) ++defs;
        const auto call = line.find("toCompositeScale(");
        if (call == std::string::npos) continue;
        const std::string args = line.substr(call + std::string("toCompositeScale(").size());
        // An inlined glyph() call inside the argument list is the defect; a
        // bare identifier is the fix.
        if (args.find(".glyph(") != std::string::npos ||
            args.find("->glyph(") != std::string::npos) {
            ++inlined;
        } else {
            ++sequenced;
        }
    }

    // Non-vacuity FIRST: if the scan matched nothing the pattern changed and
    // this test has stopped checking anything.
    ASSERT_EQ(defs, 1)
        << "expected exactly ONE definition of the shared toCompositeScale "
           "path, found " << defs << ". Criterion 1 is that both passes apply "
           "the scale on a single shared path; two definitions means two "
           "places to fix and one to forget.";
    ASSERT_EQ(sequenced + inlined, 2)
        << "expected exactly 2 toCompositeScale call sites (the measuring pass "
           "and the drawing pass), found " << (sequenced + inlined)
        << ". The scan no longer matches the code, so this guard is vacuous.";

    EXPECT_EQ(inlined, 0)
        << "GitHub #307: " << inlined << " call site(s) pass the result of "
           "glyph() directly into toCompositeScale alongside the GlyphSource "
           "out-param. Function arguments are UNSEQUENCED in C++, so the "
           "source may be read BEFORE glyph() writes it -- leaving the stale "
           "GlyphSource::None, so the \"already pre-scaled\" guard never "
           "fires and glyphScale is applied TWICE to FreeType glyphs. Assign "
           "the out-param in its own statement first.";
    EXPECT_EQ(sequenced, 2)
        << "only " << sequenced << " of the 2 call sites read the GlyphSource "
           "from a preceding statement";
}
