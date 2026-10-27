#pragma once

#include "textedit/TextEditModel.h"

#include <string>
#include <vector>

namespace pdfe::core {
class Document;
class MuPdfContext;
class Page;
}

namespace pdfe::tre {

/// Phase 6 plumbing: given the PDF font name of a subsetted embedded
/// font and a typed text string, decide which codepoints can't be
/// encoded by that font and produce `FormattingRun` entries that route
/// the unencodable ranges to either:
///   1. A new Type 3 PDF font bundle built from `EnrichedAtlas`-
///      synthesized glyphs (recipe transforms + style-transferred
///      FreeType fallback) — preserves the embedded font's visual
///      identity.
///   2. A metric-compatible system font picked by
///      `SubstituteFontSelector` — used when the atlas can't supply
///      the codepoint at acceptable quality.
///
/// Codepoints the source font already encodes are left out of the
/// returned runs entirely (the existing emit path will use the source
/// font for those indices). The runs returned cover ONLY the
/// unencodable indices, so the caller can append them to any
/// pre-existing user-formatting runs without conflict.
class AutoMissingGlyphResolver {
public:
    /// Resolve unencodable codepoints in @p text against the embedded
    /// font @p sourceFontName on @p page. Returns a vector of
    /// `FormattingRun` entries, each covering one contiguous range of
    /// unencodable indices and pointing at either a Type 3 bundle's
    /// resource name (via `preInstalledResName`) or a system font
    /// (via `fontFilePath`). Returns an empty vector when:
    ///   - The source font isn't found on the page.
    ///   - All typed codepoints are encodable in the source font.
    ///   - The atlas/synthesis pipeline can't run (missing tessdata,
    ///     malformed font, etc.) — caller falls back to the existing
    ///     emit path with no runs.
    ///
    /// Side effect: if any synthesized Type 3 fonts are produced, they
    /// are installed on @p page (added to `/Resources/Font` under fresh
    /// `T3subN` names) before this function returns. The returned runs'
    /// `preInstalledResName` fields reference those installed names.
    /// @param descriptorItalic  Italic per the source span's /FontDescriptor
    ///     (name-independent). When this or @p descriptorBold is true it takes
    ///     precedence over the font-name keyword scan for the fallback style, so
    ///     a style the name doesn't spell out is still matched. Both-false falls
    ///     back to name parsing.
    /// @param descriptorBold  Bold per the source span's /FontDescriptor.
    static std::vector<pdfe::textedit::FormattingRun> resolve(
        pdfe::core::MuPdfContext* context,
        pdfe::core::Page* page,
        pdfe::core::Document* document,
        const std::string& sourceFontName,
        float sourceFontSizePt,
        const std::u32string& text,
        bool descriptorItalic = false,
        bool descriptorBold = false);

    /// Resolved font style: bare family plus the bold/italic decision the
    /// fallback path uses. Exposed for testing the descriptor-vs-name precedence.
    struct ResolvedStyle {
        std::string family;
        bool bold = false;
        bool italic = false;
    };

    /// Parse @p baseFontName into family + style. When @p descriptorItalic or
    /// @p descriptorBold is true, the descriptor flags take precedence over the
    /// name-keyword scan for the style (family is always name-derived). This is
    /// the exact decision the new-character fallback makes; exposed so it can be
    /// unit-tested with a name that carries no style keyword.
    static ResolvedStyle parseStyle(const std::string& baseFontName,
                                    bool descriptorItalic = false,
                                    bool descriptorBold = false);

private:
    /// Friend-access helper: extract the embedded font's binary stream
    /// for the named font on the page. Returns empty bytes on miss.
    static std::vector<unsigned char> extractEmbeddedFontBytes(
        pdfe::core::MuPdfContext* context,
        pdfe::core::Page* page,
        pdfe::core::Document* document,
        const std::string& targetBaseFont);
};

} // namespace pdfe::tre
