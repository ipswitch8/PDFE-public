#include "ThemeManager.h"
#include "../app/AppSettings.h"

#include <QApplication>
#include <QSettings>
#include <QStyle>
#include <QStyleHints>

namespace pdfe::theme {

ThemeManager::ThemeManager(pdfe::app::AppSettings* settings, QObject* parent)
    : QObject(parent), settings_(settings)
{
    // WIN-35: follow a LIVE OS light<->dark switch. paletteFor() reads
    // QStyleHints::colorScheme() only at the moment it is called, so without
    // this subscription a user flipping Windows between light and dark
    // mid-session stayed on the palette derived at startup until restart.
    //
    // `this` as the context object means the connection is torn down with the
    // ThemeManager (no leak, no dangling call after destruction), and because
    // it is made once in the constructor there is exactly one handler per
    // instance.
    if (auto* hints = QGuiApplication::styleHints()) {
        connect(hints, &QStyleHints::colorSchemeChanged,
                this, &ThemeManager::onSystemColorSchemeChanged);
    }
}

void ThemeManager::onSystemColorSchemeChanged(Qt::ColorScheme scheme)
{
    // An EXPLICIT Light/Dark choice is a user preference, not a mirror of the
    // OS. Overriding it here would be a new bug, so only Theme::System follows.
    if (current_ != Theme::System) {
        return;
    }

    // Derive from the scheme the signal carried rather than re-reading
    // QStyleHints, so the applied palette matches the notification exactly.
    // Qt::ColorScheme::Unknown falls to Light, matching paletteFor().
    const Theme effective =
        (scheme == Qt::ColorScheme::Dark) ? Theme::Dark : Theme::Light;

    QApplication::setPalette(paletteFor(effective));

    // current_ stays Theme::System (the user's choice is unchanged) and
    // nothing is persisted — only the derived palette moved.
    emit systemPaletteRefreshed(effective);
}

QString ThemeManager::themeToString(Theme t)
{
    switch (t) {
        case Theme::Light:  return "light";
        case Theme::Dark:   return "dark";
        case Theme::System: return "system";
    }
    return "system";
}

Theme ThemeManager::themeFromString(const QString& s)
{
    if (s == "light") return Theme::Light;
    if (s == "dark")  return Theme::Dark;
    return Theme::System;
}

QPalette ThemeManager::paletteFor(Theme t)
{
    if (t == Theme::System) {
        // Follow OS: Qt 6 exposes the OS color scheme via QStyleHints.
        const auto scheme = QGuiApplication::styleHints()->colorScheme();
        t = (scheme == Qt::ColorScheme::Dark) ? Theme::Dark : Theme::Light;
    }

    if (t == Theme::Light) {
        QPalette p = QApplication::style()->standardPalette();
        return p;
    }

    // Dark palette — based on Qt Fusion dark defaults.
    QPalette p;
    p.setColor(QPalette::Window,          QColor(53, 53, 53));
    p.setColor(QPalette::WindowText,      Qt::white);
    p.setColor(QPalette::Base,            QColor(35, 35, 35));
    p.setColor(QPalette::AlternateBase,   QColor(53, 53, 53));
    p.setColor(QPalette::ToolTipBase,     Qt::white);
    p.setColor(QPalette::ToolTipText,     Qt::white);
    p.setColor(QPalette::Text,            Qt::white);
    p.setColor(QPalette::Button,          QColor(53, 53, 53));
    p.setColor(QPalette::ButtonText,      Qt::white);
    p.setColor(QPalette::BrightText,      Qt::red);
    p.setColor(QPalette::Link,            QColor(42, 130, 218));
    p.setColor(QPalette::Highlight,       QColor(42, 130, 218));
    p.setColor(QPalette::HighlightedText, Qt::black);
    p.setColor(QPalette::PlaceholderText, QColor(150, 150, 150));

    // GitHub #217 -- THE SHADE ROLES. Without these the dark theme does not
    // repaint the PdfCanvas viewport.
    //
    // PdfCanvas::paintEvent fills its whole rect with
    // palette().color(QPalette::Dark) (and the widget's backgroundRole is
    // QPalette::Dark too, PdfCanvas.cpp:51). QPalette::Dark is one of the five
    // SHADE roles -- Light / Midlight / Mid / Dark / Shadow -- and none of them
    // was set above.
    //
    // Two separate mechanisms then conspire to leave the shade light:
    //
    //   * setColor() sets exactly ONE role. Qt derives the shades from the
    //     Button/Window brush only when the palette is CONSTRUCTED from a
    //     brush; assigning Window and Button afterwards does not recompute
    //     them.
    //   * QApplication::setPalette() merges this palette with the style's
    //     standard palette using the resolve mask, so a role this function
    //     never touched is filled in from the LIGHT standard palette.
    //
    // Either way QPalette::Dark stayed at its light-mode value (#a0a0a0 on
    // Fusion -- measured), so the dark theme's canvas shade was a LIGHT grey.
    //
    // This alone was not visible, which is why it survived: a second defect
    // was masking it. AccessibilityHelper::isHighContrastMode() classified
    // every ordinary palette as a Windows High-Contrast theme, so adaptColor
    // threw this role away and substituted QPalette::Window -- the canvas
    // tracked the theme by accident, through a broken accessibility detector,
    // never through the role its own paintEvent names. Fixing that detector
    // (AccessibilityHelper.cpp, same issue) is what makes this assignment
    // load-bearing; neither change is sufficient on its own.
    //
    // The values below keep the same ordering the shade roles have in a light
    // palette (Light brightest ... Shadow darkest) around the 53,53,53 Window
    // base, so any widget that uses them for bevels still gets a sane 3D look.
    p.setColor(QPalette::Light,    QColor(75, 75, 75));
    p.setColor(QPalette::Midlight, QColor(64, 64, 64));
    p.setColor(QPalette::Mid,      QColor(45, 45, 45));
    p.setColor(QPalette::Dark,     QColor(30, 30, 30));
    p.setColor(QPalette::Shadow,   QColor(20, 20, 20));
    return p;
}

// Meta-audit follow-up F33 — settings hive identity.
//
// These two helpers used to be a literal `QSettings qs("PDFE", "PDFE")` at
// each call site. An explicit org/app pair pins the hive IN THE SOURCE: it
// resolves to HKCU\Software\PDFE\PDFE no matter what
// QCoreApplication::organizationName()/applicationName() say, so neither of
// the project's two settings seams can reach it —
//   * pdfe_test::SettingsScope moves the process-global org/app identity;
//   * PDFE_SETTINGS_PATH moves pdfe::app::AppSettings onto an INI file.
// A literal pair ignores both, which is why F28 could close the test side of
// the registry-pollution class and not this.
//
// The fix is the seam that already existed, not a new one. ThemeManager is
// constructed WITH an AppSettings* (MainWindow.cpp:398) and its own header
// already documented that it "persists the choice via AppSettings" — the
// literal QSettings was the code disagreeing with the header. Routing through
// settings_ puts theme persistence behind both seams at once.
//
// The key ("Preferences/theme") and the stored strings are unchanged, and
// src/main.cpp:50,52 set applicationName/organizationName to "PDFE"/"PDFE" —
// the same pair the literal named. A default QSettings() in the shipped
// application therefore resolves to the SAME hive it always did, so no
// installed user's persisted theme is orphaned by this change.
//
// settings_ may legitimately be null (the constructor accepts it and
// setTheme() already tested for it). In that case fall back to a default
// QSettings(), which is still identity-derived — the fallback must not
// reintroduce a literal.
QString ThemeManager::persistedThemeString() const
{
    if (settings_) {
        return settings_->theme();
    }
    QSettings qs;  // application-level org/app identity
    return qs.value(QStringLiteral("Preferences/theme"),
                    QStringLiteral("system")).toString();
}

void ThemeManager::persistThemeString(const QString& value)
{
    if (settings_) {
        settings_->setTheme(value);
        settings_->sync();
        return;
    }
    QSettings qs;  // application-level org/app identity
    qs.setValue(QStringLiteral("Preferences/theme"), value);
    qs.sync();
}

void ThemeManager::setTheme(Theme theme)
{
    current_ = theme;
    QApplication::setPalette(paletteFor(theme));
    if (settings_) {
        persistThemeString(themeToString(theme));
    }
    emit themeChanged(theme);
}

void ThemeManager::applyPersistedTheme()
{
    // PIN THE PALETTE UNDER PDFE_TEST_DETERMINISTIC_LAYOUT.
    //
    // The persisted theme defaults to Theme::System, which derives the palette
    // from the OS colour scheme. That makes every theme-derived surface a
    // property of the SESSION rather than of the product, and a pixel oracle
    // over such a surface asserts the session. Measured in CI run 38163981959:
    // `phase5_print_preview_dialog_open` differed on 47% of pixels at a mean
    // luma delta of 21 with flat areas and ink BOTH identical -- i.e. only the
    // mid-tone dialog chrome moved, because the palette had.
    //
    // This is the same argument, and the same remedy, as the dock layout. That
    // is also per-user-profile persisted state, it also made captures differ
    // between machines and profiles, and PDFE_TEST_DETERMINISTIC_LAYOUT already
    // pins it (GitHub #136). The theme belongs under the same flag.
    //
    // Light rather than Dark because paletteFor() already falls back to Light
    // for Qt::ColorScheme::Unknown, so Light is this application's neutral.
    //
    // current_ is set to Theme::Light, NOT Theme::System, deliberately: it makes
    // onSystemColorSchemeChanged() return early, so a live OS light<->dark flip
    // (WIN-35) cannot move the palette halfway through a visual test.
    //
    // NOT persisted. setTheme() writes the value to the settings hive, and a
    // test run must not rewrite the preferences of the account it runs as.
    if (qEnvironmentVariableIsSet("PDFE_TEST_DETERMINISTIC_LAYOUT")) {
        current_ = Theme::Light;
        QApplication::setPalette(paletteFor(Theme::Light));
        emit themeChanged(Theme::Light);
        return;
    }
    setTheme(themeFromString(persistedThemeString()));
}

} // namespace pdfe::theme
