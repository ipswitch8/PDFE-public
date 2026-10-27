#pragma once

#include <QObject>
#include <QPalette>
#include <QString>

namespace pdfe::app {
class AppSettings;
}

namespace pdfe::theme {

enum class Theme {
    Light,
    Dark,
    System,
};

/// Applies Light / Dark / System palettes to the whole Qt application
/// and persists the choice via AppSettings. A single instance should be
/// owned by MainWindow (or equivalent root) and have applyTheme() called
/// on app start and whenever the user changes the theme.
class ThemeManager : public QObject {
    Q_OBJECT
public:
    explicit ThemeManager(pdfe::app::AppSettings* settings, QObject* parent = nullptr);

    /// Load the persisted theme from settings and apply it immediately.
    /// Safe to call even if settings were never written — falls back to
    /// Theme::System.
    void applyPersistedTheme();

    /// Apply a specific theme and persist it.
    void setTheme(Theme theme);

    /// Current theme (last applied).
    Theme theme() const { return current_; }

    /// Build the palette for a given theme. Static for testing.
    static QPalette paletteFor(Theme theme);

    /// Convert Theme ↔ persisted string ("light", "dark", "system").
    static QString themeToString(Theme theme);
    static Theme   themeFromString(const QString& s);

    /// Re-derive and re-apply the application palette for an OS colour-scheme
    /// change (WIN-35). Connected to QStyleHints::colorSchemeChanged in the
    /// constructor; public so a test can drive it directly as well as through
    /// the signal.
    ///
    /// Deliberately a NO-OP unless the current theme is Theme::System — an
    /// explicit Light/Dark choice by the user must survive the OS flipping,
    /// otherwise following the OS would silently override the preference.
    void onSystemColorSchemeChanged(Qt::ColorScheme scheme);

signals:
    void themeChanged(Theme newTheme);

    /// Emitted when a live OS colour-scheme change caused the palette to be
    /// re-derived while the theme is Theme::System. @p effective is the
    /// concrete Light/Dark palette that was applied. Not emitted when the user
    /// has explicitly chosen Light or Dark (that choice is not overridden).
    void systemPaletteRefreshed(Theme effective);

private:
    /// Read / write the persisted theme string ("light" / "dark" / "system").
    ///
    /// F33: these route through @c settings_ (which honours both the
    /// application-level org/app identity and the PDFE_SETTINGS_PATH seam),
    /// falling back to a default-constructed QSettings — also
    /// identity-derived — when no AppSettings was supplied. Neither path
    /// names an organization or application literally; doing so would pin
    /// the hive in the source and make the class unreachable by test
    /// isolation, which is the defect F33 closed.
    QString persistedThemeString() const;
    void    persistThemeString(const QString& value);

    pdfe::app::AppSettings* settings_ = nullptr;
    Theme current_ = Theme::System;
};

} // namespace pdfe::theme
