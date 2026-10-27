// -*- coding: utf-8 -*-
//
// Phase F8 / gap WIN-35 — ThemeManager must follow a LIVE OS light<->dark
// switch, and must NOT override an explicit user Light/Dark choice when it
// does.
//
// Before the fix, ThemeManager read QGuiApplication::styleHints()->colorScheme()
// exactly once, inside paletteFor(), and nothing in src/ ever connected to
// QStyleHints::colorSchemeChanged. A user flipping Windows between light and
// dark mid-session stayed on the palette derived at startup until restart.
//
// These tests drive the REAL signal (QStyleHints::colorSchemeChanged) rather
// than calling the handler directly, so they fail if the connection is removed
// as well as if the handler regresses. The control test is the other half:
// an explicit Light/Dark preference must survive an OS switch untouched.

#include <gtest/gtest.h>

#include "theme/ThemeManager.h"
#include "app/AppSettings.h"
#include "support/SettingsIsolation.h"

#include <QApplication>
#include <QColor>
#include <QMetaObject>
#include <QPalette>
#include <QSettings>
#include <QSignalSpy>
#include <QStyleHints>

#include <memory>
#include <optional>

using namespace pdfe::theme;

namespace {

// The dark palette's Window colour is the hard-coded ThemeManager dark base.
const QColor kDarkWindow(53, 53, 53);

// Drive the platform signal itself. QStyleHints::colorSchemeChanged is a
// signal, and signals are invokable methods, so this delivers a genuine
// emission through every connection ThemeManager made — no back door into
// the handler.
bool emitOsColorSchemeChanged(Qt::ColorScheme scheme)
{
    QStyleHints* hints = QGuiApplication::styleHints();
    if (!hints) return false;
    return QMetaObject::invokeMethod(hints, "colorSchemeChanged",
                                     Qt::DirectConnection,
                                     Q_ARG(Qt::ColorScheme, scheme));
}

} // namespace

// F33: the clears below used to name HKCU\Software\PDFE\PDFE — the SHIPPED
// application's hive — so a suite run destroyed the developer's real saved
// theme. A SettingsScope now moves the whole fixture into a purged
// PDFE-Test hive; the clears target that (bare QSettings) instead. See the
// fixture comment in test_theme_and_field_count.cpp for the full reasoning.
class ThemeOsSchemeFollowTest : public ::testing::Test {
protected:
    void SetUp() override {
        scope_.emplace(QStringLiteral("PDFE-Test"),
                       QStringLiteral("ThemeOsSchemeFollowTest"),
                       /*purgeOnExit=*/true);
        QSettings().clear();
        QSettings().sync();
        settings_ = std::make_unique<pdfe::app::AppSettings>();
        savedPalette_ = QApplication::palette();
    }
    void TearDown() override {
        // The application palette is process-global; restore it so this test
        // cannot colour any later test in the shard.
        QApplication::setPalette(savedPalette_);
        QSettings().clear();
        QSettings().sync();
        settings_.reset();
        scope_.reset();
    }
    std::optional<pdfe_test::SettingsScope> scope_;
    std::unique_ptr<pdfe::app::AppSettings> settings_;
    QPalette savedPalette_;
};

// WIN-35 core: theme = System, OS flips to dark -> palette actually changes.
TEST_F(ThemeOsSchemeFollowTest, SystemThemeFollowsLiveOsSwitchToDark)
{
    ThemeManager mgr(settings_.get());
    mgr.setTheme(Theme::System);
    QSignalSpy refreshed(&mgr, &ThemeManager::systemPaletteRefreshed);

    // Force a known non-dark starting point so the assertion below cannot
    // pass merely because the host OS already happens to be dark.
    QApplication::setPalette(ThemeManager::paletteFor(Theme::Light));
    ASSERT_NE(QApplication::palette().color(QPalette::Window), kDarkWindow);

    ASSERT_TRUE(emitOsColorSchemeChanged(Qt::ColorScheme::Dark))
        << "could not deliver QStyleHints::colorSchemeChanged";

    EXPECT_EQ(QApplication::palette().color(QPalette::Window), kDarkWindow)
        << "an OS switch to dark must re-derive the palette while theme=System";
    EXPECT_EQ(mgr.theme(), Theme::System)
        << "following the OS must not rewrite the user's theme selection";
    EXPECT_EQ(refreshed.count(), 1)
        << "the refresh must be reported exactly once";
}

// The same in reverse — dark -> light must also be followed.
TEST_F(ThemeOsSchemeFollowTest, SystemThemeFollowsLiveOsSwitchToLight)
{
    ThemeManager mgr(settings_.get());
    mgr.setTheme(Theme::System);
    QApplication::setPalette(ThemeManager::paletteFor(Theme::Dark));
    ASSERT_EQ(QApplication::palette().color(QPalette::Window), kDarkWindow);

    ASSERT_TRUE(emitOsColorSchemeChanged(Qt::ColorScheme::Light));

    EXPECT_NE(QApplication::palette().color(QPalette::Window), kDarkWindow)
        << "an OS switch to light must leave the dark palette behind";
    EXPECT_EQ(QApplication::palette().color(QPalette::Window),
              ThemeManager::paletteFor(Theme::Light).color(QPalette::Window));
}

// CONTROL 1: an explicit Light choice must NOT be overridden by the OS.
TEST_F(ThemeOsSchemeFollowTest, ExplicitLightIsNotOverriddenByOsSwitchToDark)
{
    ThemeManager mgr(settings_.get());
    mgr.setTheme(Theme::Light);
    QSignalSpy refreshed(&mgr, &ThemeManager::systemPaletteRefreshed);
    const QPalette applied = QApplication::palette();
    ASSERT_NE(applied.color(QPalette::Window), kDarkWindow);

    ASSERT_TRUE(emitOsColorSchemeChanged(Qt::ColorScheme::Dark));

    EXPECT_EQ(QApplication::palette().color(QPalette::Window),
              applied.color(QPalette::Window))
        << "an explicit Light preference must survive the OS going dark";
    EXPECT_EQ(mgr.theme(), Theme::Light);
    EXPECT_EQ(refreshed.count(), 0)
        << "no palette refresh may be reported for an explicitly themed app";
}

// CONTROL 2: an explicit Dark choice must NOT be overridden either.
TEST_F(ThemeOsSchemeFollowTest, ExplicitDarkIsNotOverriddenByOsSwitchToLight)
{
    ThemeManager mgr(settings_.get());
    mgr.setTheme(Theme::Dark);
    QSignalSpy refreshed(&mgr, &ThemeManager::systemPaletteRefreshed);
    ASSERT_EQ(QApplication::palette().color(QPalette::Window), kDarkWindow);

    ASSERT_TRUE(emitOsColorSchemeChanged(Qt::ColorScheme::Light));

    EXPECT_EQ(QApplication::palette().color(QPalette::Window), kDarkWindow)
        << "an explicit Dark preference must survive the OS going light";
    EXPECT_EQ(mgr.theme(), Theme::Dark);
    EXPECT_EQ(refreshed.count(), 0);
}

// A destroyed ThemeManager must not keep reacting (no leaked connection, and
// no call into freed memory). Emitting after destruction must be inert.
TEST_F(ThemeOsSchemeFollowTest, ConnectionIsReleasedWithTheManager)
{
    {
        ThemeManager mgr(settings_.get());
        mgr.setTheme(Theme::System);
    }
    QApplication::setPalette(ThemeManager::paletteFor(Theme::Light));
    const QColor before = QApplication::palette().color(QPalette::Window);

    ASSERT_TRUE(emitOsColorSchemeChanged(Qt::ColorScheme::Dark));

    EXPECT_EQ(QApplication::palette().color(QPalette::Window), before)
        << "a destroyed ThemeManager must no longer respond to the OS signal";
}

// Two live managers must not multiply the effect (the connection is made once
// per instance, in the constructor — not on every applyTheme call).
TEST_F(ThemeOsSchemeFollowTest, RepeatedSetThemeDoesNotDuplicateTheConnection)
{
    ThemeManager mgr(settings_.get());
    mgr.setTheme(Theme::System);
    mgr.setTheme(Theme::Dark);
    mgr.setTheme(Theme::System);
    mgr.applyPersistedTheme();
    mgr.setTheme(Theme::System);
    QSignalSpy refreshed(&mgr, &ThemeManager::systemPaletteRefreshed);

    ASSERT_TRUE(emitOsColorSchemeChanged(Qt::ColorScheme::Dark));

    EXPECT_EQ(refreshed.count(), 1)
        << "the OS handler must run exactly once per emission";
}
