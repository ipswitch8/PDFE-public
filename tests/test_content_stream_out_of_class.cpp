// -*- coding: utf-8 -*-
//
// Out-of-class findings surfaced by the content-stream state-continuity sweep
// (docs/content-stream-state-continuity-audit.md, "Out-of-class findings";
// pipeline content-stream-continuity Phase 3).
//
// This file covers finding #3 (TRE glyph fallback). Findings #1, #2 and #4 are
// already fixed in HEAD (commit b63ad8c, pipeline e5b9c231 Phase 2) and locked
// by dedicated regression tests -- this file does not duplicate them:
//   * #1 RedactionVerifier full-containment -> intersection (SECURITY):
//         tests/test_redaction_partial_overlap.cpp
//         (PartiallyOverlappingLeftoverTextIsDetected + fully-contained control)
//   * #2 HeaderFooter /F1 resource-name collision:
//         tests/test_hf_watermark_adversarial.cpp
//         (HeaderUsesFreshFontKeyNotCollidingF1)
//   * #4 Watermark + HeaderFooter non-zero MediaBox origin:
//         tests/test_hf_watermark_adversarial.cpp
//         (Header/WatermarkCenteredDespiteNonZeroMediaBoxOrigin)
//
// -------------------------------------------------------------------------
// Finding #3 -- TRE glyph fallback wrote Unicode codepoints as hex GLYPH
// INDICES. The reconstruction font is embedded via pdf_add_cid_font (Type0 /
// Identity-H), so the bytes of a `<...> Tj` show string are glyph indices, not
// character codes. The old fallback emitted the raw codepoint value as the
// glyph index (e.g. 'A' U+0041 -> <0041>), which selects whatever glyph sits at
// index 0x41 in the font program -- the wrong glyph for virtually every font.
//
// Oracle (glyph-index / CID): pick a codepoint whose glyph index in the font
// DIFFERS from its numeric codepoint value (true for essentially every real
// font). PdfContentWriter::encodeGlyphIndicesHex() -- the single encoder both
// the normal and the error-fallback paths now route through -- must emit that
// codepoint's GLYPH INDEX, and must NOT emit the raw codepoint. The
// EXPECT_NE(...) below is exactly the string the buggy fallback produced, so
// this test is RED against the old raw-codepoint logic and GREEN after the fix.

#include <gtest/gtest.h>

#include "tre/PdfContentWriter.h"
#include "core/MuPdfContext.h"

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

extern "C" {
#include <mupdf/fitz.h>
}

namespace {

std::string hex4(int v) {
    char b[8];
    std::snprintf(b, sizeof(b), "%04x", v & 0xFFFF);
    return b;
}

} // namespace

TEST(ContentStreamOutOfClass, TreGlyphFallbackEmitsGlyphIndicesNotCodepoints) {
    auto ctxWrap = std::make_shared<pdfe::core::MuPdfContext>();

    struct Probe {
        bool found = false;
        uint32_t cp = 0;
        int gid = 0;
        std::string emitted;
    } p;

    ctxWrap->execute([&](fz_context* ctx) {
        // Self-contained base-14 font (URW substitute) with a real cmap whose
        // glyph indices are not equal to the ASCII codepoints.
        fz_font* font = fz_new_base14_font(ctx, "Helvetica");
        ASSERT_NE(font, nullptr);

        // Find a printable-ASCII codepoint whose glyph index differs from the
        // codepoint value -- the case the bug renders wrong.
        for (uint32_t cp = 0x20; cp <= 0x7E; ++cp) {
            int gid = fz_encode_character(ctx, font, static_cast<int>(cp));
            if (gid != 0 && gid != static_cast<int>(cp)) {
                p.found = true;
                p.cp = cp;
                p.gid = gid;
                break;
            }
        }

        if (p.found) {
            p.emitted = pdfe::tre::PdfContentWriter::encodeGlyphIndicesHex(
                ctx, font, std::vector<uint32_t>{p.cp});
        }
        fz_drop_font(ctx, font);
    });

    ASSERT_TRUE(p.found)
        << "test needs a codepoint whose glyph index differs from its value";

    // GREEN: the codepoint is mapped to its GLYPH INDEX.
    EXPECT_EQ(p.emitted, hex4(p.gid))
        << "codepoint U+" << hex4(p.cp) << " was not mapped to glyph index "
        << p.gid;

    // RED against the old fallback: it emitted the raw codepoint as the glyph
    // index, which is a different (wrong) glyph.
    EXPECT_NE(p.emitted, hex4(static_cast<int>(p.cp)))
        << "raw Unicode codepoint emitted as glyph index -> wrong glyph "
           "rendered (out-of-class finding #3)";
}

// Guard: a codepoint the font genuinely cannot encode degrades to GID 0
// (.notdef), never to the raw codepoint value.
TEST(ContentStreamOutOfClass, TreGlyphFallbackUnresolvableDegradesToNotdef) {
    auto ctxWrap = std::make_shared<pdfe::core::MuPdfContext>();
    std::string emitted;
    bool unresolvable = false;
    uint32_t cp = 0x10FFFD;  // Supplementary Private Use Area-B, not in base14.

    ctxWrap->execute([&](fz_context* ctx) {
        fz_font* font = fz_new_base14_font(ctx, "Helvetica");
        ASSERT_NE(font, nullptr);
        unresolvable = (fz_encode_character(ctx, font, static_cast<int>(cp)) == 0);
        emitted = pdfe::tre::PdfContentWriter::encodeGlyphIndicesHex(
            ctx, font, std::vector<uint32_t>{cp});
        fz_drop_font(ctx, font);
    });

    ASSERT_TRUE(unresolvable) << "expected the probe codepoint to be absent";
    EXPECT_EQ(emitted, "0000")
        << "unresolvable codepoint must degrade to .notdef (GID 0), not the "
           "raw codepoint";
}
