// -*- coding: utf-8 -*-
// Regression test for the INBOUND text-state leak in BatchEngine text watermark
// (Register H1 / docs/content-stream-state-continuity-audit.md, pipeline
// e5b9c231 Phase 2).
//
// The text watermark is appended as a raw content stream and emitted as
// "q 0.5 g /GS0 gs BT /F1 sz Tf <Tm> (text) Tj ET Q". It sets colour and an
// absolute Tm but never resets the horizontal text scale, so on a page that
// leaves an ambient "-100 Tz" active the watermark is drawn mirrored.
//
// Fix: emit the shared knownTextStateBasis() (100 Tz + 0 Tc/Tw/Ts/Tr) right
// after the watermark's `q`; the watermark's own "0.5 g" colour still applies.
//
// Oracle: apply a rotation-0 text watermark to the ambient -100 Tz fixture,
// reopen, read the watermark text back via fitz stext and assert it is NOT
// mirrored (upper-right x > upper-left x).
#include <gtest/gtest.h>
#include "batch/BatchEngine.h"
#include "batch/BatchTypes.h"
#include "core/MuPdfContext.h"

#include <filesystem>
#include <memory>
#include <string>

extern "C" {
#include <mupdf/fitz.h>
}

using namespace pdfe::batch;

namespace {

// First-glyph geometry+color of a line, for the untouched-neighbor oracle.
struct Glyph { bool found = false; float ulx = 0, urx = 0, lly = 0; uint32_t argb = 0; };
Glyph readGlyph(const std::filesystem::path& pdf, const std::u32string& needle) {
    Glyph gg;
    fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    fz_register_document_handlers(ctx);
    fz_document* doc = nullptr; fz_stext_page* stext = nullptr;
    fz_try(ctx) {
        doc = fz_open_document(ctx, pdf.string().c_str());
        stext = fz_new_stext_page_from_page_number(ctx, doc, 0, nullptr);
        for (fz_stext_block* b = stext->first_block; b; b = b->next) {
            if (b->type != FZ_STEXT_BLOCK_TEXT) continue;
            for (fz_stext_line* ln = b->u.t.first_line; ln; ln = ln->next) {
                std::u32string t;
                for (fz_stext_char* ch = ln->first_char; ch; ch = ch->next)
                    t.push_back(static_cast<char32_t>(ch->c));
                if (t.find(needle) == std::u32string::npos) continue;
                fz_stext_char* f = ln->first_char;
                gg.found = true; gg.ulx = f->quad.ul.x; gg.urx = f->quad.ur.x;
                gg.lly = f->quad.ll.y; gg.argb = f->argb & 0xFFFFFFu;
            }
        }
    }
    fz_always(ctx) { if (stext) fz_drop_stext_page(ctx, stext); if (doc) fz_drop_document(ctx, doc); }
    fz_catch(ctx) {}
    fz_drop_context(ctx);
    return gg;
}

struct Dir { bool found = false; bool mirrored = false; };
Dir readDir(const std::filesystem::path& pdf, const std::u32string& needle) {
    Dir d;
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
                std::u32string t;
                for (fz_stext_char* ch = ln->first_char; ch; ch = ch->next)
                    t.push_back(static_cast<char32_t>(ch->c));
                if (t.find(needle) == std::u32string::npos) continue;
                fz_quad q = ln->first_char->quad;
                d.found = true;
                d.mirrored = !(q.ur.x > q.ul.x);
            }
        }
    }
    fz_always(ctx) {
        if (stext) fz_drop_stext_page(ctx, stext);
        if (doc) fz_drop_document(ctx, doc);
    }
    fz_catch(ctx) {}
    fz_drop_context(ctx);
    return d;
}

} // namespace

TEST(WatermarkStateLeak, TextWatermarkNotMirroredByAmbientNegTz) {
    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    std::filesystem::path src =
        std::filesystem::path(TEST_FIXTURES_DIR) / "ambient_neg_tz_page.pdf";
    ASSERT_TRUE(std::filesystem::exists(src)) << src.string();

    WatermarkConfig cfg;
    cfg.text = "WMMIRROR";
    cfg.rotation = 0.0f;     // isolate the horizontal-scale (mirror) oracle
    cfg.opacity = 1.0f;
    cfg.fontSize = 40.0f;

    std::filesystem::path out =
        std::filesystem::temp_directory_path() / "pdfe_watermark_state_leak.pdf";
    std::filesystem::remove(out);
    BatchEngine eng(ctx);
    ASSERT_NO_THROW(eng.applyWatermark(src, out, cfg));

    Dir d = readDir(out, U"WMMIRROR");
    ASSERT_TRUE(d.found) << "watermark text not found after apply";
    EXPECT_FALSE(d.mirrored)
        << "text watermark is horizontally mirrored -- ambient -100 Tz leaked "
           "into the watermark fragment";
}

// M-b untouched-neighbor: applying the watermark must not disturb the page's
// pre-existing "PAGE" text (position or color).
TEST(WatermarkStateLeak, UntouchedNeighborUnchanged) {
    std::filesystem::path src =
        std::filesystem::path(TEST_FIXTURES_DIR) / "ambient_neg_tz_page.pdf";
    Glyph before = readGlyph(src, U"PAGE");
    ASSERT_TRUE(before.found) << "fixture must contain neighbor 'PAGE' text";

    WatermarkConfig cfg;
    cfg.text = "WMMIRROR";
    cfg.rotation = 0.0f; cfg.opacity = 1.0f; cfg.fontSize = 40.0f;
    std::filesystem::path out =
        std::filesystem::temp_directory_path() / "pdfe_watermark_nbr.pdf";
    std::filesystem::remove(out);
    BatchEngine eng(std::make_shared<pdfe::core::MuPdfContext>());
    ASSERT_NO_THROW(eng.applyWatermark(src, out, cfg));

    Glyph after = readGlyph(out, U"PAGE");
    ASSERT_TRUE(after.found) << "neighbor 'PAGE' missing after watermark";
    EXPECT_EQ(after.argb, before.argb) << "neighbor color changed by watermark";
    EXPECT_NEAR(after.ulx, before.ulx, 1.0f) << "neighbor x moved by watermark";
    EXPECT_NEAR(after.lly, before.lly, 1.0f) << "neighbor y moved by watermark";
    EXPECT_NEAR(after.urx - after.ulx, before.urx - before.ulx, 1.0f)
        << "neighbor glyph advance/scale changed by watermark";
}
