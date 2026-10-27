#pragma once

#include <QImage>

namespace pdfe::tre {

class GlyphAtlas;

/// Measured visual characteristics of scanned text glyphs.
/// Computed from atlas glyphs during calibration, then used to
/// post-process FreeType fallback glyphs for visual consistency.
struct ScanStyle {
    float avgStemWidth = 0.0f;        ///< Median stem width from atlas glyphs (px)
    float edgeRoughness = 0.0f;       ///< Std-dev of alpha in edge band [20,240]
    float fgNoiseLevel = 0.0f;        ///< Std-dev of RGB in opaque (alpha>240) pixels
    float edgeTransitionWidth = 0.0f; ///< Avg pixels for alpha 20→240 transition
    float blurSigma = 0.0f;           ///< Derived: max(0, (edgeTransition - 1.5) * 0.4)
    int scanFgVal = 50;               ///< Foreground (ink) gray value from scan
    int scanBgVal = 210;              ///< Background (paper) gray value from scan
    int sampleCount = 0;
    bool valid = false;
};

/// Post-processes FreeType-rendered glyphs to match the visual appearance
/// of scanned text. Calibrate once from atlas glyphs, then apply() to
/// each FreeType fallback glyph.
///
/// Processing pipeline:
///   1. Stroke weight matching (morphological dilation/erosion)
///   2. Edge roughening (alpha noise in transition band)
///   3. Gaussian blur (soften crisp FreeType edges)
///   4. Foreground noise injection (RGB variation in opaque pixels)
class ScanStyleTransfer {
public:
    /// Measure scan characteristics from atlas glyphs.
    /// Call once after atlas is built; subsequent apply() calls use cached style.
    void calibrate(const GlyphAtlas& atlas);

    /// Get the measured scan style (for inspection/testing).
    [[nodiscard]] const ScanStyle& style() const { return style_; }

    /// C5: would the current calibration significantly degrade a
    /// crisp (e.g. LCD subpixel-rendered) FreeType glyph? Returns
    /// true when the blur sigma or foreground-noise level exceeds
    /// practical thresholds, meaning the subpixel detail would be
    /// erased by apply() and LCD rendering is wasted work.
    ///
    /// A false return indicates the current calibration is mild
    /// enough that LCD subpixel rendering survives the pipeline
    /// mostly intact — typically the case for clean, high-DPI
    /// scans with low measured blur.
    ///
    /// Default thresholds:
    ///   blurSigma > 0.5 → significant blur
    ///   fgNoiseLevel > 5.0 → significant interior noise
    ///
    /// See docs/image-pdf-risks.md (C5).
    [[nodiscard]] bool wouldSignificantlyDegrade(
        float blurThreshold = 0.5f,
        float noiseThreshold = 5.0f) const;

    /// Post-process a FreeType-rendered glyph to match scan appearance.
    /// Input/output: ARGB32_Premultiplied with FreeType coverage in alpha.
    /// Returns input unchanged if not calibrated or input is null.
    [[nodiscard]] QImage apply(const QImage& ftGlyph) const;

    /// Post-process a FreeType glyph composited onto a white background.
    /// Uses the same contrast remapping as the AtlasComparisonDialog's
    /// applyScanAppearance: maps black→scanFgVal, white→scanBgVal, adds
    /// paper noise and blur.  Produces grayscale output matching scan atlas
    /// glyph appearance for consistent overlay/save rendering.
    [[nodiscard]] QImage applyOnWhiteBg(const QImage& whiteBgGlyph) const;

    // --- Individual stages (public for unit testing) ---

    /// Adjust stroke weight via morphological dilation/erosion.
    /// Skipped if target stem width is within 15% of measured.
    static QImage adjustStrokeWeight(const QImage& glyph, float targetStemWidth);

    /// Add Gaussian noise to alpha channel in edge band [20,240].
    /// Seed ensures deterministic output for caching consistency.
    static QImage roughenEdges(const QImage& glyph, float roughness, uint32_t seed);

    /// Apply separable Gaussian blur to all ARGB channels.
    /// Skipped if sigma < 0.3.
    static QImage applyBlur(const QImage& glyph, float sigma);

    /// Add per-channel RGB noise to opaque (alpha>200) pixels.
    /// Skipped if noiseLevel < 1.0.
    static QImage injectNoise(const QImage& glyph, float noiseLevel, uint32_t seed);

    // --- Measurement utilities (public for testing) ---

    /// Std-dev of alpha values in edge band [20,240].
    /// NOTE: alpha-based. Affected by how isolateAlpha() computes
    /// the mask. Prefer measureEdgeRoughnessFromLuminance() when
    /// calibrating from atlas glyphs with authentic scan RGB.
    static float measureEdgeRoughness(const QImage& glyph);

    /// Std-dev of un-premultiplied RGB in opaque (alpha>240) pixels.
    static float measureForegroundNoise(const QImage& glyph);

    /// Average pixel count in alpha transitions (20→240) across horizontal scans.
    /// NOTE: alpha-based. Same caveat as measureEdgeRoughness.
    static float measureEdgeTransitionWidth(const QImage& glyph);

    /// C4-invariant: std-dev of RGB luminance values in pixels
    /// selected by the alpha edge band [20,240]. The alpha mask is
    /// only a region selector; the measurement is of authentic
    /// scan luminance content, so changes in how isolateAlpha()
    /// computes the mask do not shift the measurement. Used by
    /// calibrate() in place of measureEdgeRoughness().
    static float measureEdgeRoughnessFromLuminance(const QImage& glyph);

    /// C4-invariant: average pixel count for RGB luminance
    /// transitions (bright > 200 to dark < 60) across horizontal
    /// scans. Measures actual scan blur instead of alpha-channel
    /// transition width. Used by calibrate() in place of
    /// measureEdgeTransitionWidth().
    static float measureEdgeTransitionWidthFromLuminance(const QImage& glyph);

private:
    ScanStyle style_;
};

} // namespace pdfe::tre
