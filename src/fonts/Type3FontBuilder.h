#pragma once

#include <QImage>
#include <QRect>

#include <cstdint>
#include <string>
#include <vector>

namespace pdfe::core {
class MuPdfContext;
class Document;
}

namespace pdfe::fonts {

/// One glyph entry for a Type 3 font under construction.
///
/// Glyph space convention: PDF Type 3 fonts canonically use a 1000-unit
/// em (matched by the FontMatrix `[0.001 0 0 0.001 0 0]` we emit). All
/// metrics in this struct are expressed in those 1000-units-per-em
/// glyph-space coordinates. The bitmap itself is sampled at whatever
/// resolution the caller produced (e.g. EnrichedAtlas reference line
/// height in OCR pixels) and is mapped onto the bbox at render time.
struct Type3Glyph {
    char32_t codepoint = 0;     ///< Unicode codepoint
    QImage image;               ///< Grayscale-or-ARGB; alpha is interpreted as ink coverage
    int advanceWidth = 1000;    ///< Glyph advance in 1/1000 em units (W in horizontal text)
    QRect bbox;                 ///< Glyph painted region in 1/1000 em units; `(x,y,w,h)`
};

/// Builds a PDF Type 3 font dictionary from a vector of `Type3Glyph`.
///
/// Output: an indirect-reference `pdf_obj*` that points to a font dict
/// of the form
///   << /Type /Font /Subtype /Type3
///      /FontBBox [...] /FontMatrix [0.001 0 0 0.001 0 0]
///      /CharProcs << /uXXXX <stream-ref> ... >>
///      /Encoding << /Type /Encoding /Differences [c1 /uX c2 /uY ...] >>
///      /FirstChar c /LastChar c
///      /Widths [w0 w1 ...]
///      /Resources << /ProcSet [/PDF /ImageB] >>
///   >>
///
/// Each CharProc stream is `d0 wx 0 q wPx 0 0 hPx 0 0 cm BI...ID...EI Q`,
/// using an ASCIIHex-filtered grayscale inline image so the painted ink
/// inherits the current text-state fill color (per PDF §9.6.5).
///
/// Caller is responsible for adding the returned indirect ref to the
/// page's `/Resources/Font` dict under a unique name.
class Type3FontBuilder {
public:
    /// Build a Type 3 font dict from the given glyphs.
    /// Returns the indirect reference, or `nullptr` if `glyphs` is empty
    /// or any required pointer is null. The returned pointer is owned by
    /// the PDF document and remains valid for the document's lifetime.
    static void* build(
        pdfe::core::MuPdfContext* context,
        pdfe::core::Document* document,
        const std::vector<Type3Glyph>& glyphs);

    /// PDF character name for a Unicode codepoint, formatted as `/uXXXX`
    /// (4 uppercase hex digits) for codepoints < 0x10000, `/uXXXXXXXX`
    /// for codepoints in the supplementary planes. Public so tests can
    /// verify the encoding stability without re-implementing the rule.
    static std::string nameForCodepoint(char32_t codepoint);
};

} // namespace pdfe::fonts
