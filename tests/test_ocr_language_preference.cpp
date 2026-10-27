// -*- coding: utf-8 -*-
// OCR language selection UI (karen #3): validates the AppSettings
// round-trip and the PreferencesDialog combo surfaces + persists the
// Tesseract language code. The engine already supported multiple
// languages via OcrConfig.language; this test ensures the UI path
// reaches the setting from the user's click.
#include <gtest/gtest.h>
#include "app/AppSettings.h"
#include "ui/PreferencesDialog.h"
#include "support/SettingsIsolation.h"

#include <optional>

#include <QComboBox>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QCoreApplication>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>

namespace {

// ISOLATION (meta-audit follow-up F8): this fixture used to set
// QSettings::setPath(QSettings::IniFormat, ...) and clear an IniFormat
// QSettings. That isolated NOTHING. `AppSettings`, with PDFE_SETTINGS_PATH
// unset, constructs a DEFAULT `QSettings()` — NativeFormat, i.e. the Windows
// registry under organizationName/applicationName. So the clear() emptied an
// INI file nothing ever read, while every write landed in
// HKCU\Software\PDFE-Test\OcrLangTest\Preferences and PERSISTED ACROSS RUNS.
//
// The consequence was a test that rotted into a deterministic failure:
// MultiLanguageCodeAccepted wrote ocrLanguage=eng+fra+deu to the registry,
// that value outlived the process, and DefaultLanguageIsEnglish then read it
// back on every subsequent run. It was previously logged as a "known
// transient that heals on rerun" — it was not transient, it was a latent
// isolation bug whose symptom depended on whether the registry had been
// written yet. Reruns could not heal it once the key existed.
//
// The fix uses PDFE_SETTINGS_PATH, the seam AppSettings already provides for
// exactly this (see src/app/AppSettings.cpp): it forces IniFormat against an
// explicit throwaway file, so each test gets genuinely fresh settings and
// nothing touches the user's real preferences or survives the process.
class OcrLanguagePreferenceTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(tempDir_.isValid()) << "could not create a temp settings dir";
        settingsFile_ = tempDir_.filePath("ocr_lang_prefs.ini");

        // Point AppSettings at a per-test INI that does not exist yet, so the
        // first read genuinely sees defaults rather than a previous run.
        qputenv("PDFE_SETTINGS_PATH", settingsFile_.toLocal8Bit());

        // Kept for any code path that still resolves settings by org/app name.
        //
        // ISOLATION, PART 2 (meta-audit follow-up F28). The F8 fix above covers
        // pdfe::app::AppSettings ONLY — that is the one class that consults
        // PDFE_SETTINGS_PATH. These two calls are process-global and were never
        // undone, so from the moment this fixture first ran, every LATER test
        // in the same pdfe_tests.exe that constructed a bare `QSettings`
        // resolved to HKCU\Software\PDFE-Test\OcrLangTest instead of the
        // product hive. A live registry query on the dev box found
        // ...\OcrLangTest\pdfe\imageStamps, \RecentFiles, \Security and
        // \TrustAnchors under it — none of which this fixture writes. They came
        // from ImageStampLibrary, the recent-files/preferences tests and the
        // LTV trust-anchor tests, downstream of the leak.
        //
        // SettingsScope restores org/app/version/defaultFormat in TearDown on
        // every exit path and purges the test-only hive so it does not survive
        // the process. See tests/test_settings_isolation.cpp for the guard that
        // fails if this is removed.
        scope_.emplace(QStringLiteral("PDFE-Test"),
                       QStringLiteral("OcrLangTest"),
                       /*purgeOnExit=*/true);
    }

    void TearDown() override {
        // Never leak the override into unrelated tests sharing this process.
        qunsetenv("PDFE_SETTINGS_PATH");
        scope_.reset();
    }

    QTemporaryDir tempDir_;
    QString settingsFile_;
    std::optional<pdfe_test::SettingsScope> scope_;
};

} // namespace

TEST_F(OcrLanguagePreferenceTest, DefaultLanguageIsEnglish)
{
    pdfe::app::AppSettings settings;
    EXPECT_EQ(settings.ocrLanguage().toStdString(), "eng");
}

TEST_F(OcrLanguagePreferenceTest, SetLanguagePersists)
{
    {
        pdfe::app::AppSettings s1;
        s1.setOcrLanguage("fra");
        s1.sync();
    }
    pdfe::app::AppSettings s2;
    EXPECT_EQ(s2.ocrLanguage().toStdString(), "fra");
}

TEST_F(OcrLanguagePreferenceTest, MultiLanguageCodeAccepted)
{
    pdfe::app::AppSettings settings;
    settings.setOcrLanguage("eng+fra+deu");
    settings.sync();
    pdfe::app::AppSettings reread;
    EXPECT_EQ(reread.ocrLanguage().toStdString(), "eng+fra+deu");
}

TEST_F(OcrLanguagePreferenceTest, PreferencesDialogExposesCombo)
{
    pdfe::app::AppSettings settings;
    pdfe::ui::PreferencesDialog dlg(&settings);
    auto* combo = dlg.findChild<QComboBox*>("ocrLanguageCombo");
    ASSERT_NE(combo, nullptr) << "PreferencesDialog should expose ocrLanguageCombo";
    // At minimum the dropdown should offer several common languages.
    EXPECT_GE(combo->count(), 10);
    EXPECT_GE(combo->findData(QString("eng")), 0);
    EXPECT_GE(combo->findData(QString("fra")), 0);
    EXPECT_GE(combo->findData(QString("jpn")), 0);
}

TEST_F(OcrLanguagePreferenceTest, DialogLoadsCurrentLanguageOnOpen)
{
    pdfe::app::AppSettings settings;
    settings.setOcrLanguage("deu");
    settings.sync();

    pdfe::ui::PreferencesDialog dlg(&settings);
    auto* combo = dlg.findChild<QComboBox*>("ocrLanguageCombo");
    ASSERT_NE(combo, nullptr);
    EXPECT_EQ(combo->currentData().toString().toStdString(), "deu");
}

TEST_F(OcrLanguagePreferenceTest, AcceptPersistsComboSelection)
{
    pdfe::app::AppSettings settings;
    pdfe::ui::PreferencesDialog dlg(&settings);
    auto* combo = dlg.findChild<QComboBox*>("ocrLanguageCombo");
    ASSERT_NE(combo, nullptr);
    int fraIdx = combo->findData(QString("fra"));
    ASSERT_GE(fraIdx, 0);
    combo->setCurrentIndex(fraIdx);

    auto* box = dlg.findChild<QDialogButtonBox*>("preferencesButtonBox");
    ASSERT_NE(box, nullptr);
    auto* ok = box->button(QDialogButtonBox::Ok);
    ASSERT_NE(ok, nullptr);
    ok->click();

    pdfe::app::AppSettings reread;
    EXPECT_EQ(reread.ocrLanguage().toStdString(), "fra");
}

TEST_F(OcrLanguagePreferenceTest, DialogAcceptsFreeTextLanguageCode)
{
    pdfe::app::AppSettings settings;
    pdfe::ui::PreferencesDialog dlg(&settings);
    auto* combo = dlg.findChild<QComboBox*>("ocrLanguageCombo");
    ASSERT_NE(combo, nullptr);
    ASSERT_TRUE(combo->isEditable());
    combo->setEditText("eng+fra");

    auto* box = dlg.findChild<QDialogButtonBox*>("preferencesButtonBox");
    auto* ok = box->button(QDialogButtonBox::Ok);
    ok->click();

    pdfe::app::AppSettings reread;
    EXPECT_EQ(reread.ocrLanguage().toStdString(), "eng+fra");
}

TEST_F(OcrLanguagePreferenceTest, EmptyFreeTextFallsBackToEng)
{
    pdfe::app::AppSettings settings;
    settings.setOcrLanguage("fra");
    settings.sync();

    pdfe::ui::PreferencesDialog dlg(&settings);
    auto* combo = dlg.findChild<QComboBox*>("ocrLanguageCombo");
    combo->setEditText("   ");  // whitespace-only

    auto* box = dlg.findChild<QDialogButtonBox*>("preferencesButtonBox");
    auto* ok = box->button(QDialogButtonBox::Ok);
    ok->click();

    pdfe::app::AppSettings reread;
    EXPECT_EQ(reread.ocrLanguage().toStdString(), "eng")
        << "Whitespace-only input should fall back to eng";
}
