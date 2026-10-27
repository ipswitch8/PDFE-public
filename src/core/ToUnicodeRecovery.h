#pragma once

#include <string>

// MuPDF opaque types (forward-declared at GLOBAL scope so this header stays
// light — see CLAUDE.md's MuPDF gotchas).
struct fz_context;
struct fz_page;

namespace pdfe::core {

/// Is *cp* a codepoint that extraction cannot usefully hand to search, copy,
/// accessibility or redaction-by-search?
///
/// The four unusable classes, and why each is unusable rather than merely odd:
///   * ``0`` / ``U+FFFD`` — no mapping at all, or an explicit replacement char.
///   * ``U+0000..U+001F`` — C0 controls. MuPDF's last-resort behaviour for a
///     simple font whose glyph name resolves to nothing is to pass the raw
///     CHARACTER CODE through as the "Unicode", so a font using codes 1..6
///     extracts as six control characters.
///   * ``U+007F..U+009F`` — DEL plus the C1 controls, i.e. the same raw-code
///     passthrough for the 0x80..0x9F band where WinAnsi carries real
///     punctuation (curly quotes, em dash, euro).
///   * ``U+E000..U+F8FF`` — the BMP Private Use Area. The Adobe Expert glyph
///     names (``Ssmall``, ``zerooldstyle``, ``Asuperior`` …) are IN the Adobe
///     Glyph List and map to the Corporate Use Subarea ``U+F600..U+F8FF``, so a
///     strict AGL lookup "succeeds" and yields text that no search will ever
///     match and no clipboard paste will ever render.
bool isUnusableExtractedCodepoint(int cp);

/// Recover a Unicode codepoint from a PostScript/PDF *glyphName*, going beyond
/// what ``fz_unicode_from_glyph_name`` does.
///
/// Resolution order (first usable answer wins; a PUA/control answer is treated
/// as no answer at every step):
///   1. strict Adobe Glyph List lookup;
///   2. the ``uniXXXX`` / ``uXXXX[XX]`` numeric conventions;
///   3. the ``name.suffix`` / ``name_suffix`` variant conventions — strip the
///      suffix and retry 1-2 (``S.sc``, ``one.tf``);
///   4. the Adobe Expert-set descriptive suffixes — ``small``, ``oldstyle``,
///      ``superior``, ``inferior``, ``numerator``, ``denominator``, ``smallhyphen``
///      — strip and retry 1-2, so ``Ssmall`` recovers ``S`` (U+0053) instead of
///      the Corporate-Use ``U+F773`` a strict AGL lookup returns.
///
/// Returns 0 when nothing usable can be recovered. Never throws, never longjmps.
int recoverUnicodeFromGlyphName(const std::string& glyphName);

/// Install a synthesized ``/ToUnicode`` CMap on every simple font reachable
/// from *page* that (a) has no ``/ToUnicode`` of its own and (b) has at least
/// one character code whose Unicode value :func:`recoverUnicodeFromGlyphName`
/// (or the font's base encoding) can recover but MuPDF's own glyph-name
/// handling cannot.
///
/// WHY THIS EXISTS (gap PDF-72 / GitHub #23). MuPDF already recovers a great
/// deal from glyph names — plain AGL names, ``uniXXXX``, and dotted/underscored
/// variants all extract correctly with no help from PDFE (measured, not
/// assumed). What it does NOT do is notice that an answer is unusable: an
/// Adobe Expert small-caps font extracts as Corporate-Use PUA codepoints, and a
/// font whose glyph names carry no Unicode information at all extracts as raw
/// character codes. Both read as "text extracted fine" to every caller while
/// being unsearchable — which is the redaction-by-search FALSE NEGATIVE class
/// recorded as A3-48: an operator redacts by searching for a word, the search
/// reports zero matches, and nothing is redacted.
///
/// SCOPE IS DELIBERATELY NARROW. A ``/ToUnicode`` is installed only when PDFE's
/// answer is strictly better than MuPDF's for at least one code, so an ordinary
/// document is left byte-identical. The synthesized CMap covers EVERY code the
/// font can map (not only the improved ones), because MuPDF stops consulting
/// glyph names entirely once a ``/ToUnicode`` is present — a partial CMap would
/// break the codes it omitted.
///
/// Returns the number of font dictionaries patched (0 on any error — this is
/// best-effort recovery and must never make a page fail to open).
int recoverMissingToUnicode(struct fz_context* ctx, struct fz_page* page);

}  // namespace pdfe::core
