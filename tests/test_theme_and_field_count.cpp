// -*- coding: utf-8 -*-
// Phase 8d: ThemeManager + StatusBar::formFieldCount label tests.
#include <gtest/gtest.h>
#include "theme/ThemeManager.h"
#include "app/AppSettings.h"
#include "ui/StatusBar.h"
#include "support/SettingsIsolation.h"

#include <QApplication>
#include <QLabel>
#include <QPalette>
#include <QSettings>
#include <QSignalSpy>

#include <memory>
#include <optional>

using namespace pdfe::theme;
using namespace pdfe::ui;

// -------- ThemeManager --------

// F33: this fixture used to `QSettings("PDFE", "PDFE").clear()` in both SetUp
// and TearDown — that is HKCU\Software\PDFE\PDFE, the hive the SHIPPED
// application uses. Running pdfe_tests.exe on a machine with PDFE installed
// therefore wiped the user's real theme, recent-file list and preferences.
// F28 measured that happening and recorded it as a carried finding, because
// fixing it needed a product change first: ThemeManager named the hive
// literally, so pointing the fixture at a test identity would have had no
// effect on where the class under test actually wrote.
//
// With ThemeManager routed through AppSettings (F33), a SettingsScope moves
// the whole fixture — AppSettings, ThemeManager and any bare QSettings below —
// into HKCU\Software\PDFE-Test\ThemeManagerTest, which purgeOnExit deletes.
// The `clear()` calls are kept but now target the SCOPED hive (bare
// QSettings), preserving the per-test clean slate without touching HKCU\PDFE.
class ThemeManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        scope_.emplace(QStringLiteral("PDFE-Test"),
                       QStringLiteral("ThemeManagerTest"),
                       /*purgeOnExit=*/true);
        // Clean settings so tests start from a known state.
        QSettings().clear();
        QSettings().sync();
        settings = std::make_unique<pdfe::app::AppSettings>();
    }
    void TearDown() override {
        QSettings().clear();
        QSettings().sync();
        settings.reset();
        scope_.reset();
    }
    std::optional<pdfe_test::SettingsScope> scope_;
    std::unique_ptr<pdfe::app::AppSettings> settings;
};

TEST_F(ThemeManagerTest, ThemeToAndFromStringRoundTrip)
{
    EXPECT_EQ(ThemeManager::themeToString(Theme::Light),  QString("light"));
    EXPECT_EQ(ThemeManager::themeToString(Theme::Dark),   QString("dark"));
    EXPECT_EQ(ThemeManager::themeToString(Theme::System), QString("system"));

    EXPECT_EQ(ThemeManager::themeFromString("light"),  Theme::Light);
    EXPECT_EQ(ThemeManager::themeFromString("dark"),   Theme::Dark);
    EXPECT_EQ(ThemeManager::themeFromString("system"), Theme::System);
    EXPECT_EQ(ThemeManager::themeFromString("banana"), Theme::System);  // fallback
}

TEST_F(ThemeManagerTest, LightPaletteDiffersFromDark)
{
    QPalette light = ThemeManager::paletteFor(Theme::Light);
    QPalette dark  = ThemeManager::paletteFor(Theme::Dark);
    // Dark palette's Window color is the hardcoded (53,53,53).
    EXPECT_EQ(dark.color(QPalette::Window), QColor(53, 53, 53));
    // Light palette's Window color should not be the dark base.
    EXPECT_NE(light.color(QPalette::Window), QColor(53, 53, 53));
}

TEST_F(ThemeManagerTest, SetThemePersistsAndEmitsSignal)
{
    ThemeManager mgr(settings.get());
    QSignalSpy spy(&mgr, &ThemeManager::themeChanged);

    mgr.setTheme(Theme::Dark);
    EXPECT_EQ(mgr.theme(), Theme::Dark);
    ASSERT_EQ(spy.count(), 1);

    // Persisted value should round-trip. Bare QSettings — the scoped identity
    // installed by the fixture, which is also the one ThemeManager now uses.
    QSettings qs;
    qs.sync();
    EXPECT_EQ(qs.value("Preferences/theme").toString(), QString("dark"));
}

TEST_F(ThemeManagerTest, ApplyPersistedThemeReadsSettingsValue)
{
    {
        QSettings qs;  // scoped identity — see the fixture comment
        qs.setValue("Preferences/theme", "dark");
        qs.sync();
    }
    ThemeManager mgr(settings.get());
    mgr.applyPersistedTheme();
    EXPECT_EQ(mgr.theme(), Theme::Dark);
}

TEST_F(ThemeManagerTest, DefaultSystemThemeWhenNothingPersisted)
{
    ThemeManager mgr(settings.get());
    mgr.applyPersistedTheme();  // settings are empty (cleared in SetUp)
    EXPECT_EQ(mgr.theme(), Theme::System);
}

TEST_F(ThemeManagerTest, ApplyChangesApplicationPalette)
{
    const QPalette before = QApplication::palette();
    ThemeManager mgr(settings.get());
    mgr.setTheme(Theme::Dark);
    const QPalette after = QApplication::palette();
    EXPECT_EQ(after.color(QPalette::Window), QColor(53, 53, 53))
        << "setTheme(Dark) should update the application palette";
    // Reset for next test to avoid polluting the shared app palette.
    QApplication::setPalette(before);
}

// ---- PDFE_TEST_DETERMINISTIC_LAYOUT pins the palette (DCA 0115) ----------
//
// The persisted theme defaults to Theme::System, which derives the palette
// from the OS colour scheme -- so every theme-derived surface becomes a
// property of the SESSION, and a pixel oracle over one asserts the session.
// Measured in CI run 38163981959: phase5_print_preview_dialog_open differed on
// 47% of pixels at a mean luma delta of 21, with flat areas AND ink both
// identical -- only the mid-tone dialog chrome had moved, because the palette
// had. Same argument as the dock layout, which the same flag already pins.

class DeterministicLayoutPaletteTest : public ThemeManagerTest {
protected:
    void SetUp() override {
        ThemeManagerTest::SetUp();
        had_ = qEnvironmentVariableIsSet(kFlag);
        if (had_) prev_ = qEnvironmentVariable(kFlag);
        qputenv(kFlag, QByteArray("1"));
    }
    void TearDown() override {
        if (had_) qputenv(kFlag, prev_.toUtf8());
        else      qunsetenv(kFlag);
        ThemeManagerTest::TearDown();
    }
    static constexpr const char* kFlag = "PDFE_TEST_DETERMINISTIC_LAYOUT";
    bool had_ = false;
    QString prev_;
};

TEST_F(DeterministicLayoutPaletteTest, PinsLightEvenWhenDarkIsPersisted)
{
    {
        QSettings qs;  // scoped identity -- see the fixture comment
        qs.setValue("Preferences/theme", "dark");
        qs.sync();
    }
    const QPalette before = QApplication::palette();
    ThemeManager mgr(settings.get());
    mgr.applyPersistedTheme();

    EXPECT_EQ(mgr.theme(), Theme::Light)
        << "the flag must override an explicitly persisted dark theme; "
           "otherwise a visual baseline still depends on whose profile ran it";
    EXPECT_EQ(QApplication::palette().color(QPalette::Window),
              ThemeManager::paletteFor(Theme::Light).color(QPalette::Window))
        << "the APPLIED palette is what a capture sees, so assert that rather "
           "than only the enum";
    EXPECT_NE(QApplication::palette().color(QPalette::Window), QColor(53, 53, 53))
        << "dark base leaked through the pin";
    QApplication::setPalette(before);
}

TEST_F(DeterministicLayoutPaletteTest, DoesNotPersistThePin)
{
    {
        QSettings qs;
        qs.setValue("Preferences/theme", "dark");
        qs.sync();
    }
    const QPalette before = QApplication::palette();
    ThemeManager mgr(settings.get());
    mgr.applyPersistedTheme();

    QSettings qs;
    qs.sync();
    EXPECT_EQ(qs.value("Preferences/theme").toString(), QString("dark"))
        << "a test run must not rewrite the preferences of the account it runs "
           "as; setTheme() persists, which is why the pin does not call it";
    QApplication::setPalette(before);
}

TEST_F(DeterministicLayoutPaletteTest, PinSurvivesAnOsColorSchemeChange)
{
    const QPalette before = QApplication::palette();
    ThemeManager mgr(settings.get());
    mgr.applyPersistedTheme();
    ASSERT_EQ(mgr.theme(), Theme::Light);

    // WIN-35 makes Theme::System follow a live OS flip. The pin sets
    // current_ to Light precisely so that handler returns early -- otherwise a
    // user (or the OS) flipping to dark mid-suite would move the palette
    // between a capture and its comparison.
    // Called directly: the header declares this public precisely so "a test can
    // drive it directly as well as through the signal". It is not a slot, so
    // QMetaObject::invokeMethod by name would fail at run time.
    mgr.onSystemColorSchemeChanged(Qt::ColorScheme::Dark);
    EXPECT_EQ(QApplication::palette().color(QPalette::Window),
              ThemeManager::paletteFor(Theme::Light).color(QPalette::Window))
        << "an OS dark switch moved the pinned palette";
    QApplication::setPalette(before);
}

// -------- StatusBar::formFieldCount --------

TEST(StatusBarFormFieldCount, LabelExistsWithAccessibleName)
{
    StatusBar sb;
    auto* label = sb.findChild<QLabel*>("formFieldCount");
    ASSERT_NE(label, nullptr) << "formFieldCount label missing";
    EXPECT_EQ(label->accessibleName(), QString("Form Field Count"));
}

TEST(StatusBarFormFieldCount, DefaultTextIsNoFormFields)
{
    StatusBar sb;
    auto* label = sb.findChild<QLabel*>("formFieldCount");
    ASSERT_NE(label, nullptr);
    EXPECT_EQ(label->text(), QString("No form fields"));
}

TEST(StatusBarFormFieldCount, SetCountUpdatesText)
{
    StatusBar sb;
    auto* label = sb.findChild<QLabel*>("formFieldCount");
    ASSERT_NE(label, nullptr);

    sb.setFormFieldCount(0);
    EXPECT_EQ(label->text(), QString("No form fields"));

    sb.setFormFieldCount(1);
    EXPECT_EQ(label->text(), QString("1 form field"));

    sb.setFormFieldCount(7);
    EXPECT_EQ(label->text(), QString("7 form fields"));

    // Negative treated as 0.
    sb.setFormFieldCount(-3);
    EXPECT_EQ(label->text(), QString("No form fields"));
}

TEST(StatusBarFormFieldCount, ClearInfoResetsFormFieldCount)
{
    StatusBar sb;
    sb.setFormFieldCount(12);
    sb.clearInfo();
    auto* label = sb.findChild<QLabel*>("formFieldCount");
    ASSERT_NE(label, nullptr);
    EXPECT_EQ(label->text(), QString("No form fields"));
}
