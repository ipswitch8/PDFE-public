#pragma once

#include "fonts/FontMetricProfile.h"
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace pdfe::core {
class SystemFontDiscovery;
struct FontMetrics;
}

namespace pdfe::fonts {

class EmbeddedFontAnalyzer;
class FontAnalysisCache;

struct SubstituteResult {
    FontMetricProfile profile;     // Selected substitute font profile
    std::string filePath;          // Path to system font file
    float matchScore = 0.0f;       // Lower = better (distance)
    bool isKnownPair = false;      // True if from known-pair table
};

class SubstituteFontSelector {
public:
    SubstituteFontSelector(
        EmbeddedFontAnalyzer* analyzer,
        FontAnalysisCache* cache
    );
    ~SubstituteFontSelector();

    // Find the best substitute for an embedded font that covers the needed chars.
    std::optional<SubstituteResult> findSubstitute(
        const FontMetricProfile& embeddedProfile,
        const std::u32string& textNeeded
    );

    // Return top N candidates sorted by match score (lowest/best first).
    // Used by NCC fallback when CNN is unavailable.
    std::vector<SubstituteResult> findTopCandidates(
        const FontMetricProfile& embeddedProfile,
        const std::u32string& textNeeded,
        int maxCandidates = 5
    );

    /// Return all non-decorative system fonts as candidates (no metric filtering).
    /// Used when CNN visual ranking is the primary discriminator and needs the
    /// broadest possible pool. Each result has matchScore = 0.
    std::vector<SubstituteResult> findAllNonDecorativeCandidates(
        const std::u32string& textNeeded
    );

    /// Look up a font name in the known-pair table and return the system font path.
    /// The name is lowercased and stripped of hyphens/spaces before matching.
    /// Returns empty string if no match found.
    static std::string lookupKnownPairPath(const std::string& fontName);

    SubstituteFontSelector(const SubstituteFontSelector&) = delete;
    SubstituteFontSelector& operator=(const SubstituteFontSelector&) = delete;

private:
    EmbeddedFontAnalyzer* analyzer_;
    FontAnalysisCache* cache_;
    std::unique_ptr<pdfe::core::SystemFontDiscovery> systemFonts_;

    // Known font pairs: embedded name → system font file path
    struct KnownPair {
        std::string embeddedPattern;  // Lowercase prefix match
        std::string systemFilePath;
    };
    static const std::vector<KnownPair>& knownPairs();

    // Try known pair lookup
    std::optional<SubstituteResult> tryKnownPair(
        const FontMetricProfile& embeddedProfile,
        const std::u32string& textNeeded
    );

    // Score candidates from system font enumeration
    std::optional<SubstituteResult> scoreSystemFonts(
        const FontMetricProfile& embeddedProfile,
        const std::u32string& textNeeded
    );
};

} // namespace pdfe::fonts
