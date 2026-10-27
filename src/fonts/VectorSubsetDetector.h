#pragma once

#include <string>
#include <vector>

namespace pdfe::core {
class Page;
class Document;
class MuPdfContext;
}

namespace pdfe::fonts {

/// How a page's text resources should be classified for the text-edit pipeline.
///
/// The text-edit pipeline previously used a single binary gate
/// (`Page::isImageDominated()`): true → run the OCR + glyph-atlas + synthesis
/// pipeline; false → re-encode through the embedded font and accept .notdef
/// for unencodable codepoints. That binary loses the case where a page has
/// real vector text but the embedded fonts are subsetted and therefore
/// can't encode anything the user types beyond what was already on the page.
///
/// PageFontClass replaces the binary so the four downstream branches
/// (atlas-overlay, vector-subset extension, vector-complete passthrough,
/// image-dominated invisible-text) can be selected explicitly.
enum class PageFontClass {
    ImageDominated,    ///< Bitmap/scan with invisible OCR text
    VectorSubsetted,   ///< Vector text, at least one embedded font is subsetted
    VectorComplete,    ///< Vector text, no subsetted embedded fonts (or no fonts)
};

class VectorSubsetDetector {
public:
    /// Classify a page's font resources for routing in the text-edit pipeline.
    ///
    /// Order of precedence:
    ///   1. ImageDominated (delegated to `Page::isImageDominated`).
    ///   2. VectorSubsetted iff any font on the page has a 6-letter
    ///      subset prefix (e.g. "AAAAAA+Alef") or its descendant CIDFont
    ///      does.
    ///   3. VectorComplete otherwise.
    static PageFontClass classify(
        pdfe::core::Page* page,
        pdfe::core::Document* document,
        pdfe::core::MuPdfContext* context);

    /// Return the list of `BaseFont` names on the page that have a subset
    /// prefix. Order matches the page's `/Font` dict iteration order; entries
    /// from `/DescendantFonts[0]` for Type0 wrappers report the descendant's
    /// `BaseFont`. Returns an empty vector for pages with no subsetted fonts.
    static std::vector<std::string> subsettedFontNames(
        pdfe::core::Page* page,
        pdfe::core::Document* document,
        pdfe::core::MuPdfContext* context);

    /// True if @p baseFont starts with the standard PDF subset prefix
    /// (six uppercase ASCII letters followed by '+'). Public so tests and
    /// other callers don't have to re-implement the check.
    static bool hasSubsetPrefix(const std::string& baseFont);

private:
    /// Friend-access helper: collect every BaseFont name on the page,
    /// including DescendantFonts[0] BaseFont for Type0 wrappers.
    /// Lives on the class so `friend class VectorSubsetDetector` in
    /// `Page` grants it access to `Page::fzPage()`.
    static std::vector<std::string> collectBaseFontNames(
        pdfe::core::Page* page,
        pdfe::core::Document* document,
        pdfe::core::MuPdfContext* context);
};

} // namespace pdfe::fonts
