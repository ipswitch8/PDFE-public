// Per-span provenance predicate tests (pipeline e7d3c2a1 Phase 5, defects #1+#5).
//
// These lock the contract that text-edit routing decisions are made from the
// provenance of the SPAN being edited, never from a page-wide image-area or
// "any font on the page is subsetted" verdict:
//
//   #1  TextSpan::isOcrInvisibleLayer() — a span keeps the Tr=3 invisible path
//       only when its OWN font is a glyphless OCR placeholder. A real vector
//       heading on an image-heavy page must classify as visible text.
//
//   #5  VectorSubsetDetector::hasSubsetPrefix(span.fontName) — the expensive
//       Type-3 / auto-resolution path runs only when THIS span's own font is
//       subsetted; a span using a complete font (e.g. Arial) stays out of it
//       even if some other font on the page is subsetted.

#include <gtest/gtest.h>

#include "textedit/TextEditModel.h"
#include "fonts/VectorSubsetDetector.h"

#include <string>

using pdfe::textedit::TextSpan;
using pdfe::textedit::isGlyphlessOcrFontName;
using pdfe::fonts::VectorSubsetDetector;

namespace {

TextSpan spanWithFont(const std::string& fontName) {
    TextSpan s;
    s.fontName = fontName;
    s.fontSize = 12.0f;
    return s;
}

} // namespace

// ─── #1: isOcrInvisibleLayer() — the five required provenance cases ──────────

// Case 1: an OCR invisible-text layer span → true.
TEST(TextSpanProvenance, OcrInvisibleLayer_True) {
    EXPECT_TRUE(spanWithFont("GlyphLessFont").isOcrInvisibleLayer());
}

// Case 2: a live embedded font (real text-show operators) → false.
TEST(TextSpanProvenance, LiveEmbeddedFont_False) {
    EXPECT_FALSE(spanWithFont("ArialMT").isOcrInvisibleLayer());
    EXPECT_FALSE(spanWithFont("Helvetica-Bold").isOcrInvisibleLayer());
}

// Case 3: the span's own font is complete even though another font on the
// page may be subsetted. The predicate inspects only this span → false.
TEST(TextSpanProvenance, SpanCompleteFontWhilePageHasSubset_False) {
    // "Arial" is the span's own (complete) font. Whatever else the page
    // carries is irrelevant to a per-span decision.
    EXPECT_FALSE(spanWithFont("Arial").isOcrInvisibleLayer());
}

// Case 4: a span lifted from a scanned page's OCR layer → true. Scanned-page
// OCR layers are emitted with a glyphless placeholder font.
TEST(TextSpanProvenance, ScannedPageOcrSpan_True) {
    EXPECT_TRUE(spanWithFont("GlyphLessFont").isOcrInvisibleLayer());
    // Subset-prefixed glyphless font (some producers wrap it) still matches.
    EXPECT_TRUE(spanWithFont("ABCDEF+GlyphLessFont").isOcrInvisibleLayer());
}

// Case 5: a span from a vector page drawn with a (subset-prefixed) embedded
// real font → false. The subset prefix marks a genuine embedded font, not an
// OCR layer — it must render as visible vector text.
TEST(TextSpanProvenance, VectorPageEmbeddedFont_False) {
    EXPECT_FALSE(spanWithFont("ABCDEF+LiberationSerif").isOcrInvisibleLayer());
    EXPECT_FALSE(spanWithFont("REFXWK+Liberation Serif").isOcrInvisibleLayer());
}

// ─── #1 robustness: name matching is provenance, not a brittle exact string ──

TEST(TextSpanProvenance, GlyphlessMatchIsCaseInsensitive) {
    EXPECT_TRUE(isGlyphlessOcrFontName("glyphlessfont"));
    EXPECT_TRUE(isGlyphlessOcrFontName("GLYPHLESSFONT"));
    EXPECT_TRUE(isGlyphlessOcrFontName("Glyphless"));
}

TEST(TextSpanProvenance, EmptyOrUnknownFontIsNotInvisible) {
    // The safe failure mode is "visible": anything not clearly glyphless is
    // treated as real text so edits never silently vanish.
    EXPECT_FALSE(spanWithFont("").isOcrInvisibleLayer());
    EXPECT_FALSE(spanWithFont("TimesNewRomanPSMT").isOcrInvisibleLayer());
    EXPECT_FALSE(isGlyphlessOcrFontName("MyCoolFont"));
}

// ─── #5: per-span subset gate for the Type-3 / auto-resolution path ──────────
//
// applyText() runs AutoMissingGlyphResolver only when
// hasSubsetPrefix(span.fontName) is true. These document that the gate is a
// pure function of the span's OWN font name, so a complete-font span never
// enters the expensive path regardless of other fonts on the page.

TEST(TextSpanProvenance, CompleteFontSpanSkipsType3Path) {
    // Arial (complete) → no subset prefix → applyText does NOT auto-resolve.
    EXPECT_FALSE(VectorSubsetDetector::hasSubsetPrefix(spanWithFont("Arial").fontName));
    EXPECT_FALSE(VectorSubsetDetector::hasSubsetPrefix(spanWithFont("ArialMT").fontName));
}

TEST(TextSpanProvenance, SubsettedFontSpanEntersType3Path) {
    // A genuinely subsetted span → prefix present → applyText auto-resolves
    // unencodable codepoints for THIS span only.
    EXPECT_TRUE(VectorSubsetDetector::hasSubsetPrefix(
        spanWithFont("ABCDEF+LiberationSerif").fontName));
}

// A glyphless OCR span is invisible-layer (#1) AND is not treated as a
// vector-subset Type-3 candidate by font name unless it carries a subset
// prefix — the two predicates are independent and per-span.
TEST(TextSpanProvenance, OcrAndSubsetPredicatesAreIndependent) {
    TextSpan ocr = spanWithFont("GlyphLessFont");
    EXPECT_TRUE(ocr.isOcrInvisibleLayer());
    EXPECT_FALSE(VectorSubsetDetector::hasSubsetPrefix(ocr.fontName));

    TextSpan vec = spanWithFont("ABCDEF+LiberationSerif");
    EXPECT_FALSE(vec.isOcrInvisibleLayer());
    EXPECT_TRUE(VectorSubsetDetector::hasSubsetPrefix(vec.fontName));
}
