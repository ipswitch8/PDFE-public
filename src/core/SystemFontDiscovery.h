#pragma once

#include "FontMetrics.h"
#include <cstdint>
#include <vector>
#include <memory>
#include <optional>
#include <string>

namespace pdfe::core {

/// Pipeline G phase-9: PANOSE-based serif classification.
///
/// PANOSE byte 1 is bSerifStyle. Per the OS/2 table spec:
///   0  = Any         (no PANOSE classification)
///   1  = No Fit      (no PANOSE match)
///   2..10 = serif subclasses
///   11..15 = sans-serif subclasses
namespace SystemFontDiscoveryPanose {

enum class SerifKind {
    Unknown,  ///< PANOSE byte 1 is 0 or 1; caller must use a fallback.
    Serif,
    Sans,
};

/// Classify a PANOSE byte-1 (bSerifStyle) value into Serif / Sans /
/// Unknown. Exposed at namespace scope so tests can lock the
/// byte→serif decision directly without touching DirectWrite.
inline SerifKind serifFromPanoseByte1(std::uint8_t b)
{
    if (b >= 2 && b <= 10) return SerifKind::Serif;
    if (b >= 11 && b <= 15) return SerifKind::Sans;
    return SerifKind::Unknown;
}

} // namespace SystemFontDiscoveryPanose

/// Enumerates system-installed fonts and provides lookup by name or properties.
/// Platform-specific: uses DirectWrite on Windows, Fontconfig on Linux/macOS.
class SystemFontDiscovery {
public:
    SystemFontDiscovery();
    ~SystemFontDiscovery();

    /// Enumerate all system fonts (cached after first call).
    const std::vector<FontMetrics>& enumerateFonts();

    /// Find font by exact PostScript name.
    std::optional<FontMetrics> findByPostScriptName(const std::string& name);

    /// Find font by family name and style properties.
    std::optional<FontMetrics> findByFamilyStyle(
        const std::string& family,
        FontWeight weight = FontWeight::Normal,
        FontStyle style = FontStyle::Normal
    );

    // Non-copyable
    SystemFontDiscovery(const SystemFontDiscovery&) = delete;
    SystemFontDiscovery& operator=(const SystemFontDiscovery&) = delete;

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace pdfe::core
