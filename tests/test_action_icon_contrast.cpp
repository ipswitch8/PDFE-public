// -*- coding: utf-8 -*-
//
// Toolbar action icons must stay legible under every theme.
//
// WHAT WAS WRONG BEFORE
// ---------------------
// The 26 action SVGs (src/ui/icons/action_*.svg) hard-coded stroke="#303030".
// The dark theme's toolbar is #353535, so every toolbar glyph was dark grey on
// dark grey -- a WCAG contrast ratio of about 1.1:1. The user report: "dark
// mode support is working but icon colors are mostly black or dark grey and
// not easy to see". The existing icon tests (test_ui_structure_bugs.cpp) only
// proved an icon rasterises to SOME pixels and that icons differ from one
// another; neither looks at colour, so both stayed green.
//
// WHAT THIS FILE ASSERTS
// ----------------------
//   * every SVG source draws only in currentColor (no fixed ink colour);
//   * every icon, under both ThemeManager palettes, renders opaque ink whose
//     contrast against the toolbar background (Window and Button roles) is
//     at least 3:1 -- WCAG 2.x SC 1.4.11, non-text contrast;
//   * Disabled mode renders different pixels than Normal mode;
//   * one QIcon instance, set once, changes its pixels when the application
//     palette switches -- the live Light/Dark/System switch path;
//   * the icons MainWindow actually puts on its toolbars meet the same bar
//     under the dark palette, so the wiring cannot regress to a plain QIcon;
//   * a HiDPI (devicePixelRatio 2) request yields a full-resolution pixmap
//     that is just as legible;
//   * the Customize Toolbars dialog's copies of the icons follow the theme.
//
// MUTATIONS KILLED (each applied, pdfe_tests rebuilt, ActionIconContrast.*
// run; 2026-11-22, Linux Release. The failing tests listed are the ones that
// actually failed.)
//   * STROKE="#303030" restored in make_action_icons.py, SVGs regenerated
//     (dark ink contrast measured 1.08:1) -> 7 fail: SvgSourcesDrawOnly-
//     InCurrentColor, EveryIconIsLegibleUnderLightAndDark, DisabledMode-
//     DiffersFromNormal, LiveThemeSwitchRepaintsTheSameIcon, MainWindow-
//     ToolbarIconsAreLegibleUnderDarkTheme, HiDpiPixmapIsFullResolution-
//     AndLegible, CustomizeToolbarsDialogIconsFollowTheTheme.
//   * actionIcon() returns a plain QIcon(path) -> QtSvg draws currentColor
//     black; 1 fails: MainWindowToolbarIconsAreLegibleUnderDarkTheme (the
//     only test that goes through MainWindow's wiring).
//   * the engine resolves its ink colour once (static) instead of per
//     request -> 6 fail: EveryIconIsLegible..., DisabledModeDiffers...,
//     LiveThemeSwitch..., MainWindowToolbarIcons..., HiDpiPixmap...,
//     CustomizeToolbarsDialogIcons....
//   * ThemeManager's dark Disabled-group colours removed -> 2 fail:
//     DisabledModeDiffersFromNormal, DarkPaletteDisabledTextIsDistinct.
//   * ink colour dropped from the engine's QPixmapCache key -> 6 fail, the
//     same six as the resolved-once mutation.
//
// DOCK TITLE-BAR BUTTONS (added 2026-11-26)
// -----------------------------------------
// With the toolbar fixed, the float/close buttons on every dock title bar
// were still dark grey on the dark theme: 2.28:1 against #353535, measured
// on v0.5.5 and reproduced by MainWindowDockTitleButtonsAreLegibleUnder-
// LightAndDark before the fix (all 13 docks, both buttons, Dark only). Those
// icons come from the STYLE (QStyle::standardIcon), and Fusion's are fixed
// pixmaps. ThemedIconStyle wraps the application style and serves them from
// :/icons/style/*.svg through PaletteIconEngine.
//
// MUTATIONS KILLED (each applied, pdfe_tests rebuilt, ActionIconContrast.*
// run; 2026-11-26, Linux Release):
//   * MainWindow no longer calls ThemedIconStyle::installOnApplication()
//     -> 3 fail: MainWindowDockTitleButtonsAreLegible..., DockTitleButtons-
//     FollowALiveThemeSwitch, ThemedIconStyleInstallsOnce....
//   * the SP_TitleBarCloseButton / SP_DockWidgetCloseButton mapping removed
//     -> 3 fail: MainWindowDockTitleButtonsAreLegible..., DockTitleButtons-
//     FollowALiveThemeSwitch, ThemedIconStyleReplacesOnlyTheDockIcons.
//   * style_dock_float.svg given a fixed stroke="#303030" -> 3 fail:
//     MainWindowDockTitleButtonsAreLegible..., StyleIconSvgsDrawOnlyIn-
//     CurrentColor, ThemedIconStyleReplacesOnlyTheDockIcons.
//   * installOnApplication() without its idempotence check -> 1 fails:
//     ThemedIconStyleInstallsOnceAroundTheExistingStyle.

#include <gtest/gtest.h>

#include "theme/ThemeManager.h"
#include "ui/CustomizeToolbarsDialog.h"
#include "ui/ToolbarLayoutManager.h"
#include "ui/MainWindow.h"
#include "ui/PaletteIconEngine.h"
#include "ui/ThemedIconStyle.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QIcon>
#include <QListView>
#include <QStandardItemModel>
#include <QImage>
#include <QPalette>
#include <QPixmap>
#include <QRegularExpression>
#include <QStyle>
#include <QToolBar>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

// RCC init for the pdfe_ui static library's action_icons.qrc (see the same
// declaration in MainWindow.cpp for why it must be pinned).
extern int qInitResources_action_icons();

namespace {

using pdfe::theme::Theme;
using pdfe::theme::ThemeManager;
using pdfe::ui::PaletteIconEngine;

// Every alias in src/ui/action_icons.qrc. ExpectedSetMatchesResources()
// keeps this list honest against the compiled resource.
const std::vector<std::string> kIcons = {
    "open", "save", "undo", "redo", "zoom_in", "zoom_out", "fit_width",
    "fit_page", "select", "sticky_note", "highlight", "underline",
    "strikeout", "form", "image", "edit_text", "redaction", "ink",
    "text_box", "rectangle", "circle", "link", "line", "stamp",
    "attach_file", "print_preview",
};

constexpr int kPx = 32;
constexpr double kMinContrast = 3.0;  // WCAG 2.x SC 1.4.11

QString pathFor(const std::string& name)
{
    return QStringLiteral(":/icons/actions/%1.svg").arg(QString::fromStdString(name));
}

double channel(int v)
{
    const double c = v / 255.0;
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

double luminance(QRgb c)
{
    return 0.2126 * channel(qRed(c)) + 0.7152 * channel(qGreen(c)) +
           0.0722 * channel(qBlue(c));
}

double contrast(QRgb a, QRgb b)
{
    const double la = luminance(a), lb = luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

QImage render(const QIcon& icon, QIcon::Mode mode = QIcon::Normal)
{
    const QPixmap pm = icon.pixmap(QSize(kPx, kPx), mode);
    if (pm.isNull()) return QImage();
    return pm.toImage().convertToFormat(QImage::Format_ARGB32);
}

// Opaque ink pixels only: anti-aliased edges are partly background and say
// nothing about the colour the glyph is drawn in.
std::vector<QRgb> inkPixels(const QImage& img)
{
    std::vector<QRgb> out;
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x) {
            const QRgb c = img.pixel(x, y);
            if (qAlpha(c) >= 250) out.push_back(c);
        }
    return out;
}

// Restores the application palette whatever a test does to it.
class ActionIconContrast : public ::testing::Test {
protected:
    void SetUp() override
    {
        qInitResources_action_icons();
        saved_ = QApplication::palette();
    }
    void TearDown() override { QApplication::setPalette(saved_); }

    static void usePalette(Theme t) { QApplication::setPalette(ThemeManager::paletteFor(t)); }

    // Asserts the worst opaque ink pixel of @p img clears kMinContrast against
    // both toolbar background roles of the current palette.
    static void expectLegible(const QImage& img, const std::string& who)
    {
        ASSERT_FALSE(img.isNull()) << who << ": icon produced no pixmap";
        const std::vector<QRgb> ink = inkPixels(img);
        ASSERT_GE(ink.size(), 8u) << who << ": icon has no opaque ink pixels";
        const QPalette pal = QApplication::palette();
        for (QPalette::ColorRole bgRole : {QPalette::Window, QPalette::Button}) {
            const QRgb bg = pal.color(QPalette::Active, bgRole).rgb();
            double worst = 1e9;
            for (QRgb c : ink) worst = std::min(worst, contrast(c, bg));
            EXPECT_GE(worst, kMinContrast)
                << who << ": ink contrast " << worst << ":1 against "
                << (bgRole == QPalette::Window ? "Window " : "Button ")
                << QColor(bg).name().toStdString() << " is below "
                << kMinContrast << ":1";
        }
    }

    QPalette saved_;
};

const char* themeName(Theme t) { return t == Theme::Dark ? "Dark" : "Light"; }

}  // namespace

TEST_F(ActionIconContrast, ExpectedSetMatchesResources)
{
    QStringList inResource = QDir(QStringLiteral(":/icons/actions")).entryList(QDir::Files);
    inResource.sort();
    QStringList expected;
    for (const auto& n : kIcons) expected << QString::fromStdString(n) + QStringLiteral(".svg");
    expected.sort();
    EXPECT_EQ(inResource, expected);
    EXPECT_EQ(kIcons.size(), 26u);
}

TEST_F(ActionIconContrast, SvgSourcesDrawOnlyInCurrentColor)
{
    // Any stroke/fill value other than currentColor or none is a fixed ink
    // colour that cannot follow the theme.
    const QRegularExpression paint(QStringLiteral("(?:stroke|fill)=\"([^\"]*)\""));
    for (const auto& name : kIcons) {
        QFile f(pathFor(name));
        ASSERT_TRUE(f.open(QIODevice::ReadOnly)) << name;
        const QString svg = QString::fromUtf8(f.readAll());
        int painted = 0;
        for (auto it = paint.globalMatch(svg); it.hasNext();) {
            const QString v = it.next().captured(1);
            EXPECT_TRUE(v == QLatin1String("currentColor") || v == QLatin1String("none"))
                << name << ": fixed paint colour '" << v.toStdString() << "'";
            if (v == QLatin1String("currentColor")) ++painted;
        }
        EXPECT_GE(painted, 1) << name << ": draws nothing in currentColor";
    }
}

TEST_F(ActionIconContrast, EveryIconIsLegibleUnderLightAndDark)
{
    for (Theme t : {Theme::Light, Theme::Dark}) {
        usePalette(t);
        for (const auto& name : kIcons) {
            const QIcon icon = PaletteIconEngine::icon(pathFor(name));
            ASSERT_FALSE(icon.isNull()) << name;
            expectLegible(render(icon), name + " / " + themeName(t));
        }
    }
}

TEST_F(ActionIconContrast, DisabledModeDiffersFromNormal)
{
    for (Theme t : {Theme::Light, Theme::Dark}) {
        usePalette(t);
        for (const auto& name : kIcons) {
            const QIcon icon = PaletteIconEngine::icon(pathFor(name));
            const QImage normal = render(icon, QIcon::Normal);
            const QImage disabled = render(icon, QIcon::Disabled);
            ASSERT_FALSE(normal.isNull()) << name;
            ASSERT_FALSE(disabled.isNull()) << name;
            EXPECT_NE(normal, disabled)
                << name << " / " << themeName(t)
                << ": a disabled icon looks exactly like an enabled one";
        }
    }
}

TEST_F(ActionIconContrast, LiveThemeSwitchRepaintsTheSameIcon)
{
    usePalette(Theme::Light);
    // One instance, created once, never re-set -- the toolbar's situation.
    const QIcon icon = PaletteIconEngine::icon(pathFor("save"));
    const QImage light = render(icon);
    expectLegible(light, "save / Light");

    usePalette(Theme::Dark);
    const QImage dark = render(icon);
    expectLegible(dark, "save / Dark (after live switch)");
    EXPECT_NE(light, dark) << "the icon kept its Light pixels after switching to Dark";

    // The dark ink is the light one inverted in brightness, not a near miss.
    const auto li = inkPixels(light), di = inkPixels(dark);
    ASSERT_FALSE(li.empty());
    ASSERT_FALSE(di.empty());
    EXPECT_LT(luminance(li.front()), 0.2);
    EXPECT_GT(luminance(di.front()), 0.6);

    usePalette(Theme::Light);
    EXPECT_EQ(render(icon), light) << "switching back did not restore the Light pixels";
}

TEST_F(ActionIconContrast, MainWindowToolbarIconsAreLegibleUnderDarkTheme)
{
    pdfe::ui::MainWindow mw;
    usePalette(Theme::Dark);
    auto* mainTb = mw.findChild<QToolBar*>(QStringLiteral("mainToolBar"));
    ASSERT_NE(mainTb, nullptr);
    auto* annotTb = mw.annotationToolBarForTesting();
    ASSERT_NE(annotTb, nullptr);

    int checked = 0;
    for (QToolBar* tb : {mainTb, annotTb}) {
        for (QAction* a : tb->actions()) {
            if (a->isSeparator() || a->icon().isNull()) continue;
            ++checked;
            expectLegible(render(a->icon()),
                          a->objectName().toStdString() + " ('" + a->text().toStdString() + "')");
        }
    }
    EXPECT_GE(checked, 10) << "expected the toolbars to carry at least ten icons";
}

TEST_F(ActionIconContrast, DarkPaletteDisabledTextIsDistinct)
{
    const QPalette dark = ThemeManager::paletteFor(Theme::Dark);
    const QRgb active = dark.color(QPalette::Active, QPalette::ButtonText).rgb();
    const QRgb disabled = dark.color(QPalette::Disabled, QPalette::ButtonText).rgb();
    EXPECT_NE(active, disabled)
        << "the dark palette's Disabled ButtonText equals the Active one, so a "
           "disabled toolbar icon is indistinguishable from an enabled one";
    // Still readable on the dark toolbar, just dimmer than enabled ink.
    const QRgb window = dark.color(QPalette::Active, QPalette::Window).rgb();
    EXPECT_GE(contrast(disabled, window), kMinContrast);
    EXPECT_LT(contrast(disabled, window), contrast(active, window));
}

TEST_F(ActionIconContrast, HiDpiPixmapIsFullResolutionAndLegible)
{
    for (Theme t : {Theme::Light, Theme::Dark}) {
        usePalette(t);
        const QIcon icon = PaletteIconEngine::icon(pathFor("open"));
        const QPixmap pm = icon.pixmap(QSize(kPx, kPx), 2.0);
        ASSERT_FALSE(pm.isNull()) << themeName(t);
        EXPECT_EQ(pm.size(), QSize(2 * kPx, 2 * kPx)) << themeName(t);
        EXPECT_DOUBLE_EQ(pm.devicePixelRatio(), 2.0) << themeName(t);
        expectLegible(pm.toImage().convertToFormat(QImage::Format_ARGB32),
                      std::string("open @2x / ") + themeName(t));
        // A 1x request of the same logical size must stay 1x after the 2x
        // one (the two share nothing that could leak the ratio).
        const QPixmap pm1 = icon.pixmap(QSize(kPx, kPx), 1.0);
        EXPECT_EQ(pm1.size(), QSize(kPx, kPx)) << themeName(t);
    }
}

TEST_F(ActionIconContrast, CustomizeToolbarsDialogIconsFollowTheTheme)
{
    usePalette(Theme::Light);
    QAction open(QStringLiteral("Open"));
    open.setObjectName(QStringLiteral("openAction"));
    open.setIcon(PaletteIconEngine::icon(pathFor("open")));

    pdfe::ui::ToolbarLayout layout;
    layout.toolbarName = QStringLiteral("Main Toolbar");
    pdfe::ui::CustomizeToolbarsDialog dlg(std::vector<QAction*>{&open},
                                          std::vector<pdfe::ui::ToolbarLayout>{layout},
                                          nullptr);
    auto* list = dlg.findChild<QListView*>(QStringLiteral("availableList"));
    ASSERT_NE(list, nullptr);
    ASSERT_NE(list->model(), nullptr);
    ASSERT_EQ(list->model()->rowCount(), 1);
    const QIcon copied =
        qvariant_cast<QIcon>(list->model()->index(0, 0).data(Qt::DecorationRole));
    ASSERT_FALSE(copied.isNull());

    const QImage light = render(copied);
    expectLegible(light, "Customize Toolbars 'Open' / Light");
    usePalette(Theme::Dark);
    const QImage dark = render(copied);
    expectLegible(dark, "Customize Toolbars 'Open' / Dark");
    EXPECT_NE(light, dark);
}

// ---------------------------------------------------------------------------
// Dock title-bar buttons (float / close). These icons come from the STYLE,
// not from PDFE's own SVGs: QDockWidget asks style()->standardIcon() for
// SP_TitleBarNormalButton and SP_TitleBarCloseButton. Fusion -- the style on
// Linux -- answers with fixed dark-grey PNGs that ignore the palette, so on
// the dark theme the Pages / Search / Comments dock buttons measured about
// 2.3:1 against the #353535 title bar (2026-11-26, v0.5.5 under Xvfb).
// ---------------------------------------------------------------------------

namespace {

// Every float/close button on every dock MainWindow creates.
std::vector<QAbstractButton*> dockTitleButtons(const pdfe::ui::MainWindow& mw)
{
    std::vector<QAbstractButton*> out;
    for (QDockWidget* dock : mw.findChildren<QDockWidget*>()) {
        for (const char* name : {"qt_dockwidget_closebutton", "qt_dockwidget_floatbutton"}) {
            if (auto* b = dock->findChild<QAbstractButton*>(QLatin1String(name),
                                                            Qt::FindDirectChildrenOnly))
                out.push_back(b);
        }
    }
    return out;
}

std::string describe(const QAbstractButton* b)
{
    const QWidget* dock = b->parentWidget();
    return (dock ? dock->objectName().toStdString() : std::string("?")) + "/" +
           b->objectName().toStdString();
}

}  // namespace

TEST_F(ActionIconContrast, MainWindowDockTitleButtonsAreLegibleUnderLightAndDark)
{
    pdfe::ui::MainWindow mw;
    const std::vector<QAbstractButton*> buttons = dockTitleButtons(mw);
    // Pages, Layers, Bookmarks, Search, Properties, ... each carry both.
    ASSERT_GE(buttons.size(), 6u) << "expected the main window's docks to have title buttons";
    for (Theme t : {Theme::Light, Theme::Dark}) {
        usePalette(t);
        for (QAbstractButton* b : buttons) {
            ASSERT_FALSE(b->icon().isNull()) << describe(b);
            expectLegible(render(b->icon()), describe(b) + " / " + themeName(t));
        }
    }
}

TEST_F(ActionIconContrast, DockTitleButtonsFollowALiveThemeSwitch)
{
    pdfe::ui::MainWindow mw;
    usePalette(Theme::Light);
    const std::vector<QAbstractButton*> buttons = dockTitleButtons(mw);
    ASSERT_FALSE(buttons.empty());
    QAbstractButton* close = buttons.front();
    // The button's icon is set once by QDockWidget and never re-set by a
    // palette change, so the same QIcon must repaint for the new palette.
    const QImage light = render(close->icon());
    usePalette(Theme::Dark);
    const QImage dark = render(close->icon());
    expectLegible(dark, describe(close) + " / Dark (after live switch)");
    EXPECT_NE(light, dark) << describe(close) << " kept its Light pixels after switching to Dark";
}

TEST_F(ActionIconContrast, StyleIconSvgsDrawOnlyInCurrentColor)
{
    const QRegularExpression paint(QStringLiteral("(?:stroke|fill)=\"([^\"]*)\""));
    const QStringList files = QDir(QStringLiteral(":/icons/style")).entryList(QDir::Files);
    EXPECT_EQ(files, (QStringList{QStringLiteral("dock_close.svg"), QStringLiteral("dock_float.svg")}));
    for (const QString& file : files) {
        QFile f(QStringLiteral(":/icons/style/") + file);
        ASSERT_TRUE(f.open(QIODevice::ReadOnly)) << file.toStdString();
        const QString svg = QString::fromUtf8(f.readAll());
        int painted = 0;
        for (auto it = paint.globalMatch(svg); it.hasNext();) {
            const QString v = it.next().captured(1);
            EXPECT_TRUE(v == QLatin1String("currentColor") || v == QLatin1String("none"))
                << file.toStdString() << ": fixed paint colour '" << v.toStdString() << "'";
            if (v == QLatin1String("currentColor")) ++painted;
        }
        EXPECT_GE(painted, 1) << file.toStdString() << ": draws nothing in currentColor";
    }
}

TEST_F(ActionIconContrast, ThemedIconStyleReplacesOnlyTheDockIcons)
{
    using pdfe::ui::ThemedIconStyle;
    for (QStyle::StandardPixmap sp : {QStyle::SP_TitleBarNormalButton,
                                      QStyle::SP_TitleBarCloseButton,
                                      QStyle::SP_DockWidgetCloseButton}) {
        const QString path = ThemedIconStyle::replacementPathFor(sp);
        ASSERT_FALSE(path.isEmpty()) << "standard pixmap " << int(sp) << " is not replaced";
        EXPECT_TRUE(QFile::exists(path)) << path.toStdString();
    }

    // Everything else goes to the base style untouched: same pixels as the
    // base style's own icon.
    ThemedIconStyle proxy;  // wraps the application's default style
    ASSERT_NE(proxy.baseStyle(), nullptr);
    for (QStyle::StandardPixmap sp : {QStyle::SP_MessageBoxWarning,
                                      QStyle::SP_DialogOkButton,
                                      QStyle::SP_ArrowUp,
                                      QStyle::SP_TitleBarMinButton}) {
        EXPECT_TRUE(ThemedIconStyle::replacementPathFor(sp).isEmpty()) << int(sp);
        EXPECT_EQ(render(proxy.standardIcon(sp)), render(proxy.baseStyle()->standardIcon(sp)))
            << "standard pixmap " << int(sp) << " was changed by the proxy";
    }

    for (Theme t : {Theme::Light, Theme::Dark}) {
        usePalette(t);
        for (QStyle::StandardPixmap sp : {QStyle::SP_TitleBarNormalButton,
                                          QStyle::SP_TitleBarCloseButton}) {
            expectLegible(render(proxy.standardIcon(sp)),
                          "standard pixmap " + std::to_string(int(sp)) + " / " + themeName(t));
        }
    }
}

TEST_F(ActionIconContrast, ThemedIconStyleInstallsOnceAroundTheExistingStyle)
{
    using pdfe::ui::ThemedIconStyle;
    pdfe::ui::MainWindow mw;  // installs it
    auto* installed = qobject_cast<ThemedIconStyle*>(QApplication::style());
    ASSERT_NE(installed, nullptr) << "MainWindow did not install ThemedIconStyle";
    ASSERT_NE(installed->baseStyle(), nullptr);
    EXPECT_EQ(qobject_cast<ThemedIconStyle*>(installed->baseStyle()), nullptr)
        << "the proxy wraps another proxy";
    EXPECT_FALSE(installed->baseStyle()->name().isEmpty());

    EXPECT_FALSE(ThemedIconStyle::installOnApplication()) << "a second install replaced the style";
    EXPECT_EQ(QApplication::style(), installed);
    pdfe::ui::MainWindow second;
    EXPECT_EQ(QApplication::style(), installed) << "a second MainWindow replaced the style";
}
