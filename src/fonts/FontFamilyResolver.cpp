#include "FontFamilyResolver.h"
#include "core/SystemFontDiscovery.h"
#include "core/FontMetrics.h"
#include <unordered_map>
#include <vector>

namespace pdfe::fonts {

class FontFamilyResolver::Impl {
public:
    Impl() = default;

    std::string resolve(const std::string& familyName, bool bold, bool italic) {
        // Build cache key
        std::string cacheKey = familyName + "|" + (bold ? "b" : "") + "|" + (italic ? "i" : "");

        // Check cache
        auto it = cache_.find(cacheKey);
        if (it != cache_.end()) {
            return it->second;
        }

        // Map boolean flags to weight/style enums
        core::FontWeight weight = bold ? core::FontWeight::Bold : core::FontWeight::Normal;
        core::FontStyle style = italic ? core::FontStyle::Italic : core::FontStyle::Normal;

        // Build the lookup chain: the requested family first, then any
        // metric-compatible substitutes. Layout matches across these
        // because the substitutes share glyph widths with the originals.
        // Critical on Linux where Microsoft fonts (Arial, Times New Roman,
        // Calibri, etc.) typically aren't installed but the free
        // alternatives are.
        std::vector<std::string> candidates;
        candidates.push_back(familyName);
        for (const auto& alt : metricCompatibleSubstitutes(familyName)) {
            candidates.push_back(alt);
        }

        std::string filePath;
        for (const auto& candidate : candidates) {
            auto result = systemFonts_.findByFamilyStyle(candidate, weight, style);
            if (result.has_value()) {
                filePath = result->filePath;
                break;
            }
        }
        // If nothing matched, filePath remains empty (negative-cached).

        // Store in cache and return
        cache_[cacheKey] = filePath;
        return filePath;
    }

private:
    // Free metric-compatible substitutes for common Microsoft fonts.
    // Glyph widths and advances match the originals so document layout
    // is identical when the requested font is unavailable.
    static const std::vector<std::string>& metricCompatibleSubstitutes(
        const std::string& family)
    {
        static const std::vector<std::string> empty;
        static const std::unordered_map<std::string, std::vector<std::string>> table = {
            // Microsoft Core Fonts → free alternatives
            {"Arial",            {"Liberation Sans", "Arimo", "Nimbus Sans",
                                  "DejaVu Sans"}},
            {"Times New Roman",  {"Liberation Serif", "Tinos", "Nimbus Roman",
                                  "DejaVu Serif"}},
            {"Courier New",      {"Liberation Mono", "Cousine", "Nimbus Mono",
                                  "DejaVu Sans Mono"}},
            // Post-2007 Microsoft (Office) fonts → Chromium OS Croscore extras
            {"Calibri",          {"Carlito"}},
            {"Cambria",          {"Caladea"}},
            // Palatino-family serifs. Embedded subsets often carry the
            // foundry name "PalatinoLTPro"/"Palatino LT Pro", which is NOT an
            // installed family — the installed Windows family is "Palatino
            // Linotype". Without these aliases the resolver returns empty and
            // the caller falls back to a SANS face (Arial), producing a
            // sans-serif replacement glyph over serif body text. Map the
            // foundry/short names to the real installed serif and to free
            // metric-compatible serif substitutes for Linux/CI.
            {"PalatinoLTPro",    {"Palatino Linotype", "Palatino",
                                  "Book Antiqua", "URW Palladio L", "P052"}},
            {"Palatino LT Pro",  {"Palatino Linotype", "Palatino",
                                  "Book Antiqua", "URW Palladio L", "P052"}},
            {"Palatino Linotype",{"Palatino", "Book Antiqua",
                                  "URW Palladio L", "P052"}},
            {"Palatino",         {"Palatino Linotype", "Book Antiqua",
                                  "URW Palladio L", "P052"}},
            {"Book Antiqua",     {"Palatino Linotype", "Palatino",
                                  "URW Palladio L", "P052"}},
            // Reverse mappings for documents authored with the substitute
            // already — if "Liberation Sans" is requested but only "Arial"
            // is on the system, use Arial.
            {"Liberation Sans",  {"Arial", "Arimo", "DejaVu Sans"}},
            {"Liberation Serif", {"Times New Roman", "Tinos", "DejaVu Serif"}},
            {"Liberation Mono",  {"Courier New", "Cousine", "DejaVu Sans Mono"}},
            {"Arimo",            {"Arial", "Liberation Sans", "DejaVu Sans"}},
            {"Tinos",             {"Times New Roman", "Liberation Serif"}},
            {"Cousine",           {"Courier New", "Liberation Mono"}},
            {"Carlito",          {"Calibri"}},
            {"Caladea",          {"Cambria"}},
        };
        auto it = table.find(family);
        return (it != table.end()) ? it->second : empty;
    }

    core::SystemFontDiscovery systemFonts_;
    std::unordered_map<std::string, std::string> cache_;
};

FontFamilyResolver::FontFamilyResolver()
    : pImpl_(std::make_unique<Impl>()) {
}

FontFamilyResolver::~FontFamilyResolver() = default;

FontFamilyResolver::FontFamilyResolver(FontFamilyResolver&&) noexcept = default;
FontFamilyResolver& FontFamilyResolver::operator=(FontFamilyResolver&&) noexcept = default;

std::string FontFamilyResolver::resolve(const std::string& familyName,
                                         bool bold, bool italic) {
    return pImpl_->resolve(familyName, bold, italic);
}

} // namespace pdfe::fonts
