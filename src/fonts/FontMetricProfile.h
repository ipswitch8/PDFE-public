#pragma once

#include <string>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <set>

namespace pdfe::fonts {

/// Primary script classification for a font.
enum class FontScript : uint8_t {
    Unknown = 0,
    Latin,      ///< Primarily Latin (Western European)
    Arabic,     ///< Primarily Arabic
    Cyrillic,   ///< Primarily Cyrillic
    CJK,        ///< Primarily CJK (Chinese/Japanese/Korean)
    Devanagari, ///< Primarily Devanagari (Hindi, etc.)
    Hebrew,     ///< Primarily Hebrew
    Thai,       ///< Primarily Thai
    Other       ///< Primarily some other script
};

struct FontMetricProfile {
    // === Identity ===
    std::string postScriptName;
    std::string familyName;
    std::string styleName;

    // === Table Metrics (from OS/2 table, normalized to UPM) ===
    int unitsPerEm = 1000;
    float ascender = 0.0f;      // Normalized (0.0-1.0 of UPM)
    float descender = 0.0f;     // Negative
    float xHeight = 0.0f;       // Normalized
    float capHeight = 0.0f;     // Normalized
    int weightClass = 400;      // usWeightClass from OS/2
    int widthClass = 5;         // usWidthClass from OS/2
    uint8_t panose[10] = {};     // PANOSE classification from OS/2 table

    // === Measured Metrics (from glyph outline analysis) ===
    float stemWeightV = 0.0f;   // Vertical stem width (normalized)
    float stemWeightH = 0.0f;   // Horizontal stem width (normalized)
    float xToCapRatio = 0.0f;   // xHeight / capHeight
    float ascToCapRatio = 0.0f; // ascender / capHeight
    float avgCharWidth = 0.0f;  // Mean advance of lowercase a-z (normalized)
    /// Maximum cap-top-to-descender-bottom extent measured by actually
    /// rendering the font's glyphs at a sample pixel size, divided by
    /// that pixel size. Non-zero only when the analyzer has access to
    /// a renderable face (analyzeOwnedFace path). For text-edit overlays
    /// this is the value to use — it is independent of the font's
    /// (often unreliable) OS/2 declarations.
    float renderedVisibleExtent = 0.0f;
    /// The two components of @ref renderedVisibleExtent broken out so
    /// callers can position baselines correctly. renderedMaxTop = top of
    /// tallest rendered glyph from baseline / pixelSize; renderedMinBottom
    /// = bottom of deepest descender from baseline / pixelSize (a
    /// non-negative magnitude). Sum equals @ref renderedVisibleExtent.
    /// Both are 0 if the analyzer couldn't render glyphs.
    float renderedMaxTop = 0.0f;
    float renderedMinBottom = 0.0f;

    // === Structural Classification ===
    bool isSerif = false;       // Measured via horizontal extent analysis on 'I'
    bool isMonospace = false;   // All measured advances within 5% tolerance
    bool isItalic = false;      // Italic angle != 0 or style flag
    float italicAngle = 0.0f;   // From post table or glyph lean measurement
    float serifExtent = 0.0f;   // Width of serif relative to stem (0 = sans)

    // === Source ===
    std::string filePath;       // Path to font file (system fonts) or empty (embedded)

    // === Coverage ===
    int glyphCount = 0;         // Total glyphs in font
    bool hasLatinBasic = false;  // U+0020-007E
    bool hasLatinExtA = false;   // U+0100-017F
    bool hasCyrillic = false;    // U+0400-04FF
    bool hasCJK = false;         // U+4E00-9FFF

    // === Script Classification ===
    /// Dominant script by glyph count. Retained only as a FALLBACK hint for the
    /// script-mismatch penalty when @ref supportedScripts has not been populated
    /// (e.g. legacy callers / synthetic query profiles). For real fonts the
    /// authoritative answer to "can this font render script X" is
    /// @ref supportedScripts / @ref coversScript — a multi-script font (e.g.
    /// NotoSans) must not be reduced to a single dominant script.
    FontScript primaryScript = FontScript::Unknown;

    /// Full set of scripts the font actually has glyph coverage for, derived
    /// from glyph-range analysis (EmbeddedFontAnalyzer::classifyCoverage). A
    /// font is "compatible" with a needed script iff that script is a member.
    /// Empty means "not analyzed for coverage" — callers then fall back to the
    /// single @ref primaryScript comparison (preserves legacy behavior).
    std::set<FontScript> supportedScripts;

    // === Convenience ===
    bool isValid() const;

    /// True if this font can render @p s. Uses @ref supportedScripts when
    /// populated (multi-script aware, defect #8); otherwise falls back to the
    /// single @ref primaryScript so legacy/synthetic profiles behave exactly as
    /// before. FontScript::Unknown is always considered covered (fallback).
    bool coversScript(FontScript s) const;

    /// Metric + categorical distance to @p other (lower = better match).
    float distanceTo(const FontMetricProfile& other) const;

    /// Per-run overload (defect #7): drives the script-mismatch penalty from
    /// @p queryScriptOverride — the script of the specific text RUN being
    /// matched — instead of this profile's region-wide @ref primaryScript. Pass
    /// FontScript::Unknown to fall back to @ref primaryScript (identical to the
    /// single-argument overload). The penalty checks whether @p other (the
    /// candidate font) covers the needed run script.
    float distanceTo(const FontMetricProfile& other,
                     FontScript queryScriptOverride) const;
};

} // namespace pdfe::fonts
