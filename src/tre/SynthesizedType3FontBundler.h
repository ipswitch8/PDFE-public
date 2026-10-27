#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace pdfe::core {
class Document;
class MuPdfContext;
class Page;
}

namespace pdfe::tre {
class EnrichedAtlas;

/// Result of bundling a set of EnrichedAtlas-synthesized glyphs into a
/// fresh Type 3 PDF font resource on a page.
///
/// `fontIndirectRef` is the `pdf_obj*` for the new font dict (already
/// added to the page's `/Resources/Font` dict under `resourceName`).
/// `codepointToCode` maps Unicode codepoints to their assigned
/// 1-byte font code — content-stream emitters convert a typed string
/// into the byte sequence that selects each glyph in this font.
/// `codepointsBundled` is the same set in iteration order, preserved
/// for callers that want to know which codepoints made it into the
/// bundle (some may have been skipped when the atlas couldn't supply
/// them).
struct SynthesizedFontBundle {
    void* fontIndirectRef = nullptr;
    std::string resourceName;
    std::vector<char32_t> codepointsBundled;
    std::unordered_map<char32_t, uint8_t> codepointToCode;
};

/// Builds a Type 3 PDF font from EnrichedAtlas-synthesized glyphs and
/// installs it on the given page as a new `/Font` resource.
///
/// Supports the option-B end state: when a vector PDF span has typed
/// codepoints that are missing from its embedded subset, this class
/// asks the EnrichedAtlas (built per-line by phase 3) for synthesized
/// images of those codepoints, packages them into a Type 3 font via
/// `Type3FontBuilder`, and adds the result to the page so a
/// content-stream font switch + show-string call can render the
/// missing glyphs in their actual synthesized pixel form (preserving
/// the unique-font visual identity).
///
/// Codepoints the EnrichedAtlas can't supply (atlas miss + no recipe
/// + no FreeType fallback path configured) are excluded from the
/// bundle. Phase 6's wiring falls back to `SubstituteFontSelector` for
/// those.
class SynthesizedType3FontBundler {
public:
    /// Build and install a Type 3 font bundle on @p page covering the
    /// requested @p missingCodepoints.
    ///
    /// @return  Bundle with `fontIndirectRef` non-null and
    ///          `codepointsBundled` non-empty when at least one
    ///          codepoint was successfully bundled. When the atlas
    ///          can't supply any of the requested codepoints, returns
    ///          a bundle with `fontIndirectRef == nullptr` and
    ///          `codepointsBundled` empty (the page's `/Font` dict is
    ///          left untouched in that case).
    static SynthesizedFontBundle build(
        pdfe::core::MuPdfContext* context,
        pdfe::core::Page* page,
        pdfe::core::Document* document,
        const EnrichedAtlas& atlas,
        const std::vector<char32_t>& missingCodepoints);
};

} // namespace pdfe::tre
