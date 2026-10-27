// -*- coding: utf-8 -*-
//
// Content-stream state-continuity pipeline — Phase 2 REGRESSION SUITE.
//
// WHAT THIS IS
// ------------
// The HARD-ASSERTION counterpart to the phase-1 discovery probe
// (tests/test_content_stream_adversarial_ambient.cpp). Phase 1 was a
// measurement instrument: it RECORDED that three TEXT injectors —
// AlignmentInjector, HeaderFooterEngine and TRE PdfContentWriter::writeTextBlock
// — rendered their injected text HALF-SIZE when the host page left an ambient
// "0.5 0 0 0.5 0 0 cm" (N-up / imposed) transform active on /Contents. This file
// promotes those three corrupting cells to real EXPECT/ASSERT oracles.
//
// THE BUG (phase-1 gap register, cell {injector} x cm)
// ----------------------------------------------------
// The shared text-injection preamble (core/ContentStreamPreamble.h,
// knownTextStateBasis) resets Tz/Tc/Tw/Ts/Tr + fill colour but CANNOT reset the
// CTM (a `cm` multiplies, it cannot replace). A bare append therefore inherits
// the page's ambient CTM and the glyphs render at 0.5x. The image injectors were
// already immune because they append through
// core/PageContentGuard.h::appendContentIsolatingAmbientCtm, which brackets the
// EXISTING page content in `q ... Q` so its outer `Q` restores the page-initial
// identity CTM before the appended fragment runs.
//
// THE FIX (phase 2)
// -----------------
// Route the three text injectors' appends through that SAME shared helper. The
// oracle here is the injected glyph height RELATIVE to a pre-existing "KEEPME"
// neighbour drawn safely in its own q...Q: under the bug sizeRatio ~= 0.5; after
// the fix it must be ~= 1.0. We also require the glyph to be upright (not
// mirrored), visible (ink present in the rendered ROI) and on-page.
//
// This is a correctness suite: a corrupt cell is a RED test, never data. Do NOT
// weaken an oracle or skip a cell to reach green.

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"

#include "reflow/AlignmentPipeline.h"
#include "reflow/BlockParamsBuilder.h"
#include "reflow/ReflowTypes.h"
#include "headerfooter/HeaderFooterEngine.h"
#include "headerfooter/HeaderFooterConfig.h"
#include "tre/PdfContentWriter.h"
#include "tre/TreTypes.h"
#include "fonts/PdfFontEmbedder.h"

#include <QImage>
#include <QPainter>
#include <QRectF>

extern "C" {
#include <mupdf/fitz.h>
}

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#ifndef TEST_FIXTURES_DIR
#define TEST_FIXTURES_DIR "."
#endif

namespace {

namespace fs = std::filesystem;
using pdfe::core::Document;
using pdfe::core::MuPdfContext;
using pdfe::core::Page;

// The imposed / N-up hostile fixture: /Contents leaves "0.5 0 0 0.5 0 0 cm"
// active (no matching Q), so a naively-appended fragment inherits the 0.5x CTM.
fs::path cmFixture() {
    return fs::path(TEST_FIXTURES_DIR) / "adversarial_ambient" / "aa_cm.pdf";
}
// R-33: the SHEAR fixture leaves "1 0 0.5 1 0 0 cm" (a horizontal skew) live at
// the end of the base content stream. A pure shear has determinant 1, so it
// preserves glyph HEIGHT and WIDTH — the half-size CTM oracle is blind to it.
// It manifests ONLY as a skewed glyph quad (left edge no longer vertical).
fs::path shearFixture() {
    return fs::path(TEST_FIXTURES_DIR) / "adversarial_ambient" / "aa_shear.pdf";
}
// R-33 rotate: the ROTATE fixture leaves "0.9659 0.2588 -0.2588 0.9659 0 0 cm"
// (a +15° rotation) live at the end of the base content stream. A rotation is
// determinant-1 like a shear (glyph HEIGHT and WIDTH ~preserved) so the half-size
// oracle is blind to it, AND unlike a pure shear it tilts BOTH edges — the tell
// is the glyph's TOP EDGE slope (level for upright, ≈tan15°≈0.27 if leaked). The
// fixture's own EDITME span is drawn UNDER the live rotation, so it is a built-in
// positive control that stext reports the tilt.
fs::path rotateFixture() {
    return fs::path(TEST_FIXTURES_DIR) / "adversarial_ambient" / "aa_rotate.pdf";
}
// R-34 mediabox: non-zero MediaBox origin [100 100 712 892] + half-scale
// "0.5 0 0 0.5 0 0 cm" ambient (imposition / N-up). An injector that ignores the
// MediaBox origin drops the injected element off the visible page; the on-page
// oracle proves it lands ON the offset page (origin added).
fs::path mediaboxFixture() {
    return fs::path(TEST_FIXTURES_DIR) / "adversarial_ambient" / "aa_mediabox.pdf";
}
fs::path outPath(const std::string& tag) {
    return fs::temp_directory_path() / ("pdfe_ctmreset_" + tag + ".pdf");
}

// ── Glyph-quad oracle (raw fitz, isolated context — a fresh viewer's read). ──
struct LineHit {
    bool found = false;
    float firstUlx = 0, firstUrx = 0;   // first glyph, for the mirror test
    float firstLlx = 0;                 // first glyph lower-left x — SHEAR test
    float firstUly = 0, firstUry = 0;   // first glyph top-edge y — ROTATE test
    float glyphH = 0;                   // first glyph height (points) — size test
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

// Top-edge horizontal slope of a glyph quad: Δy / Δx across the upper edge
// (ul → ur). ~0 for an upright glyph (top edge level); ~±tan(θ) under a leaked
// rotation CTM of θ degrees (both edges tilt). Independent of a pure horizontal
// shear, which leaves the top edge level and only tilts the left edge — so this
// is the ROTATE-specific tell that the shear oracle (left-edge slope) cannot see.
inline float topEdgeSlope(const LineHit& h) {
    const float dx = h.firstUrx - h.firstUlx;
    if (std::fabs(dx) < 1e-3f) return 0.0f;
    return (h.firstUry - h.firstUly) / dx;
}

LineHit readLine(const fs::path& pdf, const std::u32string& needle) {
    LineHit h;
    fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    fz_register_document_handlers(ctx);
    fz_document* doc = nullptr;
    fz_stext_page* stext = nullptr;
    fz_try(ctx) {
        doc = fz_open_document(ctx, pdf.string().c_str());
        stext = fz_new_stext_page_from_page_number(ctx, doc, 0, nullptr);
        for (fz_stext_block* b = stext->first_block; b && !h.found; b = b->next) {
            if (b->type != FZ_STEXT_BLOCK_TEXT) continue;
            for (fz_stext_line* ln = b->u.t.first_line; ln; ln = ln->next) {
                std::u32string t;
                for (fz_stext_char* ch = ln->first_char; ch; ch = ch->next)
                    t.push_back(static_cast<char32_t>(ch->c));
                if (t.find(needle) == std::u32string::npos) continue;
                fz_stext_char* f = ln->first_char;
                h.found = true;
                h.firstUlx = f->quad.ul.x;
                h.firstUrx = f->quad.ur.x;
                h.firstLlx = f->quad.ll.x;
                h.firstUly = f->quad.ul.y;
                h.firstUry = f->quad.ur.y;
                h.glyphH = std::fabs(f->quad.ll.y - f->quad.ul.y);
                float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
                for (fz_stext_char* ch = ln->first_char; ch; ch = ch->next) {
                    const fz_quad& q = ch->quad;
                    for (const fz_point& p : {q.ul, q.ur, q.ll, q.lr}) {
                        x0 = std::min(x0, p.x); x1 = std::max(x1, p.x);
                        y0 = std::min(y0, p.y); y1 = std::max(y1, p.y);
                    }
                }
                h.x0 = x0; h.y0 = y0; h.x1 = x1; h.y1 = y1;
                break;
            }
        }
    }
    fz_always(ctx) {
        if (stext) fz_drop_stext_page(ctx, stext);
        if (doc) fz_drop_document(ctx, doc);
    }
    fz_catch(ctx) {}
    fz_drop_context(ctx);
    return h;
}

QImage renderPage0(const fs::path& pdf, float scale) {
    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, pdf);
    if (!doc || doc->pageCount() == 0) return QImage();
    auto page = doc->page(0);
    if (!page) return QImage();
    QImage rgba = page->render(page->boundingBox(), scale);
    if (rgba.isNull()) return QImage();
    QImage canvas(rgba.size(), QImage::Format_RGB888);
    canvas.fill(Qt::white);
    QPainter p(&canvas);
    p.drawImage(0, 0, rgba);
    p.end();
    return canvas;
}

long inkInRoi(const QImage& im, float scale, float px0, float py0, float px1,
              float py1) {
    if (im.isNull()) return 0;
    int x0 = std::max(0, static_cast<int>(std::floor(std::min(px0, px1) * scale)) - 2);
    int y0 = std::max(0, static_cast<int>(std::floor(std::min(py0, py1) * scale)) - 2);
    int x1 = std::min(im.width() - 1, static_cast<int>(std::ceil(std::max(px0, px1) * scale)) + 2);
    int y1 = std::min(im.height() - 1, static_cast<int>(std::ceil(std::max(py0, py1) * scale)) + 2);
    long ink = 0;
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            QRgb p = im.pixel(x, y);
            if (qRed(p) < 235 || qGreen(p) < 235 || qBlue(p) < 235) ++ink;
        }
    return ink;
}

// Shared assertions: the injected line must be present, upright, visible, on the
// page, and full-size relative to the KEEPME neighbour. `sizeRatio` under the
// bug is ~0.5 (half-scale ambient CTM); after the fix it must be ~1.0. The band
// [0.80,1.20] is well clear of the 0.5 failure and tighter than the phase-1
// probe's deliberately-lenient [0.70,1.40].
void assertInjectedFullSizeAndClean(const fs::path& out,
                                    const std::u32string& needle) {
    const float scale = 3.0f;
    LineHit inj  = readLine(out, needle);
    LineHit keep = readLine(out, U"KEEPME");
    QImage im = renderPage0(out, scale);

    ASSERT_TRUE(inj.found) << "injected text not found in stext after reopen";
    ASSERT_TRUE(keep.found) << "KEEPME neighbour not found — cannot size-normalise";
    ASSERT_FALSE(im.isNull()) << "page 0 failed to render";

    // Upright (not mirrored by an ambient -100 Tz etc.).
    EXPECT_GT(inj.firstUrx, inj.firstUlx)
        << "injected glyph is mirrored (ur.x <= ul.x)";

    // Visible: real ink inside the injected line's rendered ROI.
    const long ink = inkInRoi(im, scale, inj.x0, inj.y0, inj.x1, inj.y1);
    EXPECT_GT(ink, 60) << "injected text left no ink (invisible / off-page)";

    // Full-size: this is the core CTM-reset assertion. Bug => ~0.5.
    ASSERT_GT(keep.glyphH, 0.1f);
    const float ratio = inj.glyphH / keep.glyphH;
    EXPECT_GT(ratio, 0.80f)
        << "injected text is UNDER-SIZED (sizeRatio=" << ratio
        << ") — ambient N-up CTM leaked into the fragment";
    EXPECT_LT(ratio, 1.20f)
        << "injected text is OVER-SIZED (sizeRatio=" << ratio << ")";

    // On-page: the injected line's bbox must fall within the page media box.
    QImage im1 = renderPage0(out, 1.0f);
    ASSERT_FALSE(im1.isNull());
    EXPECT_GE(inj.x0, -1.0f);
    EXPECT_GE(inj.y0, -1.0f);
    EXPECT_LE(inj.x1, static_cast<float>(im1.width()) + 1.0f);
    EXPECT_LE(inj.y1, static_cast<float>(im1.height()) + 1.0f);
}

// R-33 SHEAR oracle. In addition to the full-size / upright / visible / on-page
// checks, prove the injected glyph is NOT SKEWED by the ambient shear CTM. A
// pure shear preserves height+width (so assertInjectedFullSizeAndClean passes
// even if the shear leaks) — the tell is the glyph's LEFT EDGE slope. For an
// upright glyph the upper-left and lower-left corners share an x (vertical left
// edge); a leaked "0.5" horizontal shear tilts it so ll.x - ul.x ≈ ±0.5·height.
// The KEEPME neighbour is drawn safely in its own q…Q, so its left-edge slope is
// the un-sheared reference the injected glyph must match.
void assertInjectedUprightUnsheared(const fs::path& out,
                                    const std::u32string& needle) {
    // Reuse the full-size / upright / visible / on-page contract first.
    assertInjectedFullSizeAndClean(out, needle);
    if (::testing::Test::HasFatalFailure()) return;

    LineHit inj  = readLine(out, needle);
    LineHit keep = readLine(out, U"KEEPME");
    ASSERT_TRUE(inj.found);
    ASSERT_TRUE(keep.found);
    ASSERT_GT(inj.glyphH, 0.1f);

    // Left-edge horizontal slope (x-run over the glyph's full height). ~0 for an
    // upright glyph; ~±0.5·height under a leaked "1 0 0.5 1" shear.
    const float injSkew  = inj.firstLlx  - inj.firstUlx;
    const float keepSkew = keep.firstLlx - keep.firstUlx;

    // Under the hostile shear a leak would put injSkew near ±0.5·glyphH (≈ ±9pt
    // for an 18pt glyph). Require the injected left edge to be as vertical as the
    // safe KEEPME neighbour's, well inside the 0.5·h leak signature.
    EXPECT_NEAR(injSkew, keepSkew, 0.20f * inj.glyphH)
        << "injected glyph is SHEARED (left-edge slope injSkew=" << injSkew
        << " vs un-sheared KEEPME keepSkew=" << keepSkew
        << ", glyphH=" << inj.glyphH
        << ") — ambient shear CTM leaked into the fragment";
    // Absolute backstop independent of the neighbour: a 0.5 shear on an 18pt
    // glyph yields |skew| ≈ 9pt; require the injected left edge to stay near
    // vertical (|skew| < 0.25·h ≈ 4.5pt).
    EXPECT_LT(std::fabs(injSkew), 0.25f * inj.glyphH)
        << "injected glyph left edge is not vertical (skew=" << injSkew
        << ", glyphH=" << inj.glyphH << ") — leaked ambient shear";
}

// R-33 ROTATE oracle. In addition to the full-size / upright / visible / on-page
// contract, prove the injected glyph is NOT ROTATED by the ambient rotate CTM.
// A rotation preserves height+width (so assertInjectedFullSizeAndClean passes
// even if it leaks) and, unlike a pure shear, tilts BOTH edges — so the tell is
// the glyph's TOP EDGE slope (Δy/Δx across ul→ur): ~0 for upright, ≈±tan(15°)
// (≈0.27) under a leaked "+15°" rotate. The safe KEEPME neighbour (own q…Q) is
// the level reference. NON-TAUTOLOGY CONTROL: the fixture's own EDITME span is
// drawn UNDER the live rotation, so its top edge MUST read tilted — proving the
// oracle can actually see rotation and the injected mark's levelness is meaningful.
void assertInjectedUprightUnrotated(const fs::path& out,
                                    const std::u32string& needle) {
    // Reuse the full-size / upright / visible / on-page contract first.
    assertInjectedFullSizeAndClean(out, needle);
    if (::testing::Test::HasFatalFailure()) return;

    LineHit inj  = readLine(out, needle);
    LineHit keep = readLine(out, U"KEEPME");
    LineHit edit = readLine(out, U"EDITME");   // fixture span drawn under rotate
    ASSERT_TRUE(inj.found);
    ASSERT_TRUE(keep.found);

    const float injSlope  = topEdgeSlope(inj);
    const float keepSlope = topEdgeSlope(keep);

    // Positive control: the fixture's EDITME is drawn under the live +15° rotate,
    // so its top edge MUST be tilted (|slope| well above the level threshold and
    // near tan15°≈0.27). If this fails the fixture/stext isn't exposing rotation,
    // which would make the injected-mark check below a tautology.
    ASSERT_TRUE(edit.found) << "EDITME (rotate positive control) not found";
    EXPECT_GT(std::fabs(topEdgeSlope(edit)), 0.15f)
        << "EDITME top-edge slope=" << topEdgeSlope(edit)
        << " — expected the fixture's under-rotate span to read TILTED; the "
           "oracle cannot prove the injected mark is un-rotated otherwise";

    // Injected mark must be as level as the un-rotated KEEPME neighbour.
    EXPECT_NEAR(injSlope, keepSlope, 0.10f)
        << "injected glyph is ROTATED (top-edge slope injSlope=" << injSlope
        << " vs level KEEPME keepSlope=" << keepSlope
        << ") — ambient rotate CTM leaked into the fragment";
    // Absolute backstop: a leaked +15° rotate yields |slope| ≈ 0.27; require the
    // injected top edge to stay near level (|slope| < 0.12, well below 0.27).
    EXPECT_LT(std::fabs(injSlope), 0.12f)
        << "injected glyph top edge is not level (slope=" << injSlope
        << ") — leaked ambient rotate CTM";
}

// R-34 MEDIABOX-OFFSET oracle. On a page whose MediaBox origin is [100 100 …],
// an injector that computes its placement in a naive 0-origin page space drops
// the injected element OFF the visible page (to the left / below). Prove the
// injected element lands ON the offset page at the correct position: its rendered
// bbox is fully within the page and its LEFT edge is a small POSITIVE margin
// (origin added), never the negative value a dropped origin would produce.
void assertInjectedOnPageAtMediaboxOffset(const fs::path& out,
                                          const std::u32string& needle) {
    const float scale = 3.0f;
    LineHit inj = readLine(out, needle);
    QImage im = renderPage0(out, scale);
    QImage im1 = renderPage0(out, 1.0f);

    ASSERT_TRUE(inj.found) << "injected text not found in stext after reopen";
    ASSERT_FALSE(im.isNull()) << "page 0 failed to render";
    ASSERT_FALSE(im1.isNull());
    const float W = static_cast<float>(im1.width());
    const float H = static_cast<float>(im1.height());

    // Visible: real ink inside the injected line's rendered ROI.
    const long ink = inkInRoi(im, scale, inj.x0, inj.y0, inj.x1, inj.y1);
    EXPECT_GT(ink, 60) << "injected text left no ink (invisible / off-page)";

    // On-page: the injected line's bbox must fall within the (0-origin-normalised)
    // media box. If the origin were ignored the footer would sit at normalised
    // y ≈ H+64 (off the bottom) and x ≈ -64 (off the left).
    EXPECT_GE(inj.x0, -1.0f) << "injected left edge off the page (x0=" << inj.x0 << ")";
    EXPECT_GE(inj.y0, -1.0f);
    EXPECT_LE(inj.x1, W + 1.0f);
    EXPECT_LE(inj.y1, H + 1.0f) << "injected bottom off the page (y1=" << inj.y1
                                << ", H=" << H << ")";

    // CORRECT POSITION (the origin-add discriminator): the left footer is placed
    // at MediaBox-origin.x + margin(36). With the origin added the normalised left
    // edge is a small POSITIVE margin (~36); if the origin were dropped it would be
    // ~ -64 (36 - 100). Require a clearly positive, near-margin left edge.
    EXPECT_GT(inj.x0, 2.0f)
        << "injected left edge x0=" << inj.x0 << " is not positive — the injector "
           "ignored the MediaBox origin (placement shifted off the page by -origin)";
    EXPECT_LT(inj.x0, 90.0f)
        << "injected left edge x0=" << inj.x0 << " is too far right for a "
           "left-margin footer — unexpected placement";
    // A footer sits in the lower half of the page.
    EXPECT_GT(inj.y0, H * 0.5f)
        << "injected footer y0=" << inj.y0 << " not in the lower half (H=" << H << ")";
}

}  // namespace

// ═════════════════════════════════════════════════════════════════════════════
// AlignmentInjector x ambient cm — was CORRUPT (half-size) in phase 1.
// ═════════════════════════════════════════════════════════════════════════════
TEST(CtmReset, AlignmentInjectorFullSizeUnderAmbientCm) {
    ASSERT_TRUE(fs::exists(cmFixture())) << "fixture missing: " << cmFixture();
    auto ctx = std::make_shared<MuPdfContext>();
    pdfe::reflow::AlignmentPipeline pipe(ctx);
    auto params = pdfe::reflow::BlockParamsBuilder::build(
        "ALIGNMARK", QRectF(72.0, 400.0, 400.0, 40.0), 18.0f,
        pdfe::reflow::Alignment::Left);
    pipe.queueAlignment(0, params);
    fs::path out = outPath("align_cm");
    fs::remove(out);
    pipe.flush(cmFixture(), out);
    assertInjectedFullSizeAndClean(out, U"ALIGNMARK");
}

// ═════════════════════════════════════════════════════════════════════════════
// HeaderFooterEngine x ambient cm — was CORRUPT (half-size) in phase 1.
// ═════════════════════════════════════════════════════════════════════════════
TEST(CtmReset, HeaderFooterEngineFullSizeUnderAmbientCm) {
    ASSERT_TRUE(fs::exists(cmFixture())) << "fixture missing: " << cmFixture();
    auto ctx = std::make_shared<MuPdfContext>();
    pdfe::headerfooter::HeaderFooterEngine eng(ctx);
    pdfe::headerfooter::HeaderFooterConfig cfg;
    cfg.rangeMode = pdfe::headerfooter::PageRangeMode::All;
    cfg.centerHeader = "HFMARK";
    cfg.fontSize = 18.0f;
    fs::path out = outPath("hf_cm");
    fs::remove(out);
    eng.apply(cmFixture(), out, cfg);
    assertInjectedFullSizeAndClean(out, U"HFMARK");
}

// ═════════════════════════════════════════════════════════════════════════════
// TRE PdfContentWriter::writeTextBlock x ambient cm — was CORRUPT (half) phase 1.
// ═════════════════════════════════════════════════════════════════════════════
TEST(CtmReset, TreWriteTextBlockFullSizeUnderAmbientCm) {
    const fs::path arial = "C:\\Windows\\Fonts\\arial.ttf";
    ASSERT_TRUE(fs::exists(cmFixture())) << "fixture missing: " << cmFixture();
    // A real TTF is required to embed + shape the reconstructed block. On the
    // Windows targets this is always present; treat absence as a hard failure
    // (SKIP=FAIL) rather than a silent pass.
    ASSERT_TRUE(fs::exists(arial)) << "arial.ttf unavailable: " << arial;

    auto ctx = std::make_shared<MuPdfContext>();
    pdfe::fonts::PdfFontEmbedder embedder(ctx.get());
    pdfe::tre::PdfContentWriter writer(ctx.get(), &embedder);
    auto doc = Document::open(ctx, cmFixture());
    ASSERT_TRUE(doc != nullptr);
    auto page = doc->page(0);
    ASSERT_TRUE(page != nullptr);

    pdfe::tre::FontMatch fm;
    fm.fontFilePath = arial.string();
    fm.sizePt = 18.0f;
    fm.color = pdfe::tre::RgbColor{0, 0, 0};
    pdfe::tre::PdfRect rect{72.0, 300.0, 340.0, 325.0};  // fitz-space, Y-down
    std::string opsStr =
        writer.writeTextBlock(page.get(), doc.get(), "TREMARK", fm, rect);
    ASSERT_FALSE(opsStr.empty()) << "writeTextBlock produced no operators";
    ASSERT_TRUE(writer.appendContentStream(page.get(), doc.get(), opsStr))
        << "appendContentStream failed";

    fs::path out = outPath("tre_cm");
    fs::remove(out);
    doc->saveAs(out);
    page.reset();
    doc.reset();
    assertInjectedFullSizeAndClean(out, U"TREMARK");
}

// ═════════════════════════════════════════════════════════════════════════════
// R-33 SHEAR class — the zero-coverage sibling of the -100 Tz mirror bug. The
// PageContentGuard `q … Q` bracket restores the page-initial identity CTM before
// each appended fragment, so ANY ambient transform (scale, shear, rotate) is
// neutralised. These assert the injected text lands upright AND un-skewed under
// an ambient "1 0 0.5 1 0 0 cm" horizontal shear, which the half-size oracle
// alone cannot see (shear is determinant-1: height + width are preserved).
// ═════════════════════════════════════════════════════════════════════════════
TEST(CtmReset, AlignmentInjectorUprightUnshearedUnderAmbientShear) {
    ASSERT_TRUE(fs::exists(shearFixture())) << "fixture missing: " << shearFixture();
    auto ctx = std::make_shared<MuPdfContext>();
    pdfe::reflow::AlignmentPipeline pipe(ctx);
    auto params = pdfe::reflow::BlockParamsBuilder::build(
        "ALIGNMARK", QRectF(72.0, 400.0, 400.0, 40.0), 18.0f,
        pdfe::reflow::Alignment::Left);
    pipe.queueAlignment(0, params);
    fs::path out = outPath("align_shear");
    fs::remove(out);
    pipe.flush(shearFixture(), out);
    assertInjectedUprightUnsheared(out, U"ALIGNMARK");
}

TEST(CtmReset, HeaderFooterEngineUprightUnshearedUnderAmbientShear) {
    ASSERT_TRUE(fs::exists(shearFixture())) << "fixture missing: " << shearFixture();
    auto ctx = std::make_shared<MuPdfContext>();
    pdfe::headerfooter::HeaderFooterEngine eng(ctx);
    pdfe::headerfooter::HeaderFooterConfig cfg;
    cfg.rangeMode = pdfe::headerfooter::PageRangeMode::All;
    cfg.centerHeader = "HFMARK";
    cfg.fontSize = 18.0f;
    fs::path out = outPath("hf_shear");
    fs::remove(out);
    eng.apply(shearFixture(), out, cfg);
    assertInjectedUprightUnsheared(out, U"HFMARK");
}

TEST(CtmReset, TreWriteTextBlockUprightUnshearedUnderAmbientShear) {
    const fs::path arial = "C:\\Windows\\Fonts\\arial.ttf";
    ASSERT_TRUE(fs::exists(shearFixture())) << "fixture missing: " << shearFixture();
    ASSERT_TRUE(fs::exists(arial)) << "arial.ttf unavailable: " << arial;

    auto ctx = std::make_shared<MuPdfContext>();
    pdfe::fonts::PdfFontEmbedder embedder(ctx.get());
    pdfe::tre::PdfContentWriter writer(ctx.get(), &embedder);
    auto doc = Document::open(ctx, shearFixture());
    ASSERT_TRUE(doc != nullptr);
    auto page = doc->page(0);
    ASSERT_TRUE(page != nullptr);

    pdfe::tre::FontMatch fm;
    fm.fontFilePath = arial.string();
    fm.sizePt = 18.0f;
    fm.color = pdfe::tre::RgbColor{0, 0, 0};
    pdfe::tre::PdfRect rect{72.0, 300.0, 340.0, 325.0};  // fitz-space, Y-down
    std::string opsStr =
        writer.writeTextBlock(page.get(), doc.get(), "TREMARK", fm, rect);
    ASSERT_FALSE(opsStr.empty()) << "writeTextBlock produced no operators";
    ASSERT_TRUE(writer.appendContentStream(page.get(), doc.get(), opsStr))
        << "appendContentStream failed";

    fs::path out = outPath("tre_shear");
    fs::remove(out);
    doc->saveAs(out);
    page.reset();
    doc.reset();
    assertInjectedUprightUnsheared(out, U"TREMARK");
}

// ═════════════════════════════════════════════════════════════════════════════
// R-33 ROTATE class — the second half of the zero-coverage CTM class (promoted
// here from the SUCCEED()-based discovery probe to HARD assertions). Under an
// ambient "+15° rotate cm" the PageContentGuard `q … Q` bracket restores the
// page-initial identity CTM before each appended fragment, so the injected text
// lands UPRIGHT / un-rotated (top edge level). The fixture's own EDITME span —
// drawn under the live rotate — is the built-in positive control proving the
// oracle actually sees rotation (so the injected-mark check is no tautology).
// ═════════════════════════════════════════════════════════════════════════════
TEST(CtmReset, AlignmentInjectorUprightUnrotatedUnderAmbientRotate) {
    ASSERT_TRUE(fs::exists(rotateFixture())) << "fixture missing: " << rotateFixture();
    auto ctx = std::make_shared<MuPdfContext>();
    pdfe::reflow::AlignmentPipeline pipe(ctx);
    auto params = pdfe::reflow::BlockParamsBuilder::build(
        "ALIGNMARK", QRectF(72.0, 400.0, 400.0, 40.0), 18.0f,
        pdfe::reflow::Alignment::Left);
    pipe.queueAlignment(0, params);
    fs::path out = outPath("align_rotate");
    fs::remove(out);
    pipe.flush(rotateFixture(), out);
    assertInjectedUprightUnrotated(out, U"ALIGNMARK");
}

TEST(CtmReset, HeaderFooterEngineUprightUnrotatedUnderAmbientRotate) {
    ASSERT_TRUE(fs::exists(rotateFixture())) << "fixture missing: " << rotateFixture();
    auto ctx = std::make_shared<MuPdfContext>();
    pdfe::headerfooter::HeaderFooterEngine eng(ctx);
    pdfe::headerfooter::HeaderFooterConfig cfg;
    cfg.rangeMode = pdfe::headerfooter::PageRangeMode::All;
    cfg.centerHeader = "HFMARK";
    cfg.fontSize = 18.0f;
    fs::path out = outPath("hf_rotate");
    fs::remove(out);
    eng.apply(rotateFixture(), out, cfg);
    assertInjectedUprightUnrotated(out, U"HFMARK");
}

TEST(CtmReset, TreWriteTextBlockUprightUnrotatedUnderAmbientRotate) {
    const fs::path arial = "C:\\Windows\\Fonts\\arial.ttf";
    ASSERT_TRUE(fs::exists(rotateFixture())) << "fixture missing: " << rotateFixture();
    ASSERT_TRUE(fs::exists(arial)) << "arial.ttf unavailable: " << arial;

    auto ctx = std::make_shared<MuPdfContext>();
    pdfe::fonts::PdfFontEmbedder embedder(ctx.get());
    pdfe::tre::PdfContentWriter writer(ctx.get(), &embedder);
    auto doc = Document::open(ctx, rotateFixture());
    ASSERT_TRUE(doc != nullptr);
    auto page = doc->page(0);
    ASSERT_TRUE(page != nullptr);

    pdfe::tre::FontMatch fm;
    fm.fontFilePath = arial.string();
    fm.sizePt = 18.0f;
    fm.color = pdfe::tre::RgbColor{0, 0, 0};
    pdfe::tre::PdfRect rect{72.0, 300.0, 340.0, 325.0};  // fitz-space, Y-down
    std::string opsStr =
        writer.writeTextBlock(page.get(), doc.get(), "TREMARK", fm, rect);
    ASSERT_FALSE(opsStr.empty()) << "writeTextBlock produced no operators";
    ASSERT_TRUE(writer.appendContentStream(page.get(), doc.get(), opsStr))
        << "appendContentStream failed";

    fs::path out = outPath("tre_rotate");
    fs::remove(out);
    doc->saveAs(out);
    page.reset();
    doc.reset();
    assertInjectedUprightUnrotated(out, U"TREMARK");
}

// ═════════════════════════════════════════════════════════════════════════════
// R-34 MEDIABOX-OFFSET class — the imposition / N-up pattern (non-zero MediaBox
// origin). HeaderFooterEngine reads the raw MediaBox origin and offsets every
// placement, so a left footer lands ON the visible offset page (left edge at a
// small positive margin). An injector that dropped the origin would place the
// footer off the left/bottom edge. Uses the guarded HeaderFooterEngine text
// injector against the [100 100 712 892]-origin fixture.
// ═════════════════════════════════════════════════════════════════════════════
TEST(CtmReset, HeaderFooterEngineLandsOnPageUnderMediaboxOrigin) {
    ASSERT_TRUE(fs::exists(mediaboxFixture())) << "fixture missing: " << mediaboxFixture();
    auto ctx = std::make_shared<MuPdfContext>();
    pdfe::headerfooter::HeaderFooterEngine eng(ctx);
    pdfe::headerfooter::HeaderFooterConfig cfg;
    cfg.rangeMode = pdfe::headerfooter::PageRangeMode::All;
    cfg.leftFooter = "MBMARK";     // placed at (origin.x + margin, origin.y + margin)
    cfg.fontSize = 18.0f;
    fs::path out = outPath("hf_mediabox");
    fs::remove(out);
    eng.apply(mediaboxFixture(), out, cfg);
    assertInjectedOnPageAtMediaboxOffset(out, U"MBMARK");
}
