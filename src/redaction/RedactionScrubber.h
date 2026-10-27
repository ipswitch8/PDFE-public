#pragma once

// Confidentiality-surface scrubbing for redaction.
//
// `pdf_redact_page` only touches a page's content stream, its images and its
// redaction annotations. Everything ELSE that can carry the redacted text —
// form XObjects, overlapping annotations, the tagged-structure tree
// (/ActualText, /Alt), document metadata + XMP, embedded files, outlines and
// document JavaScript — is left untouched by MuPDF. This module scrubs those
// surfaces.
//
// Scope policy (deliberate, see RedactionApplyOptions):
//   * PAGE-scoped surfaces (form-XObject text, overlapping annotations,
//     struct-tree entries for the removed spans) are scrubbed
//     UNCONDITIONALLY — the user asked for that region to be gone.
//   * DOCUMENT-scoped surfaces (metadata/XMP, embedded files, outlines,
//     document JavaScript) are scrubbed only WHERE THEY CARRY THE REDACTED
//     TEXT. Blanket-deleting every bookmark because one page was redacted
//     would be its own defect. `fullSanitize` opts in to wholesale removal.
//
// MuPDF gotcha compliance: every entry point wraps its own fz_try/fz_catch so
// no fz_throw escapes across a caller's C++ stack objects, and all C++
// containers are constructed BEFORE the fz_try in the frame that owns them.

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <string>
#include <vector>

namespace pdfe::redaction {

/// Counters for what a scrub pass actually removed (used by tests + logging).
struct ScrubStats {
    int annotationsRemoved = 0;
    int xobjectStringsBlanked = 0;
    int pageStringsBlanked = 0;
    int structEntriesRemoved = 0;
    int infoKeysRemoved = 0;
    int xmpOccurrencesRemoved = 0;
    int embeddedFilesRemoved = 0;
    int outlineItemsRemoved = 0;
    int javascriptEntriesRemoved = 0;
};

/// Collect the whitespace-delimited words whose glyphs intersect any of
/// `rects`, i.e. the text the redaction is about to destroy. MUST be called
/// BEFORE pdf_redact_page — afterwards the text is gone.
/// Words shorter than `minLen` are ignored (they match far too broadly).
/// Appends to `wordsOut`; never throws.
void collectRedactedWords(fz_context* ctx,
                          fz_page* page,
                          const std::vector<fz_rect>& rects,
                          std::vector<std::string>& wordsOut,
                          size_t minLen = 4);

/// Page-scoped scrub. Call BEFORE pdf_redact_page (it deletes the
/// annotations that overlap the redaction, which must happen while the
/// redaction annotations are still present to identify them).
void scrubPageScopedSurfaces(fz_context* ctx,
                             pdf_document* doc,
                             pdf_page* page,
                             const std::vector<fz_rect>& rects,
                             const std::vector<std::string>& words,
                             ScrubStats& stats);

/// Residual scrub of the PAGE content stream itself. Call AFTER
/// pdf_redact_page. MuPDF removes the text-showing operators it considers
/// covered, but a redacted word can still sit in the content stream as a
/// marked-content property — `/Span << /ActualText (secret) >> BDC` — which
/// PDFE's own extractor can read back. This blanks any string operand still
/// carrying a redacted word. It is a no-op when the redaction already removed
/// everything (the normal case), so it never rewrites a clean page.
void scrubPageContentResiduals(fz_context* ctx,
                               pdf_document* doc,
                               pdf_page* page,
                               const std::vector<std::string>& words,
                               ScrubStats& stats);

/// Document-scoped scrub: metadata/XMP, embedded files, outlines, JavaScript.
/// With `fullSanitize == false` only entries carrying one of `words` are
/// removed; with `true` the surfaces are removed wholesale.
void scrubDocumentScopedSurfaces(fz_context* ctx,
                                 pdf_document* doc,
                                 const std::vector<std::string>& words,
                                 bool fullSanitize,
                                 ScrubStats& stats);

/// Exposed for testing: blank the contents of every PDF string literal / hex
/// string in `in` that contains one of `needles`, preserving operator
/// structure ("(SECRET)Tj" -> "()Tj"). Inline-image data (BI..ID..EI) is
/// copied verbatim. Returns the number of strings blanked; `out` receives the
/// rewritten stream.
int blankMatchingStrings(const std::string& in,
                         const std::vector<std::string>& needles,
                         std::string& out);

} // namespace pdfe::redaction
