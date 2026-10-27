#pragma once

#include "tre/TreTypes.h"
#include <QImage>
#include <vector>

namespace pdfe::tre {

/// Result of verifying whether two HOCR lines share the same font.
struct FontVerifyResult {
    bool compatible = false;     ///< True if the candidate line appears to use the same font
    float confidence = 0.0f;     ///< Overall confidence [0, 1] (weighted combination)
    float nccScore = 0.0f;       ///< Pixel-based NCC of shared characters [0, 1]
    float metricScore = 0.0f;    ///< HOCR metric agreement [0, 1]
    int sharedChars = 0;         ///< Number of codepoints compared via NCC
};

/// Verifies whether two HOCR lines use the same visual font by:
///   1. Comparing HOCR metric properties (xSizePx, ascender/descender ratios)
///   2. Performing pixel-based NCC on shared character glyphs cropped from the page
///
/// Rejects candidate lines that differ significantly, preventing wrong-font
/// glyphs from contaminating the atlas.
class LineFontVerifier {
public:
    /// Thresholds for font compatibility determination.
    struct Thresholds {
        float minCombinedScore = 0.55f;      ///< Minimum weighted combination for compatibility
        float minMetricScore = 0.60f;        ///< Below this -> reject without NCC (early exit)
        float metricWeight = 0.3f;           ///< Weight for metric score in combination
        float nccWeight = 0.7f;              ///< Weight for NCC score in combination
        int minSharedCharsForNcc = 2;        ///< Need at least this many shared chars for NCC
    };

    /// Verify whether candidateLine uses the same font as referenceLine.
    /// @param pageImage     The full scanned page image.
    /// @param refLine       The reference line (edited line or highest-confidence line).
    /// @param candidateLine The line to check for font compatibility.
    /// @param thresholds    Configurable thresholds (defaults are reasonable).
    /// @return Verification result with scores and compatibility flag.
    static FontVerifyResult verify(
        const QImage& pageImage,
        const HocrLine& refLine,
        const HocrLine& candidateLine,
        const Thresholds& thresholds = defaultThresholds());

private:
    /// Returns a Thresholds with default values. Used as the default
    /// argument for verify(); calling it from a function body lets us
    /// avoid using Thresholds's default member initializers in the
    /// declaration context, which clang-cl rejects under C++17.
    static Thresholds defaultThresholds();

    /// Compare HOCR line-level metric properties.
    /// Returns a score [0, 1] where 1.0 means metrics are identical.
    static float compareMetrics(const HocrLine& refLine, const HocrLine& candidateLine);

    /// Find codepoints present in both lines (intersection of character sets).
    static std::vector<char32_t> findSharedCodepoints(
        const HocrLine& lineA, const HocrLine& lineB);

    /// Compare shared character glyphs via binarized NCC.
    /// Crops each shared character from the page image and compares pixel patterns.
    /// Returns average NCC across all shared pairs, or -1 if no valid comparisons.
    static float compareSharedGlyphs(
        const QImage& pageImage,
        const HocrLine& refLine,
        const HocrLine& candidateLine,
        const std::vector<char32_t>& sharedCodepoints);

    /// Crop a character's bounding box from the page image.
    /// Returns null QImage if bbox is invalid or out of bounds.
    static QImage cropChar(const QImage& pageImage, const HocrChar& ch);

    /// Find the highest-confidence instance of a codepoint in a line.
    static const HocrChar* findBestChar(const HocrLine& line, char32_t codepoint);
};

} // namespace pdfe::tre
