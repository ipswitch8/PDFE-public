#pragma once

#include <QImage>
#include <string>
#include <unordered_map>
#include <vector>

namespace pdfe::tre {

/// Type alias for a map of codepoint → rendered template image.
using TemplateMap = std::unordered_map<char32_t, QImage>;

/// Renders single-character reference templates using FreeType.
///
/// These templates are used by GlyphAtlas::harvestLine() to score candidate
/// glyph crops from the scan against the expected character shape. The crop
/// with the highest NCC (normalized cross-correlation) against the template
/// is selected, avoiding contamination from adjacent characters.
class GlyphTemplateCache {
public:
    /// Render templates for the given codepoints using the specified font.
    ///
    /// Each template is a grayscale-on-transparent image at the given pixel
    /// height, with the glyph positioned using the font's ascender metric.
    /// Codepoints not present in the font are silently omitted.
    ///
    /// @param fontFilePath   Path to the .ttf/.otf font file.
    /// @param fontFaceIndex  Face index within the font file (usually 0).
    /// @param pixelHeight    Target image height in pixels (typically lineH from HOCR).
    /// @param codepoints     Codepoints to render templates for.
    /// @return Map from codepoint to rendered template. Missing glyphs omitted.
    static TemplateMap renderTemplates(
        const std::string& fontFilePath,
        int fontFaceIndex,
        int pixelHeight,
        const std::vector<char32_t>& codepoints);
};

} // namespace pdfe::tre
