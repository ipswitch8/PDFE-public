#include "AccessibilityHelper.h"
#include <QApplication>
#include <QPalette>
#include <algorithm>
#include <cmath>
#include <optional>

#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#endif

namespace pdfe::ui {

namespace {
    // TEST-ONLY high-contrast override. std::nullopt = use the real OS/palette
    // query; set = force that value. Lives in an anonymous namespace so it is
    // internal to this translation unit.
    std::optional<bool> g_highContrastOverride;

    // GitHub #217. Window/WindowText contrast at or above which a palette is
    // taken to BE a High-Contrast theme when no OS signal is available. The
    // WCAG formula's ceiling is 21.0 (pure black against pure white), which is
    // what the Windows High-Contrast themes actually use; an ordinary readable
    // palette lands well below this. See isHighContrastMode() for the measured
    // values that made the previous 7.0 always-true.
    constexpr double kHighContrastFallbackRatio = 20.0;

    // GitHub #225 TEST seam. On Windows the OS query always succeeds, so the
    // palette fallback below is unreachable in production and its threshold
    // had no guard. Setting this makes isHighContrastMode() behave as though
    // the query were unavailable, so the fallback runs and can be asserted on.
    bool g_osQueryUnavailable = false;
}

void AccessibilityHelper::setOsQueryUnavailableForTesting(bool unavailable) {
    g_osQueryUnavailable = unavailable;
}

void AccessibilityHelper::clearOsQueryUnavailableForTesting() {
    g_osQueryUnavailable = false;
}

bool AccessibilityHelper::isHighContrastPalette(const QColor& window,
                                                const QColor& windowText) {
    return contrastRatio(window, windowText) >= kHighContrastFallbackRatio;
}

void AccessibilityHelper::setHighContrastOverrideForTesting(bool on) {
    g_highContrastOverride = on;
}

void AccessibilityHelper::clearHighContrastOverrideForTesting() {
    g_highContrastOverride.reset();
}

double AccessibilityHelper::relativeLuminance(const QColor& color) {
    // Get RGB values in 0.0-1.0 range
    const double r = color.redF();
    const double g = color.greenF();
    const double b = color.blueF();

    // Linearize each channel (gamma correction)
    auto linearize = [](double value) -> double {
        if (value <= 0.04045) {
            return value / 12.92;
        } else {
            return std::pow((value + 0.055) / 1.055, 2.4);
        }
    };

    const double rLinear = linearize(r);
    const double gLinear = linearize(g);
    const double bLinear = linearize(b);

    // Calculate relative luminance using WCAG 2.1 formula
    return 0.2126 * rLinear + 0.7152 * gLinear + 0.0722 * bLinear;
}

double AccessibilityHelper::contrastRatio(const QColor& foreground, const QColor& background) {
    const double l1 = relativeLuminance(foreground);
    const double l2 = relativeLuminance(background);

    // Ensure L1 is the lighter color
    const double lighter = std::max(l1, l2);
    const double darker = std::min(l1, l2);

    // WCAG 2.1 contrast ratio formula
    return (lighter + 0.05) / (darker + 0.05);
}

bool AccessibilityHelper::isHighContrastMode() {
    // TEST seam: an installed override short-circuits the real query so
    // headless tests can simulate the Windows High-Contrast theme.
    if (g_highContrastOverride.has_value()) {
        return *g_highContrastOverride;
    }
    // GitHub #225: pretend the OS query is unavailable so the palette
    // fallback below is reachable on a platform where it otherwise is not.
    if (!g_osQueryUnavailable) {
#ifdef Q_OS_WIN
    // Windows-specific detection using SystemParametersInfoW.
    //
    // GitHub #217: when this call SUCCEEDS its answer is AUTHORITATIVE, in
    // both directions. It used to return true for HCF_HIGHCONTRASTON and
    // otherwise FALL THROUGH to the palette heuristic below — which then said
    // "yes, high contrast" for every legible palette PDFE has ever shipped
    // (see the heuristic's own comment). The effect was that
    // isHighContrastMode() was permanently TRUE on Windows, so adaptColor()'s
    // "normal mode: keep the surface's colour" branch was unreachable in the
    // product and every custom-painted surface silently had the
    // High-Contrast accommodation applied to it, always.
    HIGHCONTRAST hc = {};
    hc.cbSize = sizeof(HIGHCONTRAST);

    if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(HIGHCONTRAST), &hc, 0)) {
        return (hc.dwFlags & HCF_HIGHCONTRASTON) != 0;
    }
#endif
    }

    // Fallback for platforms with no OS signal (and for a failed query above).
    //
    // GitHub #217: the threshold here was 7.0 — WCAG AAA — which is a bar an
    // ORDINARY, well-designed palette clears comfortably and therefore cannot
    // distinguish a High-Contrast theme from a merely readable one. Measured
    // on PDFE's own palettes:
    //
    //     Fusion light   black on #efefef    ratio 18.3   -> "high contrast"
    //     PDFE dark      white on #353535    ratio 12.3   -> "high contrast"
    //
    // Both were classified as High-Contrast, which is what made the check
    // always-true. A real HC theme is a NEAR-MAXIMAL pair — the Windows HC
    // themes are pure white on pure black or the reverse, ratio 21.0, the
    // formula's ceiling. Requiring 20.0 admits those and excludes an ordinary
    // theme, so the accommodation is applied to the users who asked for it
    // rather than to everyone.
    if (QApplication::instance()) {
        const QPalette palette = QApplication::palette();
        const QColor windowColor = palette.color(QPalette::Window);
        const QColor textColor = palette.color(QPalette::WindowText);

        // Shared with the directly-testable entry point so the branch a test
        // exercises and the branch production takes cannot drift apart.
        return isHighContrastPalette(windowColor, textColor);
    }

    return false;
}

QColor AccessibilityHelper::focusIndicatorColor() {
    // Try to get system highlight color
    if (QApplication::instance()) {
        const QPalette palette = QApplication::palette();
        const QColor highlightColor = palette.color(QPalette::Highlight);

        // Check if the color is bright enough
        if (relativeLuminance(highlightColor) >= 0.1) {
            return highlightColor;
        }
    }

    // Fallback to Windows-style bright blue
    return QColor("#0078D7");
}

QColor AccessibilityHelper::adaptColor(const QColor& normal, QPalette::ColorRole role) {
    // Normal mode: keep the surface's hardcoded default color.
    if (!isHighContrastMode()) {
        return normal;
    }

    // High-Contrast mode: pull the color for this role from the active
    // system palette so custom-painted surfaces track the HC theme.
    if (QApplication::instance()) {
        QColor adapted = QApplication::palette().color(role);
        if (adapted.isValid()) {
            // Preserve the caller's alpha so translucent overlays stay
            // translucent while their hue follows the HC palette.
            adapted.setAlpha(normal.alpha());
            return adapted;
        }
    }

    // No application/palette available (e.g. very early startup): fall back
    // to the caller's color rather than an invalid one.
    return normal;
}

bool AccessibilityHelper::meetsWcagAA(const QColor& foreground, const QColor& background) {
    return contrastRatio(foreground, background) >= 4.5;
}

bool AccessibilityHelper::meetsWcagAAA(const QColor& foreground, const QColor& background) {
    return contrastRatio(foreground, background) >= 7.0;
}

} // namespace pdfe::ui
