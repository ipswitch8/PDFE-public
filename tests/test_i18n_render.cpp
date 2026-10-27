// -*- coding: utf-8 -*-
//
// C-03 (coverage-expansion register) — i18n RENDERING oracles (RTL / CJK /
// combining), at the DOCUMENT+RENDER level.
//
// WHY THIS CLASS EXISTS
// ---------------------
// tests/test_textshaper_i18n.cpp already asserts the SHAPING layer
// (TextShaper / BitmapCompositor) directly — glyph clusters, joining, vertical
// advance. What was ZERO-coverage (register.json C-03) is the RENDER level: a
// real on-disk PDF page carrying i18n text, opened + rendered through PDFE's
// Document/Page path, with glyph-quad + pixel oracles. Text-extraction
// round-trips are blind to shaping / direction / mark-attachment, so "Arabic
// rendered unshaped or LTR", "CJK rendered tofu / mis-positioned", and
// "combining marks detached" all slip past every existing suite.
//
// WHAT THESE TESTS DRIVE
// ----------------------
// The fixtures (tests/fixtures/gen_i18n_render_corpus.py) bake HarfBuzz-shaped
// glyph geometry (bidi reorder + Arabic GSUB joining + GPOS mark attachment)
// into the content stream at generation time. This test reopens each fixture
// with an ISOLATED fz_context (a fresh viewer's read, exactly like
// test_content_stream_ctm_reset.cpp) and reads the baked per-glyph quads via
// fz_stext, plus renders page 0 through the real pdfe::core::Page::render path
// to measure ink. The baked positions are reader-version-independent, so PDFE's
// MuPDF 1.26.10 reproduces them.
//
// NON-VACUITY (each oracle is proven able to FAIL)
//   * RTL: the SAME monotonic-direction test is applied to a Latin control
//     ("LTR"): its logical x-centres must strictly INCREASE while Hebrew's must
//     strictly DECREASE. If Hebrew were laid out LTR it would increase and the
//     decreasing assertion would RED. Same function, opposite required result.
//   * CJK: a deliberately BLANK control band must read ~0 ink, proving the ink
//     meter is not vacuous; a dropped/tofu glyph would break the exact-codepoint
//     match or leave ~0 ink in the glyph ROI.
//   * Combining: a SPACING-accent control ("x" + U+00B4) is measured beside the
//     combining sequence ("x" + U+0301). The combining mark must attach
//     (zero-advance, over the base) while the spacing accent advances the pen —
//     a differential that a detached-mark bug flips.
//
// This is a correctness suite: a corrupt cell is a RED test, never data. Do NOT
// weaken an oracle or skip a cell to reach green.

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"

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
#include <optional>
#include <string>
#include <vector>

#ifndef TEST_FIXTURES_DIR
#define TEST_FIXTURES_DIR "."
#endif

namespace {

namespace fs = std::filesystem;
using pdfe::core::Document;
using pdfe::core::MuPdfContext;
using pdfe::core::Page;

fs::path fixture(const std::string& name) {
    return fs::path(TEST_FIXTURES_DIR) / name;
}

// One rendered glyph as MuPDF's stext reports it: its Unicode code point and
// its quad in page (fitz, Y-down) space.
struct Glyph {
    uint32_t cp = 0;
    float cx = 0, cy = 0;         // quad centre
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // quad bounds
    float width() const { return x1 - x0; }
    float height() const { return y1 - y0; }
};

// Read EVERY glyph on page 0 in content-stream order, via an isolated fz_context
// (a fresh viewer's read). Mirrors the raw-fitz reader in
// test_content_stream_ctm_reset.cpp.
std::vector<Glyph> readGlyphs(const fs::path& pdf) {
    std::vector<Glyph> out;
    fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    fz_register_document_handlers(ctx);
    fz_document* doc = nullptr;
    fz_stext_page* stext = nullptr;
    fz_try(ctx) {
        doc = fz_open_document(ctx, pdf.string().c_str());
        stext = fz_new_stext_page_from_page_number(ctx, doc, 0, nullptr);
        for (fz_stext_block* b = stext->first_block; b; b = b->next) {
            if (b->type != FZ_STEXT_BLOCK_TEXT) continue;
            for (fz_stext_line* ln = b->u.t.first_line; ln; ln = ln->next) {
                for (fz_stext_char* ch = ln->first_char; ch; ch = ch->next) {
                    const fz_quad& q = ch->quad;
                    Glyph g;
                    g.cp = static_cast<uint32_t>(ch->c);
                    g.x0 = std::min({q.ul.x, q.ur.x, q.ll.x, q.lr.x});
                    g.x1 = std::max({q.ul.x, q.ur.x, q.ll.x, q.lr.x});
                    g.y0 = std::min({q.ul.y, q.ur.y, q.ll.y, q.lr.y});
                    g.y1 = std::max({q.ul.y, q.ur.y, q.ll.y, q.lr.y});
                    // x-centre is the shaped pen span (ul->ur), robust to the
                    // quad-bound min/max above for slanted marks.
                    g.cx = 0.5f * (q.ul.x + q.ur.x);
                    g.cy = 0.5f * (q.ul.y + q.ll.y);
                    out.push_back(g);
                }
            }
        }
    }
    fz_always(ctx) {
        if (stext) fz_drop_stext_page(ctx, stext);
        if (doc) fz_drop_document(ctx, doc);
    }
    fz_catch(ctx) {}
    fz_drop_context(ctx);
    return out;
}

// Glyphs whose quad-centre y falls in [y0,y1) — used to isolate a script band.
std::vector<Glyph> band(const std::vector<Glyph>& all, float y0, float y1) {
    std::vector<Glyph> r;
    for (const auto& g : all)
        if (g.cy >= y0 && g.cy < y1) r.push_back(g);
    return r;
}

// First glyph on the page with the given code point (nullopt if absent).
std::optional<Glyph> byCp(const std::vector<Glyph>& gs, uint32_t cp) {
    for (const auto& g : gs)
        if (g.cp == cp) return g;
    return std::nullopt;
}

// Render page 0 through the real product path (Page::render), flattened onto
// white, for pixel-ink measurement.
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

// Count non-white pixels in a page-space ROI (points -> pixels via `scale`).
long inkInRoi(const QImage& im, float scale, float px0, float py0, float px1,
              float py1) {
    if (im.isNull()) return 0;
    int x0 = std::max(0, static_cast<int>(std::floor(std::min(px0, px1) * scale)));
    int y0 = std::max(0, static_cast<int>(std::floor(std::min(py0, py1) * scale)));
    int x1 = std::min(im.width() - 1, static_cast<int>(std::ceil(std::max(px0, px1) * scale)));
    int y1 = std::min(im.height() - 1, static_cast<int>(std::ceil(std::max(py0, py1) * scale)));
    long ink = 0;
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            QRgb p = im.pixel(x, y);
            if (qRed(p) < 235 || qGreen(p) < 235 || qBlue(p) < 235) ++ink;
        }
    return ink;
}

// True iff the logical-order x-centres are strictly monotonic in `dir`
// (+1 = increasing, -1 = decreasing). The core RTL/LTR discriminator.
bool strictlyMonotonic(const std::vector<float>& xs, int dir) {
    for (size_t i = 1; i < xs.size(); ++i) {
        const float d = xs[i] - xs[i - 1];
        if (dir > 0 && !(d > 0.01f)) return false;
        if (dir < 0 && !(d < -0.01f)) return false;
    }
    return true;
}

}  // namespace

// ═════════════════════════════════════════════════════════════════════════════
// C-03 RTL — Hebrew lays out RIGHT-TO-LEFT (first logical char is rightmost) and
// Arabic is contextually JOINED (shaped to presentation forms). A Latin control
// on the same page proves the direction oracle discriminates (it must increase).
// ═════════════════════════════════════════════════════════════════════════════
TEST(I18nRender, HebrewLaysOutRightToLeftAndLatinLeftToRight) {
    const fs::path pdf = fixture("i18n_rtl.pdf");
    ASSERT_TRUE(fs::exists(pdf)) << "fixture missing: " << pdf;

    const auto all = readGlyphs(pdf);
    ASSERT_FALSE(all.empty()) << "no glyphs extracted from " << pdf;

    // Hebrew "shalom" in LOGICAL order: shin, lamed, vav, final-mem.
    const std::vector<uint32_t> hebLogical = {0x05E9, 0x05DC, 0x05D5, 0x05DD};
    std::vector<float> hebX;
    for (uint32_t cp : hebLogical) {
        auto g = byCp(all, cp);
        ASSERT_TRUE(g.has_value())
            << "Hebrew code point U+" << std::hex << cp
            << " not found in rendered page — text dropped or not embedded";
        hebX.push_back(g->cx);
    }
    // RTL: reading the codepoints in LOGICAL order, their on-page x-centres must
    // STRICTLY DECREASE (first logical char is the rightmost). This is the
    // definitive right-to-left layout proof and is independent of the order
    // stext arrays the glyphs in.
    EXPECT_TRUE(strictlyMonotonic(hebX, /*dir=*/-1))
        << "Hebrew did not lay out right-to-left: logical-order x-centres "
        << "[" << hebX[0] << "," << hebX[1] << "," << hebX[2] << "," << hebX[3]
        << "] must strictly DECREASE. Ascending => rendered LTR (unshaped bidi).";

    // Latin control "LTR" in logical order: L, T, R -> x must strictly INCREASE.
    // Same function, opposite required result => the RTL oracle is non-vacuous.
    const std::vector<uint32_t> latLogical = {0x004C, 0x0054, 0x0052};
    std::vector<float> latX;
    for (uint32_t cp : latLogical) {
        auto g = byCp(all, cp);
        ASSERT_TRUE(g.has_value()) << "Latin control glyph U+" << std::hex << cp
                                   << " missing";
        latX.push_back(g->cx);
    }
    EXPECT_TRUE(strictlyMonotonic(latX, /*dir=*/+1))
        << "Latin control must lay out left-to-right (x strictly increasing); if "
           "this fails the direction oracle is miscalibrated, not the RTL case.";
}

TEST(I18nRender, ArabicIsContextuallyJoinedNotIsolated) {
    const fs::path pdf = fixture("i18n_rtl.pdf");
    ASSERT_TRUE(fs::exists(pdf)) << "fixture missing: " << pdf;

    const auto all = readGlyphs(pdf);
    // The Arabic word sits on the middle band (y ~110..142 in page space).
    const auto ar = band(all, 100.0f, 170.0f);
    ASSERT_GE(ar.size(), 4u) << "Arabic band has too few glyphs";

    // Contextual joining maps the base letters (U+0620..U+06FF) to Arabic
    // Presentation-Form-B glyphs (U+FB50..U+FEFF). If shaping were skipped the
    // glyphs would carry the ISOLATED base code points (U+06xx) instead. Require
    // the majority of the band to be presentation forms — proof of GSUB joining.
    int presentation = 0, isolatedBase = 0;
    for (const auto& g : ar) {
        if (g.cp >= 0xFB50 && g.cp <= 0xFEFF) ++presentation;
        else if (g.cp >= 0x0620 && g.cp <= 0x06FF) ++isolatedBase;
    }
    EXPECT_GE(presentation, 3)
        << "Arabic was not contextually joined: expected Presentation-Form-B "
           "glyphs (U+FB50..U+FEFF) but saw " << presentation << " (isolated "
           "base forms: " << isolatedBase << "). Isolated forms => unshaped.";

    // And the shaped Arabic run must lay out right-to-left too: descending
    // x-centres across the band in stext order.
    std::vector<float> arX;
    for (const auto& g : ar) arX.push_back(g.cx);
    // stext returns the band in logical order for this fixture; require the run
    // to trend right-to-left (rightmost glyph well right of the leftmost).
    const float span = arX.front() - arX.back();
    EXPECT_GT(span, 20.0f)
        << "Arabic run does not span right-to-left (front x=" << arX.front()
        << " back x=" << arX.back() << ")";
}

// ═════════════════════════════════════════════════════════════════════════════
// C-03 CJK — Chinese glyphs render as real, full-width ideographs with genuine
// interior ink (not tofu / dropped), and a blank control band proves the ink
// meter is non-vacuous.
// ═════════════════════════════════════════════════════════════════════════════
TEST(I18nRender, CjkGlyphsRenderRealFullWidthInkNotTofu) {
    const fs::path pdf = fixture("i18n_cjk.pdf");
    ASSERT_TRUE(fs::exists(pdf)) << "fixture missing: " << pdf;

    const auto all = readGlyphs(pdf);
    // "zhong wen ce shi" in reading order.
    const std::vector<uint32_t> cjkLogical = {0x4E2D, 0x6587, 0x6D4B, 0x8BD5};

    std::vector<Glyph> cjk;
    for (uint32_t cp : cjkLogical) {
        auto g = byCp(all, cp);
        ASSERT_TRUE(g.has_value())
            << "CJK code point U+" << std::hex << cp << " not extracted — the "
               "glyph was dropped or ToUnicode is broken (tofu class)";
        cjk.push_back(*g);
    }

    // Geometry: each ideograph is a FULL-WIDTH SQUARE (w ~= h) of a real size.
    // A collapsed/space glyph would be zero-width; a Latin fallback would be
    // narrow (w << h).
    for (const auto& g : cjk) {
        ASSERT_GT(g.height(), 12.0f) << "CJK glyph unrealistically short";
        const float ratio = g.width() / g.height();
        EXPECT_GT(ratio, 0.75f)
            << "CJK glyph not full-width (w/h=" << ratio << ") — collapsed or "
               "Latin-fallback rather than an ideograph";
        EXPECT_LT(ratio, 1.30f) << "CJK glyph unexpectedly wide (w/h=" << ratio << ")";
    }

    // Reading order: x-centres strictly increase (horizontal LTR).
    std::vector<float> xs;
    for (const auto& g : cjk) xs.push_back(g.cx);
    EXPECT_TRUE(strictlyMonotonic(xs, /*dir=*/+1))
        << "CJK glyphs are not in left-to-right reading order";

    // Ink: each ideograph must leave substantial interior ink (real strokes),
    // and a BLANK control band must be essentially empty (meter non-vacuity).
    const float scale = 3.0f;
    QImage im = renderPage0(pdf, scale);
    ASSERT_FALSE(im.isNull()) << "page 0 failed to render";

    for (const auto& g : cjk) {
        const long ink = inkInRoi(im, scale, g.x0, g.y0, g.x1, g.y1);
        const float area = std::max(1.0f, g.width() * g.height() * scale * scale);
        const float frac = static_cast<float>(ink) / area;
        EXPECT_GT(frac, 0.05f)
            << "CJK glyph U+" << std::hex << g.cp << " rendered near-empty (ink "
               "fraction " << std::dec << frac << ") — tofu / dropped glyph";
    }

    // Blank control band: the lower half of the page carries no text. Its ink
    // must be ~0, proving the ink meter above actually measures glyph strokes
    // rather than reporting ink everywhere.
    const long blankInk = inkInRoi(im, scale, 40.0f, 150.0f, 360.0f, 290.0f);
    EXPECT_LT(blankInk, 40)
        << "blank control band is not empty (ink=" << blankInk
        << ") — the ink meter is vacuous";
}

// ═════════════════════════════════════════════════════════════════════════════
// C-03 combining — a combining mark ATTACHES to its base (zero-advance, centred
// over the base) while a spacing accent ADVANCES the pen. The two are measured
// side by side so the attachment is proven by a differential, not an absolute.
// ═════════════════════════════════════════════════════════════════════════════
TEST(I18nRender, CombiningMarkAttachesWhileSpacingAccentAdvances) {
    const fs::path pdf = fixture("i18n_combining.pdf");
    ASSERT_TRUE(fs::exists(pdf)) << "fixture missing: " << pdf;

    const auto all = readGlyphs(pdf);

    // Combining band (upper): base 'x', U+0301 COMBINING ACUTE, 'N'.
    const auto up = band(all, 20.0f, 100.0f);
    // Spacing band (lower): base 'x', U+00B4 ACUTE ACCENT, 'N'.
    const auto lo = band(all, 100.0f, 200.0f);

    auto baseUp = byCp(up, 0x0078);
    auto mark = byCp(up, 0x0301);
    auto nUp = byCp(up, 0x004E);
    auto baseLo = byCp(lo, 0x0078);
    auto accent = byCp(lo, 0x00B4);
    auto nLo = byCp(lo, 0x004E);

    ASSERT_TRUE(baseUp && mark && nUp)
        << "combining band missing a glyph (base/mark/N) — the combining cluster "
           "was normalised away or not extracted";
    ASSERT_TRUE(baseLo && accent && nLo)
        << "spacing control band missing a glyph (base/accent/N)";

    // (1) ATTACHMENT: the combining mark's x-centre falls WITHIN the base
    // glyph's x-span — it sits ON the base, not detached beside it.
    EXPECT_GE(mark->cx, baseUp->x0 - 0.5f);
    EXPECT_LE(mark->cx, baseUp->x1 + 0.5f)
        << "combining mark U+0301 (cx=" << mark->cx << ") is not over its base "
           "'x' [" << baseUp->x0 << "," << baseUp->x1 << "] — mark DETACHED";

    // (2) ZERO-ADVANCE: the base+mark pair advances by only the base's width, so
    // the following 'N' begins at the base's right edge (the mark consumed no
    // horizontal space).
    EXPECT_NEAR(nUp->x0, baseUp->x1, 3.0f)
        << "combining mark advanced the pen (N at x0=" << nUp->x0 << ", base right "
           "edge=" << baseUp->x1 << ") — a combining mark must be zero-advance";

    // (3) DIFFERENTIAL / non-vacuity: the SPACING accent does the opposite —
    // it sits to the RIGHT of the base (no overlap) and DOES advance the pen, so
    // its 'N' is pushed clearly further right than the combining case's 'N'.
    EXPECT_GE(accent->cx, baseLo->x1 - 0.5f)
        << "spacing accent U+00B4 overlaps its base — control is miscalibrated";
    EXPECT_GT(nLo->x0 - nUp->x0, 8.0f)
        << "spacing accent did not advance the pen more than the combining mark "
           "(spacing N x0=" << nLo->x0 << ", combining N x0=" << nUp->x0 << "); "
           "the two cases are indistinguishable, so the attachment check is "
           "vacuous.";

    // (4) The base+mark cluster still leaves real ink over the base (the mark
    // rendered, not swallowed).
    const float scale = 3.0f;
    QImage im = renderPage0(pdf, scale);
    ASSERT_FALSE(im.isNull());
    const long ink = inkInRoi(im, scale, baseUp->x0, baseUp->y0, baseUp->x1,
                              baseUp->y1);
    EXPECT_GT(ink, 60) << "combining base+mark left no ink";
}
