// test_ui_high_contrast.cpp
// R3-17: High-Contrast theme adaptation for custom-painted surfaces.
//
// Proves that AccessibilityHelper::isHighContrastMode() is actually wired so
// custom-painted surfaces (PdfCanvas viewport/page, search + text-edit
// selection overlays) can adapt to the Windows High-Contrast theme. The paint
// paths pick their colors through AccessibilityHelper::adaptColor(normal, role);
// this suite drives that seam directly, simulating HC via the test override so
// no OS setting has to change.
//
// RED (pre-fix): AccessibilityHelper had no adaptColor()/override seam and the
// paint code used hardcoded QColor literals, so a simulated HC theme produced
// the SAME color as normal mode (no adaptation). GREEN: adaptColor() returns
// the HC system-palette color for the role when HC is active.

#include <gtest/gtest.h>
#include "ui/AccessibilityHelper.h"

#include <QApplication>
#include <QColor>
#include <QPalette>

using namespace pdfe::ui;

namespace {

// A distinctive palette whose HC roles are nowhere near the hardcoded overlay
// defaults, so "adapted == this palette's role color" cannot pass by accident.
QPalette makeHighContrastPalette() {
    QPalette hc;
    hc.setColor(QPalette::Window,     QColor(0, 0, 0));        // pure black backdrop
    hc.setColor(QPalette::WindowText, QColor(255, 255, 255));  // pure white text
    hc.setColor(QPalette::Base,       QColor(0, 0, 0));        // black page substrate
    hc.setColor(QPalette::Highlight,  QColor(0, 255, 255));    // cyan selection
    return hc;
}

} // namespace

class HighContrastAdaptTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_NE(QApplication::instance(), nullptr)
            << "QApplication must exist (created in test_main.cpp)";
        savedPalette_ = QApplication::palette();
    }

    void TearDown() override {
        // Always clear the override + restore the palette so no state leaks to
        // sibling tests (which query the REAL isHighContrastMode()).
        AccessibilityHelper::clearHighContrastOverrideForTesting();
        QApplication::setPalette(savedPalette_);
    }

    QPalette savedPalette_;
};

// The test override seam flips isHighContrastMode() deterministically and
// clears back to the real query.
TEST_F(HighContrastAdaptTest, OverrideSeam_TogglesIsHighContrastMode) {
    AccessibilityHelper::setHighContrastOverrideForTesting(true);
    EXPECT_TRUE(AccessibilityHelper::isHighContrastMode());

    AccessibilityHelper::setHighContrastOverrideForTesting(false);
    EXPECT_FALSE(AccessibilityHelper::isHighContrastMode());

    // After clearing, the value falls back to the real OS/palette query and no
    // longer honours the override (we only assert it does not throw / is stable
    // across two calls).
    AccessibilityHelper::clearHighContrastOverrideForTesting();
    const bool a = AccessibilityHelper::isHighContrastMode();
    const bool b = AccessibilityHelper::isHighContrastMode();
    EXPECT_EQ(a, b);
}

// Normal mode: adaptColor returns the hardcoded default unchanged — surfaces
// keep their normal appearance.
TEST_F(HighContrastAdaptTest, AdaptColor_NoHighContrast_ReturnsNormal) {
    AccessibilityHelper::setHighContrastOverrideForTesting(false);
    QApplication::setPalette(makeHighContrastPalette()); // present but must be ignored

    const QColor searchYellow(255, 255, 0, 100);
    const QColor adapted = AccessibilityHelper::adaptColor(searchYellow, QPalette::Highlight);

    EXPECT_EQ(adapted, searchYellow)
        << "Outside HC mode the hardcoded default must be returned verbatim";
}

// High-Contrast mode: adaptColor pulls the color from the system palette for
// the requested role instead of the hardcoded default. This is the core
// adaptation assertion for the search-highlight overlay color.
TEST_F(HighContrastAdaptTest, AdaptColor_HighContrast_SelectionAdaptsToPalette) {
    QApplication::setPalette(makeHighContrastPalette());
    AccessibilityHelper::setHighContrastOverrideForTesting(true);

    const QColor searchYellow(255, 255, 0, 100);  // hardcoded non-HC default
    const QColor adapted = AccessibilityHelper::adaptColor(searchYellow, QPalette::Highlight);

    const QColor expectedHue = QApplication::palette().color(QPalette::Highlight); // cyan

    // It must ADAPT: differ from the hardcoded default...
    EXPECT_NE(adapted.rgb(), searchYellow.rgb())
        << "In HC mode the overlay color must not stay the hardcoded yellow";
    // ...and match the HC system-palette Highlight hue.
    EXPECT_EQ(adapted.red(),   expectedHue.red());
    EXPECT_EQ(adapted.green(), expectedHue.green());
    EXPECT_EQ(adapted.blue(),  expectedHue.blue());
}

// The translucency of overlay defaults must survive adaptation (search fill and
// selection fill are semi-transparent; only the hue should track the theme).
TEST_F(HighContrastAdaptTest, AdaptColor_HighContrast_PreservesAlpha) {
    QApplication::setPalette(makeHighContrastPalette());
    AccessibilityHelper::setHighContrastOverrideForTesting(true);

    const QColor selectionFill(0, 120, 215, 80); // text-edit selection default, alpha 80
    const QColor adapted = AccessibilityHelper::adaptColor(selectionFill, QPalette::Highlight);

    EXPECT_EQ(adapted.alpha(), 80)
        << "Alpha of the hardcoded overlay default must be preserved in HC mode";
    // Hue still adapts to the palette Highlight (cyan), not the default blue.
    EXPECT_NE(adapted.rgb(), selectionFill.rgb());
}

// The canvas backdrop (Window role) and page substrate (Base role) also adapt,
// proving the wiring is per-role rather than a single hardcoded swap.
TEST_F(HighContrastAdaptTest, AdaptColor_HighContrast_BackdropAndPageAdaptPerRole) {
    QApplication::setPalette(makeHighContrastPalette());
    AccessibilityHelper::setHighContrastOverrideForTesting(true);

    // Backdrop: normal mode uses a mid grey (QPalette::Dark). In HC it must
    // become the HC Window color (black here).
    const QColor backdrop = AccessibilityHelper::adaptColor(QColor(128, 128, 128), QPalette::Window);
    EXPECT_EQ(backdrop, QColor(0, 0, 0));

    // Page substrate: normal mode is white paper. In HC it becomes Base (black).
    const QColor page = AccessibilityHelper::adaptColor(QColor(Qt::white), QPalette::Base);
    EXPECT_EQ(page, QColor(0, 0, 0));
    EXPECT_NE(page, QColor(Qt::white))
        << "In HC mode the page substrate must not stay hardcoded white";
}
