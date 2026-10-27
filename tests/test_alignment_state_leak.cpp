// -*- coding: utf-8 -*-
// Regression test for the INBOUND fill-colour leak in AlignmentWriter
// (Register H1 / docs/content-stream-state-continuity-audit.md, pipeline
// e5b9c231 Phase 2).
//
// AlignmentInjector appends the writer's fragment as a separate /Contents
// stream; PDF concatenates content streams with no graphics-state reset, so the
// fragment inherits ambient state. AlignmentWriter emits Tf/Tw/Tm/Tj inside a
// q...Q but sets NO fill colour, so on a page whose ambient fill is non-black
// the aligned text is drawn in that colour. On ambient_white_fill.pdf the
// ambient fill is white, so an un-fixed writer renders the aligned text white
// (invisible).
//
// Fix: AlignmentWriter must emit a known text-state basis (black fill + reset
// Tz/Tc/Ts/Tr) inside its q...Q.
//
// Oracle (M-b: edited element AND untouched neighbor): the injected aligned text
// must render in a visible (black) colour, and the pre-existing black "NEIGHBOR"
// text must be unchanged. Colours are read back via fitz stext after inject +
// reopen -- exactly what a viewer renders.
#include <gtest/gtest.h>
#include "reflow/AlignmentPipeline.h"
#include "reflow/BlockParamsBuilder.h"
#include "reflow/ReflowTypes.h"
#include "core/MuPdfContext.h"

#include <QRectF>
#include <filesystem>
#include <memory>
#include <string>

extern "C" {
#include <mupdf/fitz.h>
}

using namespace pdfe::reflow;

namespace {

struct Found { bool found = false; uint32_t argb = 0; };

// argb of the first glyph of whichever rendered line contains `needle`.
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

// Inject a centered aligned paragraph and return the saved path.
std::filesystem::path injectAligned() {
    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    AlignmentPipeline pipe(ctx);
    auto params = BlockParamsBuilder::build(
        "ALIGNED VISIBLE MARKER",
        QRectF(72.0, 400.0, 400.0, 40.0), 14.0f, Alignment::Center);
    pipe.queueAlignment(0, params);
    std::filesystem::path out =
        std::filesystem::temp_directory_path() / "pdfe_align_leak_out.pdf";
    pipe.flush(fixture(), out);
    return out;
}

} // namespace

// PRIMARY: the injected aligned text must be visible (not the ambient white).
TEST(AlignmentStateLeak, AlignedTextNotInvisibleUnderAmbientWhiteFill) {
    std::filesystem::path out = injectAligned();
    Found aligned = readColor(out, U"ALIGNED VISIBLE MARKER");
    ASSERT_TRUE(aligned.found) << "injected aligned text not found after reopen";
    EXPECT_NE(aligned.argb, 0xFFFFFFu)
        << "aligned text is white -- ambient fill colour leaked into the fragment";
    EXPECT_EQ(aligned.argb, 0x000000u)
        << "aligned text should render at the known black basis (argb="
        << std::hex << aligned.argb << ")";
}

// M-b untouched-neighbor: pre-existing black "NEIGHBOR" text stays black.
TEST(AlignmentStateLeak, UntouchedNeighborTextUnchanged) {
    Found before = readColor(fixture(), U"NEIGHBOR");
    ASSERT_TRUE(before.found) << "fixture must contain black 'NEIGHBOR' text";
    ASSERT_EQ(before.argb, 0x000000u);

    std::filesystem::path out = injectAligned();
    Found after = readColor(out, U"NEIGHBOR");
    ASSERT_TRUE(after.found) << "neighbor text missing after injection";
    EXPECT_EQ(after.argb, before.argb)
        << "pre-existing neighbor color changed by the alignment injection";
}
