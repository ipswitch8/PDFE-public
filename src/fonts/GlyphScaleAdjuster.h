#pragma once

#include <cstdint>
#include <string>

namespace pdfe::fonts {

class TextShaper;

struct ScaleAdjustment {
    float horizontalScale = 1.0f;   // Multiply substitute width by this to match original
    float adjustedFontSize = 0.0f;  // Font size (same as input by default)
    float widthRatio = 1.0f;        // substituteWidth / originalWidth
};

class GlyphScaleAdjuster {
public:
    explicit GlyphScaleAdjuster(TextShaper* shaper);
    ~GlyphScaleAdjuster();

    // Compute scaling to make substitute font text match original font's width.
    // Both faces must be OUR owned FT_Face (not MuPDF's).
    ScaleAdjustment computeScale(
        void* originalFace,
        void* substituteFace,
        const std::u32string& text,
        float originalFontSize
    );

    GlyphScaleAdjuster(const GlyphScaleAdjuster&) = delete;
    GlyphScaleAdjuster& operator=(const GlyphScaleAdjuster&) = delete;

private:
    TextShaper* shaper_;  // non-owning
};

} // namespace pdfe::fonts
