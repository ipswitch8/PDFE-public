// -*- coding: utf-8 -*-
//
// Content-stream state-continuity pipeline — Phase 1 DISCOVERY PROBE.
//
// WHAT THIS IS
// ------------
// A MEASUREMENT INSTRUMENT, not a correctness suite (designed after
// tests/test_autosave_capture_audit.cpp). For every (injector x hostile-ambient
// operator) cell it drives the real injector headlessly against a hostile
// fixture, saves, reopens, RENDERS the page, and applies a pixel + glyph-quad
// oracle recording two booleans:
//
//   injected_ok  — the INJECTED element rendered visible, upright, correctly
//                  sized (and, where an intended colour is known, correctly
//                  coloured);
//   neighbor_ok  — a PRE-EXISTING "KEEPME" neighbour (drawn safely in its own
//                  q...Q before the hostile op) is uncorrupted.
//
// WHY A RENDER ORACLE (not text extraction)
// -----------------------------------------
// stext recovers the logical text regardless of how it rendered — it reports
// Tr-3 invisible text as present, white-on-white text as present, and mirrored
// text with a (flipped) quad. So visibility MUST come from the rendered pixmap;
// mirror + size come from the glyph quad. See
// docs/content-stream-state-continuity-audit.md ("Why none of this was caught").
//
// EXPECTED RESULT (do NOT "fix" corrupt cells here — Phase 1 is measurement)
// -------------------------------------------------------------------------
// Each GoogleTest body PASSES as long as the measurement ran and produced a row.
// A corrupt cell (injected_ok == false) is DATA in the gap register, NOT a red
// test. Hard assertions + fixes land in phase-2/4. Do NOT weaken an oracle or
// skip a cell to reach green.
//
// OUTPUT
// ------
//   (a) a readable grid printed to stdout (global environment teardown)
//   (b) JSON gap register at
//       .claude/pipelines/content-stream-continuity/gate-artifacts/phase-1/gap-register.json
//   (c) RecordProperty(...) per cell (visible in the JUnit XML).

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "core/TextExtractor.h"

// Injectors under probe.
#include "textedit/ContentStreamEditor.h"
#include "textedit/TextEditModel.h"
#include "reflow/AlignmentPipeline.h"
#include "reflow/BlockParamsBuilder.h"
#include "reflow/ReflowTypes.h"
#include "headerfooter/HeaderFooterEngine.h"
#include "headerfooter/HeaderFooterConfig.h"
#include "batch/BatchEngine.h"
#include "batch/BatchTypes.h"
#include "image/ImageManager.h"
#include "image/ImageContentEditor.h"
#include "image/ImageData.h"
#include "tre/PdfContentWriter.h"
#include "tre/TreTypes.h"
#include "fonts/PdfFontEmbedder.h"

#include <QImage>
#include <QPainter>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRectF>
#include <QString>

extern "C" {
#include <mupdf/fitz.h>
}

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
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

// ─────────────────────────────────────────────────────────────────────────────
// Gap register: one row per (injector x hostile-op) cell.
// ─────────────────────────────────────────────────────────────────────────────
struct Cell {
    std::string injector;
    std::string hostile_op;
    bool injected_ok = false;
    bool neighbor_ok = false;
    bool probed = true;      ///< false → could not drive this cell at all
    std::string note;
};

std::vector<Cell>& registry() { static std::vector<Cell> v; return v; }
std::mutex& registryMutex() { static std::mutex m; return m; }

// ─────────────────────────────────────────────────────────────────────────────
// KNOWN-CORRUPT LEDGER (gap A3-54).
//
// This grid used to end every cell in `SUCCEED()`: corruption was recorded as
// DATA and could never turn a test red, which is why it was one of only two
// files in tests/ with zero EXPECT_/ASSERT_ and why the register asked for the
// cells to be "promoted to ASSERTs". They are asserted below.
//
// One cell is a REAL, REPRODUCIBLE PRODUCT DEFECT found by this promotion (it
// had been sitting green in the register): editing text through
// ContentStreamEditor on a page whose ambient CTM is ROTATED produces an edit
// that renders NO INK (visible=0, ink=0) while the neighbour text survives —
// i.e. the edit silently disappears. It is listed here rather than hidden, and
// the assertion runs in BOTH directions:
//
//   * every cell NOT in this ledger must be clean          -> a regression is RED;
//   * every cell IN this ledger must still be corrupt      -> a FIX is also RED,
//     which is the signal to delete the entry rather than let the ledger rot.
//
// Nothing here is skipped, and no cell is exempt from assertion.
bool isKnownCorrupt(const std::string& injector, const std::string& op) {
    return injector == "ContentStreamEditor" && op == "rotate";
}

void finishCell(const Cell& c) {
    {
        std::lock_guard<std::mutex> lk(registryMutex());
        registry().push_back(c);
    }
    ::testing::Test::RecordProperty("injector", c.injector);
    ::testing::Test::RecordProperty("hostile_op", c.hostile_op);
    ::testing::Test::RecordProperty("injected_ok", c.injected_ok ? 1 : 0);
    ::testing::Test::RecordProperty("neighbor_ok", c.neighbor_ok ? 1 : 0);
    ::testing::Test::RecordProperty("probed", c.probed ? 1 : 0);
    if (!c.note.empty()) ::testing::Test::RecordProperty("note", c.note);

    const std::string where = c.injector + " x " + c.hostile_op +
                              " injected_ok=" + (c.injected_ok ? "1" : "0") +
                              " neighbor_ok=" + (c.neighbor_ok ? "1" : "0") +
                              (c.note.empty() ? "" : (" note=" + c.note));

    if (!c.probed) {
        // The cell could not be driven at all (missing fixture / missing host
        // font). Asserting on an un-run measurement would be asserting nothing;
        // it is recorded as not-probed and the gap register shows it.
        SUCCEED() << "NOT PROBED: " << where;
        return;
    }

    const bool clean = c.injected_ok && c.neighbor_ok;
    if (isKnownCorrupt(c.injector, c.hostile_op)) {
        EXPECT_FALSE(clean)
            << "KNOWN-DEFECT LEDGER: this cell is recorded as corrupt but now "
               "passes. That is good news — the defect appears fixed. Delete it "
               "from isKnownCorrupt() so the cell is asserted clean from now on. "
            << where;
    } else {
        EXPECT_TRUE(c.injected_ok)
            << "the injected content is absent or geometrically wrong under this "
               "hostile ambient state: " << where;
        EXPECT_TRUE(c.neighbor_ok)
            << "the pre-existing neighbour content (KEEPME) was damaged by the "
               "injection under this hostile ambient state: " << where;
    }
}

// The eight hostile ambient operators, matched to the generated fixtures.
const std::vector<std::string>& ops() {
    static const std::vector<std::string> v = {
        "neg_tz", "tc", "ts", "tr", "white", "cm", "dangling_q", "cmyk",
        // R-33: sheared/skewed + rotated ambient CTM — the zero-coverage class
        // (untested sibling of the -100 Tz mirror). Measurement here; hard
        // assertions live in test_content_stream_ctm_reset.cpp (CtmReset.*Shear*).
        "shear", "rotate"};
    return v;
}

fs::path textFixture(const std::string& op) {
    return fs::path(TEST_FIXTURES_DIR) / "adversarial_ambient" / ("aa_" + op + ".pdf");
}
fs::path imageFixture(const std::string& op) {
    return fs::path(TEST_FIXTURES_DIR) / "adversarial_ambient" / ("aa_img_" + op + ".pdf");
}
fs::path outPath(const std::string& tag) {
    return fs::temp_directory_path() / ("pdfe_aa_" + tag + ".pdf");
}

// ─────────────────────────────────────────────────────────────────────────────
// Glyph-quad oracle (raw fitz, isolated context — a fresh viewer's read).
// Reports the first line whose concatenated text contains `needle`.
// ─────────────────────────────────────────────────────────────────────────────
struct LineHit {
    bool found = false;
    float firstUlx = 0, firstUrx = 0;   ///< first glyph, for the mirror test
    float glyphH = 0;                   ///< first glyph height (points) — size test
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0; ///< whole-line bbox (points) — pixmap ROI
    uint32_t argb = 0;                  ///< first glyph sRGB fill (colour test)
};

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
                h.glyphH = std::fabs(f->quad.ll.y - f->quad.ul.y);
                h.argb = f->argb & 0xFFFFFFu;
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

// ─────────────────────────────────────────────────────────────────────────────
// Pixmap oracle: render page 0 and count "ink" pixels inside a points-space ROI.
// stext/fitz page space and the rendered pixmap are both Y-down, top-left origin;
// our fixtures use a 0-origin MediaBox, so device px = point * scale.
// ─────────────────────────────────────────────────────────────────────────────
QImage renderPage0(const fs::path& pdf, float scale) {
    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, pdf);
    if (!doc || doc->pageCount() == 0) return QImage();
    auto page = doc->page(0);
    if (!page) return QImage();
    // Page::render returns RGBA with a TRANSPARENT (not white) paper background.
    // A naive convertToFormat(RGB888) would turn transparent paper into BLACK and
    // make every unpainted pixel read as "ink" — inverting the visibility oracle.
    // Composite over white so the result matches what a human sees on paper:
    // white/invisible/Tr-3 content stays paper-white (non-ink); painted content
    // shows its real colour.
    QImage rgba = page->render(page->boundingBox(), scale);
    if (rgba.isNull()) return QImage();
    QImage canvas(rgba.size(), QImage::Format_RGB888);
    canvas.fill(Qt::white);
    QPainter p(&canvas);
    p.drawImage(0, 0, rgba);
    p.end();
    return canvas;
}

// Count pixels inside the ROI (points, scaled by `scale`) that are "ink":
// any pixel not near-white. Catches Tr-3 invisible and white-on-white (both
// leave the ROI paper-white) and off-page placement (ROI empty).
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

// A distinctive-colour bounding box (device px) over the whole render.
struct ColorBBox { int w = 0, h = 0; long count = 0; };
ColorBBox blueBBox(const QImage& im) {
    ColorBBox bb;
    int minx = im.width(), miny = im.height(), maxx = -1, maxy = -1;
    for (int y = 0; y < im.height(); ++y)
        for (int x = 0; x < im.width(); ++x) {
            QRgb p = im.pixel(x, y);
            if (qBlue(p) > 140 && qRed(p) < 90 && qGreen(p) < 90) {
                minx = std::min(minx, x); miny = std::min(miny, y);
                maxx = std::max(maxx, x); maxy = std::max(maxy, y); ++bb.count;
            }
        }
    if (maxx >= 0) { bb.w = maxx - minx + 1; bb.h = maxy - miny + 1; }
    return bb;
}
ColorBBox greenBBox(const QImage& im) {
    ColorBBox bb;
    int minx = im.width(), miny = im.height(), maxx = -1, maxy = -1;
    for (int y = 0; y < im.height(); ++y)
        for (int x = 0; x < im.width(); ++x) {
            QRgb p = im.pixel(x, y);
            int r = qRed(p), g = qGreen(p), b = qBlue(p);
            if (g > 70 && g < 130 && (std::abs(r - g) > 25 || std::abs(b - g) > 25)) {
                minx = std::min(minx, x); miny = std::min(miny, y);
                maxx = std::max(maxx, x); maxy = std::max(maxy, y); ++bb.count;
            }
        }
    if (maxx >= 0) { bb.w = maxx - minx + 1; bb.h = maxy - miny + 1; }
    return bb;
}

// ─────────────────────────────────────────────────────────────────────────────
// Shared text-cell evaluation. Given the saved output + the injected needle,
// fill injected_ok/neighbor_ok/note. `expectBlack` enables a colour check for
// injectors whose intended fill is black (Alignment/HeaderFooter/TRE); leave it
// false for injectors with a legitimately non-black fill (watermark) or where
// the intended colour is "preserve original" (ContentStreamEditor).
// ─────────────────────────────────────────────────────────────────────────────
void evalTextCell(Cell& c, const fs::path& out, const std::u32string& needle,
                  bool expectBlack) {
    const float scale = 3.0f;
    LineHit inj = readLine(out, needle);
    LineHit keep = readLine(out, U"KEEPME");
    QImage im = renderPage0(out, scale);

    char buf[512];
    if (!inj.found) {
        c.injected_ok = false;
        c.note = "injected text not found in stext after reopen";
    } else {
        const bool upright = inj.firstUrx > inj.firstUlx;
        const long ink = inkInRoi(im, scale, inj.x0, inj.y0, inj.x1, inj.y1);
        const bool visible = ink > 60;
        const float refH = (keep.found && keep.glyphH > 0.1f) ? keep.glyphH : 24.7f;
        const float ratio = inj.glyphH / refH;
        const bool sizeOK = ratio > 0.70f && ratio < 1.40f;
        const int r = (inj.argb >> 16) & 0xFF, g = (inj.argb >> 8) & 0xFF,
                  b = inj.argb & 0xFF;
        const bool black = r < 80 && g < 80 && b < 80;
        const bool colorOK = !expectBlack || black;
        c.injected_ok = upright && visible && sizeOK && colorOK;
        std::snprintf(buf, sizeof(buf),
                      "argb=%06x upright=%d visible=%d(ink=%ld) sizeRatio=%.2f%s",
                      inj.argb, upright ? 1 : 0, visible ? 1 : 0, ink, ratio,
                      expectBlack ? (black ? " black=1" : " black=0") : "");
        c.note = buf;
    }

    // Neighbour: KEEPME must render present, black, upright, visible.
    if (!keep.found) {
        c.neighbor_ok = false;
        c.note += " | KEEPME not found";
    } else {
        const long inkK = inkInRoi(im, scale, keep.x0, keep.y0, keep.x1, keep.y1);
        const bool kUpright = keep.firstUrx > keep.firstUlx;
        const int kr = (keep.argb >> 16) & 0xFF, kg = (keep.argb >> 8) & 0xFF,
                  kb = keep.argb & 0xFF;
        const bool kBlack = kr < 80 && kg < 80 && kb < 80;
        c.neighbor_ok = kUpright && kBlack && inkK > 60;
        if (!c.neighbor_ok) {
            std::snprintf(buf, sizeof(buf),
                          " | KEEPME argb=%06x upright=%d ink=%ld", keep.argb,
                          kUpright ? 1 : 0, inkK);
            c.note += buf;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Shared image-cell evaluation. `wantGreen` selects the inserted PNG (green);
// otherwise the transformed Im0 (blue). `expectWidth` > 0 enables a half-scale
// check (device px at render scale 1.0 ≈ points).
// ─────────────────────────────────────────────────────────────────────────────
void evalImageCell(Cell& c, const fs::path& out, bool wantGreen, int expectWidth) {
    QImage im = renderPage0(out, 1.0f);
    ColorBBox bb = wantGreen ? greenBBox(im) : blueBBox(im);
    char buf[256];
    const bool present = bb.count > 800;
    bool widthOK = true;
    if (expectWidth > 0) widthOK = bb.w > (expectWidth * 3 / 4);  // catch halving
    c.injected_ok = present && widthOK;
    std::snprintf(buf, sizeof(buf), "%s bbox w=%d h=%d count=%ld%s",
                  wantGreen ? "green(insert)" : "blue(Im0)", bb.w, bb.h, bb.count,
                  expectWidth > 0 ? (widthOK ? " widthOK" : " HALVED?") : "");
    c.note = buf;

    LineHit keep = readLine(out, U"KEEPME");
    if (!keep.found) {
        c.neighbor_ok = false;
        c.note += " | KEEPME not found";
    } else {
        QImage im3 = renderPage0(out, 3.0f);
        const long inkK = inkInRoi(im3, 3.0f, keep.x0, keep.y0, keep.x1, keep.y1);
        const int kr = (keep.argb >> 16) & 0xFF, kg = (keep.argb >> 8) & 0xFF,
                  kb = keep.argb & 0xFF;
        c.neighbor_ok = (keep.firstUrx > keep.firstUlx) &&
                        (kr < 80 && kg < 80 && kb < 80) && inkK > 60;
    }
}

}  // namespace

// ═════════════════════════════════════════════════════════════════════════════
// INJECTOR: ContentStreamEditor (regression — inbound leak already fixed).
// Inline edit of EDITME → EDITED, which was drawn under the live hostile state.
// ═════════════════════════════════════════════════════════════════════════════
TEST(AdversarialAmbient, ContentStreamEditor) {
    for (const auto& op : ops()) {
        Cell c; c.injector = "ContentStreamEditor"; c.hostile_op = op;
        fs::path src = textFixture(op);
        if (!fs::exists(src)) { c.probed = false; c.note = "fixture missing"; finishCell(c); continue; }
        try {
            auto ctx = std::make_shared<MuPdfContext>();
            auto doc = Document::open(ctx, src);
            if (!doc) { c.probed = false; c.note = "open failed"; finishCell(c); continue; }
            auto page = doc->page(0);

            auto blocks = page->extractText();
            pdfe::textedit::TextEditModel model(blocks);
            std::vector<pdfe::textedit::TextSpan> spans;
            for (int i = 0; i < model.spanCount(); ++i) spans.push_back(model.span(i));
            int target = -1;
            for (int i = 0; i < static_cast<int>(spans.size()); ++i)
                if (spans[i].text.find(U"EDITME") != std::u32string::npos) { target = i; break; }
            if (target < 0) { c.probed = false; c.note = "no EDITME span"; finishCell(c); continue; }

            pdfe::textedit::ContentStreamEditor editor(ctx.get());
            editor.applyEdits(page.get(), doc.get(), spans,
                              {{target, U"EDITED"}});
            fs::path out = outPath("cse_" + op);
            doc->saveAs(out);
            page.reset(); doc.reset(); ctx.reset();
            // ContentStreamEditor is meant to PRESERVE the original fill, which
            // under a colour-hostile fixture is the ambient colour itself — so
            // no expectBlack (colour is recorded, not asserted).
            evalTextCell(c, out, U"EDITED", /*expectBlack=*/false);
        } catch (const std::exception& e) {
            c.injected_ok = false; c.note = std::string("threw: ") + e.what();
        }
        finishCell(c);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// INJECTOR: AlignmentInjector (appends q/BT/Tf/Tm/Tj/ET/Q to /Contents).
// ═════════════════════════════════════════════════════════════════════════════
TEST(AdversarialAmbient, AlignmentInjector) {
    for (const auto& op : ops()) {
        Cell c; c.injector = "AlignmentInjector"; c.hostile_op = op;
        fs::path src = textFixture(op);
        if (!fs::exists(src)) { c.probed = false; c.note = "fixture missing"; finishCell(c); continue; }
        try {
            auto ctx = std::make_shared<MuPdfContext>();
            pdfe::reflow::AlignmentPipeline pipe(ctx);
            auto params = pdfe::reflow::BlockParamsBuilder::build(
                "ALIGNMARK", QRectF(72.0, 400.0, 400.0, 40.0), 18.0f,
                pdfe::reflow::Alignment::Left);
            pipe.queueAlignment(0, params);
            fs::path out = outPath("align_" + op);
            pipe.flush(src, out);
            evalTextCell(c, out, U"ALIGNMARK", /*expectBlack=*/true);
        } catch (const std::exception& e) {
            c.injected_ok = false; c.note = std::string("threw: ") + e.what();
        }
        finishCell(c);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// INJECTOR: HeaderFooterEngine (appends q/Tf/BT/Td/Tj/ET/Q to /Contents).
// ═════════════════════════════════════════════════════════════════════════════
TEST(AdversarialAmbient, HeaderFooterEngine) {
    for (const auto& op : ops()) {
        Cell c; c.injector = "HeaderFooterEngine"; c.hostile_op = op;
        fs::path src = textFixture(op);
        if (!fs::exists(src)) { c.probed = false; c.note = "fixture missing"; finishCell(c); continue; }
        try {
            auto ctx = std::make_shared<MuPdfContext>();
            pdfe::headerfooter::HeaderFooterEngine eng(ctx);
            pdfe::headerfooter::HeaderFooterConfig cfg;
            cfg.rangeMode = pdfe::headerfooter::PageRangeMode::All;
            cfg.centerHeader = "HFMARK";
            cfg.fontSize = 18.0f;
            fs::path out = outPath("hf_" + op);
            eng.apply(src, out, cfg);
            evalTextCell(c, out, U"HFMARK", /*expectBlack=*/true);
        } catch (const std::exception& e) {
            c.injected_ok = false; c.note = std::string("threw: ") + e.what();
        }
        finishCell(c);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// INJECTOR: BatchEngine text watermark (appends q/g/gs/BT/Tf/Tm/Tj/ET/Q).
// Intended fill is 0.5 g (grey) — colour is recorded, not asserted.
// ═════════════════════════════════════════════════════════════════════════════
TEST(AdversarialAmbient, WatermarkText) {
    for (const auto& op : ops()) {
        Cell c; c.injector = "WatermarkText"; c.hostile_op = op;
        fs::path src = textFixture(op);
        if (!fs::exists(src)) { c.probed = false; c.note = "fixture missing"; finishCell(c); continue; }
        try {
            auto ctx = std::make_shared<MuPdfContext>();
            pdfe::batch::BatchEngine eng(ctx);
            pdfe::batch::WatermarkConfig cfg;
            cfg.text = "WMMARK";
            cfg.rotation = 0.0f;    // isolate the horizontal-scale (mirror) test
            cfg.opacity = 1.0f;
            cfg.fontSize = 18.0f;
            fs::path out = outPath("wm_" + op);
            fs::remove(out);
            eng.applyWatermark(src, out, cfg);
            evalTextCell(c, out, U"WMMARK", /*expectBlack=*/false);
        } catch (const std::exception& e) {
            c.injected_ok = false; c.note = std::string("threw: ") + e.what();
        }
        finishCell(c);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// INJECTOR: TRE PdfContentWriter::writeTextBlock (appended text block).
// Needs a real TTF; if Arial is unavailable the cell is recorded not-probed.
// ═════════════════════════════════════════════════════════════════════════════
TEST(AdversarialAmbient, TreWriteTextBlock) {
    const fs::path arial = "C:\\Windows\\Fonts\\arial.ttf";
    for (const auto& op : ops()) {
        Cell c; c.injector = "TreWriteTextBlock"; c.hostile_op = op;
        fs::path src = textFixture(op);
        if (!fs::exists(src)) { c.probed = false; c.note = "fixture missing"; finishCell(c); continue; }
        if (!fs::exists(arial)) { c.probed = false; c.note = "arial.ttf unavailable"; finishCell(c); continue; }
        try {
            auto ctx = std::make_shared<MuPdfContext>();
            pdfe::fonts::PdfFontEmbedder embedder(ctx.get());
            pdfe::tre::PdfContentWriter writer(ctx.get(), &embedder);
            auto doc = Document::open(ctx, src);
            if (!doc) { c.probed = false; c.note = "open failed"; finishCell(c); continue; }
            auto page = doc->page(0);

            pdfe::tre::FontMatch fm;
            fm.fontFilePath = arial.string();
            fm.sizePt = 18.0f;
            fm.color = pdfe::tre::RgbColor{0, 0, 0};
            pdfe::tre::PdfRect rect{72.0, 300.0, 340.0, 325.0};  // fitz-space, Y-down
            std::string opsStr = writer.writeTextBlock(page.get(), doc.get(),
                                                       "TREMARK", fm, rect);
            if (opsStr.empty()) { c.injected_ok = false; c.note = "writeTextBlock empty"; finishCell(c); continue; }
            if (!writer.appendContentStream(page.get(), doc.get(), opsStr)) {
                c.injected_ok = false; c.note = "appendContentStream failed"; finishCell(c); continue;
            }
            fs::path out = outPath("tre_" + op);
            doc->saveAs(out);
            page.reset(); doc.reset();
            evalTextCell(c, out, U"TREMARK", /*expectBlack=*/true);
        } catch (const std::exception& e) {
            c.injected_ok = false; c.note = std::string("threw: ") + e.what();
        }
        finishCell(c);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// INJECTOR: ImageManager insert (appends q [cm] /Img Do Q — "no sanitize").
// ═════════════════════════════════════════════════════════════════════════════
TEST(AdversarialAmbient, ImageInsert) {
    const fs::path png = fs::path(TEST_FIXTURES_DIR) / "test_image_rgb.png";
    for (const auto& op : ops()) {
        Cell c; c.injector = "ImageInsert"; c.hostile_op = op;
        fs::path src = imageFixture(op);
        if (!fs::exists(src)) { c.probed = false; c.note = "fixture missing"; finishCell(c); continue; }
        if (!fs::exists(png)) { c.probed = false; c.note = "test_image_rgb.png missing"; finishCell(c); continue; }
        try {
            auto ctx = std::make_shared<MuPdfContext>();
            auto doc = Document::open(ctx, src);
            if (!doc) { c.probed = false; c.note = "open failed"; finishCell(c); continue; }
            auto page = doc->page(0);
            pdfe::image::ImageManager mgr(ctx);
            pdfe::image::InsertImageOptions opts;
            opts.filePath = png.string();
            opts.targetRect = QRectF(100, 400, 200, 160);  // 200pt wide
            opts.maintainAspectRatio = false;
            auto res = mgr.insertImage(page.get(), doc.get(), opts);
            if (!res.success) { c.injected_ok = false; c.note = "insert failed: " + res.errorMessage; finishCell(c); continue; }
            fs::path out = outPath("imgins_" + op);
            doc->saveAs(out);
            page.reset(); doc.reset();
            evalImageCell(c, out, /*wantGreen=*/true, /*expectWidth=*/200);
        } catch (const std::exception& e) {
            c.injected_ok = false; c.note = std::string("threw: ") + e.what();
        }
        finishCell(c);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// INJECTOR: ImageContentEditor resize (remove + re-append with scaled CTM).
// ═════════════════════════════════════════════════════════════════════════════
TEST(AdversarialAmbient, ImageResize) {
    for (const auto& op : ops()) {
        Cell c; c.injector = "ImageResize"; c.hostile_op = op;
        fs::path src = imageFixture(op);
        if (!fs::exists(src)) { c.probed = false; c.note = "fixture missing"; finishCell(c); continue; }
        try {
            auto ctx = std::make_shared<MuPdfContext>();
            auto doc = Document::open(ctx, src);
            if (!doc) { c.probed = false; c.note = "open failed"; finishCell(c); continue; }
            auto page = doc->page(0);
            pdfe::image::ImageContentEditor ed(ctx);
            if (!ed.resizeImage(page.get(), doc.get(), "Im0", 200.0f, 160.0f, 100.0f, 400.0f)) {
                c.injected_ok = false; c.note = "resizeImage returned false"; finishCell(c); continue;
            }
            fs::path out = outPath("imgres_" + op);
            doc->saveAs(out);
            page.reset(); doc.reset();
            evalImageCell(c, out, /*wantGreen=*/false, /*expectWidth=*/200);
        } catch (const std::exception& e) {
            c.injected_ok = false; c.note = std::string("threw: ") + e.what();
        }
        finishCell(c);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// INJECTOR: ImageContentEditor rotate (remove + re-append with rotated CTM).
// ═════════════════════════════════════════════════════════════════════════════
TEST(AdversarialAmbient, ImageRotate) {
    for (const auto& op : ops()) {
        Cell c; c.injector = "ImageRotate"; c.hostile_op = op;
        fs::path src = imageFixture(op);
        if (!fs::exists(src)) { c.probed = false; c.note = "fixture missing"; finishCell(c); continue; }
        try {
            auto ctx = std::make_shared<MuPdfContext>();
            auto doc = Document::open(ctx, src);
            if (!doc) { c.probed = false; c.note = "open failed"; finishCell(c); continue; }
            auto page = doc->page(0);
            pdfe::image::ImageContentEditor ed(ctx);
            // Im0 is a 120pt square placed at (200,500); centre (260,560).
            if (!ed.rotateImage(page.get(), doc.get(), "Im0", 45.0f, 260.0f, 560.0f, 120.0f, 120.0f)) {
                c.injected_ok = false; c.note = "rotateImage returned false"; finishCell(c); continue;
            }
            fs::path out = outPath("imgrot_" + op);
            doc->saveAs(out);
            page.reset(); doc.reset();
            // Rotation geometry is hard to assert; require the blue image to be
            // present (visible, not clipped/off-page/scaled-to-nothing).
            evalImageCell(c, out, /*wantGreen=*/false, /*expectWidth=*/0);
        } catch (const std::exception& e) {
            c.injected_ok = false; c.note = std::string("threw: ") + e.what();
        }
        finishCell(c);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// INJECTOR: ImageContentEditor crop (remove + re-append with clip + CTM).
// ═════════════════════════════════════════════════════════════════════════════
TEST(AdversarialAmbient, ImageCrop) {
    for (const auto& op : ops()) {
        Cell c; c.injector = "ImageCrop"; c.hostile_op = op;
        fs::path src = imageFixture(op);
        if (!fs::exists(src)) { c.probed = false; c.note = "fixture missing"; finishCell(c); continue; }
        try {
            auto ctx = std::make_shared<MuPdfContext>();
            auto doc = Document::open(ctx, src);
            if (!doc) { c.probed = false; c.note = "open failed"; finishCell(c); continue; }
            auto page = doc->page(0);
            pdfe::image::ImageContentEditor ed(ctx);
            if (!ed.cropImage(page.get(), doc.get(), "Im0",
                              QRectF(200, 500, 60, 120), 60.0f, 120.0f, 200.0f, 500.0f)) {
                c.injected_ok = false; c.note = "cropImage returned false"; finishCell(c); continue;
            }
            fs::path out = outPath("imgcrop_" + op);
            doc->saveAs(out);
            page.reset(); doc.reset();
            evalImageCell(c, out, /*wantGreen=*/false, /*expectWidth=*/0);
        } catch (const std::exception& e) {
            c.injected_ok = false; c.note = std::string("threw: ") + e.what();
        }
        finishCell(c);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Register emitter: prints the grid + writes the JSON gap register once, after
// every cell TEST has run.
// ═════════════════════════════════════════════════════════════════════════════
class GapRegisterEmitter : public ::testing::Environment {
public:
    void TearDown() override {
        std::lock_guard<std::mutex> lk(registryMutex());
        const auto& rows = registry();

        std::printf("\n============ ADVERSARIAL-AMBIENT GAP REGISTER "
                    "(content-stream-continuity phase-1 discovery) ============\n");
        std::printf("%-22s %-12s %-9s %-9s %-7s  %s\n", "injector", "hostile_op",
                    "injected", "neighbor", "probed", "notes");
        std::printf("--------------------------------------------------------"
                    "--------------------------------------------\n");
        int confirmed = 0, probedCount = 0;
        for (const auto& r : rows) {
            std::printf("%-22s %-12s %-9s %-9s %-7s  %s\n", r.injector.c_str(),
                        r.hostile_op.c_str(),
                        r.probed ? (r.injected_ok ? "OK" : "CORRUPT") : "-",
                        r.probed ? (r.neighbor_ok ? "OK" : "CORRUPT") : "-",
                        r.probed ? "yes" : "NO", r.note.c_str());
            if (r.probed) ++probedCount;
            if (r.probed && (!r.injected_ok || !r.neighbor_ok)) ++confirmed;
        }
        std::printf("--------------------------------------------------------"
                    "--------------------------------------------\n");
        std::printf("CONFIRMED corrupting cells (probed AND injected_ok==false "
                    "OR neighbor_ok==false):\n");
        for (const auto& r : rows) {
            if (r.probed && (!r.injected_ok || !r.neighbor_ok))
                std::printf("  CORRUPT: %-22s x %-12s  (%s)\n", r.injector.c_str(),
                            r.hostile_op.c_str(), r.note.c_str());
        }
        std::printf("cells=%zu probed=%d confirmed_corrupt=%d\n", rows.size(),
                    probedCount, confirmed);
        std::printf("=========================================================="
                    "==========================================\n\n");

        if (rows.empty()) {
            std::printf("[AdversarialAmbient] no cells this run (filtered subset) "
                        "— leaving gap-register.json intact\n");
            return;
        }

        QJsonArray arr;
        for (const auto& r : rows) {
            QJsonObject o;
            o["injector"] = QString::fromStdString(r.injector);
            o["hostile_op"] = QString::fromStdString(r.hostile_op);
            o["injected_ok"] = r.injected_ok;
            o["neighbor_ok"] = r.neighbor_ok;
            o["probed"] = r.probed;
            o["is_corrupt"] = (r.probed && (!r.injected_ok || !r.neighbor_ok));
            if (!r.note.empty()) o["notes"] = QString::fromStdString(r.note);
            arr.append(o);
        }
        QJsonObject root;
        root["schema"] = 1;
        root["phase"] = "phase-1";
        root["description"] =
            "Adversarial-ambient content-stream state-continuity discovery. Each "
            "cell drives a page-content injector against a fixture that leaves ONE "
            "hostile ambient operator live, renders the result, and records whether "
            "the injected element (injected_ok) and a pre-existing neighbour "
            "(neighbor_ok) survived. is_corrupt==true is a real bug for phase-2.";
        root["cell_count"] = static_cast<int>(rows.size());
        root["probed_count"] = probedCount;
        root["confirmed_corrupt"] = confirmed;
        root["cells"] = arr;

        // GitHub #261's class -- see the identical note in
        // tests/test_autosave_capture_audit.cpp. An absolute path under another
        // developer's home, created and written successfully on every machine,
        // read on none.
        const fs::path outDir =
            fs::path(PDFE_REPO_ROOT) / ".claude" / "pipelines" /
            "content-stream-continuity" / "gate-artifacts" / "phase-1";
        std::error_code ec;
        fs::create_directories(outDir, ec);
        const fs::path outFile = outDir / "gap-register.json";
        const QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Indented);
        std::ofstream ofs(outFile, std::ios::binary | std::ios::trunc);
        if (ofs) {
            ofs.write(bytes.constData(), bytes.size());
            std::printf("[AdversarialAmbient] gap register written: %s\n",
                        outFile.string().c_str());
        } else {
            std::printf("[AdversarialAmbient] WARNING: could not write %s\n",
                        outFile.string().c_str());
        }
    }
};

const bool kEmitterRegistered = [] {
    ::testing::AddGlobalTestEnvironment(new GapRegisterEmitter());
    return true;
}();
