#pragma once

#include "FontMetrics.h"
#include <memory>
#include <string>

namespace pdfe::core {

/// Finds the best system font match for a requested font.
///
/// Matching priority:
/// 1. Exact PostScript name match
/// 2. Common font aliases (e.g., Helvetica → Arial)
/// 3. Family + style match
/// 4. Weighted scoring across all system fonts
/// 5. Bundled fallback fonts (DejaVu family)
///
/// This is the initial (Phase 2) implementation. Full metric-compatible
/// substitution with glyph-level analysis is deferred to Phase 6.
class BasicFontMatcher {
public:
    BasicFontMatcher();
    ~BasicFontMatcher();

    /// Find the best match for a font by its PostScript or family name.
    FontMetrics findMatch(const std::string& requestedFont);

    /// Find the best match with specific requirements.
    FontMetrics findMatch(
        const std::string& family,
        FontWeight weight,
        FontStyle style,
        bool preferSerif
    );

    // Non-copyable
    BasicFontMatcher(const BasicFontMatcher&) = delete;
    BasicFontMatcher& operator=(const BasicFontMatcher&) = delete;

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace pdfe::core
