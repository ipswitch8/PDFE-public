// -*- coding: utf-8 -*-
//
// GitHub #216 and #217 — two user-visible native-UI defects, asserted at the
// level where each one actually lives.
//
// #216 — View > Search Panel did not raise a TABIFIED dock
// --------------------------------------------------------
// MainWindow::onToggleSearchPanel was `dock->setVisible(!dock->isVisible())`.
// A dock tabbed BEHIND a sibling is not hidden — it is a mapped child that is
// simply not the current tab — so isVisible() is true and the toggle HID the
// panel the user had just asked to see. MainWindow::onFind (Ctrl+F) had the
// correct show()+raise() shape all along, which is why the shortcut worked and
// the menu item read as inert.
//
// The oracle below is the one the product's own testDockStateQueryPipe seam
// uses for "raised": `!isHidden() && !visibleRegion().isEmpty()`. A dock that
// is behind a tab has an EMPTY visible region, so this distinguishes the two
// states that isVisible() cannot. That requires a real mapped window, so these
// cases show() the MainWindow and wait for exposure.
//
// #217 — the dark theme did not repaint the PdfCanvas viewport
// -------------------------------------------------------------
// PdfCanvas::paintEvent fills its rect with palette().color(QPalette::Dark),
// and its backgroundRole is QPalette::Dark. ThemeManager::paletteFor(Dark) set
// Window/Base/Text/Button/... and never a single one of the five SHADE roles,
// so QPalette::Dark kept its light-mode value and the canvas backdrop was
// pixel-identical across a theme switch.
//
// This is asserted on RASTERISED PIXELS, not on the palette object. A palette
// assertion alone would still pass if PdfCanvas stopped consuming the role;
// test_theme_dark_pixels.cpp makes the same argument for the dialogs, and the
// canvas is the surface it never covered.

#include <gtest/gtest.h>

#include "theme/ThemeManager.h"
#include "ui/AccessibilityHelper.h"
#include "ui/MainWindow.h"
#include "ui/PdfCanvas.h"

#include <QAction>
#include <QApplication>
#include <QColor>
#include <QDockWidget>
#include <QImage>
#include <QPalette>
#include <QTest>

#include <cmath>
#include <map>
#include <string>

using pdfe::theme::Theme;
using pdfe::theme::ThemeManager;
using pdfe::ui::AccessibilityHelper;
using pdfe::ui::MainWindow;
using pdfe::ui::PdfCanvas;

namespace {

// ── #216 helpers ──────────────────────────────────────────────────────────

// The product's own definition of "raised", copied from the
// testDockStateQueryPipe seam in MainWindow.cpp so the test and the seam
// cannot drift apart silently.
bool dockIsRaised(const QDockWidget* dock)
{
    return dock && !dock->isHidden() && !dock->visibleRegion().isEmpty();
}

QAction* action(MainWindow& mw, const char* name)
{
    return mw.findChild<QAction*>(QString::fromLatin1(name));
}

QDockWidget* dock(MainWindow& mw, const char* name)
{
    return mw.findChild<QDockWidget*>(QString::fromLatin1(name));
}

void settle(int ms = 120)
{
    QApplication::processEvents();
    QTest::qWait(ms);
    QApplication::processEvents();
}

// ── #217 helpers ──────────────────────────────────────────────────────────

double relativeLuminance(const QColor& c)
{
    auto lin = [](double v) {
        v /= 255.0;
        return (v <= 0.03928) ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * lin(c.red()) + 0.7152 * lin(c.green()) + 0.0722 * lin(c.blue());
}

struct CanvasStats {
    double meanLuminance = 0.0;
    QColor dominant;
    int sampled = 0;
};

CanvasStats analyse(const QImage& imgIn)
{
    CanvasStats s;
    if (imgIn.isNull()) return s;
    const QImage img = imgIn.convertToFormat(QImage::Format_RGB32);
    std::map<QRgb, int> histogram;
    double sum = 0.0;
    int n = 0;
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) {
            const QColor c = img.pixelColor(x, y);
            sum += relativeLuminance(c);
            ++histogram[c.rgb()];
            ++n;
        }
    }
    if (n == 0) return s;
    s.sampled = n;
    s.meanLuminance = sum / n;
    QRgb best = 0;
    int bestCount = -1;
    for (const auto& kv : histogram) {
        if (kv.second > bestCount) { bestCount = kv.second; best = kv.first; }
    }
    s.dominant = QColor(best);
    return s;
}

double colourDistance(const QColor& a, const QColor& b)
{
    const double dr = a.red()   - b.red();
    const double dg = a.green() - b.green();
    const double db = a.blue()  - b.blue();
    return std::sqrt(dr * dr + dg * dg + db * db);
}

// Rasterise an EMPTY PdfCanvas (no document — the viewport backdrop is exactly
// what #217 is about) under `theme`.
QImage renderCanvasUnderTheme(Theme theme)
{
    QApplication::setPalette(ThemeManager::paletteFor(theme));
    PdfCanvas canvas;
    // setPalette as well as the application palette: grab() on a never-shown
    // widget must not depend on when Qt gets round to propagating the
    // application-wide change.
    canvas.setPalette(ThemeManager::paletteFor(theme));
    canvas.resize(320, 240);
    canvas.ensurePolished();
    return canvas.grab().toImage();
}

class PaletteRestoringTest : public ::testing::Test {
protected:
    void SetUp() override { saved_ = QApplication::palette(); }
    void TearDown() override {
        // Both are process-global; leaking either would change what a
        // later-running case measures.
        AccessibilityHelper::clearHighContrastOverrideForTesting();
        QApplication::setPalette(saved_);
    }
    QPalette saved_;
};

using CanvasThemeTest = PaletteRestoringTest;

// ═══════════════════════════════════════════════════════════════════════════
// #216 — the View-menu toggle raises a tabified dock
// ═══════════════════════════════════════════════════════════════════════════

// Shared setup: show the window, tabify `behind` under `front`, and leave
// `behind` NOT the current tab. Returns false when the rig cannot map a window
// at all (no interactive desktop), which callers report as a hard failure with
// a diagnosis rather than a silent skip.
bool arrangeTabbedBehind(MainWindow& mw, QDockWidget* front, QDockWidget* behind)
{
    mw.resize(1200, 820);
    mw.show();
    if (!QTest::qWaitForWindowExposed(&mw, 5000)) {
        return false;
    }
    front->setVisible(true);
    behind->setVisible(true);
    settle();
    mw.tabifyDockWidget(front, behind);
    settle();
    front->raise();          // push `behind` to the back of the tab group
    settle();
    return true;
}

TEST_F(PaletteRestoringTest, ToggleSearchPanelRaisesADockTabbedBehindProperties)
{
    ASSERT_NE(qApp, nullptr);

    MainWindow mw;
    auto* searchDock = dock(mw, "searchPanelDock");
    auto* propsDock  = dock(mw, "propertiesPanelDock");
    ASSERT_NE(searchDock, nullptr);
    ASSERT_NE(propsDock, nullptr);

    ASSERT_TRUE(arrangeTabbedBehind(mw, propsDock, searchDock))
        << "the MainWindow never became exposed, so 'tabbed behind' cannot be "
           "established. This is a desktop-session problem on the runner, not "
           "a product result — do not weaken the case to get past it.";

    // PRECONDITION, asserted rather than assumed: Search is shown but NOT the
    // current tab. Without this the case could pass against a dock that was
    // merely hidden, which is the state the OLD code already handled.
    ASSERT_FALSE(searchDock->isHidden())
        << "Search dock is hidden; this case must exercise the SHOWN-but-"
           "behind-a-tab state, which is the one #216 is about";
    ASSERT_TRUE(searchDock->visibleRegion().isEmpty())
        << "Search dock is already the current tab — the tabify arrangement "
           "did not take, so the toggle would be measured in the wrong state";
    ASSERT_TRUE(dockIsRaised(propsDock))
        << "Properties should be the raised tab after the arrangement";

    auto* toggle = action(mw, "toggleSearchPanelAction");
    ASSERT_NE(toggle, nullptr);
    toggle->trigger();
    settle(250);

    EXPECT_FALSE(searchDock->isHidden())
        << "View > Search Panel HID the panel the user asked to see — the "
           "isVisible() toggle treating tabbed-behind as 'showing' (#216)";
    EXPECT_TRUE(dockIsRaised(searchDock))
        << "View > Search Panel did not raise the Search dock to the front of "
           "its tab group; it is still behind Properties (#216)";
    EXPECT_TRUE(toggle->isChecked())
        << "the menu item's tick must agree with the dock it just raised";
}

TEST_F(PaletteRestoringTest, ToggleSearchPanelStillHidesAnAlreadyRaisedDock)
{
    ASSERT_NE(qApp, nullptr);

    MainWindow mw;
    auto* searchDock = dock(mw, "searchPanelDock");
    auto* propsDock  = dock(mw, "propertiesPanelDock");
    ASSERT_NE(searchDock, nullptr);
    ASSERT_NE(propsDock, nullptr);

    ASSERT_TRUE(arrangeTabbedBehind(mw, propsDock, searchDock));
    searchDock->raise();     // now SEARCH is the current tab
    settle();
    ASSERT_TRUE(dockIsRaised(searchDock));

    auto* toggle = action(mw, "toggleSearchPanelAction");
    ASSERT_NE(toggle, nullptr);
    toggle->trigger();
    settle(250);

    // The raise fix must not cost the toggle its ability to close the panel.
    EXPECT_TRUE(searchDock->isHidden())
        << "toggling an already-raised Search panel must CLOSE it; the fix "
           "for #216 must not turn the toggle into a show-only action";
    EXPECT_FALSE(toggle->isChecked());
}

TEST_F(PaletteRestoringTest, SiblingPanelTogglesRaiseATabbedDockToo)
{
    ASSERT_NE(qApp, nullptr);

    // #216 names Search because the shipped layout tabifies it. The other
    // three View-menu panel toggles had the identical one-line body, so they
    // are one user dock-drag from the identical symptom. This case pins all of
    // them so a future edit cannot quietly restore the bug on three of four.
    struct Row { const char* dockName; const char* actionName; };
    const Row rows[] = {
        {"bookmarkPanelDock",   "toggleBookmarkPanelAction"},
        {"pagePanelDock",       "togglePagePanelAction"},
        {"propertiesPanelDock", "togglePropertiesPanelAction"},
    };

    for (const auto& row : rows) {
        SCOPED_TRACE(row.actionName);

        MainWindow mw;
        auto* target = dock(mw, row.dockName);
        auto* front  = dock(mw, "searchPanelDock");
        ASSERT_NE(target, nullptr);
        ASSERT_NE(front, nullptr);
        ASSERT_NE(target, front);

        ASSERT_TRUE(arrangeTabbedBehind(mw, front, target));
        ASSERT_FALSE(target->isHidden());
        ASSERT_TRUE(target->visibleRegion().isEmpty())
            << row.dockName << " did not end up behind the Search tab";

        auto* toggle = action(mw, row.actionName);
        ASSERT_NE(toggle, nullptr);
        toggle->trigger();
        settle(250);

        EXPECT_TRUE(dockIsRaised(target))
            << row.actionName << " did not raise " << row.dockName
            << " out from behind its sibling tab";
        EXPECT_TRUE(toggle->isChecked());
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// #217 — the dark theme repaints the PdfCanvas viewport
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(PaletteRestoringTest, ThemeToggleActionChecksItselfAgainstTheActualTheme)
{
    ASSERT_NE(qApp, nullptr);

    // #217's evidence trail. "Toggle Dark Theme" decides which way to go from
    // its own checked state, and nothing seeded that state from the theme the
    // window actually launched in — it always started unchecked. Launching
    // already in Dark therefore made the action a NO-OP that set Dark again,
    // and the probe that drove it recorded pixel-identical captures and
    // concluded the CANVAS was at fault. The action must agree with reality.
    MainWindow mw;
    auto* toggle = action(mw, "themeToggleAction");
    auto* darkAct = action(mw, "themeDarkAction");
    auto* lightAct = action(mw, "themeLightAction");
    ASSERT_NE(toggle, nullptr);
    ASSERT_NE(darkAct, nullptr);
    ASSERT_NE(lightAct, nullptr);

    darkAct->trigger();
    QApplication::processEvents();
    EXPECT_TRUE(toggle->isChecked())
        << "the theme is Dark but 'Toggle Dark Theme' is unchecked, so "
           "triggering it would set Dark again and change nothing (#217)";

    lightAct->trigger();
    QApplication::processEvents();
    EXPECT_FALSE(toggle->isChecked())
        << "the theme is Light but 'Toggle Dark Theme' is still checked";

    // And the round trip: from Dark, the toggle must actually reach Light.
    darkAct->trigger();
    QApplication::processEvents();
    toggle->trigger();
    QApplication::processEvents();
    EXPECT_EQ(QApplication::palette().color(QPalette::Window),
              ThemeManager::paletteFor(Theme::Light).color(QPalette::Window))
        << "toggling from Dark did not land on the Light palette";
}

TEST_F(CanvasThemeTest, DarkPaletteDefinesTheShadeRoleTheCanvasPaintsWith)
{
    ASSERT_NE(qApp, nullptr);

    const QPalette light = ThemeManager::paletteFor(Theme::Light);
    const QPalette dark  = ThemeManager::paletteFor(Theme::Dark);

    // PdfCanvas::paintEvent reads exactly this role. If the dark palette does
    // not move it, no amount of repainting can change the viewport.
    const QColor lightShade = light.color(QPalette::Dark);
    const QColor darkShade  = dark.color(QPalette::Dark);

    EXPECT_NE(lightShade, darkShade)
        << "ThemeManager::paletteFor(Dark) leaves QPalette::Dark at its "
           "light-mode value (" << lightShade.name().toStdString()
        << "); PdfCanvas paints its viewport backdrop with that role, so the "
           "canvas cannot follow the theme (#217)";
    EXPECT_LT(relativeLuminance(darkShade), relativeLuminance(lightShade))
        << "the dark theme's canvas shade is not darker than the light one";

    // The whole shade family, not just the one role the canvas happens to use
    // today — a partially-populated family is what produced #217.
    for (auto role : {QPalette::Light, QPalette::Midlight, QPalette::Mid,
                      QPalette::Dark, QPalette::Shadow}) {
        EXPECT_NE(light.color(role), dark.color(role))
            << "shade role " << static_cast<int>(role)
            << " is identical in the light and dark palettes";
    }
}

TEST_F(CanvasThemeTest, AnOrdinaryThemeIsNotClassifiedAsHighContrast)
{
    ASSERT_NE(qApp, nullptr);

    // The second half of #217, and the one that made the first half inert.
    //
    // PdfCanvas::paintEvent does not use QPalette::Dark directly — it passes
    // it through AccessibilityHelper::adaptColor, which in High-Contrast mode
    // DISCARDS the caller's colour and substitutes QPalette::Window. The
    // fallback detector classified any palette with a Window/WindowText
    // contrast over 7.0 (WCAG AAA) as High-Contrast, and both of PDFE's own
    // themes clear that easily — so the detector was permanently true, the
    // canvas never consulted the shade role the source says it paints with,
    // and adaptColor's documented "normal mode" branch was unreachable.
    //
    // Measured below rather than asserted from the source, so a future edit to
    // either the threshold or the palettes reddens this.
    AccessibilityHelper::clearHighContrastOverrideForTesting();

    for (const auto theme : {Theme::Light, Theme::Dark}) {
        SCOPED_TRACE(theme == Theme::Light ? "Light" : "Dark");
        const QPalette p = ThemeManager::paletteFor(theme);
        QApplication::setPalette(p);

        EXPECT_FALSE(AccessibilityHelper::isHighContrastMode())
            << "PDFE's own theme is being detected as a Windows High-Contrast "
               "theme (Window/WindowText contrast "
            << AccessibilityHelper::contrastRatio(p.color(QPalette::Window),
                                                  p.color(QPalette::WindowText))
            << "). adaptColor then overrides every custom-painted surface's "
               "colour with a palette role, for every user, always (#217).";

        // The consequence, stated directly: adaptColor must hand the canvas
        // back the shade it asked for.
        const QColor shade = p.color(QPalette::Dark);
        EXPECT_EQ(AccessibilityHelper::adaptColor(shade, QPalette::Window),
                  shade)
            << "adaptColor replaced the canvas shade with the Window colour "
               "outside High-Contrast mode";
    }

    // The accommodation must still work for the users it is for.
    AccessibilityHelper::setHighContrastOverrideForTesting(true);
    QApplication::setPalette(ThemeManager::paletteFor(Theme::Dark));
    const QColor windowRole =
        QApplication::palette().color(QPalette::Window);
    EXPECT_EQ(AccessibilityHelper::adaptColor(QColor(1, 2, 3), QPalette::Window),
              windowRole)
        << "the High-Contrast path stopped adapting — narrowing the detector "
           "must not disable the feature";
    AccessibilityHelper::clearHighContrastOverrideForTesting();
}

TEST_F(CanvasThemeTest, PdfCanvasViewportPixelsChangeWhenTheThemeGoesDark)
{
    ASSERT_NE(qApp, nullptr);

    const QImage lightImg = renderCanvasUnderTheme(Theme::Light);
    const QImage darkImg  = renderCanvasUnderTheme(Theme::Dark);
    ASSERT_FALSE(lightImg.isNull());
    ASSERT_FALSE(darkImg.isNull());
    ASSERT_EQ(lightImg.size(), darkImg.size());

    const CanvasStats ls = analyse(lightImg);
    const CanvasStats ds = analyse(darkImg);
    ASSERT_GT(ls.sampled, 10000);
    ASSERT_GT(ds.sampled, 10000);

    // The headline assertion: the PIXELS differ. This is what a user means by
    // "the canvas does not go dark", and it is what stayed false for the whole
    // life of the defect.
    EXPECT_NE(lightImg, darkImg)
        << "the PdfCanvas viewport rasterises IDENTICALLY under the light and "
           "dark palettes — switching theme does not repaint the canvas (#217)";

    EXPECT_LT(ds.meanLuminance, ls.meanLuminance)
        << "dark canvas (" << ds.meanLuminance << ") is not darker than light ("
        << ls.meanLuminance << ")";
    EXPECT_GT(ls.meanLuminance - ds.meanLuminance, 0.05)
        << "the light/dark canvas luminance gap (" << ls.meanLuminance << " -> "
        << ds.meanLuminance << ") is too small to be a real theme change";

    // And it lands on the theme's OWN shade colour, not on some other grey —
    // a mutant that darkened the canvas by an unrelated route would still red.
    const QColor expected =
        ThemeManager::paletteFor(Theme::Dark).color(QPalette::Dark);
    EXPECT_LT(colourDistance(ds.dominant, expected), 12.0)
        << "dark canvas backdrop is " << ds.dominant.name().toStdString()
        << ", expected the dark palette's QPalette::Dark shade "
        << expected.name().toStdString();
}

}  // namespace
