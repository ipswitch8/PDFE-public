#pragma once

#include <QImage>
#include <string>
#include <unordered_map>
#include <vector>

namespace pdfe::tre {

class GlyphAtlas;

/// Aggregate stroke characteristics extracted from atlas glyphs.
/// Captures the visual "fingerprint" of the scanned font for
/// consistent synthesis of missing characters.
struct FontStyleProfile {
    float avgStemWidth = 0.0f;       ///< Average vertical stem width (px)
    float avgStrokeWeight = 0.0f;    ///< Stem width / glyph height ratio
    float avgInkDensity = 0.0f;      ///< Foreground pixel ratio in bounding box
    float avgCharWidth = 0.0f;       ///< Mean character width (px)
    float avgCharHeight = 0.0f;      ///< Mean character height (px)
    float avgAspectRatio = 0.0f;     ///< Width / height
    float serifExtent = 0.0f;        ///< Baseline excess ratio (0=sans-serif, higher=more serif)
    float slantAngle = 0.0f;         ///< Degrees (0=upright, >5=italic)
    int sampleCount = 0;             ///< Glyphs contributing to profile
};

/// Describes how to synthesize a missing character from donor glyphs.
struct GlyphRecipe {
    char32_t target = 0;

    struct Strategy {
        enum Transform {
            Identity,        ///< Use donor as-is
            HFlip,           ///< Mirror left-right (e.g., d -> b)
            VFlip,           ///< Mirror top-bottom (e.g., b -> p)
            Rotate180,       ///< 180-degree rotation (e.g., 6 -> 9)
            ScaleToXHeight,  ///< Scale down to x-height (~70% of cap height)
            ScaleToCapHeight,///< Scale up to cap height (~143% of x-height)
            Composite        ///< Combine parts from multiple donors
        };

        char32_t donor = 0;
        Transform transform = Identity;
        float expectedQuality = 0.5f;

        /// For Composite transforms: sub-parts to combine.
        struct Part {
            char32_t donor = 0;
            float srcY0 = 0.0f, srcY1 = 1.0f;  ///< Normalized [0,1] source Y range
            float dstY0 = 0.0f, dstY1 = 1.0f;  ///< Normalized [0,1] destination Y range
            float srcX0 = 0.0f, srcX1 = 1.0f;  ///< Normalized [0,1] source X range
            float dstX0 = 0.0f, dstX1 = 1.0f;  ///< Normalized [0,1] destination X range
            Transform partTransform = Identity;
        };
        std::vector<Part> parts;  ///< Only used for Composite
    };

    std::vector<Strategy> strategies;  ///< Ordered best-first
};

/// Result of attempting to synthesize a glyph.
struct SynthesisResult {
    bool success = false;
    QImage image;              ///< ARGB32_Premultiplied, same format as atlas glyphs
    float quality = 0.0f;      ///< Estimated quality [0,1]
    char32_t donorUsed = 0;    ///< Which donor character was used
};

/// Synthesizes missing characters from available atlas glyphs by analyzing
/// stroke characteristics and applying geometric transformations.
///
/// When the glyph atlas is missing characters, this class attempts to create
/// visually consistent substitutes by:
///   1. Extracting a stroke profile from available atlas glyphs
///   2. Looking up synthesis recipes (e.g., 'b' from flipping 'd')
///   3. Applying geometric transforms and stroke weight matching
///
/// Falls back to FreeType (via BitmapCompositor) only when no suitable
/// donor glyph exists or synthesis quality is too low.
class GlyphSynthesizer {
public:
    GlyphSynthesizer();

    /// Extract font style profile from atlas glyphs.
    /// Must be called before synthesize().
    void buildProfile(const GlyphAtlas& atlas);

    /// Get the computed font style profile.
    [[nodiscard]] const FontStyleProfile& profile() const;

    /// Attempt to synthesize a missing glyph.
    /// Returns a null image if no suitable donor exists.
    [[nodiscard]] SynthesisResult synthesize(
        char32_t target,
        const GlyphAtlas& atlas) const;

    /// Synthesize all missing characters for a text string.
    /// Returns a map of codepoint -> synthesized QImage for successes.
    /// Characters already in the atlas are skipped.
    [[nodiscard]] std::unordered_map<char32_t, QImage> synthesizeAll(
        const std::u32string& text,
        const GlyphAtlas& atlas) const;

    /// Minimum quality threshold for accepting a synthesis.
    static constexpr float kMinQualityThreshold = 0.3f;

    // Public for testing
    static float measureStemWidth(const QImage& glyph);
    static float measureInkDensity(const QImage& glyph);
    static float measureSlant(const QImage& glyph);
    static float measureSerifExtent(const QImage& glyph);
    static const GlyphRecipe* findRecipe(char32_t target);
    static float scoreQuality(const QImage& synth, const FontStyleProfile& profile);

private:
    static FontStyleProfile extractProfile(const GlyphAtlas& atlas);
    static QImage applyTransform(const QImage& donor,
                                  GlyphRecipe::Strategy::Transform transform);
    static QImage applyComposite(const std::vector<GlyphRecipe::Strategy::Part>& parts,
                                  const GlyphAtlas& atlas, int targetW, int targetH);
    static QImage matchStrokeWeight(const QImage& glyph,
                                     const FontStyleProfile& profile);

    FontStyleProfile profile_;
    bool profileBuilt_ = false;
};

} // namespace pdfe::tre
