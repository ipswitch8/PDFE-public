// -*- coding: utf-8 -*-
// Regression test for the RedactionVerifier partial-overlap security fix
// (Register M-c / docs/content-stream-state-continuity-audit.md, pipeline
// e5b9c231 Phase 2).
//
// checkTextRemoved() flags text still present inside a redaction region. The old
// logic used FULL CONTAINMENT (glyph box entirely inside the region), so a
// leftover glyph that merely straddled the region boundary was NOT detected and
// the verifier wrongly reported the region clear -- a security false-negative.
// The fix uses INTERSECTION: any glyph overlapping the region counts as leftover.
//
// Oracle: build a region that intersects every glyph of "SECRET" but fully
// contains none (spans the width, only the middle vertical band). The old
// containment check returns true (clear -- WRONG); the intersection fix returns
// false (leftover text detected). A control region that fully contains the text
// must be detected by both (guards against over-flagging changes).
#include <gtest/gtest.h>
#include "redaction/RedactionVerifier.h"
#include "redaction/RedactionData.h"
#include "core/MuPdfContext.h"

#include <QRectF>
#include <filesystem>
#include <memory>
#include <vector>

extern "C" {
#include <mupdf/fitz.h>
}

using namespace pdfe::redaction;

namespace {

std::filesystem::path fixture() {
    return std::filesystem::path(TEST_FIXTURES_DIR) / "redaction_partial_overlap.pdf";
}

// Union bbox (device space, y-down) of all glyphs on the page.
fz_rect textUnionRect() {
    fz_rect u = fz_empty_rect;
    fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    fz_register_document_handlers(ctx);
    fz_document* doc = nullptr;
    fz_stext_page* stext = nullptr;
    fz_try(ctx) {
        doc = fz_open_document(ctx, fixture().string().c_str());
        stext = fz_new_stext_page_from_page_number(ctx, doc, 0, nullptr);
        for (fz_stext_block* b = stext->first_block; b; b = b->next) {
            if (b->type != FZ_STEXT_BLOCK_TEXT) continue;
            for (fz_stext_line* ln = b->u.t.first_line; ln; ln = ln->next)
                for (fz_stext_char* ch = ln->first_char; ch; ch = ch->next)
                    u = fz_union_rect(u, fz_rect_from_quad(ch->quad));
        }
    }
    fz_always(ctx) {
        if (stext) fz_drop_stext_page(ctx, stext);
        if (doc) fz_drop_document(ctx, doc);
    }
    fz_catch(ctx) {}
    fz_drop_context(ctx);
    return u;
}

} // namespace

TEST(RedactionPartialOverlap, PartiallyOverlappingLeftoverTextIsDetected) {
    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    RedactionVerifier verifier(ctx);

    fz_rect t = textUnionRect();
    ASSERT_LT(t.x0, t.x1) << "fixture must contain 'SECRET' text";
    const float h = t.y1 - t.y0;

    // Middle vertical band spanning the full width: intersects every glyph,
    // fully contains none (region top is below each glyph top, region bottom
    // is above each glyph bottom).
    pdfe::redaction::RedactionData partial;
    partial.pageIndex = 0;
    partial.rect = QRectF(t.x0 - 2.0, t.y0 + 0.35 * h,
                          (t.x1 - t.x0) + 4.0, 0.30 * h);

    bool clear = verifier.checkTextRemoved(fixture(),
                                           {partial});
    EXPECT_FALSE(clear)
        << "partial-overlap leftover text was not detected -- full-containment "
           "check missed a glyph straddling the redaction boundary (security "
           "false-negative)";
}

// Control: a region that fully contains the text must be detected regardless
// (so the intersection change did not weaken the fully-contained case).
TEST(RedactionPartialOverlap, FullyContainedTextStillDetected) {
    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    RedactionVerifier verifier(ctx);

    fz_rect t = textUnionRect();
    ASSERT_LT(t.x0, t.x1);
    pdfe::redaction::RedactionData full;
    full.pageIndex = 0;
    full.rect = QRectF(t.x0 - 10.0, t.y0 - 10.0,
                       (t.x1 - t.x0) + 20.0, (t.y1 - t.y0) + 20.0);

    EXPECT_FALSE(verifier.checkTextRemoved(fixture(), {full}))
        << "fully-contained leftover text must still be detected";
}
