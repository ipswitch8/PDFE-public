#pragma once

#include "FontMetrics.h"
#include <vector>
#include <optional>
#include <string>

namespace pdfe::core {

/// Provides access to bundled fallback fonts (DejaVu family).
/// These fonts are embedded as Qt resources and available even when
/// the system has no matching fonts installed.
class BundledFonts {
public:
    /// Get metadata for all bundled fonts.
    static const std::vector<FontMetrics>& getFonts();

    /// Find a bundled font by family name.
    static std::optional<FontMetrics> findByFamily(const std::string& family);

    /// Get the default sans-serif font (DejaVu Sans).
    static FontMetrics getDefaultSansSerif();

    /// Get the default serif font (DejaVu Serif).
    static FontMetrics getDefaultSerif();

    /// Get the default monospace font (DejaVu Sans Mono).
    static FontMetrics getDefaultMonospace();

private:
    static std::vector<FontMetrics> initFonts();
};

} // namespace pdfe::core
