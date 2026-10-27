#pragma once

#include <string>
#include <unordered_map>

// MuPDF opaque types (forward-declared so this header stays light).
struct fz_context;
struct fz_page;

namespace pdfe::core {

/// Map from a (possibly truncated) MuPDF ``fz_font_name`` string to the full,
/// untruncated PDF ``/BaseFont`` string.
///
/// Why this exists: MuPDF stores a font's name in a fixed ``char name[32]``
/// buffer (mupdf/fitz/font.h), so ``fz_font_name()`` silently truncates any
/// ``/BaseFont`` longer than 31 characters. Name-based heuristics downstream
/// (overlay italic/bold detection, bold classification for auto-tagging, script
/// detection) then lose whatever style/script keyword sat past character 31 —
/// the root cause of the "long embedded font name drops italic in the edit
/// overlay" class of defects. The authoritative untruncated name is the
/// ``/BaseFont`` entry in the PDF font dictionary; this map recovers it.
using BaseFontMap = std::unordered_map<std::string, std::string>;

/// Build a :type:`BaseFontMap` for every font reachable from *page*'s
/// resources, including one level deep into Form XObject resources.
///
/// Keys are the ``/BaseFont`` truncated to 31 chars (matching what
/// ``fz_font_name`` returns), stored both with and without the subset prefix so
/// a lookup with the value from ``fz_font_name`` resolves regardless of whether
/// MuPDF kept the ``ABCDEF+`` prefix in its truncated copy. If two distinct
/// ``/BaseFont`` values collide on the same 31-char key the entry is marked
/// ambiguous (resolution then yields no change, never a wrong attribution).
///
/// Never throws / never longjmps: MuPDF errors during the walk are caught
/// internally and the partial (or empty) map is returned, so callers degrade
/// gracefully to the truncated ``fz_font_name``.
BaseFontMap buildBaseFontMap(struct fz_context* ctx, struct fz_page* page);

/// Return the untruncated ``/BaseFont`` for *fzName* (as returned by
/// ``fz_font_name``) when *map* holds a strictly longer entry for it; otherwise
/// return *fzName* unchanged. The "strictly longer" guard means fonts whose
/// name was never truncated are returned byte-identical to ``fz_font_name``.
std::string resolveUntruncatedFontName(const BaseFontMap& map,
                                       const std::string& fzName);

/// Italic/bold derived from a font's PDF ``/FontDescriptor`` — the
/// name-independent style source. Both default false (unknown/no descriptor).
struct FontStyleFlags {
    bool isItalic = false;
    bool isBold = false;
};

/// Map from a (possibly truncated) ``fz_font_name`` string to the
/// :type:`FontStyleFlags` read from that font's ``/FontDescriptor``.
///
/// Why: the overlay and missing-glyph paths historically detected italic/bold by
/// substring-scanning the font NAME, which misses styles encoded without
/// keywords (abbreviations like ``-BdIt``, numeric weights, non-English). The
/// descriptor carries the style structurally — ``/Flags`` bit 7 (Italic, value
/// 64) / bit 19 (ForceBold, value 0x40000), ``/ItalicAngle`` (non-zero ⇒
/// italic), and ``/StemV`` (heavy stem ⇒ bold) — independent of the name. Keyed
/// identically to :type:`BaseFontMap` so a lookup with ``fz_font_name`` resolves.
using FontStyleMap = std::unordered_map<std::string, FontStyleFlags>;

/// Build a :type:`FontStyleMap` for every font reachable from *page*'s
/// resources (incl. one level of Form XObject resources). Handles a missing
/// ``/FontDescriptor`` gracefully (entry defaults to false/false) and reads the
/// descriptor from the descendant CIDFont for Type0 composites. Never
/// throws / longjmps; returns a partial/empty map on MuPDF error.
FontStyleMap buildFontStyleMap(struct fz_context* ctx, struct fz_page* page);

/// Return the :type:`FontStyleFlags` for *fzName* if present in *map*, else
/// ``{false, false}``.
FontStyleFlags resolveFontStyle(const FontStyleMap& map,
                                const std::string& fzName);

}  // namespace pdfe::core
