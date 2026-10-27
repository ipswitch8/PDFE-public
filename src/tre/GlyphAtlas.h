#pragma once

#include "tre/GlyphTemplateCache.h"
#include "tre/TreTypes.h"
#include <QImage>
#include <string>
#include <unordered_map>
#include <vector>

namespace pdfe::tre {

/// Builds a character-to-image map from a scanned page using HOCR per-character
/// bounding boxes. Each character image is cropped from the original scan and
/// alpha-isolated (background removed, text preserved with anti-aliased edges).
///
/// When multiple instances of the same character are found, the highest-confidence
/// instance is kept.
class GlyphAtlas {
public:
    /// Build a glyph atlas from the scanned page image and HOCR word data.
    /// Extracts character images from the page image using per-char bboxes.
    ///
    /// @param pageImage    Full scanned page image (ARGB32 or RGB32).
    /// @param lines        HOCR lines with char-level data (HocrWord.chars).
    /// @param textColor    Detected text color for alpha isolation.
    /// @param bgColor      Detected background color for alpha isolation.
    void build(const QImage& pageImage,
               const std::vector<HocrLine>& lines,
               const RgbColor& textColor,
               const RgbColor& bgColor);

    /// Build a font-verified glyph atlas.
    /// 1. Harvests all chars from referenceLine unconditionally.
    /// 2. For each line in otherLines, verifies font compatibility via
    ///    LineFontVerifier before harvesting.
    /// @return Number of donor lines that passed font verification.
    int buildVerified(const QImage& pageImage,
                      const HocrLine& referenceLine,
                      const std::vector<HocrLine>& otherLines,
                      const RgbColor& textColor,
                      const RgbColor& bgColor,
                      const TemplateMap* templates = nullptr);

    /// Merge glyphs from another atlas. Only adds codepoints not already
    /// present in this atlas (preserves existing entries).
    void mergeFrom(const GlyphAtlas& donor);

    /// Add a pre-isolated glyph directly. Only inserts if codepoint is
    /// absent or new confidence is strictly higher.
    void addGlyph(char32_t codepoint, const QImage& isolatedImage, int confidence);

    /// Replace glyph image for an existing codepoint (height normalization).
    /// Does nothing if codepoint is not in the atlas.
    void replaceGlyphImage(char32_t codepoint, QImage image);

    /// Look up a character image. Returns null QImage if not in atlas.
    [[nodiscard]] QImage glyph(char32_t codepoint) const;

    /// Check if the atlas contains a given character.
    [[nodiscard]] bool contains(char32_t codepoint) const;

    /// Number of unique characters in the atlas.
    [[nodiscard]] int size() const;

    /// Coverage: what fraction of unique characters in the given text are available [0,1].
    [[nodiscard]] float coverage(const std::u32string& text) const;

    /// Return all codepoints in the atlas (for profile extraction).
    [[nodiscard]] std::vector<char32_t> codepoints() const;

    /// Apply alpha isolation to a cropped character image.
    /// Converts opaque RGB to transparent ARGB where text pixels are opaque
    /// and background pixels are transparent.
    static QImage isolateAlpha(const QImage& crop,
                               const RgbColor& textColor,
                               const RgbColor& bgColor);

    /// Does an isolated mask contain anything that is actually ink?
    ///
    /// GitHub #266. `isolateAlpha` gives alpha 255 where a pixel IS the text
    /// colour and 128 where it is equidistant from text and background, so a
    /// mask peaking at or below 128 contains no pixel that is predominantly
    /// text — whatever was cropped, it is not a letter. Exposed only so the
    /// rule can be tested at its boundary; the harvest calls the file-local
    /// implementation directly.
    static bool maskCarriesInkForTesting(const QImage& mask);

private:
    /// Harvest glyphs from a single HOCR line into the atlas.
    /// @param addMissingOnly  When true, skip codepoints already in the atlas
    ///                        (used for donor lines to preserve reference line glyphs).
    /// @param minNccScore     Minimum NCC threshold for accepting a glyph (0 = use default 0.15).
    void harvestLine(const QImage& pageImage, const HocrLine& line,
                     const RgbColor& textColor, const RgbColor& bgColor,
                     bool addMissingOnly = false,
                     const TemplateMap* templates = nullptr,
                     float minNccScore = 0.0f);

    struct GlyphEntry {
        QImage image;
        int confidence = 0;
        float nccScore = 0.0f;  ///< NCC vs template (0 if no template available)
    };
    std::unordered_map<char32_t, GlyphEntry> glyphs_;
};

} // namespace pdfe::tre
