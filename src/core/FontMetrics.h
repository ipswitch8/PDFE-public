#pragma once

#include <string>

namespace pdfe::core {

enum class FontWeight {
    Thin = 100,
    ExtraLight = 200,
    Light = 300,
    Normal = 400,
    Medium = 500,
    SemiBold = 600,
    Bold = 700,
    ExtraBold = 800,
    Black = 900
};

enum class FontStyle {
    Normal,
    Italic,
    Oblique
};

enum class FontStretch {
    UltraCondensed,
    ExtraCondensed,
    Condensed,
    SemiCondensed,
    Normal,
    SemiExpanded,
    Expanded,
    ExtraExpanded,
    UltraExpanded
};

/// Font metadata and basic metrics used for font matching.
struct FontMetrics {
    std::string postScriptName;     ///< PostScript name (e.g., "ArialMT")
    std::string familyName;         ///< Font family (e.g., "Arial")
    std::string styleName;          ///< Style name (e.g., "Bold Italic")
    FontWeight weight = FontWeight::Normal;
    FontStyle style = FontStyle::Normal;
    FontStretch stretch = FontStretch::Normal;
    bool isSerif = false;
    bool isMonospace = false;

    // Basic metrics (from OS/2 and head tables, in font units)
    int unitsPerEm = 1000;
    int ascender = 0;
    int descender = 0;
    int xHeight = 0;
    int capHeight = 0;

    // File path (for system fonts)
    std::string filePath;

    /// Convenience: weight as integer for distance calculations.
    int weightValue() const { return static_cast<int>(weight); }
};

} // namespace pdfe::core
