#pragma once

#include <QColor>
#include <QPalette>

namespace pdfe::ui {

class AccessibilityHelper {
public:
    /// Check if the system is in high-contrast mode.
    /// Windows: uses SPI_GETHIGHCONTRAST via SystemParametersInfoW.
    /// Fallback: checks if QPalette window/text luminance ratio > 7:1.
    /// If a test override has been installed via
    /// setHighContrastOverrideForTesting(), that value is returned instead
    /// (the real OS/palette query is bypassed).
    static bool isHighContrastMode();

    /// Pick the color a custom-painted surface should use for ``role``.
    ///
    /// In normal (non-high-contrast) mode this returns ``normal`` unchanged —
    /// the surface keeps its hardcoded default appearance. In High-Contrast
    /// mode it returns the corresponding color from the active system
    /// QPalette for ``role`` instead, so PdfCanvas / overlay painting adapts
    /// to the Windows High-Contrast theme (R3-17) rather than drawing fixed
    /// colors that may be invisible against an HC background.
    ///
    /// The alpha channel of ``normal`` is preserved on the adapted color, so
    /// semi-transparent overlays (search highlight, selection) stay
    /// translucent while their hue tracks the HC palette.
    static QColor adaptColor(const QColor& normal, QPalette::ColorRole role);

    /// TEST-ONLY seam: force isHighContrastMode() to return ``on`` regardless
    /// of the real OS/palette state, so headless tests can simulate the
    /// Windows High-Contrast theme without changing system settings. Call
    /// clearHighContrastOverrideForTesting() at teardown to restore the real
    /// query. Not for production use.
    static void setHighContrastOverrideForTesting(bool on);

    /// TEST-ONLY seam: remove any override installed by
    /// setHighContrastOverrideForTesting(), restoring the real OS query.
    static void clearHighContrastOverrideForTesting();

    /// The palette heuristic isHighContrastMode() falls back on when no OS
    /// signal is available: is this Window/WindowText pair near-maximal
    /// contrast, i.e. an actual High-Contrast theme rather than a merely
    /// legible one?
    ///
    /// GitHub #225. Exposed because on Windows `SystemParametersInfoW` always
    /// SUCCEEDS, so isHighContrastMode() returns on the line above and this
    /// branch is unreachable on the platform PDFE ships on — leaving the
    /// threshold constant with no regression guard at all. Calling it directly
    /// exercises the branch with no OS query involved, so the result cannot
    /// depend on the High-Contrast setting of whatever machine runs the suite.
    ///
    /// Pure: takes its two colours as arguments and reads no global state.
    static bool isHighContrastPalette(const QColor& window,
                                      const QColor& windowText);

    /// TEST-ONLY seam: make isHighContrastMode() behave as though the OS
    /// high-contrast query were UNAVAILABLE, so it falls through to
    /// isHighContrastPalette() on the current QApplication palette.
    ///
    /// Distinct from setHighContrastOverrideForTesting(), which short-circuits
    /// the whole function and therefore can never reach the fallback. Pair with
    /// clearOsQueryUnavailableForTesting() at teardown.
    static void setOsQueryUnavailableForTesting(bool unavailable);

    /// Undo setOsQueryUnavailableForTesting(), restoring the real OS query.
    static void clearOsQueryUnavailableForTesting();

    /// Calculate WCAG 2.1 contrast ratio between two colors.
    /// Returns value between 1.0 (identical) and 21.0 (black/white).
    /// Uses relative luminance formula: L = 0.2126*R + 0.7152*G + 0.0722*B
    /// where R/G/B are linearized (gamma-corrected) values.
    static double contrastRatio(const QColor& foreground, const QColor& background);

    /// Get the system's focus indicator color (usually highlight color).
    /// Falls back to bright blue (#0078D7) if no system color available.
    static QColor focusIndicatorColor();

    /// Get relative luminance of a color per WCAG 2.1.
    /// Returns value between 0.0 (darkest) and 1.0 (lightest).
    static double relativeLuminance(const QColor& color);

    /// Check if two colors meet WCAG AA contrast requirement (4.5:1 for normal text).
    static bool meetsWcagAA(const QColor& foreground, const QColor& background);

    /// Check if two colors meet WCAG AAA contrast requirement (7:1 for normal text).
    static bool meetsWcagAAA(const QColor& foreground, const QColor& background);

private:
    AccessibilityHelper() = delete; // Static-only class
};

} // namespace pdfe::ui
