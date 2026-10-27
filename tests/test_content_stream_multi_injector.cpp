// -*- coding: utf-8 -*-
//
// Content-stream state-continuity pipeline — Phase 4 CROSS-INJECTOR LOCK.
//
// WHAT THIS IS
// ------------
// The phase-2 regression suite (tests/test_content_stream_ctm_reset.cpp) proved
// that EACH text injector, run ONCE against a hostile ambient-CTM page, resets
// the CTM via core/PageContentGuard.h::appendContentIsolatingAmbientCtm(). This
// file closes the gap karen flagged: what happens when TWO DIFFERENT injectors
// run on the SAME page, back to back?
//
// The guard is designed to be IDEMPOTENT — the first injector brackets the
// existing content in `q %PDFE-CTMGUARD ... Q`; every later injector detects the
// leading sentinel stream and appends its (self-balanced) fragment AFTER the
// existing `Q` instead of nesting a second bracket. If that idempotency check
// ever regresses, a second injector would DOUBLE-bracket the page (two guard
// streams), which this test catches directly by counting guard streams in the
// saved /Contents.
//
// THE CHAIN
// ---------
//   aa_cm.pdf  ── AlignmentInjector (ALIGNMARK) ──▶  step1.pdf
//   step1.pdf  ── HeaderFooterEngine (HFMARK)   ──▶  step2.pdf   (already guarded)
//
// ORACLES on step2.pdf:
//   (1) IDEMPOTENCY: exactly ONE %PDFE-CTMGUARD stream in page-0 /Contents.
//   (2) BOTH injected lines (ALIGNMARK, HFMARK) render full-size, upright,
//       visible and on-page — relative to the pre-existing KEEPME neighbour.
//   (3) KEEPME neighbour uncorrupted.
//
// This is a correctness suite: a corrupt cell is a RED test, never data. Do NOT
// weaken an oracle or skip a cell to reach green.

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "core/PageContentGuard.h"

#include "reflow/AlignmentPipeline.h"
#include "reflow/BlockParamsBuilder.h"
#include "reflow/ReflowTypes.h"
#include "headerfooter/HeaderFooterEngine.h"
#include "headerfooter/HeaderFooterConfig.h"

#include <QImage>
#include <QPainter>
#include <QRectF>

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
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
fs::path outPath(const std::string& tag) {
    return fs::temp_directory_path() / ("pdfe_multiinj_" + tag + ".pdf");
}

// ── Glyph-quad oracle (raw fitz, isolated context — a fresh viewer's read). ──
struct LineHit {
    bool found = false;
    float firstUlx = 0, firstUrx = 0;   // first glyph, for the mirror test
    float glyphH = 0;                   // first glyph height (points) — size test
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
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

// Count how many streams in page-0 /Contents begin with the CTM-guard sentinel.
// A correctly-idempotent guard leaves EXACTLY ONE regardless of how many
// injectors ran; a regressed idempotency check double-brackets → 2+.
int countGuardStreams(const fs::path& pdf) {
    fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    fz_register_document_handlers(ctx);
    pdf_document* doc = nullptr;
    int count = -1;  // -1 => could not inspect
    fz_try(ctx) {
        doc = pdf_open_document(ctx, pdf.string().c_str());
        pdf_obj* page = pdf_lookup_page_obj(ctx, doc, 0);
        pdf_obj* contents = pdf_dict_get(ctx, page, PDF_NAME(Contents));
        count = 0;
        if (pdf_is_array(ctx, contents)) {
            int n = pdf_array_len(ctx, contents);
            for (int i = 0; i < n; ++i)
                if (pdfe::core::streamStartsWithCtmGuard(
                        ctx, pdf_array_get(ctx, contents, i)))
                    ++count;
        } else if (contents) {
            if (pdfe::core::streamStartsWithCtmGuard(ctx, contents)) ++count;
        }
    }
    fz_always(ctx) {
        if (doc) pdf_drop_document(ctx, doc);
    }
    fz_catch(ctx) { count = -1; }
    fz_drop_context(ctx);
    return count;
}

// Shared assertions: the injected line must be present, upright, visible, on the
// page, and full-size relative to the KEEPME neighbour (bug => ~0.5x).
void assertInjectedFullSizeAndClean(const fs::path& out,
                                    const std::u32string& needle) {
    const float scale = 3.0f;
    LineHit inj  = readLine(out, needle);
    LineHit keep = readLine(out, U"KEEPME");
    QImage im = renderPage0(out, scale);

    std::string tag(needle.begin(), needle.end());
    ASSERT_TRUE(inj.found) << tag << ": injected text not found after reopen";
    ASSERT_TRUE(keep.found) << "KEEPME neighbour not found — cannot size-normalise";
    ASSERT_FALSE(im.isNull()) << "page 0 failed to render";

    EXPECT_GT(inj.firstUrx, inj.firstUlx)
        << tag << ": injected glyph is mirrored (ur.x <= ul.x)";

    const long ink = inkInRoi(im, scale, inj.x0, inj.y0, inj.x1, inj.y1);
    EXPECT_GT(ink, 60) << tag << ": injected text left no ink (invisible/off-page)";

    ASSERT_GT(keep.glyphH, 0.1f);
    const float ratio = inj.glyphH / keep.glyphH;
    EXPECT_GT(ratio, 0.80f)
        << tag << ": injected text is UNDER-SIZED (sizeRatio=" << ratio
        << ") — ambient N-up CTM leaked into the fragment";
    EXPECT_LT(ratio, 1.20f)
        << tag << ": injected text is OVER-SIZED (sizeRatio=" << ratio << ")";

    QImage im1 = renderPage0(out, 1.0f);
    ASSERT_FALSE(im1.isNull());
    EXPECT_GE(inj.x0, -1.0f);
    EXPECT_GE(inj.y0, -1.0f);
    EXPECT_LE(inj.x1, static_cast<float>(im1.width()) + 1.0f);
    EXPECT_LE(inj.y1, static_cast<float>(im1.height()) + 1.0f);
}

}  // namespace

// ═════════════════════════════════════════════════════════════════════════════
// Two different injectors, same page, hostile ambient CTM. The guard must be
// applied EXACTLY ONCE (idempotent) and BOTH injected elements must be clean.
// ═════════════════════════════════════════════════════════════════════════════
TEST(MultiInjector, AlignThenHeaderFooterGuardIsIdempotent) {
    ASSERT_TRUE(fs::exists(cmFixture())) << "fixture missing: " << cmFixture();

    // Step 1: AlignmentInjector on the raw hostile page → first guard bracket.
    fs::path step1 = outPath("step1_align");
    fs::remove(step1);
    {
        auto ctx = std::make_shared<MuPdfContext>();
        pdfe::reflow::AlignmentPipeline pipe(ctx);
        auto params = pdfe::reflow::BlockParamsBuilder::build(
            "ALIGNMARK", QRectF(72.0, 400.0, 400.0, 40.0), 18.0f,
            pdfe::reflow::Alignment::Left);
        pipe.queueAlignment(0, params);
        pipe.flush(cmFixture(), step1);
    }
    ASSERT_TRUE(fs::exists(step1)) << "step-1 (alignment) output not written";

    // After ONE injector the page must carry exactly one guard.
    EXPECT_EQ(countGuardStreams(step1), 1)
        << "first injector did not establish exactly one CTM guard";

    // Step 2: HeaderFooterEngine on the ALREADY-GUARDED page → must NOT re-bracket.
    fs::path step2 = outPath("step2_hf");
    fs::remove(step2);
    {
        auto ctx = std::make_shared<MuPdfContext>();
        pdfe::headerfooter::HeaderFooterEngine eng(ctx);
        pdfe::headerfooter::HeaderFooterConfig cfg;
        cfg.rangeMode = pdfe::headerfooter::PageRangeMode::All;
        cfg.centerHeader = "HFMARK";
        cfg.fontSize = 18.0f;
        eng.apply(step1, step2, cfg);
    }
    ASSERT_TRUE(fs::exists(step2)) << "step-2 (header/footer) output not written";

    // (1) IDEMPOTENCY: still exactly one guard after the SECOND injector.
    const int guards = countGuardStreams(step2);
    ASSERT_GE(guards, 0) << "could not inspect /Contents of step-2 output";
    EXPECT_EQ(guards, 1)
        << "CTM guard was applied " << guards << " times across two injectors — "
        << "the sentinel idempotency check regressed (double-bracketing)";

    // (2) BOTH injected elements clean under the chained edits.
    assertInjectedFullSizeAndClean(step2, U"ALIGNMARK");
    assertInjectedFullSizeAndClean(step2, U"HFMARK");

    // (3) The pre-existing neighbour survived both injectors.
    LineHit keep = readLine(step2, U"KEEPME");
    ASSERT_TRUE(keep.found) << "KEEPME neighbour lost after chained injectors";
    QImage im = renderPage0(step2, 3.0f);
    ASSERT_FALSE(im.isNull());
    EXPECT_GT(keep.firstUrx, keep.firstUlx) << "KEEPME neighbour mirrored";
    EXPECT_GT(inkInRoi(im, 3.0f, keep.x0, keep.y0, keep.x1, keep.y1), 60)
        << "KEEPME neighbour left no ink (corrupted by chained injectors)";
}
