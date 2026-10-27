#include "BasicFontMatcher.h"
#include "SystemFontDiscovery.h"
#include "BundledFonts.h"

#include <algorithm>
#include <unordered_map>
#include <cmath>

namespace pdfe::core {

// ─── Font Alias Table ────────────────────────────────────────────────────────

/// Common font aliases: PDF fonts → system fonts.
static const std::unordered_map<std::string, std::string>& fontAliases() {
    static const std::unordered_map<std::string, std::string> aliases = {
        // Standard 14 PDF fonts → Windows equivalents
        {"helvetica",           "arial"},
        {"helvetica-bold",      "arial"},
        {"helvetica-oblique",   "arial"},
        {"helvetica-boldoblique","arial"},
        {"times",               "times new roman"},
        {"times-roman",         "times new roman"},
        {"times-bold",          "times new roman"},
        {"times-italic",        "times new roman"},
        {"times-bolditalic",    "times new roman"},
        {"courier",             "courier new"},
        {"courier-bold",        "courier new"},
        {"courier-oblique",     "courier new"},
        {"courier-boldoblique", "courier new"},
        {"symbol",              "symbol"},
        {"zapfdingbats",        "wingdings"},
        // Common replacements
        {"arialmt",             "arial"},
        {"arial-boldmt",        "arial"},
        {"arial-italicmt",      "arial"},
        {"timesnewromanpsmt",   "times new roman"},
        {"timesnewromanps-boldmt","times new roman"},
    };
    return aliases;
}

/// Infer weight and style from a PostScript name suffix.
static void inferStyleFromName(const std::string& name,
                                FontWeight& weight, FontStyle& style)
{
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    if (lower.find("bold") != std::string::npos) {
        weight = FontWeight::Bold;
    }
    if (lower.find("italic") != std::string::npos ||
        lower.find("oblique") != std::string::npos) {
        style = FontStyle::Italic;
    }
    if (lower.find("light") != std::string::npos) {
        weight = FontWeight::Light;
    }
    if (lower.find("thin") != std::string::npos) {
        weight = FontWeight::Thin;
    }
    if (lower.find("medium") != std::string::npos &&
        lower.find("bold") == std::string::npos) {
        weight = FontWeight::Medium;
    }
    if (lower.find("black") != std::string::npos ||
        lower.find("heavy") != std::string::npos) {
        weight = FontWeight::Black;
    }
}

/// Infer serif preference from name.
static bool inferSerifFromName(const std::string& name) {
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    // Explicit sans = not serif
    if (lower.find("sans") != std::string::npos) return false;
    // Known serif families
    if (lower.find("serif") != std::string::npos) return true;
    if (lower.find("times") != std::string::npos) return true;
    if (lower.find("georgia") != std::string::npos) return true;
    if (lower.find("garamond") != std::string::npos) return true;
    if (lower.find("palatino") != std::string::npos) return true;
    if (lower.find("cambria") != std::string::npos) return true;
    if (lower.find("book") != std::string::npos) return true;
    // Default: sans-serif
    return false;
}

// ─── Impl ────────────────────────────────────────────────────────────────────

class BasicFontMatcher::Impl {
public:
    SystemFontDiscovery discovery;

    /// Score how well a candidate matches a request. Higher = better.
    int scoreMatch(const FontMetrics& candidate,
                   const std::string& family,
                   FontWeight weight,
                   FontStyle style,
                   bool preferSerif)
    {
        int score = 0;

        // Weight distance (0-100 points, 100 = exact match)
        int weightDist = std::abs(candidate.weightValue() - static_cast<int>(weight));
        score += std::max(0, 100 - weightDist / 2);

        // Style match (50 points for exact, 25 for normal when seeking italic)
        if (candidate.style == style) {
            score += 50;
        } else if (style == FontStyle::Italic && candidate.style == FontStyle::Oblique) {
            score += 40;
        } else if (candidate.style == FontStyle::Normal) {
            score += 10;
        }

        // Serif/sans match (30 points)
        if (candidate.isSerif == preferSerif) {
            score += 30;
        }

        // Family name substring match (20 points)
        std::string lowerCandidate = candidate.familyName;
        std::transform(lowerCandidate.begin(), lowerCandidate.end(),
            lowerCandidate.begin(), ::tolower);
        std::string lowerFamily = family;
        std::transform(lowerFamily.begin(), lowerFamily.end(),
            lowerFamily.begin(), ::tolower);

        if (lowerCandidate == lowerFamily) {
            score += 20;
        } else if (lowerCandidate.find(lowerFamily) != std::string::npos ||
                   lowerFamily.find(lowerCandidate) != std::string::npos) {
            score += 10;
        }

        // Monospace match bonus (if both or neither are monospace)
        bool requestMono = (family.find("mono") != std::string::npos ||
                           family.find("courier") != std::string::npos ||
                           family.find("consol") != std::string::npos);
        if (candidate.isMonospace == requestMono) {
            score += 15;
        }

        return score;
    }
};

// ─── Public interface ────────────────────────────────────────────────────────

BasicFontMatcher::BasicFontMatcher()
    : pImpl(std::make_unique<Impl>())
{
}

BasicFontMatcher::~BasicFontMatcher() = default;

FontMetrics BasicFontMatcher::findMatch(const std::string& requestedFont) {
    // Step 1: Try exact PostScript name match
    auto exact = pImpl->discovery.findByPostScriptName(requestedFont);
    if (exact) return *exact;

    // Step 2: Check alias table
    std::string lowerReq = requestedFont;
    std::transform(lowerReq.begin(), lowerReq.end(), lowerReq.begin(), ::tolower);

    auto aliasIt = fontAliases().find(lowerReq);
    std::string targetFamily = (aliasIt != fontAliases().end())
        ? aliasIt->second : requestedFont;

    // Infer weight and style from the name
    FontWeight weight = FontWeight::Normal;
    FontStyle style = FontStyle::Normal;
    inferStyleFromName(requestedFont, weight, style);
    bool preferSerif = inferSerifFromName(requestedFont);

    // Step 3: Try family+style match
    auto familyMatch = pImpl->discovery.findByFamilyStyle(targetFamily, weight, style);
    if (familyMatch) return *familyMatch;

    // Step 4: Score all system fonts
    const auto& allFonts = pImpl->discovery.enumerateFonts();
    int bestScore = -1;
    const FontMetrics* bestFont = nullptr;

    for (const auto& fm : allFonts) {
        int score = pImpl->scoreMatch(fm, targetFamily, weight, style, preferSerif);
        if (score > bestScore) {
            bestScore = score;
            bestFont = &fm;
        }
    }

    // Use system font if score is reasonable (> 50)
    if (bestFont && bestScore > 50) {
        return *bestFont;
    }

    // Step 5: Fall back to bundled fonts
    if (preferSerif) {
        return BundledFonts::getDefaultSerif();
    }

    // Check if monospace requested
    std::string lowerTarget = targetFamily;
    std::transform(lowerTarget.begin(), lowerTarget.end(),
        lowerTarget.begin(), ::tolower);
    if (lowerTarget.find("mono") != std::string::npos ||
        lowerTarget.find("courier") != std::string::npos) {
        return BundledFonts::getDefaultMonospace();
    }

    return BundledFonts::getDefaultSansSerif();
}

FontMetrics BasicFontMatcher::findMatch(
    const std::string& family,
    FontWeight weight,
    FontStyle style,
    bool preferSerif)
{
    // Try exact family+style first
    auto match = pImpl->discovery.findByFamilyStyle(family, weight, style);
    if (match) return *match;

    // Score all system fonts
    const auto& allFonts = pImpl->discovery.enumerateFonts();
    int bestScore = -1;
    const FontMetrics* bestFont = nullptr;

    for (const auto& fm : allFonts) {
        int score = pImpl->scoreMatch(fm, family, weight, style, preferSerif);
        if (score > bestScore) {
            bestScore = score;
            bestFont = &fm;
        }
    }

    if (bestFont && bestScore > 50) {
        return *bestFont;
    }

    // Bundled fallback
    if (preferSerif) {
        if (weight == FontWeight::Bold) {
            // Find bold serif in bundled
            auto bundled = BundledFonts::findByFamily("DejaVu Serif");
            if (bundled && bundled->weight == FontWeight::Bold) return *bundled;
            const auto& fonts = BundledFonts::getFonts();
            auto it = std::find_if(fonts.begin(), fonts.end(),
                [](const auto& fm) {
                    return fm.isSerif && fm.weight == FontWeight::Bold;
                });
            if (it != fonts.end()) return *it;
        }
        return BundledFonts::getDefaultSerif();
    }

    return BundledFonts::getDefaultSansSerif();
}

} // namespace pdfe::core
