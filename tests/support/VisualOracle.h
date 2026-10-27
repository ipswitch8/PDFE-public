#pragma once
// Shared VISUAL truth-oracle helpers for content-stream / injector tests.
//
// Follows the model `test_content_stream_neg_tz.cpp` set: read the RESULT back
// through fitz stext (glyph unicode + sRGB argb + quad geometry) after a
// save/reopen, so a test asserts what a viewer actually renders -- catching
// mirror, color-loss, mis-position, and dropped-glyph defects that a text /
// name / "did not throw" oracle is blind to.
//
// Header-only; include from a test .cpp (the tests target adds tests/ to its
// include path).

#include <mupdf/fitz.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace pdfe::testsupport {

struct Glyph {
    char32_t c = 0;
    uint32_t argb = 0;        // sRGB, 0xRRGGBB (alpha masked off)
    fz_quad quad{};
};

/// All glyphs on a page, in reading order, as rendered by fitz stext.
inline std::vector<Glyph> readGlyphs(const std::filesystem::path& pdf,
                                     int pageNum = 0) {
    std::vector<Glyph> out;
    fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    if (!ctx) return out;
    fz_register_document_handlers(ctx);
    fz_document* doc = nullptr;
    fz_stext_page* stext = nullptr;
    fz_try(ctx) {
        doc = fz_open_document(ctx, pdf.string().c_str());
        stext = fz_new_stext_page_from_page_number(ctx, doc, pageNum, nullptr);
        for (fz_stext_block* b = stext->first_block; b; b = b->next) {
            if (b->type != FZ_STEXT_BLOCK_TEXT) continue;
            for (fz_stext_line* ln = b->u.t.first_line; ln; ln = ln->next)
                for (fz_stext_char* ch = ln->first_char; ch; ch = ch->next)
                    out.push_back({static_cast<char32_t>(ch->c),
                                   ch->argb & 0xFFFFFFu, ch->quad});
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

/// Glyphs of the first rendered LINE whose text contains `needle` (empty if
/// none). Preserves per-glyph argb + quad for color/geometry oracles.
inline std::vector<Glyph> lineContaining(const std::filesystem::path& pdf,
                                         const std::u32string& needle,
                                         int pageNum = 0) {
    std::vector<Glyph> result;
    fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    if (!ctx) return result;
    fz_register_document_handlers(ctx);
    fz_document* doc = nullptr;
    fz_stext_page* stext = nullptr;
    fz_try(ctx) {
        doc = fz_open_document(ctx, pdf.string().c_str());
        stext = fz_new_stext_page_from_page_number(ctx, doc, pageNum, nullptr);
        for (fz_stext_block* b = stext->first_block; b; b = b->next) {
            if (b->type != FZ_STEXT_BLOCK_TEXT) continue;
            for (fz_stext_line* ln = b->u.t.first_line; ln; ln = ln->next) {
                std::u32string t;
                std::vector<Glyph> lineGlyphs;
                for (fz_stext_char* ch = ln->first_char; ch; ch = ch->next) {
                    t.push_back(static_cast<char32_t>(ch->c));
                    lineGlyphs.push_back({static_cast<char32_t>(ch->c),
                                          ch->argb & 0xFFFFFFu, ch->quad});
                }
                if (t.find(needle) != std::u32string::npos) result = lineGlyphs;
            }
        }
    }
    fz_always(ctx) {
        if (stext) fz_drop_stext_page(ctx, stext);
        if (doc) fz_drop_document(ctx, doc);
    }
    fz_catch(ctx) {}
    fz_drop_context(ctx);
    return result;
}

/// A run reads left-to-right (not horizontally mirrored) iff its first glyph's
/// upper-right x exceeds its upper-left x and the run advances rightward.
inline bool isLeftToRight(const std::vector<Glyph>& g) {
    if (g.size() < 2) return g.size() == 1 ? g[0].quad.ur.x > g[0].quad.ul.x : false;
    return g.front().quad.ur.x > g.front().quad.ul.x &&
           g.back().quad.ul.x > g.front().quad.ul.x;
}

/// True if any rendered glyph on the page is "visible" -- i.e. not the given
/// background color (used to catch invisible/white-on-white text).
inline bool anyGlyphColorDiffersFrom(const std::vector<Glyph>& g, uint32_t bg) {
    for (const auto& gl : g) if (gl.argb != bg) return true;
    return false;
}

} // namespace pdfe::testsupport
