#include "fonts/GlyphScaleAdjuster.h"
#include "fonts/TextShaper.h"

namespace pdfe::fonts {

GlyphScaleAdjuster::GlyphScaleAdjuster(TextShaper* shaper)
    : shaper_(shaper) {
}

GlyphScaleAdjuster::~GlyphScaleAdjuster() = default;

ScaleAdjustment GlyphScaleAdjuster::computeScale(
    void* originalFace,
    void* substituteFace,
    const std::u32string& text,
    float originalFontSize
) {
    ScaleAdjustment result;
    result.adjustedFontSize = originalFontSize;

    // Validate inputs
    if (!shaper_ || !originalFace || !substituteFace || text.empty() || originalFontSize <= 0.0f) {
        return result;
    }

    // Measure widths using both fonts at the same font size
    float originalWidth = shaper_->measureWidth(originalFace, text, originalFontSize);
    float substituteWidth = shaper_->measureWidth(substituteFace, text, originalFontSize);

    // Check for invalid measurements
    if (originalWidth <= 0.0f || substituteWidth <= 0.0f) {
        return result;
    }

    // Calculate scaling factors
    result.horizontalScale = originalWidth / substituteWidth;
    result.widthRatio = substituteWidth / originalWidth;

    return result;
}

} // namespace pdfe::fonts
