// -*- coding: utf-8 -*-
// Regression test for the INBOUND fill-colour leak in HeaderFooterEngine
// (Register H1 / docs/content-stream-state-continuity-audit.md, pipeline
// e5b9c231 Phase 2).
//
// HeaderFooterEngine appends its fragment as a separate /Contents stream and
// emits "q /F1 <sz> Tf" + BT..Tj ET per slot -- it sets ONLY the font, no fill
// colour / Tz / Tc / Tw / Ts / Tr. On a page whose ambient fill is non-black the
// header/footer text is drawn in that colour. On ambient_white_fill.pdf the
// ambient fill is white, so an un-fixed engine renders the header invisible.
//
// Fix: emit the shared knownTextStateBasis() (black fill + reset text state)
// right after the fragment's `q`.
#include <gtest/gtest.h>
#include "headerfooter/HeaderFooterEngine.h"
#include "headerfooter/HeaderFooterConfig.h"
#include "core/MuPdfContext.h"

#include <filesystem>
#include <memory>
#include <string>

extern "C" {
#include <mupdf/fitz.h>
}

using namespace pdfe::headerfooter;

namespace {

struct Found { bool found = false; uint32_t argb = 0; };

Found readColor(const std::filesystem::path& pdf, const std::u32string& needle) {
    Found r;
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
                r.found = true;
                r.argb = ln->first_char->argb & 0xFFFFFFu;
            }
        }
    }
    fz_always(ctx) {
        if (stext) fz_drop_stext_page(ctx, stext);
        if (doc) fz_drop_document(ctx, doc);
    }
    fz_catch(ctx) {}
    fz_drop_context(ctx);
    return r;
}

std::filesystem::path fixture() {
    return std::filesystem::path(TEST_FIXTURES_DIR) / "ambient_white_fill.pdf";
}

std::filesystem::path applyHF() {
    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    HeaderFooterEngine eng(ctx);
    HeaderFooterConfig cfg;
    cfg.rangeMode = PageRangeMode::All;
    cfg.centerHeader = "HF VISIBLE MARK";
    std::filesystem::path out =
        std::filesystem::temp_directory_path() / "pdfe_hf_leak_out.pdf";
    eng.apply(fixture(), out, cfg);
    return out;
}

} // namespace

// PRIMARY: the header text must be visible (not the ambient white fill).
TEST(HeaderFooterStateLeak, HeaderTextNotInvisibleUnderAmbientWhiteFill) {
    std::filesystem::path out = applyHF();
    Found hf = readColor(out, U"HF VISIBLE MARK");
    ASSERT_TRUE(hf.found) << "header text not found after apply";
    EXPECT_NE(hf.argb, 0xFFFFFFu)
        << "header text is white -- ambient fill colour leaked into the fragment";
    EXPECT_EQ(hf.argb, 0x000000u)
        << "header text should render at the known black basis (argb="
        << std::hex << hf.argb << ")";
}

// M-b untouched-neighbor: pre-existing black "NEIGHBOR" text stays black.
TEST(HeaderFooterStateLeak, UntouchedNeighborTextUnchanged) {
    Found before = readColor(fixture(), U"NEIGHBOR");
    ASSERT_TRUE(before.found);
    ASSERT_EQ(before.argb, 0x000000u);
    std::filesystem::path out = applyHF();
    Found after = readColor(out, U"NEIGHBOR");
    ASSERT_TRUE(after.found) << "neighbor text missing after apply";
    EXPECT_EQ(after.argb, before.argb)
        << "pre-existing neighbor color changed by the header/footer injection";
}
